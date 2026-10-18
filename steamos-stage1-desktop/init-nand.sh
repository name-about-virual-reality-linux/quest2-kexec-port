#!/bin/busybox sh
# Init for NAND Arch boot: ECM gadget + telnet shell, then assemble userdata
# (f2fs FBE) from /dev/sda9 and optionally switch_root into the Arch rootfs
# at /data/media/0/qkx-root.
#
# Stock Android presents userdata as dm-49 (dm target "default-key", ICE
# inline crypto) over sda9: raw sda9 reads are ciphertext, the f2fs magic
# only appears through the mapping. So this init recreates the same mapping
# (dmcreate, legacy table `AES-256-XTS <meta> /dev/sda9 0`, metadata key
# fetched over ECM HTTP), mounts the mapped device ro, then ADD_KEYs the
# file keys (nand-keys.txt, one "DESC RAW" pair per line) via kadd.
#
# Key file format (served by the Mac over ECM HTTP): one "DESC RAW" pair
# per line, where DESC is the 16-hex-char v1 descriptor and RAW is the
# 128-hex-char (64-byte) raw key. Example:
#   ff56a2ac60835a6c d56dcaab...fbe89b
#   ab9b06a9b17d657c 67a08dec...22722531
# The qkx-root path (/data/media/0) only needs the CE-user0 key, but
# adding device-DE + DE-user0 too makes the rest of userdata browsable.
# Tries nand-keys.txt (multi-key) first, falls back to legacy single-key
# nand-key.txt for back-compat.
#
# Kernel cmdline modes (qkx_nand=...):
#   ro   (default) mount userdata read-only, prove decrypt, stay in shell
#   rw   remount read-write, bind qkx-root, switch_root to Arch systemd
export PATH=/bin:/sbin
export HOME=/root
export TERM=vt100
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /config /tmp /newroot /userdata
mount -t devpts devpts /dev/pts
mount -t configfs configfs /config
mount -t tmpfs tmpfs /tmp
exec >/dev/kmsg 2>&1

log() { echo "qkx-nand: $*"; }
bootdone() { [ -w /proc/qkx_bootdone ] && echo "$1" > /proc/qkx_bootdone; }

log "reached $(uname -r)"

# Immediately activate full charging rate
echo 0 > /sys/class/power_supply/battery/input_suspend 2>/dev/null || true
echo 1 > /sys/class/power_supply/battery/rerun_aicl 2>/dev/null || true

g=/config/usb_gadget/qkx
mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" "$g/functions/ecm.usb0"
echo 0x1d6b > "$g/idVendor"
echo 0x0105 > "$g/idProduct"
echo QKX-NAND > "$g/strings/0x409/serialnumber"
echo 'quest-pro-kexec' > "$g/strings/0x409/manufacturer"
echo 'NAND Arch boot' > "$g/strings/0x409/product"
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

