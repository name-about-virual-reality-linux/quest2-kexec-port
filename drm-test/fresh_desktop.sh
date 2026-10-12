#!/bin/bash
# on-device: fresh Xvfb + openbox + per-eye terminals (single source of truth)
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p $XDG_RUNTIME_DIR
chmod 0700 $XDG_RUNTIME_DIR
rm -rf /tmp/.X99-lock
nohup Xvfb :99 -screen 0 3664x1920x24 +extension MIT-SHM > /tmp/xvfb.log 2>&1 &
sleep 2
nohup openbox > /dev/null 2>&1 &
sleep 1
xsetroot -solid "#0055cc"
nohup xterm -geometry 32x24+400+460 -fa FreeMono -fs 30 -bg black -fg green -T LEFT-EYE -e /bin/bash /tmp/eye.sh LEFT > /dev/null 2>&1 &
nohup xterm -geometry 44x24+2450+460 -fa FreeMono -fs 30 -bg black -fg green -T RIGHT-EYE -e /bin/bash /tmp/eye.sh RIGHT > /dev/null 2>&1 &
sleep 2
echo FRESH_DESKTOP_UP
xwininfo -root -tree 2>&1 | grep -cE "XTerm"
