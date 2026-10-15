[?2004h[root@(none) /]# cat /usr/local/bin/eye.sh; echo ===END_OF_FILE===
[?2004l#!/bin/bash
# $1 = LEFT or RIGHT
clear
echo "================================================="
echo "       STEAM VR - $1 EYE DISPLAY"
echo "================================================="
echo ""
cat /etc/os-release
echo ""
echo "Resolution: 1832x1920 per eye | Mode: 120Hz"
echo "Stereo Commit: Plane 101 (L) / Plane 98 (R)"
echo "================================================="
echo ""
echo "Welcome to SteamOS Stage-1 Desktop!"
echo ""
exec /bin/bash --login
