// Decoding and editing of the 4096-byte POD X3 patch (EffectDump) blob:
// tone 1 block (2048 bytes) followed by tone 2 block. A tone block is a
// 0xE4-byte header followed by 12 block records of 0x8C bytes.

import { readFileSync } from 'node:fs';
import { NS, TONE_LEN } from './protocol.js';

interface ModelTable {
  label: string;
  models: Record<string, string>;
}
interface KnobEntry {
  name: string;
  params: Record<string, string>;
}

const dataDir = new URL('../data/', import.meta.url);
const readJson = (file: string): unknown => JSON.parse(readFileSync(new URL(file, dataDir), 'utf8'));

// models.json: { amp: {id: name}, cab: {id: name}, "<table hex>": {label, models} }
const { amp: AMP_MODELS, cab: CAB_MODELS, ...EFFECT_TABLES } = readJson('models.json') as {
  amp: Record<string, string>;
  cab: Record<string, string>;
} & Record<string, ModelTable>;
// knobs.json: { "02/<table hex>/<model>": {name, params: {"<ns><idx>": name}} }
const KNOBS = readJson('knobs.json') as Record<string, KnobEntry>;

const HEADER_LEN = 0xe4;
const RECORD_LEN = 0x8c;
const MAX_PARAMS = (RECORD_LEN - 0x0c) / 8;

// Record order inside a tone block is fixed by the firmware.
export const BLOCKS = ['amp', 'cab', 'stomp', 'mod', 'delay', 'reverb', 'gate', 'comp', 'eq', 'wah', 'volume', 'loop'] as const;
export type BlockId = (typeof BLOCKS)[number];

export const BLOCK_LABELS: Record<BlockId, string> = {
  amp: 'Amp', cab: 'Cab', stomp: 'Stomp', mod: 'Mod', delay: 'Delay', reverb: 'Reverb',
  gate: 'Gate', comp: 'Comp', eq: 'EQ', wah: 'Wah', volume: 'Volume', loop: 'FX Loop',
};
// Effect tables (record +0x02) a block can choose models from.
export const BLOCK_TABLES: Partial<Record<BlockId, string[]>> = {
  stomp: ['05', '0A', '00'], mod: ['03'], delay: ['02'], reverb: ['04'], wah: ['06'],
};
const AMP_KNOBS = ['Bass', 'Middle', 'Treble', 'Drive', 'Presence', 'Volume'];
export const MICS = ['57 On Axis', '57 Off Axis', '421 Dynamic', '67 Condenser'];

export interface Param {
  key: string; // "<ns hex4><idx hex4>", e.g. "3f100003"
  idx: number;
  ns: number;
  name: string;
  value: number;
  normalized: boolean; // 0.0-1.0, otherwise real units
}

export interface Block {
  id: BlockId;
  label: string;
  model: number;
  table: number;
  category: number; // 0 amp, 1 cab, 2 effect
  slot: number;
  group: number; // 2 pre-amp, 3 amp section, 5 post-amp
  enabled: boolean;
  sync: number;
  modelName: string;
  position: 'pre' | 'amp' | 'post';
  params: Param[];
}

export interface Tone {
  name: string;
  mic: number;
  micName: string;
  room: number;
  tempo: number;
  active: boolean;
  blocks: Block[];
}

export interface Patch {
  tones: [Tone, Tone];
}

export interface ModelChoice {
  model: number;
  name: string;
  table: number | null; // null: keep the record's table (cabs)
  group?: string;
}

const hex2 = (n: number) => n.toString(16).padStart(2, '0');
export const paramKey = (idx: number, ns: number) => ns.toString(16).padStart(4, '0') + idx.toString(16).padStart(4, '0');

export function modelName(category: number, table: number, model: number): string {
  if (category === 0) return (table === 2 && AMP_MODELS[model]) || `Amp ${hex2(table)}/${model} (model pack)`;
  if (category === 1) return CAB_MODELS[model] || `Cab ${model}`;
  return EFFECT_TABLES[hex2(table).toUpperCase()]?.models[model] || `Model ${hex2(table)}/${model}`;
}

// Knobs of the fixed blocks, keyed by block id then parameter key.
const FIXED_KNOBS: Partial<Record<BlockId, Record<string, string>>> = {
  gate: { '3f000000': 'Threshold (dB)', '3f000003': 'Decay' },
  comp: { '3f100000': 'Threshold', '3f100001': 'Gain' },
  volume: { '3f000004': 'Min', '3f000005': 'Max' },
  loop: { '3f000001': 'Send', '3f000002': 'Return' },
  eq: Object.fromEntries(
    [0, 1, 2, 3].flatMap((n) => [
      [paramKey(2 * n, NS.NORMAL), `Band ${n + 1} Freq`],
      [paramKey(2 * n + 1, NS.NORMAL), `Band ${n + 1} Gain`],
    ]),
  ),
};

