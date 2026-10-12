#!/bin/sh
# Stage-1 Steam Frame (Holo Core) desktop bring-up on Quest 2 kexec Linux.
# Runs ON DEVICE inside the Holo chroot (telnet 10.42.0.2:23, after the
# overlay + network steps below). Idempotent: safe to re-run after reboot.
#
# What this does:
#   1. Writable overlay over the read-only squashfs (Holo ships no
#      /dev,/proc,/sys mountpoints -- mkdir them first, see NOTES.md).
#   2. USB-ECM default route + DNS + clock (cert validation needs it).
#   3. pacman keyring init (first-boot step) with --disable-sandbox
#      (Landlock does not exist on our 4.19 kernel).
#   4. Installs the Stage-1 desktop stack from the Holo repos
#      (mash-20251118.3, still current upstream as of 2026-10-12):
#        weston + xorg-server-xvfb + alacritty + mesa + vulkan-swrast
#      Stage 2 (gamescope session compositor) is NOT installed here --
#      it needs the dual-plane presenter validated first (see below).
#
# Display model (proven 2026-10-11/12): the panel is ONE physical
# 1920x3664 portrait surface split across two DSI links. A single
# full-width plane degrades into static; stock Android uses TWO planes
# (101 left [0,0,960,3664], 98 right [960,0,960,3664]) sharing one FB.
# drm-test/kms_atomic_dual is the proven commit path. Any compositor
# that assigns a single 1920-wide primary plane WILL show static --
# that is expected, not a regression. Stage 1 therefore renders the
# desktop OFFSCREEN (Xvfb/Weston headless, landscape 3664x1920) and
# presents via a dual-plane blit (rotate 90 CW, same as
# convert_vr_video.py's transpose=1). Native DRM-backend compositors
# (gamescope/weston --backend=drm) are Stage 3, after the presenter
# is validated with a live desktop image.
#
# Input: USB HID needs USB-host mode, but this kernel's USB-C port is
# bound to the ECM gadget (device mode) for shell+network. Both cannot
# hold the port at once. Options once this script is done:
#   (a) Bluetooth keyboard/mouse via bluez (keeps ECM shell+network);
#   (b) drop ECM (kill udhcpd/telnetd, unbind UDC) and switch to host
#       mode for a USB keyboard -- you lose the shell when you do this,
#       so only after the desktop is self-sufficient.
set -e

HOLO=/mnt/holo
RW=/mnt/holorw

if [ "$(id -u)" != 0 ]; then
  echo "run as root inside the kexec target"; exit 1
fi

# --- 1. overlay ---------------------------------------------------------
if ! mount | grep -q " $RW "; then
  echo "==> writable overlay"
  mkdir -p $HOLO $RW /tmp/ovl-upper /tmp/ovl-work
  if ! mount | grep -q " $HOLO "; then
    mount -t squashfs -o loop /arch-rootfs.squashfs $HOLO 2>/dev/null || \
    mount -t squashfs -o loop /holo-rootfs.squashfs $HOLO
  fi
  mount -t tmpfs tmpfs /tmp/ovl-upper
  mkdir -p /tmp/ovl-upper/upper /tmp/ovl-upper/work
  mount -t overlay overlay -o lowerdir=$HOLO,upperdir=/tmp/ovl-upper/upper,workdir=/tmp/ovl-upper/work $RW
  # Holo ships without these; the init bind-mounts silently no-op without them
  mkdir -p $RW/dev $RW/proc $RW/sys
  mount --bind /dev $RW/dev
  mount --bind /proc $RW/proc
  mount --bind /sys $RW/sys
  mount -t tmpfs tmpfs $RW/tmp
  echo "overlay ready: chroot $RW /bin/bash"
fi

# --- 2. network / dns / clock (inside chroot) -----------------------------
echo "==> network + clock"
chroot $RW /bin/bash -c '
  ip route add default via 10.42.0.1 dev usb0 2>/dev/null || true
  printf "nameserver 8.8.8.8\nnameserver 1.1.1.1\n" > /etc/resolv.conf
  # no RTC under kexec: set clock from host-provided value or build date fallback
  if [ -n "$QKX_DATE" ]; then date -u -s "$QKX_DATE"; fi
  getent hosts archlinuxarm.org >/dev/null && echo "dns ok" || echo "dns FAILED"
  curl -4 -sI --max-time 10 https://holo-packages.steamos.cloud/ >/dev/null && echo "https ok" || echo "https FAILED (check date -u)"
'

# --- 3. pacman keyring (first boot only) ----------------------------------
echo "==> pacman keyring"
chroot $RW /bin/bash -c '
  if ! pacman-key --list-keys >/dev/null 2>&1; then
    pacman-key --init && pacman-key --populate archlinuxarm
  fi
  pacman -Sy --disable-sandbox --noconfirm
'

# --- 4. stage-1 desktop stack ----------------------------------------------
echo "==> installing stage-1 desktop (weston, xvfb, alacritty, mesa, swrast)"
chroot $RW /bin/bash -c '
  pacman -S --disable-sandbox --noconfirm \
    weston xorg-server-xvfb alacritty \
    mesa vulkan-swrast vulkan-icd-loader wayland egl-wayland \
    konsole evtest usbutils bluez bluez-utils
'
echo "==> stage-1 packages installed. Next (manual, needs live panel):"
echo "    Xvfb :99 -screen 0 3664x1920x24 &"
echo "    DISPLAY=:99 weston --backend=x11 ...  # or x11 DE client directly"
echo "    present via dual-plane blit (kms_dual_desktop, to be validated live)"
echo "    see NOTES.md dual-plane section for plane IDs 101/98 geometry"
