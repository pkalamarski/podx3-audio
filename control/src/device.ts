// Userspace driver for the POD X3 control channel, on top of libusb-style
// access from the `usb` package (nusb). macOS has no driver for the POD's
// vendor-specific interfaces, so nothing has to be detached: we just claim
// interface 1. Only one process can hold it at a time.

import { EventEmitter } from 'node:events';
import { webusb } from 'usb';
import {
  VENDOR_ID,
  PRODUCT_IDS,
  CONTROL_INTERFACE,
  BULK_EP,
  PACKET_LEN,
  PATCH_LEN,
  TONE_LEN,
  CHANNEL_PATCH,
  CHANNEL_DEVICE,
  T,
  SUB,
  REPLY_LEN,
  SETTING,
  encode,
  StreamDecoder,
} from './protocol.js';

const TIMEOUT_MS = 3000;
const WRITE_TIMEOUT_MS = 10000; // preset memory write: wait longer for the ack
const DEBUG = !!process.env.POD_DEBUG;
// The usb package cancels a transfer when its timeout (default 1 s) expires.
// Aborting the IN read while the POD is streaming a reply can wedge the POD's
// control channel until it is power cycled, so the read effectively never
// times out; it is only cancelled when the device is closed.
const READ_TIMEOUT_MS = 0x7fffffff;

// The usb package's WebUSB device takes an optional timeout on transfers.
type Device = Omit<USBDevice, 'transferIn' | 'transferOut'> & {
  transferIn(endpointNumber: number, length: number, timeout?: number): Promise<USBInTransferResult>;
  transferOut(endpointNumber: number, data: Uint8Array, timeout?: number): Promise<USBOutTransferResult>;
};

interface Waiter {
  type: number;
  sub: number;
  len: number;
  resolve: (msg: Buffer) => void;
  reject: (err: Error) => void;
}

interface PodEvents {
  message: [msg: Buffer]; // unsolicited, e.g. 04/13 block announcements
  close: [err: Error];
}

const errorMessage = (e: unknown): string => (e instanceof Error ? e.message : String(e));

export class PodX3 extends EventEmitter<PodEvents> {
  static async open(): Promise<PodX3> {
    let dev: Device;
    try {
      dev = (await webusb.requestDevice({ filters: [{ vendorId: VENDOR_ID }] })) as Device;
    } catch {
      throw new Error('POD X3 not found on USB (is it powered on and plugged in?)');
    }
    if (!PRODUCT_IDS[dev.productId]) throw new Error(`unsupported Line 6 device 0x${dev.productId.toString(16)}`);
    await dev.open();
    try {
      await PodX3.#claimControl(dev);
    } catch (e) {
      await dev.close().catch(() => {});
      throw new Error(`cannot claim POD X3 control interface (${errorMessage(e)}); is another pod-x3 process running?`);
    }
    const pod = new PodX3(dev);
    const onDisconnect = (ev: USBConnectionEvent) => {
      if (ev.device !== dev) return;
      webusb.removeEventListener('disconnect', onDisconnect);
      if (!pod.#closed) pod.#fail(new Error('POD X3 was unplugged'));
    };
    webusb.addEventListener('disconnect', onDisconnect);
    pod.#startReader();
    await pod.#settle();
    return pod;
  }

  readonly model: string;
  // On macOS the interfaces are only instantiated once a configuration is
  // selected, even though the device already reports configuration 1. Select
  // it only in that case: re-selecting resets every interface, which would cut
  // off an audio stream running on interface 0 in another process.
  static async #claimControl(dev: Device): Promise<void> {
    try {
      await dev.claimInterface(CONTROL_INTERFACE);
    } catch (e) {
      if (!/interface not found/.test(errorMessage(e))) throw e;
      await dev.selectConfiguration(1);
      await dev.claimInterface(CONTROL_INTERFACE);
    }
  }

  #dev: Device;
  #waiter: Waiter | null = null;
  #queue: Promise<unknown> = Promise.resolve();
  #closed = false;
  #idleTimer: NodeJS.Timeout | undefined;
  #decoder: StreamDecoder;

  private constructor(dev: Device) {
    super();
    this.#dev = dev;
    this.model = PRODUCT_IDS[dev.productId] ?? 'POD X3';
    this.#decoder = new StreamDecoder((msg) => this.#onMessage(msg));
  }

