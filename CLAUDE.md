# CLAUDE.md

Monorepo for the Line 6 POD X3 on macOS:
- `audio/`: USB audio, C11 with libusb. See README.md for the protocol and tools.
- `control/`: the controller (TypeScript/Node: control channel on USB
  interface 1, server, web UI, CLI). Its own rules are in
  `control/CLAUDE.md`, which applies to everything under `control/`.

Before debugging the audio side, read `docs/audio-driver.md`. It holds the
hardware findings, the measured numbers, the open questions and a
troubleshooting playbook. Update it when you learn something new.

## Layout (audio/)

- `usb/podx3_usb.{h,c}`: the transport. All USB access goes through it.
- `common/`: header-only shared code: `ring.h` (SPSC ring), `wav.h`, `shm.h`
  (shared memory layout between helper and plug-in), `shm_io.h`, `dll.h`
  (USB frame → host time DLL), `pclock.h` (plug-in timeline).
- `tools/`: CLI tools, one file each. The Makefile builds each `tools/X.c`
  into `build/X`.
- `helper/`: `podx3d`, the USB helper daemon. It is a LaunchDaemon that runs as
  the user and serves the shared memory over XPC.
- `driver/`: `PodX3Audio.driver`, the CoreAudio HAL plug-in.
- `packaging/`: Info.plist, the LaunchDaemon plist, and install/uninstall
  scripts (sudo).
- `tests/`: hardware-free tests, run with `make test`.

Build in `audio/` with `make` (plus `make test`). There is no CMake or
pkg-config on this machine. libusb comes from Homebrew and is linked
statically.

## Hardware rules (the POD is fragile)

- Never poll IN (EP 0x86) without OUT (EP 0x02) running. Doing so wedged the
  audio engine until a power cycle. `podx3_stream_start` enforces this; keep it.
- Never cancel transfers while traffic is flowing. To stop: stop
  resubmitting, wait for the in-flight transfers, IN first and then OUT, then
  set alt 0 and release. This is `podx3_stream_stop`. A separate project's
  control channel once hung after an interrupted read.
- Never call `libusb_set_configuration` when the device is already
  configured. Only do it when claiming interface 0 returns `NOT_FOUND`.
- Audio code never touches interface 1. That interface belongs to
  `control/`, and the two parts only share the device.
- The same applies to `control/src/device.ts`: it calls
  `selectConfiguration(1)` only when claiming fails with "interface not
  found". Re-selecting the configuration resets interface 0 and stops audio.
- Only one process can stream on interface 0. Before running a hardware
  test, make sure the user is not running `monitor` or `rec`, and that the
  helper is not loaded. When installed, the helper holds interface 0 all the
  time; stop it with `sudo launchctl bootout system/com.pkalamarski.podx3d`.
  Check with `ioreg -p IOService -r -n "POD X3" -l -w0 | grep -E '\+-o|bAlternateSetting'`.
- If every IN packet is 0 bytes, the audio engine is wedged. Ask the user to
  power-cycle the POD; nothing in software has revived it.

## Real-time notes

- Use 16 IN transfers (no latency cost) and about 6 OUT transfers (1 ms of
  latency each). libusb/darwin schedules at least 4 frames ahead.
- Call `podx3_make_all_threads_realtime()` right after `podx3_open()`, before
  creating CoreAudio units or dispatch queues. It also covers libusb's internal
  event thread, which otherwise stalls for 4–6 ms.
- Use `rawcap` to measure completion gaps and `monitor` to see ring fill and
  underruns.

## System changes

Do not disable SIP and do not change security settings. Installing the HAL
plug-in and LaunchDaemon, and restarting coreaudiod, needs sudo. Give the user
the commands (they can run them with `!`); do not run them yourself.

## Commits

Never commit preset backups (`control/backups/`) or the user's preset files
(`control/presets/`). Both are gitignored and stay local.