function paramName(rec: Pick<Block, 'id' | 'category' | 'table' | 'model'>, idx: number, ns: number): string {
  const fixed = FIXED_KNOBS[rec.id]?.[paramKey(idx, ns)];
  if (fixed) return fixed;
  if (ns === NS.MIX) return 'Mix';
  if (rec.category === 0 && ns === NS.NORMAL && AMP_KNOBS[idx]) return AMP_KNOBS[idx];
  const known = KNOBS[`02/${hex2(rec.table)}/${rec.model}`];
  return known?.params[paramKey(idx, ns)] || `Param ${idx}${ns === NS.UNITS ? ' (units)' : ''}`;
}

function recordOffset(id: BlockId): number {
  return HEADER_LEN + BLOCKS.indexOf(id) * RECORD_LEN;
}

interface RawParam {
  idx: number;
  ns: number;
  value: number;
}

function readParams(block: Buffer, o: number): RawParam[] {
  const count = Math.min(block[o + 0x0b]!, MAX_PARAMS);
  const params: RawParam[] = [];
  for (let p = 0; p < count; p++) {
    const po = o + 0x0c + p * 8;
    params.push({ idx: block.readUInt16LE(po), ns: block.readUInt16LE(po + 2), value: block.readFloatLE(po + 4) });
  }
  return params;
}

export function parseTone(block: Buffer): Tone {
  const name = block.subarray(0, 16).toString('latin1').trimEnd();
  const blocks = BLOCKS.map((id): Block => {
    const o = recordOffset(id);
    const head = {
      id,
      model: block.readUInt16LE(o),
      table: block[o + 2]!,
      category: block[o + 3]!,
    };
    const group = block.readUInt16LE(o + 6);
    return {
      ...head,
      label: BLOCK_LABELS[id],
      slot: block.readUInt16LE(o + 4),
      group,
      enabled: block[o + 8] === 1,
      sync: block[o + 9]!,
      // Table 0B names past "Compressor" are unreliable; the loop is the loop.
      modelName: id === 'loop' ? 'FX Loop' : modelName(head.category, head.table, head.model),
      position: group === 2 ? 'pre' : group === 5 ? 'post' : 'amp',
      params: readParams(block, o).map(({ idx, ns, value }) => ({
        key: paramKey(idx, ns),
        idx,
        ns,
        name: paramName(head, idx, ns),
        value,
        normalized: ns !== NS.UNITS,
      })),
    };
  });
  const mic = block[0x52]!;
  return {
    name,
    mic,
    micName: MICS[mic] ?? String(mic),
    room: block.readFloatLE(0x44),
    tempo: block.readFloatLE(0x38),
    active: block[0x55] === 1,
    blocks,
  };
}

export function parsePatch(patch: Buffer): Patch {
  return { tones: [parseTone(patch.subarray(0, TONE_LEN)), parseTone(patch.subarray(TONE_LEN))] };
}

export const patchName = (patch: Buffer): string => patch.subarray(0, 16).toString('latin1').trimEnd();

export function toneBlock(patch: Buffer, tone: number): Buffer {
  return patch.subarray(tone * TONE_LEN, (tone + 1) * TONE_LEN);
}

// In-place edits on a working copy, mirroring what was sent live.
export function writeParam(patch: Buffer, tone: number, blockId: BlockId, idx: number, ns: number, value: number): boolean {
  const block = toneBlock(patch, tone);
  const o = recordOffset(blockId);
  const count = Math.min(block[o + 0x0b]!, MAX_PARAMS);
  for (let p = 0; p < count; p++) {
    const po = o + 0x0c + p * 8;
    if (block.readUInt16LE(po) === idx && block.readUInt16LE(po + 2) === ns) {
      block.writeFloatLE(value, po + 4);
      return true;
    }
  }
  return false;
}

export function writeEnabled(patch: Buffer, tone: number, blockId: BlockId, enabled: boolean): void {
  toneBlock(patch, tone)[recordOffset(blockId) + 8] = enabled ? 1 : 0;
}

// Change a block's model. The POD has no live message for this; the caller
// re-pushes the whole tone block. Knob records are rebuilt from the known
// knob list of the new model when we have one (values kept where ids match).
export function writeModel(patch: Buffer, tone: number, blockId: BlockId, table: number, model: number): void {
  const block = toneBlock(patch, tone);
  const o = recordOffset(blockId);
  const category = block[o + 3];
  const old = readParams(block, o);
  const keep = (idx: number, ns: number): RawParam => old.find((p) => p.idx === idx && p.ns === ns) ?? { idx, ns, value: 0.5 };

  let params = old;
  if (category === 0 && table === 2) {
    // Standard amps take exactly the six tone knobs.
    params = AMP_KNOBS.map((_, idx) => keep(idx, NS.NORMAL));
  } else if (category === 2) {
    const known = KNOBS[`02/${hex2(table)}/${model}`];
    if (known) {
      const knobs = Object.keys(known.params).map((k) => keep(parseInt(k.slice(4), 16), parseInt(k.slice(0, 4), 16)));
      const extra = old.filter((p) => p.ns !== NS.NORMAL && !knobs.some((k) => k.idx === p.idx && k.ns === p.ns)); // mix, real units
      params = [...knobs, ...extra].slice(0, MAX_PARAMS);
    }
  }

  block.writeUInt16LE(model, o);
  if (category !== 1) block[o + 2] = table;
  block.fill(0, o + 0x0c, o + RECORD_LEN);
  block[o + 0x0b] = params.length;
  params.forEach((p, i) => {
    const po = o + 0x0c + i * 8;
    block.writeUInt16LE(p.idx, po);
    block.writeUInt16LE(p.ns, po + 2);
    block.writeFloatLE(p.value, po + 4);
  });
}