# --- NAND assembly via dm-default-key (metadata) + fscrypt (files) ---
# Stock Android presents userdata as dm-49 (type default-key, ICE inline
# crypto) over sda9: raw sda9 reads are ciphertext (f2fs magic only appears
# through the mapping). Recreate the same mapping here, then ADD_KEY the
# file keys. Legacy table syntax (driver byte-swaps the key per u32 itself):
#   0 <len> default-key AES-256-XTS <metahex> /dev/sda9 0
NAND_OK=
ADDED=0
DMDEV=
if [ -e /sys/block/sda/sda9 ]; then
	log "sda9 present"
	# Prevent UFS from auto-hibernating or clock-gating without Android power manager
	for node in clkgate_enable clkscale_enable auto_hibern8; do
		echo 0 > /sys/devices/platform/soc/1d84000.ufshc/$node 2>/dev/null || true
	done
	KEYS=/tmp/nand-keys.txt
	META=/tmp/nand-metadata-key.txt
	got_keys=0; got_meta=0
	for attempt in $(seq 1 10); do
		wget -q http://10.42.0.1:8000/nand-keys.txt -O "$KEYS" && got_keys=1
		[ "$got_keys" = 1 ] || { wget -q http://10.42.0.1:8000/nand-key.txt -O "$KEYS" && got_keys=1; }
		wget -q http://10.42.0.1:8000/nand-metadata-key.txt -O "$META" && got_meta=1
		if [ "$got_keys" = 1 ] && [ "$got_meta" = 1 ]; then break; fi
		sleep 1
	done
	[ "$got_keys" = 1 ] || log "key fetch failed (is the Mac http server up?)"
	[ "$got_meta" = 1 ] || log "metadata key fetch failed"
	if [ "$got_keys" = 1 ] && [ "$got_meta" = 1 ]; then
		read METARAW < "$META"
		LEN=$(cat /sys/block/sda/sda9/size)
		[ -e /dev/mapper/control ] || mknod /dev/mapper/control c 10 236
		if /bin/dmcreate userdata 0 "$LEN" default-key "AES-256-XTS $METARAW /dev/sda9 0"; then
			for dm in /sys/block/dm-*; do
				[ "$(cat $dm/dm/name 2>/dev/null)" = userdata ] || continue
				N=${dm##*/dm-}
				[ -e /dev/dm-$N ] || mknod /dev/dm-$N b 253 $N
				DMDEV=/dev/dm-$N
				break
			done
		else
			log "dm mapping failed"
		fi
		rm -f "$META"
		if [ -n "$DMDEV" ] && mount -t f2fs -o ro,inlinecrypt "$DMDEV" /userdata; then
			log "$DMDEV mounted ro"
			while read DESC RAW _; do
				case "$DESC" in ""|\#*) continue;; esac
				if [ -n "$DESC" ] && [ -n "$RAW" ]; then
					if /bin/kadd /userdata "$DESC" "$RAW"; then
						ADDED=$((ADDED + 1))
					else
						log "ADD_KEY failed for $DESC"
					fi
				fi
			done < "$KEYS"
			rm -f "$KEYS"
			log "added $ADDED key(s)"
			if ls /userdata/media/0/qkx-root/bin/sh >/dev/null 2>&1; then
				log "DECRYPT OK: qkx-root readable"
				NAND_OK=1
				ls /userdata/media/0/qkx-root/etc/os-release >/dev/null 2>&1 && \
					log "os-release present"
			else
				log "decrypt FAILED: qkx-root not readable (added=$ADDED)"
			fi
		else
			log "userdata mount failed"
		fi
	fi
else
	log "no sda9; staying in RAM shell"
fi

MODE=ro
for p in $(cat /proc/cmdline); do
	case "$p" in qkx_nand=*) MODE="${p#qkx_nand=}";; esac
done

if [ "$NAND_OK" = 1 ] && [ "$MODE" = rw ]; then
	mount -o remount,rw /userdata
	TARGET_ROOT=/userdata/media/0/qkx-root
	TARGET_OS=arch
	for p in $(cat /proc/cmdline); do
		case "$p" in qkx_root=*) TARGET_OS="${p#qkx_root=}";; esac
	done
	if [ -f /userdata/media/0/boot_os ]; then
		TARGET_OS="$(cat /userdata/media/0/boot_os | tr -d '\r\n ')"
	fi
	if [ "$TARGET_OS" = "holo" ] || [ "$TARGET_OS" = "steamos" ]; then
		if [ -d /userdata/media/0/holo-root/bin ]; then
			TARGET_ROOT=/userdata/media/0/holo-root
			log "switching to Holo Core (SteamOS) at $TARGET_ROOT"
		else
			log "holo-root not found; falling back to Arch at $TARGET_ROOT"
		fi
	else
		log "switching to Arch Linux at $TARGET_ROOT"
	fi
	mount --bind "$TARGET_ROOT" /newroot
	mount --bind /dev /newroot/dev
	mount --bind /proc /newroot/proc
	mount --bind /sys /newroot/sys
	mount -t tmpfs tmpfs /newroot/tmp
	mount -t tmpfs tmpfs /newroot/run 2>/dev/null
	# Keep userdata accessible inside guest OS at /userdata
	mkdir -p /newroot/userdata
	mount --bind /userdata /newroot/userdata 2>/dev/null
	# Debug shell that survives switch_root: static binary copied into /dev/busybox
	# (/dev is a shared devtmpfs bind, always writable).
	cp /sbin/qkx-sh-static /dev/busybox
	chmod 755 /dev/busybox 2>/dev/null
	log "static /dev/busybox rc=$?; dev/busybox: $(ls -l /dev/busybox 2>&1)"
	log "userdata mount: $(mount | grep ' /userdata ')"
	log "newroot init: $(ls -l /newroot/sbin/init 2>&1)"
	# Create 4.19-compatible PID 1 init script for guest OS
	# (bypasses systemd v261 STATX_MNT_ID / EUNATCH failure on Linux 4.19)
	cat << 'EOINIT' > /newroot/sbin/qkx-init
