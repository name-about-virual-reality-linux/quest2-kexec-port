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
	KEYS=/tmp/nand-keys.txt
	META=/tmp/nand-metadata-key.txt
	got_keys=0; got_meta=0
	wget -q http://10.42.0.1:8000/nand-keys.txt -O "$KEYS" && got_keys=1
	[ "$got_keys" = 1 ] || { wget -q http://10.42.0.1:8000/nand-key.txt -O "$KEYS" && got_keys=1; }
	wget -q http://10.42.0.1:8000/nand-metadata-key.txt -O "$META" && got_meta=1
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
	log "remounting rw and switching to Arch"
	mount -o remount,rw /userdata
	mount --bind /userdata/media/0/qkx-root /newroot
	mount --bind /dev /newroot/dev
	mount --bind /proc /newroot/proc
	mount --bind /sys /newroot/sys
	mount -t tmpfs tmpfs /newroot/tmp
	mount -t tmpfs tmpfs /newroot/run 2>/dev/null
	# Debug shell that survives switch_root: static binary copied into the
	# new root + telnetd bound to its absolute post-switch path. Processes
	# survive switch_root (old root is wiped, but open fds stay), so :2323
	# stays reachable even if Arch systemd never brings up networking.
	cp /sbin/qkx-sh-static /newroot/sbin/qkx-sh-static
	log "static cp rc=$?; newroot sbin: $(ls -l /newroot/sbin/qkx-sh-static 2>&1)"
	log "userdata mount: $(mount | grep ' /userdata ')"
	log "newroot init: $(ls -l /newroot/sbin/init 2>&1)"
	(while :; do telnetd -F -l /sbin/qkx-sh-static -p 2323; sleep 1; done) &
	log "debug shell on :2323; switching root to Arch systemd"
	exec switch_root /newroot /sbin/init
fi

log "ready: telnet 10.42.0.2 (nand_ok=$NAND_OK added=$ADDED mode=$MODE)"
while [ ! -e /tmp/qkx-launch ]; do sleep 1; done
exec qkx-switch
