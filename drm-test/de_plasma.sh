#!/bin/bash
# on-device: full Plasma (Steam Frame DE stack) on Xvfb :99.
# Replaces the openbox demo: kills xterms/openbox, starts a Plasma X11
# session on the SAME Xvfb (no X restart needed). The xwd snapshot loop
# and kms_dual_image presenter keep running untouched.
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p $XDG_RUNTIME_DIR
chmod 0700 $XDG_RUNTIME_DIR
pkill -f "xterm" 2>/dev/null || true
pkill -f "openbox" 2>/dev/null || true
sleep 1
# D-Bus session (Plasma requires it); dbus-daemon ships in Holo base
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
  eval $(dbus-launch --sh-syntax 2>/dev/null) || true
fi
# Plasma X11 session in background; log to /tmp/plasma-session.log
nohup startplasma-x11 > /tmp/plasma-session.log 2>&1 &
sleep 10
echo PLASMA_STARTED
tail -5 /tmp/plasma-session.log
