# SteamOS Stage-1 Stereo Desktop for Meta Quest 2

This directory contains the verified, working Stage-1 stereo desktop environment and presentation pipeline for Valve/Collabora SteamOS (Holo Core Aarch64) running natively from UFS NAND flash on the Meta Quest 2 ("hollywood" / Qualcomm Snapdragon XR2 `kona` SoC).

## Hardware & Display Architecture

- **Panel**: JDI 1920x3664 @ 120Hz dual-pipe portrait panel (`clock=382164`).
- **DRM Subsystem**: Qualcomm MSM DRM (`/dev/dri/card0`), Connector 29.
- **CRTC**: 129 (`crtc-0`, `enable=1`, `active=1`, `plane_mask=0xc0`).
- **Hardware Planes**:
  - **Plane 101 (`plane-7`)**: Left Eye Viewport, mapped to `crtc-0`, FB 263 (1920x3664 XR24).
  - **Plane 98 (`plane-6`)**: Right Eye Viewport, mapped to `crtc-0`, FB 263 (1920x3664 XR24).
- **Desktop Geometry**:
  - Virtual Screen: 3664x1920 @ 24bpp landscape X11 root window.
  - Software 90-degree CCW rotation (`QKX_ROT=1`) mapping the 3664x1920 desktop into the 1920x3664 portrait panel scanout.
  - Optical Window Layout:
    - **Left Eye Window**: `+360+460` (75x35 cols/rows, 24pt Droid Sans Mono).
    - **Right Eye Window**: `+2192+460` (75x35 cols/rows, 24pt Droid Sans Mono).

## File Manifest

- `kms_dual_image.c`: Complete C source code for the raw ioctl KMS atomic stereo presenter. Includes the XWD colormap offset fix (`data_offset = hdr_size + ncolors * 12` = 3179 bytes) to parse standard XWD dumps cleanly.
- `kms_dual_image`: Statically linked aarch64 ELF binary compiled with GCC.
- `de_service.sh`: Desktop environment initialization service (`/usr/local/bin/de_service.sh`). Spawns `Xvfb :99`, `openbox`, `show_eyes.sh`, initial XWD screenshot, and the 0.33Hz snapshot daemon.
- `show_eyes.sh`: Optical terminal layout launcher (`/usr/local/bin/show_eyes.sh`).
- `eye.sh`: Terminal shell script (`/usr/local/bin/eye.sh`) displaying SteamVR eye markers, hardware display specs, and SteamOS release details.
- `qkx-init`: PID 1 init script (`/sbin/qkx-init`) mounting pseudo-filesystems, bringing up `usb0` at `10.42.0.2`, and starting telnet daemons.
- `rc.local`: Startup script (`/etc/rc.local`) setting network routes and launching `de_service.sh`.

## Compilation

To rebuild `kms_dual_image` statically:
```bash
docker run --rm -v $(pwd):/work -w /work qkx-build:latest \
    aarch64-linux-gnu-gcc -O2 -static -I. kms_dual_image.c -o kms_dual_image
```
