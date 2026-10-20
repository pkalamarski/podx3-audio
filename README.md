# podx3-macos

Use a Line 6 POD X3 on macOS without Line 6 drivers. This project lets you:
- use the POD as a USB audio interface (8 inputs, 2 outputs, 48 kHz);
- control it from a web page or the command line: switch presets, change
  amp, cab and effect models, turn knobs, back up and write presets;
- tune your guitar in the browser.

Line 6 has no working driver for current macOS and Apple Silicon, and the
POD X3 is not class-compliant: all of its USB interfaces are vendor-specific
(class 0xFF). Everything here runs in user space. No kernel extension is
needed and no security settings change.

Tested with a POD X3 (desktop, `0e41:414a`) on macOS 15, Apple Silicon. The
POD X3 Live (`0e41:414b`) uses the same protocol but is untested.

## Status

| Part | State |
|---|---|
| Controller: presets, models, knobs, backup, preset writes | works |
| POD → Mac audio (8 input channels) | works |
| Live monitor on the Mac's output, web tuner, WAV recording | works |
| Mac → POD audio (2 output channels) | streams; not verified by ear yet |
| CoreAudio device "POD X3" for DAWs and other apps | experimental: built and passing hardware-free tests, not yet tested |

## Requirements

- macOS 15 on Apple Silicon. Other versions are untested.
- Xcode command line tools (`clang`, `make`).
- libusb from Homebrew: `brew install libusb`. It is linked statically.
- Node.js 20 or newer.
- The POD X3 connected by USB and powered on.

## Quick start

```sh
git clone https://github.com/pkalamarski/podx3-macos.git
cd podx3-macos
./podx3
```

`./podx3` does three things:
1. Builds whatever is out of date: the audio tools, the controller's
   `npm install`, and the TypeScript build.
2. Starts the controller server in the background.
3. Opens the web UI at http://127.0.0.1:8787.

Run it again at any time; it only rebuilds what changed.

## Web UI

- **Presets:** all 64 stored presets in banks 1–16 × A–D. Click one to load
  it. The first scan reads them from the POD and takes a few seconds. After
  that the list is cached in `control/.cache/presets.json`. Use **Refresh
  list** after editing presets on the device.
- **Signal chain:** every block of the loaded preset in signal order. Each
  block has an on/off switch and a model menu (amp, cab, stomp, mod, delay,
  reverb, wah), plus a slider per knob. Changes reach the POD while you drag.
- **Tone 1 / Tone 2:** edit the second tone of dual-tone presets.
- **Monitor:** plays POD audio on the Mac's current output. You can choose
  Main, Tone 1, Tone 2, DI (dry guitar), or **Tuner only**, which sends no
  sound and only feeds the tuner.
- **Revert:** reloads the stored version of the current preset.
- **Backup:** saves all 64 presets as raw `.bin` files to
  `control/backups/<date>/`.
- **Tuner** (`/tuner.html`):
  - It listens to the dry guitar, so preset effects do not get in the way.
  - Tunings: standard, drop D/C♯/C/B, half and whole step down, C standard,
    DADGAD, open G/D/E/C, 7-string, bass, and chromatic.
  - A4 is adjustable (415–466 Hz).
  - Click a string to pick it manually and hear a reference pluck.
  - If the monitor is off, the page offers buttons to start it.
  - It can also listen to any CoreAudio input, such as a microphone.

The server listens on localhost only. Set `POD_PORT` to use another port.

## Command line

```sh
cd control
npm link                   # optional: puts `pod` on your PATH
pod help                   # or: node control/dist/cli.js help
```

The CLI starts the server in the background on first use, so the CLI and the
web UI share one USB session and can be used at the same time.

```sh
pod ui                     # start the server and open the web UI
pod status                 # connection and loaded preset
pod presets                # list stored presets
pod load 5A                # load preset 5A (or a number 0-63)
pod show                   # chain and knob values of the loaded preset
pod show 9C                # look at a stored preset without loading it
pod set amp drive 0.7      # knob values are 0.0-1.0
pod off reverb             # switch a block off / on
pod models amp plexi       # search model names
pod model amp "Brit J-800" # change the amp model
pod set amp bass 0.4 --tone 2
pod revert                 # drop live edits
pod monitor on --pair di   # POD audio on the Mac's output (main|tone1|tone2|di)
pod monitor on --silent    # tuner only, no playback
pod monitor off
pod backup                 # dump all presets to control/backups/
pod dump 4D 4D.bin         # raw bytes of one stored preset
pod build presets/*.json --out built   # preset files -> .bin (no device)
pod apply presets/1A-plexi-crunch.json   # write preset files to the POD
pod restore 4D <backup folder>         # write a slot back from a backup
pod server stop            # stop the background server and the monitor
```

