#!/usr/bin/env node
// Local HTTP server: the only process that talks to the POD over USB.
// Serves the web UI from public/ and a JSON API used by the UI and the CLI.

import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname } from 'node:path';
import { Controller } from './controller.js';
import { modelCatalog, BLOCKS, BLOCK_LABELS } from './patch.js';
import { streamTap } from './tap.js';
import { Monitor, type Pair } from './monitor.js';

const PORT = Number(process.env.POD_PORT || 8787);
const HOST = process.env.POD_HOST || '127.0.0.1';
const publicDir = new URL('../public/', import.meta.url);
const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
};

type Body = Record<string, unknown>;
type Handler = (body: Body, query: URLSearchParams) => unknown;

const ctl = new Controller();
const clients = new Set<ServerResponse>();
ctl.on('change', (ev) => {
  const data = `data: ${JSON.stringify({ ...ev, status: ctl.status() })}\n\n`;
  for (const res of clients) res.write(data);
});
const monitor = new Monitor();
monitor.on('change', (state) => {
  const data = `data: ${JSON.stringify({ type: 'monitor', monitor: state, status: ctl.status() })}\n\n`;
  for (const res of clients) res.write(data);
});

// API tones are 1-based, the protocol's are 0-based.
const toneOf = (body: Body): number => {
  const t = Number(body.tone ?? 1);
  if (t !== 1 && t !== 2) throw new Error('tone must be 1 or 2');
  return t - 1;
};

const routes: Record<string, Handler> = {
  'GET /api/status': () => ctl.status(),
  'POST /api/connect': async () => (await ctl.connect(), ctl.status()),
  'GET /api/presets': (_, q) => ctl.listPresets({ refresh: q.get('refresh') === '1' }),
  'GET /api/presets/cached': () => ctl.presets,
  'POST /api/load': (b) => ctl.load(b.slot),
  'POST /api/revert': () => ctl.revert(),
  'GET /api/peek': (_, q) => ctl.peek(q.get('slot')),
  'GET /api/patch': () => ctl.patchInfo(),
  'POST /api/param': (b) => ctl.setParam(toneOf(b), String(b.block), b.param, b.value),
  'POST /api/enabled': (b) => ctl.setEnabled(toneOf(b), String(b.block), !!b.enabled),
  'POST /api/model': (b) => ctl.setModel(toneOf(b), String(b.block), b.model),
  'POST /api/tone': (b) => ctl.selectTone(toneOf(b)),
  'POST /api/backup': () => ctl.backup(),
  'GET /api/monitor': () => monitor.state(),
  'POST /api/monitor': (b) =>
    b.on === false
      ? monitor.stop()
      : monitor.start({ pair: b.pair as Pair | undefined, silent: b.silent === undefined ? undefined : !!b.silent }),
  'GET /api/dump': (_, q) => ctl.dump(q.get('slot')),
  'POST /api/restore': (b) => ctl.restore(b.slot, b.dir),
  'POST /api/apply': (b) => ctl.apply(b.files),
  'GET /api/models': () => ({ blocks: BLOCKS.map((id) => ({ id, label: BLOCK_LABELS[id] })), models: modelCatalog() }),
};

async function readBody(req: IncomingMessage): Promise<Body> {
  let raw = '';
  for await (const chunk of req) raw += chunk;
  return raw ? (JSON.parse(raw) as Body) : {};
}

function send(res: ServerResponse, code: number, body: unknown): void {
  res.writeHead(code, { 'content-type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(body));
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url ?? '/', `http://${req.headers.host}`);
  if (url.pathname === '/api/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache', connection: 'keep-alive' });
    res.write(`data: ${JSON.stringify({ type: 'hello', status: ctl.status(), monitor: monitor.state() })}\n\n`);
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }
  if (url.pathname === '/api/tuner/stream') {
    // Raw audio for the web tuner; independent of the USB control session.
    return streamTap(res).catch((e: Error) => send(res, 503, { ok: false, error: e.message }));
  }
  const handler = routes[`${req.method} ${url.pathname}`];
  if (handler) {
    try {
      const body = req.method === 'POST' ? await readBody(req) : {};
      send(res, 200, { ok: true, result: (await handler(body, url.searchParams)) ?? null });
    } catch (e) {
      send(res, 400, { ok: false, error: e instanceof Error ? e.message : String(e) });
    }
    return;
  }
  if (req.method !== 'GET') return send(res, 404, { ok: false, error: 'not found' });
  const path = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
  if (path.includes('..')) return send(res, 400, { ok: false, error: 'bad path' });
  try {
    const file = await readFile(new URL(path, publicDir));
    res.writeHead(200, { 'content-type': MIME[extname(path)] ?? 'application/octet-stream' });
    res.end(file);
  } catch {
    send(res, 404, { ok: false, error: 'not found' });
  }
});

server.listen(PORT, HOST, () => {
  console.log(`POD X3 server on http://${HOST}:${PORT}`);
  ctl.connect().then(
    (pod) => console.log(`connected: ${pod.model}, firmware ${ctl.firmware}`),
    (e: Error) => console.log(`not connected yet: ${e.message}`),
  );
});

async function shutdown(): Promise<void> {
  server.close();
  await monitor.stop();
  await ctl.pod?.close();
  process.exit(0);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
