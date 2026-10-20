# POD X3 audio driver: implementation notes

This is the knowledge base for debugging the audio side (`audio/`). It records
what was measured on hardware, what failed and why, and which numbers are
guesses. It also gives a troubleshooting playbook. Last updated 2026-10-20.

Test setup: POD X3 desktop (`0e41:414a`, firmware 440 reported by the control
channel, 0x201004 by the `0x67` vendor request), MacBook Pro on Apple Silicon,
macOS 15.7, libusb 1.0.29 (Homebrew, linked statically).

## 1. Device facts (verified on hardware)

- **Device:** USB 2.0 high speed. `bNumConfigurations` 1. Every interface
  is class 0xFF (vendor-specific), so macOS has no driver for it.
- **macOS quirk:** the device reports configuration 1, but macOS creates the
  interfaces only after a configuration has been selected.
- **Interface 0** (audio):
  - alt 0 has no endpoints.
  - alt 1 has EP `0x86` IN and EP `0x02` OUT. Both are isochronous with sync
    type none and usage data, maxp 170 and 46 bytes, `bInterval` 1. That
    means one packet per 125 µs microframe, 8000 packets/s.
- **Interface 1** (control): bulk `0x01`/`0x81`, maxp 512. It is owned by
  `control/`.
- **Several processes can hold the device at once.** Chrome and Spotify open
  device user clients just by enumerating. Interfaces are claimed separately,
  so the controller (interface 1) and an audio process (interface 0) coexist.
  This is verified.

### Stream format

- **No header.** Packets are raw interleaved S24_3LE. The Linux driver
  (`sound/usb/line6/podhd.c`, `capture.c`, `playback.c`) agrees.
- **IN:** 8 channels, 24 bytes per frame. Packets carry 5, 6 or 7 frames,
  exactly 48 000 frames/s on average. The first packet of every stream is
  3 bytes (seen as `00 00 9d`) and must be skipped. Rare 4-frame packets occur.
- **OUT:** 2 channels, 6 bytes per frame. Nominally 6 frames per packet. We
  send as many frames as the IN side delivered, keeping running totals.
- **Channel map** (verified with a guitar):

  | Inputs | Signal |
  |---|---|
  | 1+2 | Main out |
  | 3+4 | Tone 1 |
  | 5+6 | Tone 2 |
  | 7+8 | Dry DI |

  Each pair carries the same signal on L and R. With a single-tone preset,
  Main equals Tone 1.
- **Levels in one test:** DI peak −13.7 dBFS (RMS −30). Main peak −35 dBFS
  (RMS −46), which is quiet; raise the preset output level if needed.
- **The Linux init is not needed.** The `0x67` vendor sequence (`wValue 0x11`
  plus reads of `0xF000`–`0xF080`) succeeds but is not required for audio. It
  is kept as `podx3_linux_init()`, which `rawcap --init` uses.

## 2. Rules learned the hard way

1. **Never poll IN without OUT running.**
   - Linux marks this device `LINE6_CAP_IN_NEEDS_OUT`.
   - We once streamed IN alone for 1 s. That run returned about 63 ms of real
     audio, which was the device's FIFO from an earlier alt-1 hold.
   - After that, every IN packet was 0 bytes. This lasted across OUT streams,
     the `0x67` init and alt cycling. Only a **power cycle** fixed it, while
     EP0 kept answering the whole time.
   - `podx3_stream_start` now refuses `enable_out == false`.
2. **Stop order: IN first, then OUT, and never cancel transfers.**
   - Stop resubmitting IN and wait for the IN transfers in flight. Then stop
     OUT and wait. Then select alt 0 and release (`podx3_stream_stop`).
   - The control channel once hung after an interrupted read, so we never call
     `libusb_cancel_transfer` while traffic is flowing.
3. **Never re-select the configuration on a configured device.**
   - Re-selecting resets every interface, so the other process's stream dies.
   - Select configuration 1 only when claiming fails because the interface
     does not exist: `LIBUSB_ERROR_NOT_FOUND` in libusb, or "interface not
     found" in the node `usb` library.
   - Both `audio/usb/podx3_usb.c` and `control/src/device.ts` follow this.
     The controller's old behaviour was to re-select on every connect.
