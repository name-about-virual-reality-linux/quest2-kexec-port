#!/bin/sh
set -e
echo "1. Stopping SurfaceFlinger and display composers..."
stop surfaceflinger || true
stop vendor.oculus.hardware.composer-service || true
setprop ctl.stop vendor.qti.hardware.display.composer || true
sleep 2

echo "2. Launching kms_play..."
/data/local/tmp/kms_play /data/local/tmp/testvideo.qkxv
