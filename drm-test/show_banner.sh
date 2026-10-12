#!/bin/bash
# on-device: fullscreen banner terminal on :99, large font
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
pkill -f "xterm" 2>/dev/null || true
sleep 1
xsetroot -solid "#0055cc"
nohup xterm -fullscreen -fa "FreeMono" -fs 42 -bg black -fg "#33ff33" -T QUEST-DE -e /bin/bash /tmp/banner.sh > /tmp/xterm.log 2>&1 &
sleep 2
echo BANNER_LAUNCHED
