# quest2-kexec-port

Porting [quest-kexec](https://github.com/YusufOzmen01/quest-kexec) (a
kexec loader that boots a custom Linux kernel on a Meta Quest headset
without unlocking the bootloader, originally for Quest Pro) to the
**Quest 2** ("hollywood"), plus the display bring-up and DRM/KMS
debugging work that followed once kexec itself was working.

## Status

kexec-booting a custom Linux kernel on a real Quest 2 works, and that kernel
now boots **SteamOS / Holo Core natively from the device's own UFS NAND**, with
a working stereo desktop presented to both lenses over raw KMS. See `NOTES.md`
for the full day-by-day log: the exact kernel source/commit matched to the
device, the two loader fixes needed for Quest 2 vs Quest Pro hardware, the
NAND decrypt path (dm-default-key + fscrypt keys grabbed from the running
Android kernel), and the display bring-up.

Two desktop stages exist. Stage 1 (openbox + one terminal per eye) is
**verified in-headset**; stage 2 (KDE Plasma 6) is installed on the target but
not yet confirmed on the panel -- its launcher was rewritten to health-check
itself and fall back to stage 1 rather than leave the panel dark. See
`steamos-stage1-desktop/README.md`.

While in kexec, the headset's RGB status LED blinks purple as a "this is not
Android" indicator (`quest-kexec/initramfs/qkx-led.sh`).

The earlier DSI bring-up work (raw panel, before the NAND path) is paused in
favor of [vr-shell](https://github.com/name-about-virual-reality-linux/vr-shell).

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
