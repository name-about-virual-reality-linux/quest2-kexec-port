#!/bin/bash
# de_service_kde.sh: SteamOS Stage-2 Full KDE Plasma 6 + OOBE for Quest 2
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
export XDG_CURRENT_DESKTOP=KDE
export KDE_SESSION_VERSION=6
mkdir -p $XDG_RUNTIME_DIR
chmod 0700 $XDG_RUNTIME_DIR

echo "==> Switching to SteamOS Stage-2 (KDE Plasma 6)..."

# Stop Stage-1 Openbox and standalone xterm sessions cleanly
pkill -f "openbox" 2>/dev/null || true
pkill -f "xterm" 2>/dev/null || true
sleep 1

# Start D-Bus session if not running
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    eval $(dbus-launch --sh-syntax 2>/dev/null) || true
fi

# Start real-time stereo mirroring daemon (clones left eye viewport to right eye)
pkill -f "mirror_xwd.py" 2>/dev/null || true
python3 /usr/local/bin/mirror_xwd.py /run/frame.xwd > /var/log/mirror_xwd.log 2>&1 &

# Start KDE Plasma 6 X11 session
echo "==> Starting KDE Plasma 6 session..."
nohup startplasma-x11 > /var/log/plasma-session.log 2>&1 &

# Launch KDE OOBE Onboarding Wizard
sleep 3
echo "==> Launching Plasma Onboarding (OOBE)..."
nohup plasma-welcome > /var/log/plasma-welcome.log 2>&1 &

echo "==> Stage-2 KDE Plasma 6 running!"
