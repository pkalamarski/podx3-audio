// Stateful controller around PodX3: owns the device, the list of stored
// presets and a working copy of the patch in the edit buffer, so live edits
// can be reflected back to clients. Used by the HTTP server.

import { EventEmitter } from 'node:events';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { isAbsolute, join } from 'node:path';
import { PodX3 } from './device.js';
import { PATCH_LEN, SETTING, SLOT_COUNT, parseSlot, slotName } from './protocol.js';
import { buildPreset, readPresetFile } from './preset.js';
import {
  BLOCKS,
  type Block,
  type BlockId,
  type ModelChoice,
  type Patch,
  parsePatch,
  patchName,
  toneBlock,
  writeEnabled,
  writeModel,
  writeParam,
  modelCatalog,
} from './patch.js';

const cacheDir = new URL('../.cache/', import.meta.url);
const backupsDir = decodeURIComponent(new URL('../backups/', import.meta.url).pathname);
const presetCache = new URL('presets.json', cacheDir);

export interface PresetEntry {
  slot: number;
  slotName: string;
  name: string;
}

export interface WriteResult {
  slot: string;
  name: string;
  written: boolean; // false: the slot already held these exact bytes
  verified: true;
}

export interface PatchInfo extends Patch {
  slot: number;
  slotName: string;
  dirty: boolean;
}

export interface Status {
  connected: boolean;
  model: string | null;
  firmware: number | null;
  error: string | null;
  current: { slot: number; slotName: string; name: string; dirty: boolean } | null;
  presetsCached: number;
  scanning: number | null;
}

export type ChangeEvent =
  | { type: 'status' | 'patch' | 'presets' }
  | { type: 'scan'; progress: number }
  | { type: 'param'; tone: number; block: BlockId; key: string; value: number };

interface ControllerEvents {
  change: [ev: ChangeEvent];
  'device-message': [msg: Buffer];
}

const errorMessage = (e: unknown): string => (e instanceof Error ? e.message : String(e));

export class Controller extends EventEmitter<ControllerEvents> {
  pod: PodX3 | null = null;
  firmware: number | null = null;
  presets: PresetEntry[] = loadPresetCache();
  current: { slot: number; patch: Buffer; dirty: boolean } | null = null;
  lastError: string | null = null;
  #connecting: Promise<PodX3> | null = null;
  #scanning: { progress: number; promise: Promise<PresetEntry[]> } | null = null;