4. **Only one process can stream on interface 0.** That means one of
   `monitor`, `rec`, the `podx3d` helper, or the monitor the controller server
   spawns. A second one fails with `could not open interface for exclusive
   access (0xe00002c5)`, which libusb reports as access denied or busy.

## 3. libusb on macOS: real-time findings

These measurements come from `rawcap` (completion-gap histogram) and from the
`monitor` statistics.

- **Scheduling lead.** The darwin backend (`submit_iso_transfer` in
  `darwin_usb.c`) schedules each iso transfer at `GetBusFrameNumber() + 4`,
  or at the end of the previous transfer if that is later. Every submission
  therefore lands at least 4 ms in the future. With 4 transfers in flight,
  packets were lost and a 4 ms ring underran constantly.
- **Transfer counts.**
  - IN uses 16 transfers of 8 packets (1 ms) each. They add no latency,
    because IN data is delivered as soon as each transfer completes.
  - OUT uses 6 transfers. Each OUT transfer in flight adds 1 ms of output
    latency.
  - The counts are configured separately with `n_xfers` and `n_xfers_out`.
- **The event thread needs real-time priority.**
  - libusb runs its own thread, named `org.libusb.device-hotplug`, which
    receives IOKit completions.
  - At normal priority, it stalls completions by 4–6 ms about once a second.
  - With Mach time-constraint policy (period 1 ms, computation 0.2 ms) on all
    threads, the maximum gap is 1.1–2.3 ms.
  - Tools call `podx3_make_all_threads_realtime()` right after `podx3_open()`,
    before creating AudioUnits or dispatch queues.
  - The helper looks the libusb thread up by name instead. `libusb_exit`
    recreates that thread on every reconnect, and applying the policy to all
    threads would also pin the dispatch and XPC threads.
- **Steady state.** IN completions arrive every 0.5–1.5 ms, measured as
  completion-to-completion gaps. 1005 transfers/s × 8 packets confirms
  per-microframe scheduling.

## 4. `monitor` (POD → Mac output)

- **Data path.** The USB callback converts the selected pair to float and
  pushes it into a lock-free SPSC ring (`common/ring.h`, 8192 frames). The
  CoreAudio default-output render callback reads it through a linear
  resampler.
- **Clock drift.** The POD's crystal and the Mac's are independent; the
  measured offset is a few tens of ppm.
  - A PI controller on the smoothed ring fill error sets the resampler
    ratio. The P gain is 0.001 × the relative error.
  - The I gain is 5e-7 per callback, clamped to ±1000 ppm. With 2e-6, the
    integrator ran up to +4000 ppm and oscillated.
- **Ring target.** The default is 288 frames (6 ms). It grows automatically
  to (callback size + 160) when the output pulls larger buffers.
  - The headphone jack used 512-frame callbacks, more than the whole
    288-frame ring. That caused about 94 underruns/s, heard as a buzz.
  - The monitor also listens for default-output changes and requests a
    128-frame buffer on the new device.
- **Priming.** When the output starts or after an underrun, the monitor
  skips any excess down to exactly the target. Without that, the queued
  backlog becomes latency that the slow loop takes minutes to drain.
- **Measured:** 0 underruns over 30–60 s with a 128-frame buffer on the
  MacBook speakers and on headphones. Occasional single clicks still happen:
  8 in about 10 minutes, and 15–21 in a long session.
- **Restart on stream death.** One run ended with `resubmit ep 0x02: Other
  error` (cause unknown: no sleep, and no controller reconnect at that time).
  The monitor now closes, reopens and restarts after 1 s while keeping the
  CoreAudio output alive.
- **Estimated latency** (POD input to Mac speakers, not measured end to end):
  about 6 ms ring + about 2.7 ms CoreAudio buffer + device latency + about
  1–2 ms USB. The user found it acceptable by ear.

