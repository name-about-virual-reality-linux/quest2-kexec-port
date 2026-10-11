# quest2-kexec-port

Porting [quest-kexec](https://github.com/YusufOzmen01/quest-kexec) (a
kexec loader that boots a custom Linux kernel on a Meta Quest headset
without unlocking the bootloader, originally for Quest Pro) to the
**Quest 2** ("hollywood"), plus the display bring-up and DRM/KMS
debugging work that followed once kexec itself was working.

## Status

kexec-booting a custom minimal Linux kernel (BusyBox + USB Ethernet) on
a real Quest 2 works — see `NOTES.md` for the full day-by-day log,
including the exact kernel source/commit matched to the device, the two
loader fixes needed for Quest 2 vs Quest Pro hardware, and kernel config
changes required for a clean boot.

Display bring-up (getting the kexec'd kernel to drive the Quest 2's own
panel over DSI) is still open — diagnosed as DSC bitstream corruption,
not yet resolved. That work is paused in favor of
[vr-shell](https://github.com/name-about-virual-reality-linux/vr-shell),
a VR desktop shell developed against a simulated headset so it doesn't
block on real display bring-up.

## Layout

- `quest-kexec/` — the loader itself (kernel module + initramfs + host
  tooling), forked from upstream with Quest-2-specific fixes. See its
  own `README.md` for build/usage instructions.
- `NOTES.md` — the detailed working log for the Quest 2 port and
  display debugging: exact kernel/commit matching, hardware differences
  from Quest Pro, kernel config changes, and the DSI/display
  investigation.
- `drm-test/` — standalone DRM/KMS atomic-modeset test programs, used
  while debugging the display path independent of the kexec'd kernel.
- `scratch_patch/` — in-progress DSI display driver patch work.

## Safety

A failed kexec handoff can reboot or lock the headset. See
`quest-kexec/README.md`'s own Safety section — the tooling only copies
files to `/data/local/tmp`, it doesn't flash, format, or mount
partitions.