  connect(): Promise<PodX3> {
    if (this.pod && !this.pod.closed) return Promise.resolve(this.pod);
    this.#connecting ??= (async () => {
      try {
        const pod = await PodX3.open();
        pod.on('close', (err) => {
          this.lastError = err.message || 'disconnected';
          this.pod = null;
          this.emit('change', { type: 'status' });
        });
        pod.on('message', (msg) => this.emit('device-message', msg));
        this.pod = pod;
        this.firmware = await pod.query(SETTING.FIRMWARE).catch(() => null);
        this.lastError = null;
        this.emit('change', { type: 'status' });
        return pod;
      } catch (e) {
        this.lastError = errorMessage(e);
        throw e;
      } finally {
        this.#connecting = null;
      }
    })();
    return this.#connecting;
  }

  status(): Status {
    return {
      connected: !!(this.pod && !this.pod.closed),
      model: this.pod?.model ?? null,
      firmware: this.firmware,
      error: this.lastError,
      current: this.current && {
        slot: this.current.slot,
        slotName: slotName(this.current.slot),
        name: patchName(this.current.patch),
        dirty: this.current.dirty,
      },
      presetsCached: this.presets.length,
      scanning: this.#scanning?.progress ?? null,
    };
  }

  // Read the names of all 64 stored presets (a few seconds), cached on disk.
  async scanPresets(): Promise<PresetEntry[]> {
    if (this.#scanning) return this.#scanning.promise;
    const pod = await this.connect();
    const state = { progress: 0 } as { progress: number; promise: Promise<PresetEntry[]> };
    state.promise = (async () => {
      const list: PresetEntry[] = [];
      try {
        for (let slot = 0; slot < SLOT_COUNT; slot++) {
          const patch = await pod.readPatch(slot);
          list.push({ slot, slotName: slotName(slot), name: patchName(patch) });
          state.progress = slot + 1;
          this.emit('change', { type: 'scan', progress: slot + 1 });
        }
        this.presets = list;
        savePresetCache(list);
        return list;
      } finally {
        this.#scanning = null;
        this.emit('change', { type: 'presets' });
      }
    })();
    this.#scanning = state;
    return state.promise;
  }

  // Save every stored preset as a raw 4096-byte .bin file (read-only on the POD).
  async backup(): Promise<{ dir: string; count: number }> {
    const pod = await this.connect();
    const dir = new URL(`../backups/${new Date().toISOString().replace(/[:.]/g, '-')}/`, import.meta.url);
    mkdirSync(dir, { recursive: true });
    const list: PresetEntry[] = [];
    for (let slot = 0; slot < SLOT_COUNT; slot++) {
      const patch = await pod.readPatch(slot);
      writeFileSync(new URL(`${String(slot).padStart(2, '0')}-${slotName(slot)}.bin`, dir), patch);
      list.push({ slot, slotName: slotName(slot), name: patchName(patch) });
      this.emit('change', { type: 'scan', progress: slot + 1 });
    }
    writeFileSync(new URL('index.json', dir), JSON.stringify(list, null, 1));
    this.presets = list;
    savePresetCache(list);
    this.emit('change', { type: 'presets' });
    return { dir: decodeURIComponent(dir.pathname), count: list.length };
  }

  // Raw bytes of a stored preset (read-only).
  async dump(slotInput: unknown): Promise<{ slot: string; name: string; data: string }> {
    const slot = parseSlot(slotInput);
    const pod = await this.connect();
    const patch = await pod.readPatch(slot);
    return { slot: slotName(slot), name: patchName(patch), data: patch.toString('base64') };
  }

  // Store a patch in preset memory, then read the slot back and compare it byte
  // for byte. Guards: a complete backup must exist, and the preset in the edit
  // buffer (loaded through this tool) is never overwritten under it. Slots that
  // already hold the same bytes are not rewritten.
  async writeSlot(slot: number, patch: Buffer): Promise<WriteResult> {
    if (patch.length !== PATCH_LEN) throw new Error(`patch must be ${PATCH_LEN} bytes`);
    if (!hasCompleteBackup()) throw new Error('no complete backup in backups/: run "pod backup" first');
    if (this.current?.slot === slot) throw new Error(`${slotName(slot)} is loaded in the edit buffer: load another preset first`);
    const pod = await this.connect();
    const before = await pod.readPatch(slot);
    let written = false;
    if (!before.equals(patch)) {
      await pod.writePatch(slot, patch);
      written = true;
    }
    const after = await pod.readPatch(slot);
    if (!after.equals(patch)) {
      let diff = 0;
      for (let i = 0; i < PATCH_LEN; i++) if (after[i] !== patch[i]) diff++;
      throw new Error(`${slotName(slot)} reads back different from what was written (${diff} bytes differ): restore it from the backup`);
    }
    const entry = this.presets.find((p) => p.slot === slot);
    if (entry) {
      entry.name = patchName(patch);
      savePresetCache(this.presets);
      this.emit('change', { type: 'presets' });
    }
    return { slot: slotName(slot), name: patchName(patch), written, verified: true };
  }

  // Checked before a multi-slot write starts, so it does not stop halfway.
  #checkNotLoaded(slots: number[]): void {
    const cur = this.current?.slot;
    if (cur !== undefined && slots.includes(cur)) throw new Error(`${slotName(cur)} is loaded in the edit buffer: load another preset first`);
  }

  // Write slots back from a backup directory (as made by backup()).
  async restore(slotInput: unknown, dirInput: unknown): Promise<WriteResult[]> {
    const dir = resolveBackupDir(String(dirInput ?? ''));
    const slots = String(slotInput).toLowerCase() === 'all' ? [...Array(SLOT_COUNT).keys()] : [parseSlot(slotInput)];
    const patches = slots.map((slot) => readBackupSlot(dir, slot));
    this.#checkNotLoaded(slots);
    const results: WriteResult[] = [];
    for (const [i, slot] of slots.entries()) results.push(await this.writeSlot(slot, patches[i]!));
    return results;
  }

  // Build presets from JSON files (control/presets/*.json) and store them.
  // Everything is built first, so a bad file stops the run before any write.
  async apply(files: unknown): Promise<WriteResult[]> {
    if (!Array.isArray(files) || !files.length) throw new Error('give one or more preset files');
    const built = files.map((f) => buildPreset(readPresetFile(String(f))));
    const slots = built.map((b) => b.slot);
    if (new Set(slots).size !== slots.length) throw new Error('two preset files target the same slot');
    this.#checkNotLoaded(slots);
    const results: WriteResult[] = [];
    for (const { slot, patch } of built) results.push(await this.writeSlot(slot, patch));
    return results;
  }

  async listPresets({ refresh = false } = {}): Promise<PresetEntry[]> {
    if (refresh || this.presets.length !== SLOT_COUNT) await this.scanPresets();
    return this.presets;
  }

  // Load a stored preset into the edit buffer (does not modify memory).
  async load(slotInput: unknown): Promise<PatchInfo> {
    const slot = parseSlot(slotInput);
    const pod = await this.connect();
    const patch = Buffer.from(await pod.readPatch(slot));
    await pod.loadPatch(slot, patch);
    this.current = { slot, patch, dirty: false };
    const entry = this.presets.find((p) => p.slot === slot);
    if (entry && entry.name !== patchName(patch)) {
      entry.name = patchName(patch);
      savePresetCache(this.presets);
    }
    this.emit('change', { type: 'patch' });
    return this.patchInfo()!;
  }

  // Reload the stored version of the current preset (drops live edits).
  async revert(): Promise<PatchInfo> {
    if (!this.current) throw new Error('no preset loaded yet');
    return this.load(this.current.slot);
  }

  // Read a stored preset without loading it.
  async peek(slotInput: unknown): Promise<PatchInfo> {
    const slot = parseSlot(slotInput);
    const pod = await this.connect();
    const patch = Buffer.from(await pod.readPatch(slot));
    return { slot, slotName: slotName(slot), dirty: false, ...parsePatch(patch) };
  }

  patchInfo(): PatchInfo | null {
    if (!this.current) return null;
    const { slot, patch, dirty } = this.current;
    return { slot, slotName: slotName(slot), dirty, ...parsePatch(patch) };
  }

  #block(tone: number, blockId: string) {
    const cur = this.current;
    if (!cur) throw new Error('no preset loaded yet: load one first (e.g. "1A")');
    if (tone !== 0 && tone !== 1) throw new Error('tone must be 1 or 2');
    const block = parsePatch(cur.patch).tones[tone].blocks.find((b) => b.id === blockId);
    if (!block) throw new Error(`unknown block "${blockId}" (one of: ${BLOCKS.join(', ')})`);
    return { cur, block };
  }

  async setParam(tone: number, blockId: string, param: unknown, value: unknown) {
    const { cur, block } = this.#block(tone, blockId);
    const p = findParam(block, param);
    const v = Number(value);
    if (!Number.isFinite(v)) throw new Error(`invalid value "${String(value)}"`);
    if (p.normalized && (v < 0 || v > 1)) throw new Error(`${p.name} takes 0.0-1.0`);
    const pod = await this.connect();
    await pod.setFloat(tone, block.slot, block.group, p.idx, p.ns, v);
    writeParam(cur.patch, tone, block.id, p.idx, p.ns, v);
    cur.dirty = true;
    this.emit('change', { type: 'param', tone, block: block.id, key: p.key, value: v });
    return { block: block.id, param: p.name, value: v };
  }

  async setEnabled(tone: number, blockId: string, enabled: boolean) {
    const { cur, block } = this.#block(tone, blockId);
    const pod = await this.connect();
    await pod.setBlockEnabled(tone, block.slot, block.group, enabled);
    // The amp switch also covers the cab, as GearBox does.
    if (block.id === 'amp') {
      const { block: cab } = this.#block(tone, 'cab');
      await pod.setBlockEnabled(tone, cab.slot, cab.group, enabled);
      writeEnabled(cur.patch, tone, 'cab', enabled);
    }
    writeEnabled(cur.patch, tone, block.id, enabled);
    cur.dirty = true;
    this.emit('change', { type: 'patch' });
    return { block: block.id, enabled };
  }

  async setModel(tone: number, blockId: string, modelQuery: unknown) {
    const { cur, block } = this.#block(tone, blockId);
    const choices = modelCatalog()[block.id];
    if (!choices) throw new Error(`model of "${blockId}" cannot be changed`);
    const m = findModel(choices, modelQuery);
    writeModel(cur.patch, tone, block.id, m.table ?? block.table, m.model);
    const pod = await this.connect();
    await pod.pushTone(tone, Buffer.from(toneBlock(cur.patch, tone)));
    cur.dirty = true;
    this.emit('change', { type: 'patch' });
    return { block: block.id, model: m.name };
  }

  async selectTone(tone: number) {
    const pod = await this.connect();
    await pod.selectTone(tone);
    return { tone: tone + 1 };
  }
}

