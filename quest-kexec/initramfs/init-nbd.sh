#!/bin/busybox sh
# Init for NBD Arch boot: ECM gadget + telnet shell, then connect the NBD
# rootfs from the Mac (10.42.0.1:10809) and switch_root into Arch systemd.
# Kernel cmdline qkx_root=ram forces a RAM shell (skip NBD).
export PATH=/bin:/sbin
export HOME=/root
export TERM=vt100
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /config /tmp /newroot
mount -t devpts devpts /dev/pts
mount -t configfs configfs /config
mount -t tmpfs tmpfs /tmp
exec >/dev/kmsg 2>&1

log() { echo "qkx-nbd: $*"; }
bootdone() { [ -w /proc/qkx_bootdone ] && echo "$1" > /proc/qkx_bootdone; }

log "reached $(uname -r)"

g=/config/usb_gadget/qkx
mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" "$g/functions/ecm.usb0"
echo 0x1d6b > "$g/idVendor"
echo 0x0105 > "$g/idProduct"
echo QKX-NBD > "$g/strings/0x409/serialnumber"
echo 'quest-pro-kexec' > "$g/strings/0x409/manufacturer"
echo 'NBD Arch boot' > "$g/strings/0x409/product"
echo 'ECM' > "$g/configs/c.1/strings/0x409/configuration"
echo 250 > "$g/configs/c.1/MaxPower"
echo 02:51:4b:58:00:02 > "$g/functions/ecm.usb0/dev_addr"
echo 02:51:4b:58:00:01 > "$g/functions/ecm.usb0/host_addr"
ln -s "$g/functions/ecm.usb0" "$g/configs/c.1/ecm.usb0"

udc=
for _ in 1 2 3 4 5 6 7 8 9 10; do
	for c in /sys/class/udc/*; do
		[ -e "$c" ] || continue
		echo "${c##*/}" > "$g/UDC" && udc=$c && break 2
	done
	log "waiting for UDC"
	bootdone k
	sleep 1
done
if [ -z "$udc" ]; then
	log "no UDC; forcing warm reset so the log survives"
	bootdone r
	while :; do sleep 60; done
fi
log "bound ${udc##*/}"
echo on > "$udc/device/../power/control" 2>/dev/null

ifconfig lo 127.0.0.1 up
ifconfig usb0 10.42.0.2 netmask 255.255.255.0 up
cat > /tmp/udhcpd.conf <<EOC
interface usb0
start 10.42.0.1
end 10.42.0.1
max_leases 1
lease_file /tmp/udhcpd.leases
option subnet 255.255.255.0
option lease 86400
EOC
touch /tmp/udhcpd.leases
udhcpd /tmp/udhcpd.conf
(while :; do telnetd -F -l /bin/sh -p 23; log "telnetd exited ($?); restarting"; sleep 1; done) &
echo $! > /tmp/telnet.pid

bootdone d
qkx-klogwatch -f /tmp/kernel.log >/dev/null 2>&1 &

MODE=nbd
for p in $(cat /proc/cmdline); do
	case "$p" in qkx_root=*) MODE="${p#qkx_root=}";; esac
done

NBD_OK=
if [ "$MODE" != ram ] && [ -e /sys/block/nbd0 ]; then
	log "connecting NBD root (10.42.0.1:10809)"
	nbd-client 10.42.0.1 10809 /dev/nbd0 &
	for _ in $(seq 1 30); do
		sleep 2
		SZ=$(blockdev --getsize64 /dev/nbd0 2>/dev/null)
		if [ -n "$SZ" ] && [ "$SZ" -gt 1000000 ] 2>/dev/null; then
			log "nbd0 ready (${SZ} bytes)"
			break
		fi
	done
	if mount -t ext4 -o rw /dev/nbd0 /newroot; then
		if [ -x /newroot/sbin/init ] && [ -f /newroot/etc/arch-release ]; then
			log "NBD Arch ready, switching root"
			NBD_OK=1
		else
			log "NBD mounted but no Arch (missing sbin/init or arch-release)"
			umount /newroot
		fi
	else
		log "NBD mount failed (is qemu-nbd up on the Mac?)"
	fi
else
	[ "$MODE" = ram ] && log "ram mode requested"
	[ -e /sys/block/nbd0 ] || log "no nbd0 (NBD driver missing?)"
fi

if [ "$NBD_OK" = 1 ]; then
	exec switch_root /newroot /sbin/init
fi

log "ready: telnet 10.42.0.2 (nbd_ok=$NBD_OK mode=$MODE)"
while [ ! -e /tmp/qkx-launch ]; do sleep 1; done
exec qkx-switch
