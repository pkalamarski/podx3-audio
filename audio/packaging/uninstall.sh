#!/bin/sh
# Removes the PodX3Audio plug-in and the podx3d LaunchDaemon. Idempotent.
#   sudo packaging/uninstall.sh
set -eu
LABEL=com.pkalamarski.podx3d
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
launchctl bootout system/$LABEL 2>/dev/null || true
rm -f /Library/LaunchDaemons/$LABEL.plist
if [ -e /Library/Audio/Plug-Ins/HAL/PodX3Audio.driver ]; then
    rm -rf /Library/Audio/Plug-Ins/HAL/PodX3Audio.driver
    killall coreaudiod 2>/dev/null || true
fi
echo "uninstalled"
