#!/bin/bash
# on-device: two terminals, one fully inside each eye-half. Landscape
# 3664x1920, seam at x=1832. Windows are 60 cols (~1500px at fs30):
# left spans ~150-1650, right ~2000-3500. Measured via xwininfo.
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg
pkill -f "xterm" 2>/dev/null || true
sleep 1
xterm -geometry 60x32+150+250 -fa "FreeMono" -fs 30 -bg black -fg "#33ff33" -T LEFT-EYE -e /bin/bash /tmp/eye.sh LEFT &
xterm -geometry 60x32+2000+250 -fa "FreeMono" -fs 30 -bg black -fg "#33ff99" -T RIGHT-EYE -e /bin/bash /tmp/eye.sh RIGHT &
sleep 2
echo EYES_LAUNCHED
