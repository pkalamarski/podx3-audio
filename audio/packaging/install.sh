#!/bin/sh
# Installs the PodX3Audio HAL plug-in and the podx3d LaunchDaemon. Idempotent.
#   sudo packaging/install.sh            (run `make` first, as your user)
# The daemon runs $BUILD/podx3d in place as the invoking user, so after a
# rebuild `pkill podx3d` is enough (launchd restarts it). Re-run this script
# only after rebuilding the driver.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$(cd "$HERE/.." && pwd)/build}   # the only place the build dir is named
RUN_AS=${RUN_AS:-${SUDO_USER:-}}
LABEL=com.pkalamarski.podx3d
DRIVER_SRC=$BUILD/PodX3Audio.driver
DRIVER_DST=/Library/Audio/Plug-Ins/HAL/PodX3Audio.driver
PLIST_DST=/Library/LaunchDaemons/$LABEL.plist

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -n "$RUN_AS" ] && [ "$RUN_AS" != root ] || { echo "run via sudo from your user account (or set RUN_AS)" >&2; exit 1; }
[ -x "$BUILD/podx3d" ] && [ -d "$DRIVER_SRC" ] || { echo "build first: make (as $RUN_AS)" >&2; exit 1; }

# Daemon: stop the old instance (graceful: SIGTERM stops the USB stream), install, start.
launchctl bootout system/$LABEL 2>/dev/null || true
sed -e "s|@PODX3D@|$BUILD/podx3d|" -e "s|@USER@|$RUN_AS|" "$HERE/$LABEL.plist" > "$PLIST_DST"
chown root:wheel "$PLIST_DST"
chmod 644 "$PLIST_DST"
plutil -lint "$PLIST_DST" >/dev/null
launchctl bootstrap system "$PLIST_DST"

# Plug-in.
rm -rf "$DRIVER_DST"
ditto "$DRIVER_SRC" "$DRIVER_DST"
chown -R root:wheel "$DRIVER_DST"
killall coreaudiod 2>/dev/null || true   # launchd restarts it and loads the plug-in

echo "installed. check: launchctl print system/$LABEL | grep -E 'state|pid'"
echo "logs: log stream --predicate 'subsystem BEGINSWITH \"com.pkalamarski.podx3\"'"