#!/bin/sh
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
export HOME=/root
export TERM=vt100

OS_BANNER="Linux"
[ -f /etc/os-release ] && . /etc/os-release && OS_BANNER="${PRETTY_NAME:-$NAME}"

echo "=== QKX $OS_BANNER INIT (PID 1) STARTING ===" > /dev/kmsg

# Mount standard pseudo-filesystems
[ -d /proc/1 ] || mount -t proc proc /proc
[ -d /sys/kernel ] || mount -t sysfs sysfs /sys
[ -e /dev/console ] || mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /dev/shm /run /tmp /userdata
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs tmpfs /dev/shm 2>/dev/null
mount -t tmpfs tmpfs /run 2>/dev/null
mount -t tmpfs tmpfs /tmp 2>/dev/null

# Configure network on usb0
ifconfig lo 127.0.0.1 up 2>/dev/null
ifconfig usb0 10.42.0.2 netmask 255.255.255.0 up 2>/dev/null
ip addr add 10.42.0.2/24 dev usb0 2>/dev/null
ip link set usb0 up 2>/dev/null

echo "qkx-init: network up (10.42.0.2 on usb0)" > /dev/kmsg

# Start persistent telnet debug shells in target root using /bin/bash via /dev/busybox
(while :; do (/dev/busybox telnetd -F -l /bin/bash -p 23 || /dev/busybox telnetd -F -l /bin/sh -p 23); sleep 1; done) &
(while :; do (/dev/busybox telnetd -F -l /bin/bash -p 2323 || /dev/busybox telnetd -F -l /bin/sh -p 2323); sleep 1; done) &

echo "qkx-init: shells active on :23 and :2323" > /dev/kmsg

echo "=== $OS_BANNER READY (10.42.0.2) ===" > /dev/kmsg

# Run user startup script if present
[ -x /etc/rc.local ] && /etc/rc.local &

# Reap zombie children and keep PID 1 alive
while :; do
	sleep 3600
done
EOINIT
	chmod 755 /newroot/sbin/qkx-init

	{
		echo "=== qkx pre-switch $(date -u) ==="
		echo "--- cmdline: $(cat /proc/cmdline)"
		echo "--- mounts:"; mount | grep -E 'userdata|newroot'
		echo "--- ip:"; ip addr show usb0 2>/dev/null || ifconfig usb0
		echo "--- dm:"; ls -l /dev/dm-* 2>/dev/null
		echo "--- newroot:"; ls /newroot/sbin/init /newroot/sbin/qkx-init /newroot/sbin/qkx-sh-static 2>&1
		dmesg | tail -n 40
	} > /userdata/qkx-boot.log 2>&1
	log "stopping pre-switch background tasks (klogwatch, telnetd)"
	killall -9 qkx-klogwatch 2>/dev/null || true
	killall telnetd 2>/dev/null || true
	log "executing switch_root to /sbin/qkx-init"
	exec switch_root /newroot /sbin/qkx-init
	log "switch_root EXEC FAILED rc=$?"
	while :; do sleep 30; log "post-switch survivor"; done
fi

log "ready: telnet 10.42.0.2 (nand_ok=$NAND_OK added=$ADDED mode=$MODE)"
while [ ! -e /tmp/qkx-launch ]; do sleep 1; done
exec qkx-switch
