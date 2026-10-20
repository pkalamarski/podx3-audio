// Relays the audio tap (dry guitar, mono int16 LE at 12 kHz) from the audio
// side's Unix socket (audio/common/tap.h, served by `monitor` or `podx3d`)
// to HTTP clients of the web tuner. One socket connection is shared by all
// clients and opened only while someone listens.

import { connect, type Socket } from 'node:net';
import type { ServerResponse } from 'node:http';

export const TAP_RATE = 12000;
const TAP_PATH = process.env.POD_TAP || `/tmp/podx3-tap-${process.getuid?.() ?? 0}.sock`;

const listeners = new Set<ServerResponse>();
let sock: Socket | null = null;

function closeAll(): void {
  for (const res of listeners) res.end();
  listeners.clear();
  sock?.destroy();
  sock = null;
}

function open(): Promise<Socket> {
  if (sock) return Promise.resolve(sock);
  return new Promise((resolve, reject) => {
    const s = connect(TAP_PATH);
    s.once('connect', () => {
      sock = s;
      s.on('data', (chunk) => {
        for (const res of listeners) res.write(chunk);
      });
      s.on('close', closeAll);
      s.on('error', closeAll);
      resolve(s);
    });
    s.once('error', () =>
      reject(new Error('no audio tap: start the audio side first (audio/build/monitor, or the podx3d helper)')),
    );
  });
}

export async function streamTap(res: ServerResponse): Promise<void> {
  await open();
  res.writeHead(200, {
    'content-type': 'application/octet-stream',
    'cache-control': 'no-cache',
    'x-sample-rate': String(TAP_RATE),
  });
  listeners.add(res);
  res.on('close', () => {
    listeners.delete(res);
    if (!listeners.size) {
      sock?.destroy();
      sock = null;
    }
  });
}
