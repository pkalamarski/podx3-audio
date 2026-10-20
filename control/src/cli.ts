#!/usr/bin/env node
// Command-line control of the POD X3.
// All commands go through the local server, which is started in the
// background on first use, so the CLI and the web UI share one USB session.

import { execSync, spawn } from 'node:child_process';
import { mkdirSync, openSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { PatchInfo, PresetEntry, Status, WriteResult } from './controller.js';
import type { ModelChoice } from './patch.js';

const PORT = Number(process.env.POD_PORT || 8787);
const BASE = `http://127.0.0.1:${PORT}`;

const HELP = `pod - control a Line 6 POD X3 over USB

  pod status                     connection and loaded preset
  pod presets [--refresh]        list the 64 stored presets
  pod load <slot>                load a preset into the POD, e.g. 5A or 16
  pod revert                     reload the current preset (drop live edits)
  pod show [slot]                signal chain of the loaded (or a stored) preset
  pod set <block> <param> <v>    set a knob, v = 0.0-1.0, e.g. pod set amp drive 0.7
  pod on|off <block>             switch a block, e.g. pod off reverb
  pod model <block> <name|id>    change amp/cab/stomp/mod/delay/reverb/wah model
  pod models <block> [filter]    list models for a block
  pod tone 1|2                   select the tone being edited on the POD
  pod backup                     save all 64 presets to backups/ (read-only)
  pod dump <slot> <file.bin>     save one stored preset's raw bytes (read-only)
  pod build <preset.json>... --out <dir>
                                 build preset files into .bin files (no device)
  pod apply <preset.json>...     WRITES the presets into the slots they name
  pod restore <slot|all> <backup dir>
                                 WRITES slots back from a backup folder
  pod ui                         start the server and open the web UI
  pod monitor on|off [--pair main|tone1|tone2|di] [--silent]
                                 POD audio to the Mac's output (and the tuner)
  pod server stop                stop the background server (and the monitor)

  --tone 2                       address tone 2 instead of tone 1
  --json                         print raw JSON

Blocks: amp cab stomp mod delay reverb gate comp eq wah volume loop
Web UI: ${BASE}`;

const args = process.argv.slice(2);
const flag = (name: string): string | boolean | undefined => {
  const i = args.indexOf(name);
  if (i < 0) return undefined;
  const next = args[i + 1];
  const hasValue = next !== undefined && !next.startsWith('--');
  args.splice(i, hasValue ? 2 : 1);
  return hasValue ? next : true;
};
const asJson = !!flag('--json');
const tone = Number(flag('--tone') || 1);
const refresh = !!flag('--refresh');
const outDir = flag('--out');
const pairFlag = flag('--pair');
const silentFlag = !!flag('--silent');

function printWrites(results: WriteResult[]): void {
  for (const r of results) console.log(`${r.slot.padStart(3)} ${r.name.padEnd(17)} ${r.written ? 'written, verified' : 'unchanged, verified'}`);
}

async function api<R>(method: 'GET' | 'POST', path: string, body?: unknown): Promise<R> {
  const res = await fetch(BASE + path, {
    method,
    headers: { 'content-type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });
  const json = (await res.json()) as { ok: true; result: R } | { ok: false; error: string };
  if (!json.ok) throw new Error(json.error);
  return json.result;
}

const isConnRefused = (e: unknown) =>
  e instanceof Error && (e.cause as { code?: string } | undefined)?.code === 'ECONNREFUSED';

async function ensureServer(): Promise<Status> {
  try {
    return await api<Status>('GET', '/api/status');
  } catch (e) {
    if (!isConnRefused(e)) throw e;
  }
  const logDir = new URL('../.cache/', import.meta.url);
  mkdirSync(logDir, { recursive: true });
  const log = openSync(new URL('server.log', logDir), 'a');
  spawn(process.execPath, [fileURLToPath(new URL('./server.js', import.meta.url))], {
    detached: true,
    stdio: ['ignore', log, log],
  }).unref();
  for (let i = 0; i < 50; i++) {
    await new Promise((r) => setTimeout(r, 200));
    try {
      const s = await api<Status>('GET', '/api/status');
      if (s.connected || s.error) return s;
    } catch {}
  }
  throw new Error('server did not start, see .cache/server.log');
}

const fmt = (v: number) => (Math.abs(v) <= 1 ? v.toFixed(2) : v.toFixed(1));

function printChain(p: PatchInfo): void {
  console.log(`${p.slotName}  ${p.tones[0].name}${p.dirty ? '  (edited)' : ''}`);
  p.tones.forEach((t, i) => {
    if (i === 1 && !t.active) {
      console.log(`\n Tone 2: off (dual tone disabled)`);
      return;
    }
    console.log(`\n Tone ${i + 1}: ${t.name}   mic: ${t.micName}  room: ${fmt(t.room)}`);
    for (const b of t.blocks) {
      const knobs = b.params.map((x) => `${x.name}=${fmt(x.value)}`).join(' ');
      console.log(`  ${b.enabled ? '●' : '○'} ${b.label.padEnd(8)} ${b.modelName.padEnd(28)} [${b.position}] ${knobs}`);
    }
  });
}

async function main(): Promise<void> {
  const [cmd, ...rest] = args;
  if (!cmd || cmd === 'help' || cmd === '--help') return console.log(HELP);
  if (cmd === 'server' && rest[0] === 'stop') {
    try {
      execSync(`lsof -ti tcp:${PORT} -sTCP:LISTEN | xargs kill`);
      console.log('server stopped');
    } catch {
      console.log('server not running');
    }
    return;
  }

  if (cmd === 'build') {
    // Local only: no server, no device.
    const { buildPreset, readPresetFile } = await import('./preset.js');
    const { slotName } = await import('./protocol.js');
    if (typeof outDir !== 'string' || !rest.length) throw new Error('usage: pod build <preset.json>... --out <dir>');
    mkdirSync(outDir, { recursive: true });
    for (const file of rest) {
      const { slot, patch } = buildPreset(readPresetFile(file));
      const out = join(outDir, `${String(slot).padStart(2, '0')}-${slotName(slot)}.bin`);
      writeFileSync(out, patch);
      console.log(`${slotName(slot).padStart(3)} ${patch.subarray(0, 16).toString('latin1').trimEnd().padEnd(17)} ${out}`);
    }
    return;
  }

  const status = await ensureServer();
  let out: unknown;
  switch (cmd) {
    case 'ui':
      execSync(`open ${BASE}`);
      console.log(`web UI: ${BASE}`);
      return;
    case 'monitor': {
      if (rest[0] !== 'on' && rest[0] !== 'off') throw new Error('usage: pod monitor on|off [--pair main|tone1|tone2|di] [--silent]');
      const body = rest[0] === 'off' ? { on: false } : { on: true, pair: typeof pairFlag === 'string' ? pairFlag : undefined, silent: silentFlag };
      const m = await api<{ running: boolean; pair: string; silent: boolean; output: string | null }>('POST', '/api/monitor', body);
      if (!asJson) {
        console.log(m.running ? `monitor on: ${m.pair}${m.silent ? ' (silent, tuner only)' : ` -> ${m.output ?? 'Mac output'}`}` : 'monitor off');
        return;
      }
      out = m;
      break;
    }
    case 'status':
      out = status;
      if (!asJson) {
        console.log(status.connected ? `connected: ${status.model} (firmware ${status.firmware})` : `not connected: ${status.error}`);
        const c = status.current;
        console.log(c ? `loaded: ${c.slotName} ${c.name}${c.dirty ? ' (edited)' : ''}` : 'loaded: (unknown, nothing loaded via this tool yet)');
        console.log(`web UI: ${BASE}`);
        return;
      }
      break;
    case 'presets': {
      const list = await api<PresetEntry[]>('GET', `/api/presets${refresh ? '?refresh=1' : ''}`);
      if (!asJson) {
        for (let bank = 0; bank < 16; bank++) {
          console.log(list.slice(bank * 4, bank * 4 + 4).map((p) => `${p.slotName.padStart(3)} ${p.name.padEnd(17)}`).join(''));
        }
        return;
      }
      out = list;
      break;
    }
    case 'load':
    case 'revert': {
      const p = cmd === 'load' ? await api<PatchInfo>('POST', '/api/load', { slot: rest[0] }) : await api<PatchInfo>('POST', '/api/revert');
      if (!asJson) return printChain(p);
      out = p;
      break;
    }
    case 'show': {
      const p = rest[0]
        ? await api<PatchInfo>('GET', `/api/peek?slot=${encodeURIComponent(rest[0])}`)
        : await api<PatchInfo | null>('GET', '/api/patch');
      if (!p) throw new Error('nothing loaded yet: run "pod load <slot>" or "pod show <slot>"');
      if (!asJson) return printChain(p);
      out = p;
      break;
    }
    case 'set':
      out = await api('POST', '/api/param', { tone, block: rest[0], param: rest[1], value: rest[2] });
      break;
    case 'on':
    case 'off':
      out = await api('POST', '/api/enabled', { tone, block: rest[0], enabled: cmd === 'on' });
      break;
    case 'model':
      out = await api('POST', '/api/model', { tone, block: rest[0], model: rest.slice(1).join(' ') });
      break;
    case 'models': {
      const { models } = await api<{ models: Record<string, ModelChoice[]> }>('GET', '/api/models');
      const list = models[rest[0] ?? ''];
      if (!list) throw new Error(`no model list for "${rest[0] ?? ''}" (amp cab stomp mod delay reverb wah)`);
      const q = rest.slice(1).join(' ').toLowerCase();
      const hits = list.filter((m) => m.name.toLowerCase().includes(q));
      if (!asJson) {
        for (const m of hits) console.log(`${String(m.model).padStart(4)}  ${m.name}${m.group && rest[0] === 'stomp' ? `  (${m.group})` : ''}`);
        return;
      }
      out = hits;
      break;
    }
    case 'tone':
      out = await api('POST', '/api/tone', { tone: Number(rest[0]) });
      break;
    case 'backup':
      out = await api('POST', '/api/backup');
      break;
    case 'dump': {
      if (!rest[0] || !rest[1]) throw new Error('usage: pod dump <slot> <file.bin>');
      const d = await api<{ slot: string; name: string; data: string }>('GET', `/api/dump?slot=${encodeURIComponent(rest[0])}`);
      writeFileSync(rest[1], Buffer.from(d.data, 'base64'));
      out = { slot: d.slot, name: d.name, file: resolve(rest[1]) };
      break;
    }
    case 'apply':
    case 'restore': {
      const results =
        cmd === 'apply'
          ? await api<WriteResult[]>('POST', '/api/apply', { files: rest.map((f) => resolve(f)) })
          : await api<WriteResult[]>('POST', '/api/restore', { slot: rest[0], dir: rest[1] && (rest[1].includes('/') ? resolve(rest[1]) : rest[1]) });
      if (!asJson) return printWrites(results);
      out = results;
      break;
    }
    default:
      throw new Error(`unknown command "${cmd}"\n\n${HELP}`);
  }
  console.log(asJson ? JSON.stringify(out, null, 1) : out);
}

main().catch((e: unknown) => {
  console.error(`error: ${e instanceof Error ? e.message : String(e)}`);
  process.exit(1);
});
