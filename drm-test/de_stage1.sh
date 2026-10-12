#!/bin/sh
# Stage-1 desktop on the Quest 2 panel: Xvfb (offscreen X11, landscape
# 3664x1920) + xwd snapshots presented through the proven dual-plane
# atomic path (kms_dual_image). No DRM-plane assignment risk: the
# compositor never touches /dev/dri; only the presenter does, with the
# exact planes/geometry Android's own composer uses (101/98 halves).
#
# Runs ON DEVICE in the Holo chroot. Start after de_holo_setup.sh:
#   chroot /mnt/holorw /bin/bash /tmp/holo/de_stage1.sh
# Stop: killall Xvfb xwd kms_dual_image konsole xterm
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p $XDG_RUNTIME_DIR
chmod 0700 $XDG_RUNTIME_DIR

# presenter holds DRM master; make sure nothing else does
killall -9 kms_atomic_dual kms_play_vr kms_play kms_dual_image 2>/dev/null || true

Xvfb :99 -screen 0 3664x1920x24 +extension MIT-SHM &
sleep 2
# window manager + background + one terminal
openbox &
sleep 1
xsetroot -solid "#2e3440" 2>/dev/null || true
xterm -geometry 200x60+100+100 &
# snapshot loop: xwd root window -> /tmp/frame.xwd every second
(while :; do xwd -root -silent -out /tmp/frame.xwd 2>/dev/null; sleep 1; done) &
# presenter: polls /tmp/frame.xwd, rotates 90 CW, dual-plane commit
exec /tmp/kms_dual_image /tmp/frame.xwd
