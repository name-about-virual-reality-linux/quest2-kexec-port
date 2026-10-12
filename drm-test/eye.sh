#!/bin/bash
# $1 = LEFT or RIGHT
clear
echo "==============================="
echo "  $1 EYE TERMINAL"
echo "==============================="
echo ""
cat /etc/os-release | head -3
echo ""
echo "This shell is live."
exec /bin/bash --login
