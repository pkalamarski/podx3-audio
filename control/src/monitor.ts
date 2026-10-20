// Runs the audio side's `monitor` tool (audio/build/monitor) as a child of the
// server, so the web UI can switch live monitoring (and the tuner's audio tap)
// on and off. The monitor owns USB interface 0; this process only ever touches
// interface 1, so the two coexist.

import { spawn, type ChildProcess } from 'node:child_process';
import { existsSync } from 'node:fs';
import { EventEmitter } from 'node:events';
import { fileURLToPath } from 'node:url';

export const MONITOR_BIN = fileURLToPath(new URL('../../audio/build/monitor', import.meta.url));
export const PAIRS = ['main', 'tone1', 'tone2', 'di'] as const;
export type Pair = (typeof PAIRS)[number];

export interface MonitorOptions {
  pair: Pair;
  silent: boolean; // tuner tap only, no playback
}

export interface MonitorState extends MonitorOptions {
  running: boolean;
  output: string | null; // Mac output device in use
  underruns: number | null;
  error: string | null;
}

export class Monitor extends EventEmitter<{ change: [MonitorState] }> {
  #child: ChildProcess | null = null;
  #stopping = false;
  #state: MonitorState = { running: false, pair: 'main', silent: false, output: null, underruns: null, error: null };

  state(): MonitorState {
    return { ...this.#state };
  }

  #set(patch: Partial<MonitorState>): void {
    this.#state = { ...this.#state, ...patch };
    this.emit('change', this.state());
  }

  async start(opts: Partial<MonitorOptions> = {}): Promise<MonitorState> {
    const pair = opts.pair ?? this.#state.pair;
    const silent = opts.silent ?? this.#state.silent;
    if (!PAIRS.includes(pair)) throw new Error(`pair must be one of ${PAIRS.join(', ')}`);
    if (this.#child) {
      if (pair === this.#state.pair && silent === this.#state.silent) return this.state();
      await this.stop(); // options changed: restart
    }
    if (!existsSync(MONITOR_BIN)) throw new Error('audio/build/monitor is missing: run `make` in audio/');

    const args = ['--pair', pair, ...(silent ? ['--silent'] : [])];
    const child = spawn(MONITOR_BIN, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    this.#child = child;
    this.#stopping = false;
    this.#set({ running: true, pair, silent, output: null, underruns: null, error: null });

    let lastErr = '';
    const onLine = (line: string): void => {
      const out = /^output: ([^,]+)/.exec(line);
      if (out) this.#set({ output: out[1] });
      const u = /underruns (\d+)/.exec(line);
      if (u && Number(u[1]) !== this.#state.underruns) this.#set({ underruns: Number(u[1]) });
    };
    const lines = (stream: NodeJS.ReadableStream, onErr: boolean): void => {
      let buf = '';
      stream.setEncoding('utf8');
      stream.on('data', (chunk: string) => {
        buf += chunk;
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, i).trim();
          buf = buf.slice(i + 1);
          if (!line) continue;
          if (onErr) lastErr = line;
          onLine(line);
        }
      });
    };
    lines(child.stdout!, false);
    lines(child.stderr!, true);

    child.on('exit', (code) => {
      if (this.#child !== child) return;
      this.#child = null;
      const failed = !this.#stopping && code !== 0;
      this.#set({
        running: false,
        output: null,
        error: failed ? friendlyError(lastErr) : null,
      });
    });

    // A failed open (device busy, missing) exits within a moment: report it.
    await new Promise((r) => setTimeout(r, 700));
    if (!this.#state.running && this.#state.error) throw new Error(this.#state.error);
    return this.state();
  }

  // Graceful stop: SIGINT lets the monitor drain its USB transfers (never kill -9).
  async stop(): Promise<MonitorState> {
    const child = this.#child;
    if (!child) return this.state();
    this.#stopping = true;
    const exited = new Promise<void>((r) => child.once('exit', () => r()));
    child.kill('SIGINT');
    const timeout = new Promise<'timeout'>((r) => setTimeout(() => r('timeout'), 5000));
    if ((await Promise.race([exited, timeout])) === 'timeout') child.kill('SIGTERM');
    await exited;
    return this.state();
  }
}

function friendlyError(line: string): string {
  if (/claim interface 0/i.test(line))
    return 'the POD audio interface is busy (another monitor, rec, or the installed podx3d helper)';
  if (/not found/i.test(line)) return 'POD X3 is not connected';
  return line || 'the monitor exited with an error';
}
