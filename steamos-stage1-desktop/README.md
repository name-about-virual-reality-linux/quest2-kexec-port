# SteamOS Desktop on the Meta Quest 2 (Stage 1 / Stage 2)

This directory contains the desktop bring-up for Valve/Collabora SteamOS (Holo
Core Aarch64) running natively from UFS NAND on the Meta Quest 2 ("hollywood" /
Snapdragon XR2 `kona` SoC), presented through the raw-KMS stereo presenter.

## Hardware & Display Architecture

- **Panel**: JDI 1920x3664 @ 120Hz dual-pipe portrait panel (`clock=382164`).
- **DRM Subsystem**: Qualcomm MSM DRM (`/dev/dri/card0`), Connector 29.
- **CRTC**: 129 (`crtc-0`, `enable=1`, `active=1`, `plane_mask=0xc0`).
- **Hardware Planes**:
  - **Plane 101 (`plane-7`)**: Left Eye Viewport, mapped to `crtc-0`, FB 263.
  - **Plane 98 (`plane-6`)**: Right Eye Viewport, mapped to `crtc-0`, FB 263.
- **Desktop Geometry**:
  - Virtual screen: 3664x1920 @ 24bpp landscape X11 root (Xvfb `:99`).
  - Software 90-degree CCW rotation (`QKX_ROT=1`) into the portrait scanout.
  - The left half of the desktop goes to the left eye, the right half to the
    right eye. Content must therefore be **identical in both halves** or the
    eyes never fuse -- see the eye mirror below.

## Stages

`de_service.sh` runs one of two stages against the same presenter:

| | Stage 1 | Stage 2 |
| --- | --- | --- |
| Window manager | `openbox` | `kwin_x11` |
| Shell | none (bare X) | `plasmashell` (Plasma 6) |
| Layout | one terminal per eye (`show_eyes.sh`) | full Plasma desktop, mirrored |
| Status | verified in-headset | untested on hardware |

`QKX_STAGE=auto` (the default) tries stage 2 and falls back to stage 1 if
`kwin_x11`/`plasmashell` are missing, fail to start, or die while running.

**Why the fallback matters:** the first Plasma attempt stalled between starting
Xvfb and starting `kwin_x11` -- the target had `kwin.log`/`plasmashell.log`
absent and nothing but `nohup: ignoring input` in `de_service.log` -- leaving
the panel on the presenter's test pattern with no window manager and no error
anywhere. Every step in `de_service.sh` is now existence-checked or
timeout-bounded and logs its outcome, and plasmashell is watched so the panel
can never be left showing a frozen frame.

## Eye mirroring

`mirror_xwd.py` owns the capture loop: it dumps the X root window with `xwd`,
mirrors the left-eye half onto the right-eye half, then **atomically renames**
the result over `/run/frame.xwd`. Capture and mirror used to live in two
separate processes, which let the mirror rewrite the frame file while
`kms_dual_image` was reading it (torn frames on the panel). The XWD header is
parsed properly (`header_size + ncolors * 12`, `bytes_per_line`) instead of
assuming a fixed prefix.

## "We are in kexec" status light

The initramfs arms a purple blink on the headset's RGB status LED
(`quest-kexec/initramfs/qkx-led.sh`), so a kexec'd boot is distinguishable from
Android at a glance. Purple is red + blue at full with green dark. The blink
runs in PM8150L tri-led hardware via the LED `timer` trigger, so it costs no CPU
and survives `switch_root`.

## File Manifest

- `kms_dual_image.c` / `kms_dual_image`: raw ioctl KMS atomic stereo presenter
  (reads `/run/frame.xwd` and commits planes 101 + 98).
- `de_service.sh`: the launcher -- presenter, Xvfb, frame pump, stage 2 with
  stage 1 fallback, zram swap, and all the logging.
- `de_service_kde.sh`: thin wrapper that forces `QKX_STAGE=2`.
- `mirror_xwd.py`: capture + eye-mirror + atomic frame replacement.
- `show_eyes.sh` / `eye.sh`: stage-1 per-eye terminals and their banner.
- `qkx-init`: PID 1 shim (`/sbin/qkx-init`) mounting pseudo-filesystems,
  bringing up `usb0` at `10.42.0.2`, re-arming the kexec light, and starting
  telnet shells.
- `rc.local`: network route + `de_service.sh` launch.
- `install_kde_offline.sh` / `fetch_and_install_kde.sh`: Plasma 6 package
  staging/installation (packages are pulled by the host over ECM).

## Compilation

To rebuild `kms_dual_image` statically:
```bash
docker run --rm -v $(pwd):/work -w /work qkx-build:latest \
    aarch64-linux-gnu-gcc -O2 -static -I. kms_dual_image.c -o kms_dual_image
```
