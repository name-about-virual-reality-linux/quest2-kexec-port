#!/bin/bash
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg

pkill -f "xterm" 2>/dev/null || true
sleep 1

# Left eye terminal (-ah = alwaysHighlight cursor so both eyes have identical solid cursor)
xterm -ah -geometry 75x35+360+460 -fa "Droid Sans Mono" -fs 24 -bg "#0d1117" -fg "#00ff66" -cr "#00ff66" -T "SteamOS Console" -e /usr/local/bin/eye.sh &

# Right eye terminal (-ah = alwaysHighlight cursor so both eyes have identical solid cursor)
xterm -ah -geometry 75x35+2192+460 -fa "Droid Sans Mono" -fs 24 -bg "#0d1117" -fg "#00ff66" -cr "#00ff66" -T "SteamOS Console" -e /usr/local/bin/eye.sh &

sleep 2
echo EYES_LAUNCHED
