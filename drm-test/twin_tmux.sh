#!/bin/bash
# on-device: one shared tmux session shown in both eye terminals.
# Both xterms attach to the same session: identical live content per eye,
# no fusion conflict, no seam reading. Type in either; both update.
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
pkill -9 -f "xterm" 2>/dev/null || true
sleep 1
tmux -S /tmp/desk.sock kill-server 2>/dev/null || true
nohup xterm -geometry 20x24+0+460 -fa FreeMono -fs 30 -bg black -fg green -T EYE-L -e tmux -S /tmp/desk.sock new -A -s desk > /dev/null 2>&1 &
sleep 1
nohup xterm -geometry 44x24+1630+460 -fa FreeMono -fs 30 -bg black -fg green -T EYE-R -e tmux -S /tmp/desk.sock new -A -s desk > /dev/null 2>&1 &
sleep 2
echo TWIN_UP
