#!/bin/bash
clear
echo "####################################################"
echo "#           QUEST 2  -  HOLO DESKTOP               #"
echo "####################################################"
echo ""
cat /etc/os-release
echo ""
echo "Desktop: Xvfb 3664x1920 + openbox + xterm"
echo "Panel:   dual-plane KMS, planes 101/98"
echo "Date:    $(date -u)"
echo ""
echo "Type commands below. This shell is live."
exec /bin/bash --login
