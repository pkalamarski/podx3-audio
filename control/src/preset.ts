// Preset files: a patch described as JSON (control/presets/*.json), built on
// top of a real patch used as a template (control/presets/templates/*.bin),
// so header fields nobody has decoded yet keep valid values. Only edits the
// code understands are applied: models, knobs, on/off, pre/post, tempo sync,
// name, mic, room, pan, dual tone and output trim.
//
// {
//   "slot": "1A", "name": "Plexi Crunch", "template": "single", "trim": -6, "tempo": 120,
//   "tones": [{
//     "mic": "57 Off Axis", "room": 0.3, "pan": 0,
//     "amp": { "model": "1968 Plexi Lead 100", "Drive": 0.55, ... },
//     "cab": { "model": "4x12 1968 Green 25s" },
//     "stomp": { "model": "Screamer", "Drive": 0.1, "Gain": 0.7, "Tone": 0.6 },
//     "delay": { "model": "Digital Delay", "sync": "8th dotted", "Time": 0.3, ..., "Mix": 0.2 },
//     "gate": { "Threshold (dB)": -58, "Decay": 0.04 }
//   }]
// }
//
// A block listed in a tone is switched on (unless "on": false); every block
// not listed is switched off, except amp, cab and volume (always on). Every
// knob of an enabled block with a new model has to be given. Mod, delay,
// reverb and volume go post-amp unless "position": "pre" is given. One tone
// means a single-tone patch (tone 2 gets a copy, dual tone off); two tones
// turn DUAL TONE on. "trim" is the patch output level in dB (-12..12).

import { readFileSync } from 'node:fs';
import { PATCH_LEN, TONE_LEN, parseSlot, slotName } from './protocol.js';
import {
  BLOCKS,
  BLOCK_LABELS,
  HEADER,
  MICS,
  POSITIONS,
  SYNC,
  type BlockId,
  hasKnownKnobs,
  modelCatalog,
  parsePatch,
  toneBlock,
  writeEnabled,
  writeModel,
  writeName,
  writeParam,
  writePosition,
  writeSync,
  writeToneByte,
  writeToneFloat,
} from './patch.js';

type BlockSpec = { model?: string; on?: boolean; position?: 'pre' | 'post'; sync?: string } & Record<string, unknown>;
type ToneSpec = { name?: string; mic?: string; room?: number; pan?: number } & Partial<Record<BlockId, BlockSpec>>;

export interface PresetSpec {
  slot: string;
  name: string;
  template: string;
  description?: string;
  trim?: number;
  tempo?: number;
  tones: ToneSpec[];
}

const templateDir = new URL('../presets/templates/', import.meta.url);
const ALWAYS_ON: BlockId[] = ['amp', 'cab', 'volume'];
const BLOCK_KEYS = new Set(['model', 'on', 'position', 'sync']);
const TONE_KEYS = new Set(['name', 'mic', 'room', 'pan', ...BLOCKS]);

export function readPresetFile(path: string): PresetSpec {
  const spec = JSON.parse(readFileSync(path, 'utf8')) as PresetSpec;
  if (!spec.slot || !spec.name || !spec.template || !Array.isArray(spec.tones)) {
    throw new Error(`${path}: needs "slot", "name", "template" and "tones"`);
  }
  return spec;
}

const range = (what: string, v: unknown, lo: number, hi: number): number => {
  const n = Number(v);
  if (!Number.isFinite(n) || n < lo || n > hi) throw new Error(`${what} must be ${lo}..${hi}, got ${String(v)}`);
  return n;
};

function applyBlock(patch: Buffer, tone: number, id: BlockId, spec: BlockSpec, where: string): void {
  if (spec.model !== undefined) {
    const choices = modelCatalog()[id];
    if (!choices) throw new Error(`${where}: the model of ${BLOCK_LABELS[id]} cannot be changed`);
    const q = spec.model.toLowerCase();
    const m = choices.find((c) => c.name.toLowerCase() === q);
    if (!m) throw new Error(`${where}: no ${id} model named "${spec.model}"`);
    const table = m.table ?? parsePatch(patch).tones[tone as 0 | 1].blocks.find((b) => b.id === id)!.table;
    if (id !== 'amp' && id !== 'cab' && !hasKnownKnobs(table, m.model)) {
      throw new Error(`${where}: knobs of "${m.name}" are not known (data/knobs.json); pick another model`);
    }
    writeModel(patch, tone, id, table, m.model);
  }
  if (spec.position) writePosition(patch, tone, id, spec.position);
  if (spec.sync !== undefined) {
    const sync = SYNC.indexOf(spec.sync.toLowerCase() as (typeof SYNC)[number]);
    if (sync < 0) throw new Error(`${where}: tempo sync "${spec.sync}" is not one of ${SYNC.join(', ')}`);
    writeSync(patch, tone, id, sync);
  }

  const block = parsePatch(patch).tones[tone as 0 | 1].blocks.find((b) => b.id === id)!;
  const given = Object.keys(spec).filter((k) => !BLOCK_KEYS.has(k));
  for (const key of given) {
    const hits = block.params.filter((p) => p.name.toLowerCase() === key.toLowerCase());
    if (hits.length !== 1) {
      throw new Error(`${where}: ${block.modelName} has ${hits.length ? 'several' : 'no'} knob "${key}" (has: ${block.params.map((p) => p.name).join(', ')})`);
    }
    const p = hits[0]!;
    const v = p.normalized ? range(`${where} ${key}`, spec[key], 0, 1) : range(`${where} ${key}`, spec[key], -100, 100);
    writeParam(patch, tone, id, p.idx, p.ns, v);
  }
  const on = spec.on ?? true;
  if (spec.model !== undefined && on) {
    const missing = block.params.filter((p) => !given.some((k) => k.toLowerCase() === p.name.toLowerCase()));
    if (missing.length) throw new Error(`${where}: set every knob of ${block.modelName} (missing: ${missing.map((p) => p.name).join(', ')})`);
  }
  writeEnabled(patch, tone, id, on);
}

