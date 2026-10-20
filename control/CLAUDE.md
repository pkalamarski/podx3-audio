# POD X3 controller

Userspace USB driver (TypeScript on Node.js, `usb` package) for a Line 6 POD X3 (0x0e41:0x414a)
on macOS, a local server with a web UI, and a CLI.

## Controlling the POD from Claude Code

Use the CLI; it starts the background server on first use (http://127.0.0.1:8787),
so the CLI and the web UI share the single USB session.

```
node dist/cli.js status
node dist/cli.js presets              # 64 stored presets, banks 1-16 x A-D
node dist/cli.js load 5A              # load a preset (edit buffer only)
node dist/cli.js show                 # chain of the loaded preset
node dist/cli.js set amp drive 0.7    # knob values 0.0-1.0
node dist/cli.js off reverb | on delay
node dist/cli.js model amp "Plexi"    # change model (amp cab stomp mod delay reverb wah)
node dist/cli.js models delay echo    # search model names
node dist/cli.js revert               # drop live edits
node dist/cli.js backup               # dump all presets to backups/ (read-only)
node dist/cli.js dump 4D /tmp/4D.bin  # raw bytes of one stored preset (read-only)
node dist/cli.js build presets/*.json --out /tmp/built   # preset files -> .bin, no device
node dist/cli.js apply presets/1A-plexi-crunch.json        # WRITES preset memory
node dist/cli.js restore 4D <backup-folder>  # WRITES a slot back from a backup
```

Preset files (`presets/*.json`, format in `src/preset.ts`, list in
`presets/README.md`) are built on a real patch from `presets/templates/`.

Add `--tone 2` for the second tone of dual-tone presets. When the user asks for
a sound in words ("more crunch", "add a slapback delay"), translate it into
these commands, then `show` the result.

## Rules

- Loads and edits only touch the edit buffer. Only `apply` and `restore` write
  preset memory (`02/02`, `PodX3.writePatch`). Before writing: run a fresh
  `backup` and have the user's explicit go-ahead for the exact slots. Never
  write slots the user did not name.
- Write safeguards (keep them): `Controller.writeSlot` refuses without a
  complete backup in `backups/`, refuses the slot loaded in the edit buffer,
  skips slots that already hold the same bytes, and reads every written slot
  back and compares it byte for byte. `apply` builds every file before the
  first write. The write is never retried on a missing ack.
- New write code or a new kind of preset edit: prove it on one slot first
  (write, `dump`, `cmp` with the built bytes, `cmp` neighbouring slots with
  the backup, `restore`, write again), and finish with a full `backup` compared
  file by file with the one taken before.
- Only one process may claim USB interface 1. Don't run scripts that open the
  device while the server runs; `node dist/cli.js server stop` first.
- The POD's control channel once wedged (USB alive, no bulk replies; only a power
  cycle helped). Likely cause, not proven: an IN read cancelled by the 1 s library
  timeout or a killed process mid-reply. Hence reads never time out (`src/device.ts`).
  If it happens again: power cycle and check `.cache/server.log`.
- Sources in `src/*.ts`, compiled to `dist/` (`npm run build`). After a rebuild,
  `node dist/cli.js server stop` so the next command starts the new server.
- Protocol notes: `src/protocol.ts`. Debug packet log: `POD_DEBUG=1`.