Blocks: `amp cab stomp mod delay reverb gate comp eq wah volume loop`. Add
`--json` to any command for machine-readable output.

With Claude Code, `CLAUDE.md` and `control/CLAUDE.md` explain how to drive the
CLI. You can ask in plain words, for example "load 9C and give it more gain",
and Claude runs the commands.

### Preset memory

Loading and editing only change the POD's edit buffer, and switching presets
on the device discards those edits. Only `pod apply` and `pod restore` write
preset memory:
- Writes refuse to run without a complete backup (`pod backup`).
- They refuse the slot that is loaded in the edit buffer.
- Every written slot is read back and compared byte for byte.

`pod restore <slot|all> <backup folder>` puts backed-up presets back. Preset
files (`control/presets/`) and backups (`control/backups/`) are personal and
gitignored.

## Audio tools

```sh
cd audio
make            # builds everything into audio/build/
make test       # hardware-free tests (clock DLL, rings, plug-in properties)
```

Only one process can stream on the audio interface at a time: one of these
tools, the monitor started from the web UI, or the helper daemon.

```sh
./build/monitor                 # play the POD's main output on the Mac's default output
./build/monitor --pair di       # dry guitar (inputs 7+8); also: main, tone1, tone2
./build/monitor --silent        # no playback, only the tuner feed
./build/monitor --gain -6 --target 192   # gain in dB, ring fill target in frames
./build/rec 8 capture           # record 8 s: capture.wav (8 ch) + capture-{main,tone1,tone2,di}.wav
./build/rawcap 2                # packet sizes, timing gaps, levels (diagnostics)
./build/tone 5 --freq 440       # sine to the POD's USB output
./build/tone 10 --latency       # clicks out, looks for them on input 1
./build/probe                   # claim interface 0, print descriptors, release
```

Stop any of them with Ctrl-C. They shut down cleanly: they stop resubmitting,
wait for the transfers in flight, then release the interface.

### Input channels

All channels are 24-bit and 48 kHz. Each pair carries the same signal on L and R.

| Inputs | Signal |
|---|---|
| 1+2 | Main output (full preset) |
| 3+4 | Tone 1 |
| 5+6 | Tone 2 |
| 7+8 | Dry guitar (DI), for amp simulators |

## CoreAudio device

The CoreAudio device has two parts:
- **HAL plug-in** (`PodX3Audio.driver`): presents the device as "POD X3",
  with 8 inputs and 2 outputs at 48 kHz.
- **USB helper daemon** (`podx3d`): owns the USB stream and shares audio and
  the POD's clock with the plug-in through shared memory.

The plug-in runs sandboxed inside coreaudiod, so the helper runs as a
LaunchDaemon. It runs as your user, not root, from `audio/build/podx3d`.

```sh
cd audio && make
sudo packaging/install.sh      # installs the plug-in and LaunchDaemon, restarts coreaudiod
sudo packaging/uninstall.sh    # removes both
```

- **After rebuilding the helper:** run `pkill podx3d`. launchd restarts it.
  Never use `-9`.
- **After rebuilding the plug-in:** run `install.sh` again.
- **The installed helper holds the audio interface all the time.** To use
  the monitor or the tools instead, stop it with
  `sudo launchctl bootout system/com.pkalamarski.podx3d`.
- **Logs:** `log stream --predicate 'subsystem BEGINSWITH "com.pkalamarski.podx3"'`.

## How it works

The POD has two USB interfaces, and each part of this repository uses one.
Both parts select the USB configuration only when its interfaces do not exist
yet, which happens right after the POD is plugged in. Selecting it again would
reset the other part's interface. So the parts start in any order, and
restarting the controller does not interrupt audio.

**Control (interface 1):**
- Bulk endpoints `0x01`/`0x81`, carrying 64-byte framed messages
  (`control/src/protocol.ts`).
- Presets are 4096-byte blobs, decoded and edited in `control/src/patch.ts`.
- The protocol was reverse-engineered by the community (see Credits).

