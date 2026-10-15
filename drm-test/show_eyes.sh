cat /usr/local/bin/show_eyes.sh; echo ===END_OF_FILE===
[?2004h[root@(none) /]# cat /usr/local/bin/show_eyes.sh; echo ===END_OF_FILE===
[?2004l#!/bin/bash
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg

pkill -f "xterm" 2>/dev/null || true
sleep 1

# Left eye terminal
xterm -geometry 75x35+360+460 -fa "Droid Sans Mono" -fs 24 -bg "#0d1117" -fg "#00ff66" -T "STEAM-LEFT" -e /usr/local/bin/eye.sh LEFT &

# Right eye terminal
xterm -geometry 75x35+2192+460 -fa "Droid Sans Mono" -fs 24 -bg "#0d1117" -fg "#00ff66" -T "STEAM-RIGHT" -e /usr/local/bin/eye.sh RIGHT &

sleep 2
echo EYES_LAUNCHED