function applyTone(patch: Buffer, tone: number, spec: ToneSpec, name: string, where: string): void {
  for (const key of Object.keys(spec)) if (!TONE_KEYS.has(key)) throw new Error(`${where}: unknown key "${key}"`);
  writeName(patch, tone, spec.name ?? name);
  if (spec.mic !== undefined) {
    const mic = MICS.findIndex((m) => m.toLowerCase() === String(spec.mic).toLowerCase());
    if (mic < 0) throw new Error(`${where}: mic must be one of ${MICS.join(', ')}`);
    writeToneByte(patch, tone, HEADER.MIC, mic);
  }
  if (spec.room !== undefined) writeToneFloat(patch, tone, HEADER.ROOM, range(`${where} room`, spec.room, 0, 1));
  writeToneFloat(patch, tone, HEADER.PAN, range(`${where} pan`, spec.pan ?? 0, -1, 1));
  for (const id of BLOCKS) {
    const b = spec[id];
    // Movable blocks sit where the spec says, by default post-amp (the loop
    // pre-amp), whatever the template had.
    if (POSITIONS[id]) writePosition(patch, tone, id, b?.position ?? (id === 'loop' ? 'pre' : 'post'));
    if (b) applyBlock(patch, tone, id, b, `${where} ${id}`);
    else if (!ALWAYS_ON.includes(id)) writeEnabled(patch, tone, id, false);
  }
  writeEnabled(patch, tone, 'amp', true);
  writeEnabled(patch, tone, 'cab', true);
}

export function loadTemplate(name: string): Buffer {
  if (!/^[\w-]+$/.test(name)) throw new Error(`invalid template name "${name}"`);
  const t = readFileSync(new URL(`${name}.bin`, templateDir));
  if (t.length !== PATCH_LEN) throw new Error(`template ${name} is not a ${PATCH_LEN}-byte patch`);
  return t;
}

// Build the 4096-byte patch for a preset spec. Pure: no device access.
export function buildPreset(spec: PresetSpec, template = loadTemplate(spec.template)): { slot: number; patch: Buffer } {
  const slot = parseSlot(spec.slot);
  const where = `${slotName(slot)} "${spec.name}"`;
  if (spec.tones.length !== 1 && spec.tones.length !== 2) throw new Error(`${where}: give one or two tones`);
  const patch = Buffer.from(template);
  const dual = spec.tones.length === 2;
  applyTone(patch, 0, spec.tones[0]!, spec.name, `${where} tone 1`);
  if (dual) applyTone(patch, 1, spec.tones[1]!, spec.name, `${where} tone 2`);
  else {
    // Tone 2 gets tone 1's chain, keeping its own header (it is "the same input").
    const t2 = toneBlock(patch, 1);
    const t1 = toneBlock(patch, 0);
    t1.copy(t2, 0xe4, 0xe4, TONE_LEN);
    writeName(patch, 1, spec.name);
    writeToneByte(patch, 1, HEADER.MIC, t1[HEADER.MIC]!);
    writeToneFloat(patch, 1, HEADER.ROOM, t1.readFloatLE(HEADER.ROOM));
    writeToneFloat(patch, 1, HEADER.PAN, 0);
  }
  writeToneByte(patch, 0, HEADER.ACTIVE, 1);
  writeToneByte(patch, 1, HEADER.ACTIVE, dual ? 1 : 0);
  const tempo = range(`${where} tempo`, spec.tempo ?? 120, 30, 240);
  writeToneFloat(patch, 0, HEADER.TEMPO, tempo);
  writeToneFloat(patch, 1, HEADER.TEMPO, tempo);
  writeToneFloat(patch, 0, HEADER.TRIM, range(`${where} trim`, spec.trim ?? 0, -12, 12));
  return { slot, patch };
}