**Audio (interface 0, altsetting 1):**
- **Endpoints:** EP `0x86` is isochronous IN (max 170 B) and EP `0x02` is
  isochronous OUT (max 46 B). Both use `bInterval` 1, so there is one packet
  per 125 µs microframe. This follows the Linux driver
  (`sound/usb/line6/podhd.c`); details were confirmed on hardware.
- **Packet format:** there is no header. Packets are raw interleaved S24_3LE
  frames: 24 bytes per IN frame (8 ch), 6 bytes per OUT frame (2 ch).
- **Packet size:** IN packets carry 5, 6 or 7 frames, 48 000 frames/s on
  average. Each OUT packet carries as many frames as recent IN packets did.
- **Clock:** the POD is the clock master. The monitor follows it with a
  resampler whose rate is adjusted by a PI controller on the ring fill level.
  The CoreAudio helper does the same with a DLL.

**Rules on macOS:**
- **Always send OUT while reading IN.** Polling IN with no OUT once left the
  audio engine silent until a power cycle. The library refuses to do it.
- **Never cancel transfers while traffic flows.** To stop, IN stops first,
  then OUT, and the tool waits for the transfers in flight.
- **Keep 16 IN transfers in flight.** libusb's darwin backend schedules
  isochronous transfers at least 4 ms ahead. Extra IN transfers add no
  latency; each OUT transfer adds 1 ms, so OUT uses 6.
- **Run libusb's event thread with real-time policy.** At normal priority it
  stalls completions by 4–6 ms about once a second.

[docs/audio-driver.md](docs/audio-driver.md) has the full hardware findings,
measurements, open questions and a troubleshooting playbook.

## Repository layout

| Path | What it is |
|---|---|
| `podx3` | one-command launcher |
| `audio/usb/` | USB audio transport (libusb) |
| `audio/tools/` | `monitor`, `rec`, `rawcap`, `tone`, `probe` |
| `audio/common/` | shared headers: rings, WAV, tuner tap, shared memory, clock DLL |
| `audio/helper/` | `podx3d` USB helper daemon |
| `audio/driver/` | `PodX3Audio.driver` CoreAudio HAL plug-in |
| `audio/packaging/` | plug-in Info.plist, LaunchDaemon plist, install scripts |
| `audio/tests/` | hardware-free tests |
| `control/src/protocol.ts` | control-channel framing and message encoders |
| `control/src/device.ts` | control driver (`PodX3`): claims interface 1, reads and writes |
| `control/src/patch.ts` | decoding and editing of the 4096-byte preset blob |
| `control/src/preset.ts` | builds patches from preset files |
| `control/src/controller.ts` | state: loaded preset, working copy, preset list |
| `control/src/server.ts` | HTTP API, live updates (SSE), serves `public/` |
| `control/src/monitor.ts` | starts and stops `audio/build/monitor` for the web UI |
| `control/src/tap.ts` | relays the tuner feed to the browser |
| `control/src/cli.ts` | command line client |
| `control/public/` | web UI and tuner |
| `control/data/` | model and knob names |
| `docs/` | implementation notes |

## Development

```sh
cd audio && make && make test
cd control && npm run build && npm run typecheck
POD_DEBUG=1 node control/dist/server.js   # log raw control packets
```

After rebuilding the controller, restart the server with `pod server stop`.
The next `pod` command, or `./podx3`, starts it again.

## Troubleshooting

- **The POD stops answering commands** while it is still plugged in:
  power-cycle it. The server logs to `control/.cache/server.log`.
- **Input packets are all empty** (`rawcap` shows only 0-byte packets):
  power-cycle the POD.
- **Monitor: "the POD audio interface is busy":** another tool, the helper
  or a second monitor is streaming. Stop it first.
- **Model knobs:** knob names are known for the models the factory presets
  use. On other models, labels may say "Param N".

See [docs/audio-driver.md](docs/audio-driver.md) for the full playbook.

## Credits and license

The control protocol was reverse-engineered by the community:
- [andree182/podx3](https://github.com/andree182/podx3) (GPLv2);
- [adrianoftyriel/POD-Controller](https://github.com/adrianoftyriel/POD-Controller)
  (GPLv3). This implementation follows its protocol notes, and its model and
  knob tables are in `control/data/`.

The audio format follows the Linux kernel's line6 driver.

Licensed under GPL-3.0-or-later; see [control/LICENSE](control/LICENSE).

Not affiliated with or endorsed by Line 6 / Yamaha Guitar Group.
