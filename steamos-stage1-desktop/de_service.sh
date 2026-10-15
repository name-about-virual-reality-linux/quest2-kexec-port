[?2004h[root@(none) /]# cat /usr/local/bin/de_service.sh; echo ===END_OF_FILE===
[?2004l#!/bin/bash
export DISPLAY=:99
export XDG_RUNTIME_DIR=/run/xdg
export QKX_ROT=1
export QKX_DT_MS=2000
mkdir -p $XDG_RUNTIME_DIR && chmod 0700 $XDG_RUNTIME_DIR

pkill -9 -f "Xvfb|openbox|xterm|kms_dual_image|xwd" 2>/dev/null || true
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 /run/frame.xwd*

Xvfb :99 -screen 0 3664x1920x24 +extension MIT-SHM > /var/log/xvfb.log 2>&1 &
sleep 2

openbox > /var/log/openbox.log 2>&1 &
sleep 1

xsetroot -solid "#0055aa" 2>/dev/null || true

/usr/local/bin/show_eyes.sh &
sleep 4

xwd -root -silent -out /run/frame.xwd

(while :; do
    sleep 3
    xwd -root -silent -out /run/frame.xwd.tmp 2>/dev/null && mv -f /run/frame.xwd.tmp /run/frame.xwd
done) &

exec /usr/local/bin/kms_dual_image /run/frame.xwd