## 5. Tuner tap

- **Format.** The tap carries input 7 (DI) as mono int16 little-endian at
  12 kHz: 48 kHz box-filtered and decimated by 4.
- **Socket.** It is served on `/tmp/podx3-tap-<uid>.sock` by
  `common/tap.h`, which `monitor` and `podx3d` both use.
- **Thread.** The USB callback only calls `tap_push`, which writes to a
  lock-free ring. A separate thread decimates and writes every 20 ms, and
  drops clients that fall behind.
- **Relay.** `control/src/tap.ts` relays the socket to `/api/tuner/stream`.
  `public/tuner.html` runs YIN over a 1024-sample window, with a range of
  38–1400 Hz and a threshold of 0.12. Synthetic tests detected
  41.2 / 61.7 / 81.8 / 329.7 Hz correctly.
- **Server-managed monitor.** The controller server can start and stop
  `audio/build/monitor` (`control/src/monitor.ts`, `POST /api/monitor`) and
  stops it with SIGINT on shutdown.

## 6. CoreAudio device (helper + HAL plug-in): built, not yet tested

- **Architecture.** The plug-in runs sandboxed in coreaudiod. The
  `AudioServerPlugIn.h` documentation permits standard IOKit user clients and
  declared Mach services. A separate helper owns USB:
  - `podx3d` is a LaunchDaemon in the system domain. It runs as the user (not
    root) from `audio/build/podx3d`, with `KeepAlive` set.
  - It serves Mach service `com.pkalamarski.podx3d`. The plug-in's Info.plist
    lists it under `AudioServerPlugIn_MachServices` and connects with
    `XPC_CONNECTION_MACH_SERVICE_PRIVILEGED`.
  - Shared memory is created with `mach_vm_allocate`, wrapped with
    `xpc_shmem_create`, and mapped with `xpc_shmem_map`. Its size is
    344064 bytes.
- **Shared memory layout** (`common/shm.h`):
  - Both rings (IN 8 ch, OUT 2 ch, 8192 frames each) are indexed by the
    absolute USB frame number.
  - `in_write_frame` and `out_read_frame` are cursors.
  - A `generation` counter is bumped on every stream (re)start, and frame
    numbers restart at 0.
  - The clock is protected by a seqlock.
- **Clock.**
  - The POD is the clock master. The helper runs a 2nd-order DLL
    (`common/dll.h`) from frames to `mach_absolute_time`, updated once per IN
    transfer.
  - Bandwidth is 4 Hz while locking, narrowing to 0.1 Hz. At 0.5 Hz the rate
    noise was about 200 ppm; at 0.1 Hz it is about 20 ppm.
  - Errors are clipped to ±2 ms, and the DLL resets above 50 ms.
  - `streaming` is set to 1 only after 250 ms of DLL runtime.
  - The plug-in owns its sample time (`common/pclock.h`). It re-bases on
    generation changes, helper restarts and host-clock fallback, so zero
    timestamps never go backwards.
- **Device loss detection** (helper): no IN for 500 ms, no IN within 2 s of
  start, or ≥ 50 transfer errors/s. Then: stop, close, reopen every 1 s,
  generation++.
  - Lost IN packets are filled with 6 frames of silence, so the frame
    timeline stays honest.
- **Plug-in constants** (`driver/PodX3Audio.c`) are guesses to tune after
  hardware tests:

  | Constant | Value |
  |---|---|
  | `PODX3_IN_SAFETY` | 144 |
  | `PODX3_OUT_SAFETY` | 144 |
  | `PODX3_IN_LATENCY` | 48 |
  | `PODX3_OUT_LATENCY` | 288 |
  | `PODX3_ZTS_PERIOD` | 4096 |
  | `PODX3_HEARTBEAT_MS` | 100 |

  The safety offsets are 144 rather than 96 because the DLL line tracks
  transfer completion time, and data arrives up to about 2.3 ms late.
- **Other plug-in behaviour.**
  - `DeviceCanBeDefaultSystemDevice` is 0, so system sounds do not go to the
    unverified output.
  - `PODX3AUDIO_OFFLINE=1` makes the plug-in skip the helper, for tests.