  // Swallow anything a previous session left in the POD's output (e.g. the
  // tail of a dump), so it is not taken for a reply.
  async #settle(): Promise<void> {
    const drop = () => {};
    this.on('message', drop);
    await new Promise((r) => setTimeout(r, 300));
    this.#decoder.flush();
    this.off('message', drop);
  }

  get closed(): boolean {
    return this.#closed;
  }

  // The POD only accepts OUT transfers while an IN transfer is pending, so a
  // read is kept outstanding for the whole session. One reader keeps packet
  // order trivially; the device NAKs (does not drop) while none is pending.
  #startReader(): void {
    void (async () => {
      while (!this.#closed) {
        let r: USBInTransferResult;
        try {
          r = await this.#dev.transferIn(BULK_EP, PACKET_LEN, READ_TIMEOUT_MS);
        } catch (e) {
          if (this.#closed) return;
          if (/cancel|timeout/i.test(errorMessage(e))) continue; // idle timeout: re-arm
          this.#fail(e instanceof Error ? e : new Error(String(e)));
          return;
        }
        if (!r.data || r.data.byteLength === 0) continue;
        const packet = Buffer.from(r.data.buffer, r.data.byteOffset, r.data.byteLength);
        if (DEBUG) console.error('IN ', packet.toString('hex'));
        this.#decoder.push(packet);
        clearTimeout(this.#idleTimer);
        this.#idleTimer = setTimeout(() => this.#decoder.flush(), 500);
      }
    })();
  }

  #onMessage(msg: Buffer): void {
    const w = this.#waiter;
    if (w && msg.length === w.len && msg[0] === w.type && msg[7] === w.sub) {
      this.#waiter = null;
      w.resolve(msg);
      return;
    }
    this.emit('message', msg);
  }

  #fail(err: Error): void {
    this.#closed = true;
    this.#waiter?.reject(err);
    this.#waiter = null;
    this.emit('close', err);
  }

  // Serialize every device operation: a transact must not interleave.
  #exclusive<R>(fn: () => Promise<R>): Promise<R> {
    const run = this.#queue.then(fn);
    this.#queue = run.catch(() => {});
    return run;
  }

  // One 64-byte transfer per USB packet: the POD silently drops messages whose
  // packets arrive back to back in a single large transfer.
  async #write(bytes: Buffer): Promise<void> {
    if (this.#closed) throw new Error('POD X3 is disconnected');
    if (DEBUG) console.error('OUT', bytes.length > 64 ? `${bytes.length} bytes` : bytes.toString('hex'));
    for (let o = 0; o < bytes.length; o += PACKET_LEN) {
      await this.#dev.transferOut(BULK_EP, bytes.subarray(o, o + PACKET_LEN), TIMEOUT_MS);
    }
  }

  async #transact(request: Buffer, type: number, sub: number, len: number, timeoutMs = TIMEOUT_MS): Promise<Buffer> {
    const reply = new Promise<Buffer>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#waiter = null;
        this.#decoder.flush(); // resync on the next message
        reject(new Error(`POD X3 did not answer (waiting for ${type.toString(16)}/${sub.toString(16)})`));
      }, timeoutMs);
      this.#waiter = {
        type,
        sub,
        len,
        resolve: (m) => (clearTimeout(timer), resolve(m)),
        reject: (e) => (clearTimeout(timer), reject(e)),
      };
    });
    await this.#write(request);
    return reply;
  }

  send(bytes: Buffer): Promise<void> {
    return this.#exclusive(() => this.#write(bytes));
  }

  async #readPatch(slot: number): Promise<Buffer> {
    const reply = await this.#transact(encode.requestDump(slot), T.DUMP, SUB.DUMP_REPLY, REPLY_LEN.DUMP);
    return reply.subarray(8);
  }

  // Read a stored preset (4096 bytes: tone 1 block + tone 2 block). Read-only.
  readPatch(slot: number): Promise<Buffer> {
    return this.#exclusive(() => this.#readPatch(slot));
  }

  // Store a patch in preset memory (02/02, acked by 02/03). This overwrites
  // the slot: callers must hold a backup. Not retried on a missing ack, so
  // a slow flash write is never sent twice.
  writePatch(slot: number, patch: Buffer): Promise<void> {
    const request = encode.writeDump(slot, patch);
    return this.#exclusive(async () => {
      await this.#transact(request, T.CONFIG, SUB.ACK, REPLY_LEN.ACK, WRITE_TIMEOUT_MS);
    });
  }

  #pushTone(tone: number, block: Buffer): Promise<Buffer> {
    return this.#transact(encode.pushTone(tone, block), T.CONFIG, SUB.ACK, REPLY_LEN.ACK);
  }

  // Load a patch into the edit buffer the way GearBox does: select slot, push
  // both tone blocks (each acked), select tone 1. A bare select is ignored by
  // the POD. Stored presets are never modified by this.
  loadPatch(slot: number, patch: Buffer): Promise<void> {
    if (patch.length !== PATCH_LEN) throw new Error(`patch must be ${PATCH_LEN} bytes`);
    return this.#exclusive(async () => {
      await this.#write(encode.selectSlot(slot));
      await this.#pushTone(0, patch.subarray(0, TONE_LEN));
      await this.#pushTone(1, patch.subarray(TONE_LEN));
      await this.#write(encode.deviceSetting(CHANNEL_PATCH, SETTING.SELECTED_TONE, 0));
    });
  }

  // Replace one tone of the edit buffer (used for model changes).
  async pushTone(tone: number, block: Buffer): Promise<void> {
    await this.#exclusive(() => this.#pushTone(tone, block));
  }

  query(id: number): Promise<number> {
    return this.#exclusive(async () => {
      const r = await this.#transact(encode.query(id), T.INT, SUB.QUERY_REPLY, REPLY_LEN.QUERY);
      return r.readUInt32LE(16);
    });
  }

  setFloat(tone: number, slot: number, group: number, idx: number, ns: number, value: number): Promise<void> {
    return this.send(encode.floatSet(tone, slot, group, idx, ns, value));
  }

  setBlockEnabled(tone: number, slot: number, group: number, enabled: boolean): Promise<void> {
    return this.send(encode.intSet(tone, SUB.BLOCK_ENABLED, slot, group, enabled ? 1 : 0));
  }

  selectTone(tone: number): Promise<void> {
    return this.send(encode.deviceSetting(CHANNEL_DEVICE, SETTING.SELECTED_TONE, tone));
  }

  setToneParam(tone: number, param: number, value: number, isFloat: boolean): Promise<void> {
    return this.send(encode.toneSet(tone, param, value, isFloat));
  }

  async close(): Promise<void> {
    if (this.#closed) return;
    this.#closed = true;
    clearTimeout(this.#idleTimer);
    try {
      await this.#dev.releaseInterface(CONTROL_INTERFACE);
    } catch {}
    try {
      await this.#dev.close();
    } catch {}
  }
}
