// POD X3 control-channel wire format (USB interface 1, bulk EP 0x01 / 0x81).
//
// Protocol facts come from community reverse engineering:
//   - andree182/podx3            https://github.com/andree182/podx3
//   - POD-Controller PROTOCOL.md https://github.com/adrianoftyriel/POD-Controller
// and were re-checked against a real POD X3 (0x0e41:0x414a) on macOS.

export const VENDOR_ID = 0x0e41;
export const PRODUCT_IDS: Record<number, string> = { 0x414a: 'POD X3', 0x414b: 'POD X3 Live' };

export const CONTROL_INTERFACE = 1;
export const BULK_EP = 1; // endpoint number; 0x01 OUT / 0x81 IN
export const PACKET_LEN = 64;

export const FLAG_FIRST = 0x01;
export const FLAG_CONTINUATION = 0x04;
const CHUNK_MAX_PAYLOAD = 0xfc;

export const CHANNEL_DEVICE = 0x00;
export const CHANNEL_LIVE = 0x01;
export const CHANNEL_PATCH = 0x02;

export const PATCH_LEN = 4096;
export const TONE_LEN = 2048;
export const SLOT_COUNT = 64;

// Message types (byte 0) and subcommands (byte 7) of a reassembled message.
export const T = { DUMP: 0x01, CONFIG: 0x02, INT: 0x04, TONE: 0x05, FLOAT: 0x06 } as const;
export const SUB = {
  REQUEST_DUMP: 0x00, // 02 host->POD, u32 slot
  DUMP_REPLY: 0x01, // 01 POD->host, 8 + 4096 bytes
  WRITE_DUMP: 0x02, // 02 host->POD, u32 slot + 4096 bytes: store a patch in memory
  ACK: 0x03, // 02 POD->host, 12 bytes
  PUSH_TONE: 0x04, // 02 host->POD, u32 tone + 2048 bytes
  SELECT_SLOT: 0x27, // 02 host->POD, u32 slot
  QUERY: 0x21, // 02 host->POD, u32 id
  QUERY_REPLY: 0x22, // 04 POD->host, 20 bytes
  BLOCK_MOVE: 0x12,
  BLOCK_ENABLED: 0x13,
  TEMPO_SYNC: 0x14,
  FLOAT_SET: 0x15,
  TONE_SET: 0x16,
  DEVICE_SETTING: 0x20,
} as const;
export const REPLY_LEN = { DUMP: 8 + PATCH_LEN, ACK: 12, QUERY: 20 } as const;

export const SETTING = { FIRMWARE: 0x02, SELECTED_TONE: 0x03, OUTPUTS: 0x07 } as const;

// Parameter namespaces: the high half of a 32-bit parameter id.
export const NS = { NORMAL: 0x3f10, MIX: 0x3f01, UNITS: 0x3f00 } as const;

const u32 = (v: number): Buffer => {
  const b = Buffer.alloc(4);
  b.writeUInt32LE(v >>> 0);
  return b;
};
const u16 = (v: number): Buffer => {
  const b = Buffer.alloc(2);
  b.writeUInt16LE(v);
  return b;
};
const f32 = (v: number): Buffer => {
  const b = Buffer.alloc(4);
  b.writeFloatLE(v);
  return b;
};

// 8-byte header: type, byte1, route 0A 40 <channel> 03, 00, subcommand.
function header(type: number, sub: number, channel: number, byte1 = 0): Buffer {
  return Buffer.from([type, byte1, 0x0a, 0x40, channel, 0x03, 0x00, sub]);
}

// Split a message into chunks of 4-byte header + up to 252 bytes, the way
// GearBox frames its writes. The result is sent 64 bytes per USB transfer.
export function frame(message: Buffer): Buffer {
  const parts: Buffer[] = [];
  let offset = 0;
  do {
    const n = Math.min(CHUNK_MAX_PAYLOAD, message.length - offset);
    parts.push(Buffer.from([n, 0, offset === 0 ? FLAG_FIRST : FLAG_CONTINUATION, 0]));
    parts.push(message.subarray(offset, offset + n));
    offset += n;
  } while (offset < message.length);
  return Buffer.concat(parts);
}

