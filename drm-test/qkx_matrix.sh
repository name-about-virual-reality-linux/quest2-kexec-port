#!/bin/sh
# Run kms_atomic3 variants back to back; report DSI FIFO error counts for each.
# Usage: qkx_matrix.sh [hold_seconds]
HOLD=${1:-5}
BL=/sys/class/backlight/panel0-backlight/brightness

run() {
	name=$1; bl=$2; shift 2
	dmesg -c >/dev/null
	env QKX_HOLD=$HOLD "$@" /tmp/kms_atomic3 >/tmp/k_$name.log 2>&1 &
	pid=$!
	if [ "$bl" = bl ]; then
		sleep 2
		echo 255 >$BL 2>/tmp/k_$name.blerr
	fi
	wait $pid
	ov=$(dmesg | grep -c fifo_overflow)
	un=$(dmesg | grep -ci underflow)
	ok=$(grep -o 'mode [0-9x@]*, clock [0-9]*' /tmp/k_$name.log)
	echo "$name: overflow=$ov underflow=$un [$ok] bl=$(cat $BL 2>/dev/null)"
}

run base120 - QKX_HZ=120
run hz90 - QKX_HZ=90
run hz90_crtc1 - QKX_HZ=90 QKX_CRTC=1
run hz72 - QKX_HZ=72
run hz60 - QKX_HZ=60
run hz90_bl bl QKX_HZ=90