function findParam(block: Block, query: unknown) {
  const q = String(query).toLowerCase();
  const p =
    block.params.find((x) => x.key === q) ||
    block.params.find((x) => x.name.toLowerCase() === q) ||
    block.params.find((x) => x.name.toLowerCase().startsWith(q));
  if (!p) throw new Error(`${block.label} has no parameter "${String(query)}" (has: ${block.params.map((x) => x.name).join(', ')})`);
  return p;
}

function findModel(choices: ModelChoice[], query: unknown): ModelChoice {
  const q = String(query).toLowerCase().trim();
  if (/^\d+$/.test(q)) {
    const byId = choices.filter((c) => c.model === Number(q));
    if (byId.length === 1) return byId[0]!;
  }
  const exact = choices.find((c) => c.name.toLowerCase() === q);
  if (exact) return exact;
  const hits = choices.filter((c) => c.name.toLowerCase().includes(q));
  if (hits.length === 1) return hits[0]!;
  if (!hits.length) throw new Error(`no model matches "${String(query)}"`);
  throw new Error(`"${String(query)}" is ambiguous: ${hits.slice(0, 12).map((c) => c.name).join(' | ')}`);
}

function resolveBackupDir(dir: string): string {
  if (!dir) throw new Error('give a backup directory (a folder in backups/)');
  const path = isAbsolute(dir) ? dir : existsSync(join(backupsDir, dir)) ? join(backupsDir, dir) : dir;
  if (!existsSync(join(path, 'index.json'))) throw new Error(`${dir} is not a backup directory (no index.json)`);
  return path;
}

function readBackupSlot(dir: string, slot: number): Buffer {
  const patch = readFileSync(join(dir, `${String(slot).padStart(2, '0')}-${slotName(slot)}.bin`));
  if (patch.length !== PATCH_LEN) throw new Error(`backup of ${slotName(slot)} is not ${PATCH_LEN} bytes`);
  return patch;
}

// A backup folder holding all 64 presets.
function hasCompleteBackup(): boolean {
  try {
    return readdirSync(backupsDir).some((d) => {
      const files = readdirSync(join(backupsDir, d));
      return files.includes('index.json') && files.filter((f) => f.endsWith('.bin')).length === SLOT_COUNT;
    });
  } catch {
    return false;
  }
}

function loadPresetCache(): PresetEntry[] {
  try {
    return JSON.parse(readFileSync(presetCache, 'utf8')) as PresetEntry[];
  } catch {
    return [];
  }
}

function savePresetCache(list: PresetEntry[]): void {
  mkdirSync(cacheDir, { recursive: true });
  writeFileSync(presetCache, JSON.stringify(list, null, 1));
}