- **Install:** `sudo audio/packaging/install.sh`. **Remove:**
  `sudo audio/packaging/uninstall.sh`.
- **Logs:** `log stream --predicate 'subsystem BEGINSWITH "com.pkalamarski.podx3"'`.
  The helper logs under `com.pkalamarski.podx3`, the plug-in under
  `com.pkalamarski.podx3audio`.

## 7. Unverified / open questions

- **Mac → POD output.** It streams (it is required as the IN clock), but no
  one has heard it yet. `tone` and `tone --latency` exist to test it.
- **Round-trip latency** has not been measured. The goal is under 10 ms at
  64–128 frames. If libusb's 4-frame lead is too much, the fallback is
  IOUSBHost (`IOUSBHostIsochronousTransaction`, which also gives per-frame
  timestamps).
- **The helper and plug-in** have not run on coreaudiod or on the device.
  Untested: the generation re-anchor when the controller reconnects, the
  safety-offset tuning, and whether macOS asks for background-item approval
  for the LaunchDaemon.
- **The `Other error` resubmit failure** in `monitor` is still unexplained.
- **Occasional single underruns** in `monitor` are unexplained. Candidates:
  CoreAudio callback jitter, or USB completion gaps above 2 ms.

## 8. Troubleshooting playbook

| Symptom | Check | Likely cause / fix |
|---|---|---|
| All IN packets are 0 bytes (`rawcap 2`) | `rawcap` histogram shows only `0 bytes` | Audio engine wedged (IN polled without OUT?). Power-cycle the POD. |
| `claim interface 0: Access denied` / `0xe00002c5` | `ioreg -p IOService -r -n "POD X3" -l -w0 \| grep -E '\+-o\|bAlternateSetting'` | Another streamer holds interface 0: monitor, rec, the helper, or the server-spawned monitor. Stop it. With the helper installed: `sudo launchctl bootout system/com.pkalamarski.podx3d`. |
| `interface 0 not present` at open | Freshly plugged device | Expected. The library selects configuration 1 by itself. |
| Audio stops when the controller starts | `control/src/device.ts` | Something re-selects the configuration. It must only do so on "interface not found". |
| Buzz / constant underruns in `monitor` | `callback max` in stats > ring target | The output device uses large callbacks. The target auto-grows; check the `ring fill N/target` line. |
| Periodic clicks, about 1/s | `rawcap` gap histogram has entries > 4 ms | The libusb event thread is not real-time. Check the `podx3_make_all_threads_realtime` / helper RT log. |
| Rate adj pinned at ±1000 ppm | `monitor` stats | The integrator saturated; the PI gains or the clock assumptions are wrong. |
| Tuner says "no audio" | `ls /tmp/podx3-tap-*.sock`, `curl -m2 127.0.0.1:8787/api/tuner/stream \| wc -c` (about 24 KB/s) | Monitor/helper not running. Turn it on in the UI or with `pod monitor on --silent`. |
| Control channel stops answering | `control/.cache/server.log` | The known wedge. Power-cycle. Never cancel bulk reads (see `control/CLAUDE.md`). |

### Diagnostic tools (`audio/build/`)

- `probe [s]`: claim interface 0, select alt 1, print descriptors, hold for s
  seconds. Set `PROBE_CMD` to run a command while holding.
- `rawcap [s] [--init] [--settle MS] [--xfers N] [--pkts N]`:
  - prints the packet length histogram, the arrival timeline per 50 ms, the
    completion-gap histogram, per-channel peaks and hex dumps;
  - `PODX3_RT=1` enables real-time threads.
- `rec [s] [prefix]`: record 8-channel and per-pair WAVs, with a frame-rate
  check.
- `monitor [--pair …] [--silent] [--target N] [--buffer N] [--xfers N]`:
  stats every 2 s.
- `tone [s] [--freq] [--db] [--latency] [--xfers N]`: sine or click test on
  OUT.