export const encode = {
  requestDump: (slot: number) => frame(Buffer.concat([header(T.CONFIG, SUB.REQUEST_DUMP, CHANNEL_PATCH), u32(slot)])),

  // Writes the POD's preset memory (byte 1 is 04 for this message, as GearBox
  // sends it). Acked by 02/03. POD-Controller verified it reads back byte for byte.
  writeDump: (slot: number, patch: Buffer) => {
    if (patch.length !== PATCH_LEN) throw new Error(`patch must be ${PATCH_LEN} bytes`);
    if (!Number.isInteger(slot) || slot < 0 || slot >= SLOT_COUNT) throw new Error(`invalid slot ${slot}`);
    return frame(Buffer.concat([header(T.CONFIG, SUB.WRITE_DUMP, CHANNEL_PATCH, 0x04), u32(slot), patch]));
  },

  selectSlot: (slot: number) => frame(Buffer.concat([header(T.CONFIG, SUB.SELECT_SLOT, CHANNEL_PATCH), u32(slot)])),

  pushTone: (tone: number, block: Buffer) => {
    if (block.length !== TONE_LEN) throw new Error(`tone block must be ${TONE_LEN} bytes`);
    return frame(Buffer.concat([header(T.CONFIG, SUB.PUSH_TONE, CHANNEL_PATCH, 0x02), u32(tone), block]));
  },

  query: (id: number) => frame(Buffer.concat([header(T.CONFIG, SUB.QUERY, CHANNEL_LIVE), u32(id)])),

  deviceSetting: (channel: number, id: number, value: number) =>
    frame(Buffer.concat([header(T.INT, SUB.DEVICE_SETTING, channel), u32(0), u32(id), u32(value)])),

  // <tone u32> <slot u16> <group u16> <value u32>: block on/off, tempo sync, move.
  intSet: (tone: number, sub: number, slot: number, group: number, value: number) =>
    frame(Buffer.concat([header(T.INT, sub, CHANNEL_LIVE), u32(tone), u16(slot), u16(group), u32(value)])),

  // <tone u32> <slot u16> <group u16> 01 00 00 00 <idx u16> <ns u16> <f32>
  floatSet: (tone: number, slot: number, group: number, idx: number, ns: number, value: number) =>
    frame(
      Buffer.concat([
        header(T.FLOAT, SUB.FLOAT_SET, CHANNEL_LIVE),
        u32(tone),
        u16(slot),
        u16(group),
        u32(1),
        u16(idx),
        u16(ns),
        f32(value),
      ]),
    ),

  // <tone u32> <kind u32: 1 float, 0 int> <param u32> <value>
  toneSet: (tone: number, param: number, value: number, isFloat: boolean) =>
    frame(
      Buffer.concat([
        header(T.TONE, SUB.TONE_SET, CHANNEL_LIVE),
        u32(tone),
        u32(isFloat ? 1 : 0),
        u32(param),
        isFloat ? f32(value) : u32(value),
      ]),
    ),
};

// Length of a POD->host message, from its 8-byte header, or null if unknown.
export function messageLength(msg: Buffer): number | null {
  const type = msg[0];
  const sub = msg[7];
  if (type === T.DUMP && sub === SUB.DUMP_REPLY) return REPLY_LEN.DUMP;
  if (type === T.CONFIG && sub === SUB.ACK) return REPLY_LEN.ACK;
  if (type === T.CONFIG && sub === SUB.PUSH_TONE) return 8 + 4 + TONE_LEN;
  if (type === T.INT) return 20; // 13 block on/off, 20 setting, 22 query reply, ...
  if (type === T.TONE && sub === SUB.TONE_SET) return 24;
  if (type === T.FLOAT && sub === SUB.FLOAT_SET) return 28;
  return null;
}

// Decodes the bulk-IN packet stream into messages. A packet carries chunks
// of 4-byte header + payload. The payloads form one byte stream: the POD
// does not reliably flag the first chunk of a message (a 20-byte reply can
// arrive as a 16-byte chunk flagged "continuation" plus 4 bytes in the next
// packet), so messages are cut by their known length. FLAG_FIRST only serves
// to resynchronize, and anything undecodable is dropped on flush().
export class StreamDecoder {
  #buf: Buffer = Buffer.alloc(0);

  constructor(private readonly onMessage: (msg: Buffer) => void) {}

  push(packet: Buffer): void {
    let o = 0;
    while (o + 4 <= packet.length) {
      const n = packet[o]!;
      const flags = packet[o + 2]!;
      const body = packet.subarray(o + 4, o + 4 + n);
      o += 4 + n;
      if (flags & FLAG_FIRST && this.#buf.length) this.flush();
      this.#buf = Buffer.concat([this.#buf, body]);
      this.#drain();
    }
  }

  #drain(): void {
    while (this.#buf.length >= 8) {
      const len = messageLength(this.#buf);
      if (len === null || this.#buf.length < len) return;
      this.onMessage(Buffer.from(this.#buf.subarray(0, len)));
      this.#buf = this.#buf.subarray(len);
    }
  }

  // Drop a partial or unknown message (resync point).
  flush(): void {
    if (this.#buf.length >= 8 && messageLength(this.#buf) === null) this.onMessage(Buffer.from(this.#buf));
    this.#buf = Buffer.alloc(0);
  }
}

// Slot numbering: (bank-1)*4 + channel, channel A..D = 0..3. "5A" -> 16.
export function parseSlot(input: unknown): number {
  const s = String(input).trim().toUpperCase();
  const m = s.match(/^(\d{1,2})([A-D])$/);
  if (m) {
    const bank = Number(m[1]);
    if (bank >= 1 && bank <= 16) return (bank - 1) * 4 + 'ABCD'.indexOf(m[2]!);
  } else if (/^\d+$/.test(s)) {
    const n = Number(s);
    if (n >= 0 && n < SLOT_COUNT) return n;
  }
  throw new Error(`invalid slot "${String(input)}" (use 1A..16D or 0..63)`);
}

export const slotName = (slot: number): string => `${Math.floor(slot / 4) + 1}${'ABCD'[slot % 4]}`;