// Catalog of selectable models per block, for the UI and CLI.
export function modelCatalog(): Partial<Record<BlockId, ModelChoice[]>> {
  const list = (obj: Record<string, string>) => Object.entries(obj).map(([id, name]) => ({ model: Number(id), name }));
  const cat: Partial<Record<BlockId, ModelChoice[]>> = {
    amp: list(AMP_MODELS)
      .filter((m) => m.model > 0)
      .map((m) => ({ ...m, table: 2 })),
    cab: list(CAB_MODELS).map((m) => ({ ...m, table: null })),
  };
  for (const [blockId, tables] of Object.entries(BLOCK_TABLES) as [BlockId, string[]][]) {
    cat[blockId] = tables.flatMap((t) => {
      const table = EFFECT_TABLES[t];
      if (!table) return [];
      return list(table.models)
        .filter((m) => !(t === '02' && m.model < 2)) // not offered by GearBox's delay menu
        .map((m) => ({ ...m, table: parseInt(t, 16), group: table.label }));
    });
  }
  return cat;
}

// Tone header fields (offsets into a tone block, see PROTOCOL.md of
// POD-Controller). Only fields whose meaning was confirmed by GearBox diffs.
export const HEADER = {
  NAME: 0x00, // 16 bytes ASCII, space padded
  TEMPO: 0x38, // f32 BPM
  PAN: 0x3c, // f32 -1..1 (the -L/-R factory dual tones sit at -1/+1)
  ROOM: 0x44, // f32 0..1
  MIC: 0x52, // u8 index into MICS
  ACTIVE: 0x55, // u8; on tone 2 this is the DUAL TONE switch
  TRIM: 0x60, // f32 dB, tone 1 only: patch output trim
} as const;
export const NAME_LEN = 16;

export function writeName(patch: Buffer, tone: number, name: string): void {
  if (!/^[\x20-\x7e]*$/.test(name)) throw new Error(`name "${name}" must be plain ASCII`);
  if (name.length > NAME_LEN) throw new Error(`name "${name}" is longer than ${NAME_LEN} characters`);
  toneBlock(patch, tone).write(name.padEnd(NAME_LEN, ' '), HEADER.NAME, NAME_LEN, 'latin1');
}

export function writeToneFloat(patch: Buffer, tone: number, offset: number, value: number): void {
  toneBlock(patch, tone).writeFloatLE(value, offset);
}

export function writeToneByte(patch: Buffer, tone: number, offset: number, value: number): void {
  toneBlock(patch, tone)[offset] = value;
}

// The two chain positions (slot, group) of the movable blocks.
export const POSITIONS: Partial<Record<BlockId, { pre: [number, number]; post: [number, number] }>> = {
  volume: { pre: [1, 2], post: [2, 5] },
  mod: { pre: [4, 2], post: [3, 5] },
  delay: { pre: [5, 2], post: [4, 5] },
  reverb: { pre: [6, 2], post: [5, 5] },
  loop: { pre: [12, 2], post: [9, 5] },
};

export function writePosition(patch: Buffer, tone: number, blockId: BlockId, position: 'pre' | 'post'): void {
  const pos = POSITIONS[blockId];
  if (!pos) throw new Error(`${BLOCK_LABELS[blockId]} cannot be moved`);
  const block = toneBlock(patch, tone);
  const o = recordOffset(blockId);
  block.writeUInt16LE(pos[position][0], o + 4);
  block.writeUInt16LE(pos[position][1], o + 6);
}

// Tempo sync division (record +0x09), as in the POD's FX TEMPO menu.
export const SYNC = [
  'off', 'whole', 'half dotted', 'half', 'half triplet', 'quarter dotted', 'quarter', 'quarter triplet',
  '8th dotted', '8th', '8th triplet', '16th dotted', '16th', '16th triplet',
] as const;

export function writeSync(patch: Buffer, tone: number, blockId: BlockId, sync: number): void {
  if (!Number.isInteger(sync) || sync < 0 || sync >= SYNC.length) throw new Error(`invalid tempo sync ${sync}`);
  toneBlock(patch, tone)[recordOffset(blockId) + 9] = sync;
}

// True if a model has a known knob list (so writeModel rebuilds its records).
export const hasKnownKnobs = (table: number, model: number): boolean => !!KNOBS[`02/${hex2(table)}/${model}`];
