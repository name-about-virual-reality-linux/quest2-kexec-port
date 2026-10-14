# quest-kexec ported to Quest 2 (hollywood / kona) — working state snapshot

Date: 2026-10-10. Device: Quest 2 "hollywood", build `UP1A.231005.007.A1`,
kernel `4.19.325-cip128-st12-g4b63ca9fd613`, rooted via Magisk.

## Status: WORKING

Successfully kexec-booted a custom minimal Linux kernel (BusyBox + USB
Ethernet) on this exact Quest 2, with a live shell reachable over
`10.42.0.2:23`, all 8 CPUs online, stable. Returned cleanly to stock Android
via the built-in warm-reset path.

## Exact matching kernel source

`facebookincubator/oculus-linux-kernel`, branch `oculus-quest2-kernel-master`,
commit `6e428607392d72708bc95cb7126768494c6b5ac0` ("Oculus Quest 2 kernel
project build: 5220228.2810.150"). Confirmed by: `VERSION=4 PATCHLEVEL=19
SUBLEVEL=325` in Makefile, `localversion-cip`/`localversion-st` files present,
and the build-number arithmetic matching the device's
`ro.build.version.incremental` (52202280028100150 = 5220228 + 002810-padded +
0150-padded).

## Fixes made on top of upstream quest-kexec (YusufOzmen01/quest-kexec)

All in `quest-kexec/module/loader.c`, committed locally (see `git log`):

1. `suspend_syncboss`'s device lookup was hardcoded to `spi0.0` (works on
   Quest Pro). On Quest 2, `spi0.0` is the cm710x audio codec; syncboss is
   actually on `spi1.0`. Fixed.
2. The wakeup-suppression kthread (works around `pm_wakeup_pending()`
   aborting CPU-offline operations) was only kept alive through
   `freeze_processes()`, then stopped. On this hardware, something keeps a
   wakeup source persistently pending, so `machine_shutdown()`'s own later
   CPU-offline call hit the same abort and the module's own safety check
   panicked ("native CPU shutdown failed"). Extended the suppression window
   to also cover `shutdown_cpus()` (now stopped only after
   `num_online_cpus() == 1` is confirmed).

No other upstream addresses/device names needed changes — Quest Pro and
Quest 2 are both in Qualcomm's "kona" SoC family and share the same watchdog
(`17c10000.qcom,wdt`), USB controller (`a600000.dwc3`), AOP mailbox
(`c300000.qcom,qmp-aop`), and RPMh RSC (`18200000.rsc`) addresses. The kexec
staging window (`0x90000000`-`0x93400000`) also happens to be free RAM on
Quest 2 (confirmed against `/proc/iomem`), so `tools/prepare.py` needed no
changes either.

## Target kernel config changes (on top of the device's live android.config)

Required by `HOW_TO_BUILD.md`: `CONFIG_LOCALVERSION_AUTO=n`,
`CONFIG_ION_POOL_AUTO_REFILL=n`, `CONFIG_USB_CONFIGFS_ECM=y`,
`CONFIG_DEVTMPFS=y`, `CONFIG_PRINTK_TIME=y`, `CONFIG_LOG_BUF_SHIFT=20`.

Additionally disabled to get a clean boot on Quest 2 (see `build/out-target/target.config`):

- `CONFIG_ION` — the ION secure-heap hand-off state from the stock kernel
  doesn't survive the kexec transition; `msm_ion_probe()` NULL-derefs in
  `get_ion_heap()` very early in `do_basic_setup()`. Not needed for a
  BusyBox/USB-ethernet target anyway.
- `CONFIG_SERIAL_MSM_GENI`, `CONFIG_I2C_QCOM_GENI`, `CONFIG_SPI_QCOM_GENI`,
  `CONFIG_QCOM_GENI_SE` — the GENI serial-engine SMMU mappings also don't
  survive the transition (`qupv3_2_geni_se` threw repeated "Unhandled
  context fault... accessed an unmapped address"), which stalled a
  deferred-probe worker and hung `PID 1` forever waiting on
  `deferred_probe_initcall`. Not needed for the target either.

## Reproducing the build

Docker image `qkx-build:latest` (Debian bookworm + clang 14 + lld 14 +
gcc-aarch64-linux-gnu + device-tree-compiler), see `build/Dockerfile`.

Docker volumes used during the session, exported as tarballs under
`docker-volumes/` (9.6G total backup — this is the stable-foundation snapshot
before starting Holo Core experiments, which may reuse/overwrite these same
volume names):
- `qkx-ksrc` / `qkx-out`: Android-kernel-matching build (for `quest_kexec.ko`'s Module.symvers)
- `qkx-target-ksrc` / `qkx-target-out`: target kernel build (patched + reconfigured, squashfs enabled)

To restore a volume from its tarball:
```
docker volume create qkx-out
docker run --rm -v qkx-out:/v -v "$(pwd)/docker-volumes":/backup qkx-build:latest \
  tar xf /backup/qkx-out.tar -C /v
```
(repeat per volume name)

Rebuild the module (fast, ~1 min) once `qkx-ksrc`/`qkx-out` exist:
```
docker run --rm -v <path-to-quest-kexec>:/work/quest-kexec -v qkx-ksrc:/work/ksrc -v qkx-out:/work/android-out \
  qkx-build:latest \
  make -C /work/ksrc O=/work/android-out M=/work/quest-kexec/module ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- LOCALVERSION=-g4b63ca9fd613 \
  ccflags-y=-DQKX_ENABLE_EXPERIMENTAL_JUMP modules
```

Rebuild the target Image (patches already applied in `qkx-target-ksrc`, config already set in `qkx-target-out`):
```
docker run --rm -v qkx-target-out:/out -v qkx-target-ksrc:/ksrc qkx-build:latest sh -c \
  'cd /ksrc && make O=/out ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- -j$(nproc) Image'
docker run --rm -v qkx-target-out:/out qkx-build:latest cat /out/arch/arm64/boot/Image > build/out-target/Image
```

Regenerate the boot payload (Image + initramfs + boot.dtb from the live captured device tree):
```
cd quest-kexec
python3 tools/prepare.py --live-dtb ../captured-quest2/runtime.dtb --iomem ../captured-quest2/iomem \
  --kernel-image ../build/out-target/Image --initramfs out/initramfs.gz --output ../build/out-payload \
  --cmdline "$(sed -e 's/softdog.soft_panic=1//' ../captured-quest2/cmdline) rdinit=/init nokaslr printk.devkmsg=on qkx_qmp_adopt=1 initcall_blacklist=virtual_sensor_driver_init"
```

## Running it (manual, not via tools/run.sh — that hardcodes execute=1 with no safety params)

```
adb push quest-kexec/module/quest_kexec.ko /data/local/tmp/
adb push build/out-payload/Image /data/local/tmp/qkx-Image
adb push build/out-payload/initramfs /data/local/tmp/qkx-initramfs
adb push build/out-payload/boot.dtb /data/local/tmp/qkx-boot.dtb
adb shell su -c 'insmod /data/local/tmp/quest_kexec.ko image=/data/local/tmp/qkx-Image initrd=/data/local/tmp/qkx-initramfs dtb=/data/local/tmp/qkx-boot.dtb execute=1 preserve_watchdog=1 watchdog_recovery=0 core_hang_control=2 flush_rpmh=1 suspend_syncboss=1 disconnect_qmp=0 phase_delay_ms=300'
```

~16s later: `ping 10.42.0.2`, then `telnet 10.42.0.2 23` (or `quest-kexec/tools/shell.sh`,
`quest-kexec/tools/qkx_sh.py '<cmd>'`).

Return to Android: `quest-kexec/tools/return-android.sh` (writes `r` to
`/proc/qkx_bootdone`, triggers a DRAM-preserving PMIC warm reset — ~12s back
to Android in testing).

## Unrelated, pre-existing device quirk (not caused by any of this)

This specific unit has a recurring `syncboss_spi spi1.0: bad magic from MCU`
glitch roughly every 15-150s regardless of kexec activity. Caused one
watchdog-bark panic during testing (unrelated Android `HandCam` process hang)
with no lasting effect — device recovered normally.

## Progress log

**2026-10-10: Arch Linux ARM userland booted via kexec.**

Goal: boot an Arch-Linux-ARM-family userland (same OS family as Valve/Collabora's
"Holo Core" SteamOS-ARM preview for the Steam Frame headset) on Quest 2, as a
smaller milestone before attempting Holo Core itself. Holo Core as released so
far is mainly a package repo + build tooling (no published Steam-Frame-specific
kernel/board support), so this uses plain Arch Linux ARM (archlinuxarm.org)
rootfs atop our own already-working Quest 2 kexec kernel, rather than trying to
port Holo Core's own (Snapdragon 8 Gen 3-targeted) kernel to Quest 2's
completely different "kona" SoC.

Result: **working.** Chrooted into a genuine Arch Linux ARM64 userland with a
real `bash` (5.3.15) and `pacman` (7.1.0), reaching the shell in ~6 seconds
after the kexec jump (vs ~14-16s for the plain BusyBox-only boot).

Problems hit and fixed along the way:
1. First attempt: tried streaming the ~830MB stock Arch rootfs tarball over
   the USB-ECM (`usb0`, 10.42.0.1/10.42.0.2) link at runtime, via both `wget`
   and raw `nc`. Both independently stalled at ~200MB with zero throughput for
   5+ minutes — a reliability wall in the USB gadget link for large transfers,
   not a tuning problem (MTU matched at 1500 both ends). **Fix:** abandoned
   runtime network transfer; instead extended the existing kexec staging
   mechanism (which already reliably moves the kernel Image into RAM via
   `adb push`) to also carry the rootfs, bundled inside the initramfs.
2. Trimmed the stock 2.1GB Arch rootfs to 607MB by deleting `/usr/lib/firmware`
   (960M, generic device firmware — irrelevant, we use our own kernel/drivers),
   `/boot` and `/usr/lib/modules` (193M+188M, Arch's own kernel/modules —
   irrelevant, we boot via our own kexec kernel), plus man/doc/extra locales.
3. `module/loader.c`'s initramfs cap (`SZ_2M`) and total staging window
   (`QKX_LO`/`QKX_HI`, originally `0x90000000`-`0x93400000`) were both far too
   small for a bundled rootfs. Raised `QKX_INITRD_MAX` to `0xc800000` (200M).
   Also had to **move `QKX_LO`** from `0x90000000` to `0x8bf00000`: the old
   base only has ~186M of truly clear room before the `ramoops` pstore
   carveout at `0x9ba00000` on this Quest 2 (the free "System RAM" line in
   `/proc/iomem` has several nested reserved sub-ranges — ramoops, cont_splash,
   the running kernel's own code/data — that a naive outer-bound check misses).
   `0x8bf00000` is the start of the same captured System RAM extent and gives a
   clean 251M run. Also raised `QKX_MAX_PAGES` from 32768 (128M) to 150000
   (~585M) to cover the bigger staging budget. Updated `tools/prepare.py`'s
   matching size/address checks to stay in sync.
2. First boot attempt with the bigger initramfs (bundled as a `.tar.gz`,
   extracted via `tar xzf` at init time) got far enough to start "Trying to
   unpack rootfs image as initramfs..." and continue through normal driver
   probing, but the kernel patches' own recovery-watchdog (`QKX: watchdog PID 1
   stack follows`, ~30s budget) caught `PID 1` in state `R` (running, not
   blocked) and force-reset — the cpio unpack of a rootfs with ~15,000
   individual files takes longer than that budget, even though nothing was
   actually stuck. **Fix:** package the rootfs as a **squashfs image**
   (`mksquashfs`, xz, 139MB) instead of a tarball, bundle *that single file*
   inside the initramfs, and `mount -t squashfs -o loop` it at boot instead of
   extracting — one mount syscall regardless of file count, independent of how
   many files the rootfs contains. Needed `CONFIG_SQUASHFS=y` added to the
   target kernel config (loop support was already enabled). Fixed the boot
   time problem completely (back to ~6s to a reachable shell).

New build-only tooling added (not part of upstream quest-kexec, local to this
checkout): `build/init-arch.sh` (modified `initramfs/init.sh` that mounts the
bundled squashfs and bind-mounts `/dev`,`/proc`,`/sys` into it) and
`build/build-initramfs-arch.py` (packs BusyBox + that init + the squashfs image
into a cpio, since upstream's `initramfs/build.py --extra` only accepts static
ELF binaries, not arbitrary files).

Known limitation: the mounted rootfs is **read-only** (squashfs); `/tmp` and
`/var/tmp` are tmpfs overlays, but most of the tree (including `/etc`) can't be
written to as-is. Fine for an interactive shell, not yet for `pacman -Syu` or
persisting changes.

## Progress log (cont.): networking + pacman fully working

**2026-10-10, later same session.** Goal: make the Arch environment actually
usable — writable filesystem, real internet access, working package manager.
All runtime setup below is done live over telnet after boot (not baked into
the initramfs), so it has to be redone after every reboot/re-kexec:

1. **Writable overlay.** squashfs is read-only; mounted an OverlayFS on top
   (`lowerdir=/mnt/arch`, upper+work on a tmpfs) at `/mnt/archrw`, bind-mounted
   `dev`/`proc`/`sys`, tmpfs on `/tmp`. Use `/mnt/archrw` as the chroot target
   from here on, not the read-only `/mnt/arch`.
2. **Real internet access.** `usb0` only had a local-subnet route by default
   (`ip route add default via 10.42.0.1 dev usb0` needed). On the Mac side,
   NAT needed two fixes: (a) macOS's `pf` firewall was globally **disabled**
   (`pfctl -s info` showed `Status: Disabled`) — loading a NAT rule into an
   anchor does nothing until `pf` itself is enabled (`sudo pfctl -e`); (b)
   loading the rule into a **named anchor** (`pfctl -a NAME -f -`) doesn't
   activate it either unless the main ruleset references that anchor, which
   it doesn't by default — had to load directly into the main ruleset instead
   (`echo "nat on en0 from 10.42.0.0/24 to any -> (en0)" | sudo pfctl -f -`).
3. **DNS "failures" were actually IPv6.** `archlinuxarm.org` resolved fine via
   `getent hosts`, but `curl` (no `-4`) tried the AAAA record first and our
   NAT rule is IPv4-only, so it silently failed. `curl -4` confirmed DNS was
   never actually broken.
4. **HTTPS failing even with `-4`:** `curl: SSL certificate ... not yet valid
   or the system clock is incorrect`. The target kernel boots with no
   RTC/NTP, defaulting to near the Unix epoch — any real-world cert's
   "not-before" date looks like it's in the future. Fixed with
   `date -u -s "<current UTC time>"` (manual, from the host's clock; no NTP
   client set up). Plain HTTP had worked the whole time since it does no
   cert validation, which is what made this one non-obvious.
5. **`pacman` itself needed two fixes**, both inherent to running a
   rolling-release Arch's current tooling against a fixed 4.19 kernel:
   - `error: restricting filesystem access failed because Landlock is not
     supported by the kernel!` — Landlock (Linux 5.13+) doesn't exist in
     4.19 at all, not a missing config option. Fixed with pacman's
     `--disable-sandbox` flag every invocation.
   - Fresh image has no initialized GPG keyring (`keyring is not writable` /
     `required key missing`) — expected first-boot step, not a bug. Fixed
     with `pacman-key --init && pacman-key --populate archlinuxarm`.

Result: `pacman -Sy --disable-sandbox` and
`pacman -S --disable-sandbox --noconfirm fastfetch` both completed cleanly
end to end (real network fetch, signature verification, install). Confirmed
via `fastfetch` itself reporting `Disk (/): ... - overlay` (writable) and
167 packages (165 base + fastfetch + its yyjson dependency).

One disconnect happened mid-session (device unreachable for 40+s, longer than
any of the earlier crash/reboot cycles which always recovered within ~36s) —
manual power-button restart recovered it cleanly back to stock Android, same
as every other reset tonight. Cause undetermined; didn't reproduce on retry
with the exact same commands after a fresh kexec boot (all runtime
overlay/routing/keyring state has to be redone after every reboot, see above).

**Open item for next session:** bake this runtime setup (overlay mount,
routing, resolv.conf, clock, keyring) into `init-arch.sh` / a first-boot
script instead of redoing it by hand over telnet every time.

## Progress log (cont.): the real Holo Core, booted

**2026-10-10, same session.** Goal: boot Valve/Collabora's actual Holo Core
(not just plain Arch Linux ARM), now that networking/pacman both work.

Found via web research: Holo Core publishes a real, downloadable preview —
sources at `gitlab.steamos.cloud/holo/holo-core-aarch64-preview`, binaries at
`holo-packages.steamos.cloud/holo-core-aarch64-preview/<mash-build>/`, which
directly serves a `pacman.conf`, `pacman.mirrorlist`, **and a prebuilt
`system.rootfs.zst`** (a real rootfs tarball, zstd-compressed, build
`mash-20251118.3` / `BUILD_ID=251368` at time of writing). No need to build
anything from source — same approach as plain Arch Linux ARM, just a
different source tarball:

1. Downloaded `system.rootfs.zst` (385M compressed, 1.28G uncompressed tar).
   Contains Holo Core's own kernel (`6.17.8-arch1-1`, built for Steam Frame's
   Snapdragon 8 Gen 3) — irrelevant, same as before, trimmed it out.
2. Trimmed the same way as plain Arch (`/usr/lib/modules`, `/boot`, man/doc/
   info, extra locales) **plus** `guile`/`gconv`/`perl5` (this image is a
   fuller base-devel-flavored build with gcc/python/git included, which is
   worth keeping — the extra libs weren't): 1.3G → 829M uncompressed.
3. Packed as squashfs (xz): **194.83 MiB**, just barely inside the existing
   200M `QKX_INITRD_MAX` budget (no address/loader.c changes needed this
   time — reused the exact same staging window as the Arch Linux ARM boot).
4. **Holo Core's rootfs ships without empty `/dev`, `/proc`, `/sys`
   directories** (unlike plain Arch Linux ARM, which does) — presumably
   because it expects systemd-tmpfiles to create them, which we're not
   running. The init script's `mount --bind` calls silently had nothing to
   bind onto. Fixed the same way as the writable-overlay need: mount the
   same OverlayFS trick, `mkdir` the three directories into the writable
   upper layer, *then* bind-mount.

Result: booted cleanly, `/etc/os-release` confirms
`NAME="Holo core Aarch64 port (preview)"`, `ID=holo-core`,
`VERSION_ID=mash-20251118.3`, `BUILD_ID=251368`. Same runtime setup as the
Arch boot (overlay, route, DNS, clock) applies here too, plus the
dev/proc/sys `mkdir` fix above. `fastfetch` run inside confirms OS line.

## Progress log (cont.): display/GPU — real hardware response, not yet locked

**2026-10-10, same session.** Goal: see whether the Quest 2's actual display
pipeline (DSI panel `qcom,mdss_dsi_jdi_4k_120Hz_dsc_video`, Adreno GPU) does
anything at all when driven from our kexec'd kernel. Flagged going in as a
fundamentally harder problem than CPU/boot bring-up — this is as far as
non-destructive diagnosis could tell without actually trying.

**Display driver is already built in** — no kernel config changes needed.
`techpack/display`'s Kconfig isn't gated through the normal `.config`
mechanism at all; `techpack/display/Makefile` includes
`techpack/display/config/konadisp.conf` (which does `export CONFIG_DRM_MSM=y`
etc.) whenever `CONFIG_ARCH_KONA=y`, bypassing Kconfig entirely. This is why
grepping `target.config` for `SDE`/`MDSS`/`DRM_MSM` finds nothing even on the
*live Android* config — those symbols are injected at build time, not
persisted to `.config`.

**First real boot test**, on the already-working kernel (no rebuild needed):
probing alone got remarkably far — MDSS core GDSC, both DSI PLLs, both DSI
PHYs, both DSI controllers all probed successfully, SDE RSC probed, and
critically: `Successfully bind display panel
'qcom,mdss_dsi_jdi_4k_120Hz_dsc_video'` — the exact real panel. SMMU contexts
for display (`mdp_ns`, `mdp_s`) created without the fault storms seen with
GENI. `[drm] Initialized msm_drm 1.3.0 ... on minor 0` — full DRM init,
`/dev/dri/card0` and `/dev/dri/renderD128` both exist. `cont_splash feature
not enabled` — does a clean cold init rather than assuming bootloader splash
continuity, which is correct for a kexec'd kernel.

One benign `clk_disable_unused` `WARN_ON` for `cam_cc_csiphy2/3_clk` ("status
stuck at 'on'") — stale camera clock state inherited across kexec, same class
of issue as the ION/GENI stale-state problems, but a non-fatal warning (not a
panic) and irrelevant to display.

**Tooling detour (not a hardware finding):** tried to install `modetest`
(part of `libdrm` in this repo, unusually) via `pacman` to test an actual
modeset. Got the environment into a genuinely broken state — multiple
concurrent `pacman` processes ended up running against the same local
database because closing our telnet relay connection doesn't kill remote
processes (same lesson learned earlier with the stalled `wget`; didn't apply
it here), which corrupted the local package DB and left processes in a
confusing state that even `kill -9` + `/proc/<pid>/status` couldn't make
sense of. Root cause: operator error (me) firing overlapping commands, not a
pacman or kernel issue. Abandoned this path.

**Clean path instead: wrote a minimal standalone KMS test, no package
manager, no libdrm.** `drm-test/kms_test.c` — raw ioctls only, using the
kernel's own UAPI headers (`include/uapi/drm/drm.h`, `drm_mode.h`) copied
directly from the kernel source tree, `#define __user` to compile them
outside the kernel tree, fully statically linked
(`clang --target=aarch64-linux-gnu -fuse-ld=lld -static`). No chroot, no
Arch/Holo rootfs needed — runs directly from the plain BusyBox environment
since `/dev/dri/*` is populated by devtmpfs regardless of userland. Enumerates
connectors ignoring "connected" status text (embedded DSI panels don't do
EDID hotplug detection the way external displays do), picks the first
connector with any modes, creates a dumb buffer, fills it solid color, calls
`SETCRTC`.

Result: **`SETCRTC` returned success.** Connector reports `conn_status=1`
(actually `DRM_MODE_CONNECTED`, not "unknown" as `modetest` had shown earlier
— that was a display-string quirk in modetest's own status mapping, not
reality). Picked mode `1920x3664@120Hz` (one eye, matches the panel's known
120Hz capability). 28MB dumb buffer allocated and filled with `0xffff2020`.

**On the actual headset: both eye panels flashed white repeatedly** (not the
solid red-orange we set) during the 15-second hold. dmesg explains exactly
why: `[drm:sde_encoder_phys_vid_wait_for_active] [sde error] enc28 intf1 line
count is less. line_cnt = 3686`, repeated ~20 times within 3 milliseconds —
the SDE video-mode timing engine is retrying and failing to lock a stable
active/sync state with the mode we requested, over and over (each retry =
one visible flash, matching "many flashes" exactly).

**Leading theory, untested:** this panel uses DSC (Display Stream
Compression) — `SETCRTC` alone sets basic resolution/timing but likely
doesn't configure DSC-specific parameters (slice width/height, compression
ratio/bpp target) that this panel's video-mode timing generator needs to
actually achieve lock. A bare legacy `SETCRTC` may be the wrong tool here;
the stock Android driver likely drives this through SDE's atomic/DSC-aware
commit path with additional panel-specific properties set, which raw
`drm_mode_crtc` doesn't expose.

**Bottom line: real physical hardware response (not just a software-layer
success report), on the very first attempt, with a precisely identified
failure mode.** This is a genuinely strong result for one session — went
from "will display even probe at all?" to "panel powers on, attempts sync,
fails in a specific, named, well-understood way" without needing any new
kernel patches. Next step (not attempted): either a DSC-aware userspace
tool/ioctl sequence, or inspect whether `techpack/display`'s own SDE DSC
property definitions can be set via `DRM_IOCTL_MODE_OBJ_SETPROPERTY` before
`SETCRTC` rather than needing a full atomic commit path.

Artifacts: `drm-test/kms_test.c` + headers, statically-linked `kms_test`
binary — survives reboots unchanged, can be reused directly on any future
boot via the same `wget`-into-`/tmp` approach used for `fastfetch`.

## Progress log (cont.): atomic commit + debugfs — asymmetric DSI interfaces

**2026-10-10, continued.** Switched from the legacy `SETCRTC` path to a
proper atomic commit (`drm-test/kms_atomic.c`), since Android's own
compositor always uses atomic and the legacy path may be a thin,
rarely-tested shim in this downstream SDE driver.

Needed `DRM_IOCTL_SET_CLIENT_CAP` for both `DRM_CLIENT_CAP_UNIVERSAL_PLANES`
and `DRM_CLIENT_CAP_ATOMIC` before any property actually showed up via
`DRM_IOCTL_MODE_OBJ_GETPROPERTIES` — without declaring these, every object
(CRTC/connector/plane) reported zero properties, and two primary planes
(58, 80) weren't even enumerable via `GETPLANERESOURCES`.

With that fixed, the atomic commit **succeeded** (all 13 properties across
CRTC/connector/plane resolved, no errors) — but the same symptom as the
legacy path: both eye panels flashed repeatedly rather than showing a
stable fill. dmesg showed `dsi-ctrl-0`/`dsi-ctrl-1` FIFO overflow *and*
underflow errors on both controllers, plus a `dsi_ctrl_dma_cmd_wait_for_done`
command-transfer failure — consistent with the SDE hardware DSC
encoder not being correctly engaged (our framebuffer is raw/uncompressed,
the panel expects ~3:1 DSC-compressed data per the DT's
`qcom,mdss-dsc-bit-per-pixel=8`).

**Traced the actual DSC enable code path** in
`techpack/display/msm/sde/sde_encoder.c` (`_sde_encoder_dsc_pipe_cfg`,
calls `sde_hw_dsc_config()` in `sde_hw_dsc.c`, pure MMIO register writes
from a software-computed `msm_display_dsc_info` struct — not dependent on
inherited hardware state at that layer). Confirmed `SDE_ERROR` maps to
`pr_err` (`sde_kms.h`), so it does reach dmesg — and we saw none of the
"invalid params for DSC" early-return messages this path can produce,
meaning the DSC hardware block *was* found/allocated and `dsc_config()`
*did* run with real parameters. Rules out a resource-manager allocation
failure as the cause.

**Enabled `CONFIG_DEBUG_FS`** (wasn't set at all) and rebuilt, to read live
driver state via `/sys/kernel/debug/dri/0/` instead of guessing further from
source. Found a rich debugfs tree (`crtc129/status`, `crtc129/state`,
`encoder28/status`, raw `sde_reg`/`dsi0_ctrl_reg` register dumps, etc.).

**Key finding, mid-commit (not post-failure rollback):** `crtc129/status`
showed a fully correct configuration — 1920x3664 split across two layer
mixers (960 each, matching the dual-LM topology), plane 58 bound with the
right framebuffer/format/pitch, vblank actively counting (fps~239,
i.e. ~2x120Hz, plausible for a dual-pixel-per-clock panel). **But
`encoder28/status` showed `intf:1 vsync:2288 underrun:4` (actively
scanning, recovered, low error count) vs `intf:2 vsync:0 underrun:4`
(never started a single vsync)** — a genuine asymmetry between the two
DSI interfaces, not a uniform failure. This reframes the problem: it's not
necessarily "DSC is broken everywhere," it's "one of the two DSI
interfaces (intf2) never activates," which is a narrower, more specific
target than before.

**Unresolved at end of session:** relaunched the atomic test to have the
user watch each eye individually during the live run and confirm the
intf1-working/intf2-dead asymmetry visually, but the session's usage limit
was hit immediately after relaunching, before any observation came back.
**What the user actually saw on that run was never recorded — do not
assume either outcome.** The device also went unreachable during the
pause (likely idle/USB sleep) and needed a physical reconnect + fresh
kexec boot to continue.

**Next step:** re-run `kms_atomic` (binary already on `/tmp` from the prior
boot, or re-`wget` from the Mac's `python -m http.server 8000` if that's
still running — check first) with a human actually watching both eyes
live, then if `intf2` really never activates, look at whatever in the DSI
dual-interface enable sequence (`dsi_display.c`/`sde_encoder.c`'s
multi-intf setup, possibly a second `intf_mode`/`master`-`slave` wiring
step) differs between the two interfaces — a narrower, more promising lead
than the DSC-register-level dead end this was heading toward before.

## Progress log (cont.): mmap'd framebuffer hang — likely stale hyp-secure state

**2026-10-10, next day, same investigation.** Re-ran `kms_atomic` to get a
live visual observation (previous attempt's outcome was never recorded —
session's usage limit hit right as it launched). This run **hung for real**,
a different failure mode than the earlier "flashes then recovers" pattern:

`ps` showed nothing (looked like the process had vanished), but dmesg told
the real story — PID 511 (`kms_atomic`) was stuck in the kernel,
continuously looping in `handle_mm_fault → do_page_fault →
do_translation_fault` from at least t=44s to t=170s+ (120+ seconds), right
after allocating and mapping the dumb framebuffer (output stopped exactly
after the `fb_id=` print, before the "framebuffer filled" line — i.e. it
hung on the very first write touch into the mmap'd buffer).

`kill -9` had no effect (expected: stuck in-kernel servicing a fault that
never resolves, not interruptible by a signal). The rest of the system
stayed responsive the whole time (telnet commands kept working), and the
project's own built-in warm-reset (`echo r > /proc/qkx_bootdone`) **still
recovered cleanly** back to stock Android in ~6s, even with that one thread
permanently wedged — a good sign for the recovery mechanism's robustness,
independent of this specific bug.

**Leading theory:** this is the display driver's own dumb-buffer allocator
(CMA/GEM-based, independent of ION, which is already disabled) hitting the
same class of problem the project's own code comments warned about for ION:
"stock hyp-assigns secure ION pages to other VMs; they stay locked after
kexec and any access from the next kernel hangs the CPU." Android was
actively rendering the UI (with real framebuffers in active use) right up
until the kexec jump; if the allocator handed us physical pages that are
still hyp-assigned/secure-locked from that prior use, touching them from
our fresh kernel would hang exactly like this — unresolvable page fault,
not a clean error.

This did **not** happen on the earlier successful runs (which got as far as
"framebuffer filled" and "ATOMIC commit succeeded" every time) — so it's
allocation-dependent (which physical pages the dumb-buffer ioctl happens to
hand back), not a deterministic bug. A retry might get lucky; doesn't mean
the underlying problem is fixed.

**Next step, not yet tried:** before mmap'ing and writing into the dumb
buffer, check what physical address it landed at (there's no direct ioctl
for this, but `CREATE_DUMB`'s handle could be cross-referenced via debugfs,
or just add a guard: try a `pread`/small test write of 1 byte via
`pwrite(fd, ..., map.offset)` instead of `mmap`, to see if the fault is
mmap-specific or happens on any kernel-mediated access of that memory
too). If this really is hyp-locked memory, the proper fix is likely the
same shape as the ION workaround the project itself has
(`module/ion_secmap.c` — reserves/marks stuck secure-locked physical ranges
as no-map in the DTB before jumping) but for whatever memory pool the SDE
dumb-buffer allocator draws from, which may or may not be the same pool.

## Progress log (cont.): live logging set up, intf2 confirmed never engaged

**2026-10-10, next session.** User asked for better crash/hang forensics
("extra logging... so next time we can fix it for stability"). The project
already ships exactly the right tool: `tools/qkx_follow.py` streams the
target's kernel log live over telnet to a local host file with **zero
buffering** (`open(..., "ab", buffering=0)`), so it survives a hang or
disconnect with no data loss — far better than polling `dmesg` after the
fact, and *much* better than relying on `pstore`/`console-ramoops`, which
has a fixed-size ring buffer that can get overwritten by Android's own
verbose boot logging in the time between recovery and reading it (this
genuinely happened earlier — a crash trace that was readable in full on one
pull came back truncated after a slower pull later). Usage: run
`qkx_follow.py <logfile>` in the background on the Mac before launching
anything on the device; it just works over the existing `10.42.0.2:23`
telnet link via `qkx-klogwatch` (already present in every boot, this
project's own tool, not something we added).

**Boot-time flakiness is confirmed probabilistic, not deterministic.**
Tried disabling `CONFIG_FTRACE`/`TRACING`/`EVENT_TRACING` (suspecting the
tracefs event-directory creation storm, from enabling `CONFIG_DEBUG_FS` last
session, was causing early-boot RCU stalls / `QKX watchdog PID 1` resets) —
this broke the build outright (`rpmh-rsc.c` has a genuine source-level
dependency on `CONFIG_TRACING` via `CREATE_TRACE_POINTS`/trace-event headers,
not just a Kconfig menu dependency; disabling it produces malformed
`MODULE_LICENSE`/`MODULE_DESCRIPTION` macro expansions and fails to compile).
Reverted (debugfs stayed on, tracing/ftrace back on — this is the same
config that worked fine last session). The **exact same reverted,
previously-working config then failed on its very next boot** (fresh RCU
stall, different/truncated stuck trace), **then succeeded on an immediate
retry** with zero changes. Conclusion: this is inherent probabilistic
instability in the kexec transition's early-boot timing (plausibly from the
same class of inherited-stale-hardware-state issues seen all along — camera
clocks, GENI, ION, display hyp-lock, each contributing nondeterministic
timing variance), not something tied to any specific config we've touched.
Retrying once or twice has reliably worked every time so far.

**With live logging running, re-ran `kms_atomic` twice** (debugfs mounted,
`qkx_follow.py` capturing from before launch):
- Run 1: hung for real inside `handle_mm_fault` on the mmap'd dumb buffer
  (same as the previous session's finding — allocation-dependent, see the
  "mmap'd framebuffer hang" section above). Recovered via the normal
  `qkx_bootdone` warm reset.
- Run 2 (fresh boot, retry): **completed cleanly** — `ATOMIC commit
  succeeded`, `done.`, no hang. Confirms the mmap hang really is
  allocation-dependent (which physical pages `CREATE_DUMB` happens to hand
  back), not deterministic.

**New, sharper finding from Run 2's debugfs + live log, reproduced
consistently across multiple runs now:** `encoder28/status` shows
`intf:1 vsync:3714 underrun:28` (actively scanning, recovered, roughly the
same low error rate as before) vs **`intf:2 vsync:0`** (zero, every time).
Searched the full zero-loss live log for any mention of `intf2` at all:
**zero hits.** Every `[sde error]` line in the entire captured log says
`enc28 intf1` — `intf2` is never referenced, not even in a failing attempt.
This means intf2 isn't failing a check, it's **never being engaged in the
first place**. Since the panel's DT reports
`qcom,display-topology = <0x02 0x02 0x02>` (2 layer mixers, 2 encoders, 2
interfaces — both required for a complete image), driving only intf1 would
explain why nothing visible appears even though intf1 itself eventually
looks "fine" on its own.

**Next step:** look at `sde_encoder.c`'s handling of multi-physical-encoder
topologies (this panel's `encoder28` is presumably a composite encoder
wrapping two physical sub-encoders, one per `intf`) to find why only the
first physical encoder's `enable()` actually runs. Likely candidates: a
master/slave physical-encoder concept where something about our specific
atomic commit (missing a property, or an ordering issue) only triggers the
master's path.

## Progress log (cont.): topology confirmed correct, narrowed to slave-intf hardware

**2026-10-10, same session, continued.** Added a temporary diagnostic
(`SDE_ERROR`-based, so it's guaranteed to reach dmesg — `SDE_DEBUG` uses
`pr_debug`/`DRM_DEBUG`, neither visible without dynamic-debug config) in
`sde_rm.c`'s `_sde_rm_populate_requirements()`, rebuilt, re-ran `kms_atomic`.

**Result: `QKXDBG: top=4 num_lm=2 num_comp_enc=2 num_intf=2 mode=1920x3664`.**
The resource manager correctly computes a dual-interface topology
requirement for our commit. This disproves the earlier theory that our
hand-built `MODE_ID` blob (a raw copy of the public `drm_mode_modeinfo`
struct) was losing private per-mode topology metadata and falling back to
a single-interface default — it isn't. The request is right.

**Also: `encoder28`'s two physical encoders genuinely both exist.** Traced
`sde_encoder_virt_add_phys_encs()`/`sde_encoder_setup_display()`: physical
encoders are created once, at probe time, in a loop over
`disp_info->num_of_h_tiles` (the panel's own dual-DSI hardware topology,
independent of any specific commit). Separately and more directly: the
`encoder28/status` debugfs file has always shown **two separate rows**
(`intf:1` and `intf:2`), each with its own tracked vsync/underrun counters
— if there were only one physical encoder object, there would be no
`intf:2` row to print at all. So `num_phys_encs == 2` is settled, not
theoretical.

**Checked `sde_encoder_phys_vid_enable()` (`sde_encoder_phys_vid.c`) for a
master/slave ordering bug** (the encoder's enable loop calls the
non-master/slave's `.enable()` *inside* a loop, then the master's
separately, *afterward* — a plausible place for a master-must-go-first
hardware dependency to break the slave). Found none: the function
independently sets up its own timing engine
(`sde_encoder_phys_vid_setup_timing_engine`) and ctl flush bits per
physical encoder, with no read of master-side state. Critically, this
function's own `hw_intf`/`hw_ctl`/`hw_pp`-null guard
(`SDE_ERROR("invalid hw_intf %d hw_ctl %d hw_pp %d\n", ...)`, `SDE_ERROR`
*is* `pr_err`, always visible) **never fired** in any captured log — so the
slave physical encoder's hardware block binding is valid, not null, and its
`enable()` function runs the same code path as the master's, without any
visible early return.

**Where this leaves things:** topology request ✓, both physical encoder
objects exist ✓, hardware block binding (`hw_intf`) valid for both ✓, no
software-visible bug in the per-encoder enable logic — yet the slave
interface (`intf2`, consistently, across every run) produces zero vsync
ever, while the master (`intf1`) eventually stabilizes after an initial
flash/retry period. This is now a hardware-register-state question (e.g.
a shared clock/GDSC voted only once instead of per-controller, or a
DSI command-mode init sequence that only reaches one of the two
controllers due to inherited stale "already initialized" state from
Android's pre-kexec session — same general class as every other
stale-state issue this session, just one level deeper than source reading
alone can resolve). Next step would need per-register tracing (the
`sde_dbgbus`/`dsi0_ctrl_reg`/`dsi1_ctrl_reg` raw register-dump debugfs
files already found, or comparing GDSC/clock enable state between the two
DSI controllers via `regulator` debugfs) rather than more driver source
reading.

**On stability/logging (separate from the display investigation):**
confirmed `tools/qkx_follow.py` (zero-buffered live kernel-log streaming
over the existing telnet link) is the right tool for capturing hangs in
real time — much better than `pstore`, which has a size-limited ring
buffer that visibly got overwritten by Android's own verbose boot logging
between a crash and reading it. Also confirmed via two more boot attempts
that the occasional early-boot RCU-stall/`QKX watchdog PID 1` reset is
genuinely probabilistic (identical config failed once, then succeeded
immediately on retry with zero changes) — not tied to any specific config
change made this session. A retry has reliably resolved it every time so
far; no fix identified or needed beyond "retry once."

## Progress log (cont.): register-level dig, one real finding, one red herring

**2026-10-10, same session, continued further.** Enabled
`CONFIG_DYNAMIC_DEBUG` (needed for write access to the SDE debugfs register
files — `sde_reg_fops.write` is gated behind it) and rebuilt, to directly
probe/poke `dsi0_ctrl_reg`/`dsi1_ctrl_reg` (the two DSI protocol
controllers, offsets defined in `dsi_ctrl_reg.h`: `0x04=DSI_CTRL`,
`0x08=DSI_STATUS`, `0x0C=DSI_FIFO_STATUS`).

**Correction to the previous entry's finding — important, so this isn't
chased again:** compared `dsi0`/`dsi1` registers at three points: (1)
immediately at boot, before any commit — **completely identical**,
`DSI_FIFO_STATUS=0x11111010` (clean) on both, ruling out any pre-existing
inherited stale-state difference between the two controllers. (2) during
an active commit, shortly after launch — `dsi1` showed an elevated FIFO
status (`0x55551011`), but **so did `dsi0`** this time (`0xdddd1011`,
a different but also-elevated pattern) — both controllers show *some*
transient startup anomaly, not just the slave. (3) ~3 seconds later,
**both had converged back to the identical clean `0x11111010`** — yet
`encoder28/status` *still* showed `intf:2 vsync:0` while `intf:1` kept
counting normally. So the DSI-protocol-layer FIFO status, whatever caused
it, is not what's actually gating whether an interface scans out — it
self-clears on both sides regardless of which one ends up working. The
earlier write-up treating this as "the" root cause was premature.

**Corrected understanding:** `dsi0_ctrl_reg`/`dsi1_ctrl_reg` are the MIPI
DSI protocol/lane-level controller (`dsi_ctrl_hw_1_4.c`). The actual
vsync/active-region tracking that's failing
(`sde_encoder_phys_vid_wait_for_active`'s `get_line_count()` calls) reads
from a *different* hardware block — the SDE INTF timing-generator
(`sde_hw_intf.c`), which lives in the much larger general `sde_reg`/`sde_off`
debugfs dump, not the DSI-specific files. Hadn't actually separated these
two hardware blocks before this point. **The real next step is reading the
INTF block's own registers specifically** (need the INTF block's base
offset within the `sde_reg` dump — `sde_hw_catalog.c`/`sde_hw_intf.c` would
have the offset map) and comparing INTF0 vs INTF1 state the same way, not
more DSI-controller-level comparison.

**Session reality check:** this has been a very long, many-hour,
many-rebuild-cycle investigation. Each new theory has been genuinely
falsifiable and several *were* falsified by direct measurement (blob
metadata loss — disproven; topology under-request — disproven; DSI FIFO
status as root cause — disproven just now), which is real progress in the
sense of eliminating wrong paths, even though the actual root cause remains
open. Confirmed again: early-boot crash/reset rate seems to have gone up
slightly with `CONFIG_DYNAMIC_DEBUG` enabled (2 failures before a success,
vs the previous pattern of 1-then-success) — not confirmed as a real
effect with only 3 data points, but worth tracking if it keeps happening.

## Progress log (cont.): topology fully confirmed correct; INTF register attempt unreliable

**2026-10-10, same session, continued further still.** Decoded the
`top=4` value from the earlier `QKXDBG` topology printk:
`enum sde_rm_topology_name` in `sde_rm.h` has
`NONE=0, SINGLEPIPE=1, SINGLEPIPE_DSC=2, DUALPIPE=3, DUALPIPE_DSC=4` — so
`top=4` is **`SDE_RM_TOPOLOGY_DUALPIPE_DSC`, the exactly correct topology**
for this dual-interface DSC panel. Topology selection is not the problem,
full stop.

**Tried reading the SDE INTF hardware block's own registers directly** (a
different block from the DSI protocol controller checked earlier — INTF is
`sde_hw_intf.c`, found via the captured DT's
`qcom,sde-intf-off = <0x6b000 0x6b800 0x6c000 0x6c800>` with
`qcom,sde-intf-type = "dp","dsi","dsi","dp"`, meaning our two DSI
interfaces are at relative offsets `0x6b800` (INTF_1) and `0x6c000`
(INTF_2) within the `sde_reg`/`sde_off` seekable debugfs register dump —
`sde_off` takes `<hex-offset> <hex-length>` to set the read window, found
by reading `sde_dbg_reg_base_offset_write()`'s `sscanf` format directly;
needed `CONFIG_DYNAMIC_DEBUG` enabled first, since `sde_reg_fops.write` is
gated behind it).

**This did not produce a reliable finding.** Both INTF1 and INTF2 dumps
came back byte-for-byte identical every time, including a register
(`INTF_VSYNC_PERIOD_F0`, offset `0x00C`, should hold
`vsync_period * hsync_period` — a large nonzero value per
`sde_hw_intf.c`'s own timing-setup code) reading `0x00000000` on **both**,
even while `encoder28/status` showed `intf:1` actively counting vsync in
the thousands. A real, configured, actively-scanning interface should not
have a zero vsync-period register. This strongly suggests the offset math
itself is wrong (most likely: `sde_kms->mmio`'s registered `sde_reg` base
may not align 1:1 with `qcom,sde-intf-off`'s DT values the way assumed, or
there's a per-sub-block base adjustment inside `sde_hw_intf.c` not
accounted for) rather than a real hardware finding. **Do not trust this
0x6b800/0x6c000 register comparison** — it needs the offset math
independently re-derived (e.g. from `sde_hw_intf_init()`'s own
`iomem + blk_off` calculation) before drawing any conclusion from it.

**Where this leaves the investigation, honestly:** we've now ruled out,
with real evidence, every software-level explanation that was cheap to
check: topology request (confirmed correct), physical encoder existence
and hardware-block binding (confirmed valid, no null-check errors),
encoder enable-loop logic (no master/slave ordering bug found in source),
DSI protocol-controller FIFO status (real effect, but self-clearing on
both sides regardless of outcome — not the gate). What's left is lower in
the stack than source-reading or casual debugfs-poking can reliably reach:
either a per-interface clock that's enabled in software but not actually
locked in hardware, or a power-domain/GDSC difference between the two
DSI paths. The right tool for that is `CONFIG_COMMON_CLK_DEBUG`'s
`/sys/kernel/debug/clk/` tree (per-clock `enable_count`/`rate` files,
genuinely reliable since it's maintained by the common clock framework
itself, not something requiring reverse-engineered offsets) — not yet
tried, and the natural next step if this is picked up again.

## Known limitation: only tested with `execute=1` success twice in a row

Hardware-destructive-shutdown paths (`watchdog_disable` without
`preserve_watchdog`, `disconnect_qmp=1`) were never exercised — only the
exact param set above was validated end-to-end.

## 2026-10-10, continued: CONFIG_COMMON_CLK_DEBUG results — real lead found, then a redeploy-stability wall

Enabled `CONFIG_COMMON_CLK_DEBUG=y`, rebuilt, deployed. `/sys/kernel/debug/clk/`
confirmed the lead from the previous session was right:

- With `kms_atomic`'s commit held open, `dsi0pll_vco_clk` and its entire
  derived tree (`dsi0pll_byteclk_src`, `dsi0pll_pclk_src`,
  `dsi0_phy_pll_out_byteclk`/`_dsiclk`) show real locked rates: VCO
  1146493440 Hz → byteclk 143311680 Hz, pclk 191082240 Hz.
- `dsi1pll_vco_clk` and **every single node under it** read rate=**0**,
  both via individual `clk_rate` files and the `clk_summary` table. Checked
  across three independent commits (not just one) — consistently zero.
  This is a solid, repeatable finding, not noise.

Added a temporary `DSI_ERR` printk at the top of `dsi_link_hs_clk_set_rate()`
(`techpack/display/msm/dsi/dsi_clk_manager.c`, the function that actually
calls `clk_set_rate()` on `byte_clk`/`pixel_clk` for each DSI controller
index) to see what's happening for idx=1 specifically. First attempt used
`%llu` for `byte_clk_rate`/`pix_clk_rate`, but both fields are **`u32`** in
`struct link_clk_freq` (`dsi_clk.h`) — reading them with `%llu` pulled in 4
extra garbage bytes from adjacent register/stack state (classic
format-string width mismatch, not a kernel bug). **Any numeric value from
that first printk run is untrustworthy — ignore it if this gets picked up
again.** Fixed to `%u` in a second pass.

What IS trustworthy from that first (buggy-format) run, because the
pointer/bool args were correctly matched: the function **does get called
for both idx=0 and idx=1**, with `cont_splash=0` for both (ruling out the
"still in continuous-splash handoff" theory — the early
`if (mngr->is_cont_splash_enabled) return 0;` guard is NOT the gate), and
with **distinct, non-null** `byte_clk`/`pixel_clk` pointers for each index
(so it's not a case of both indices aliasing the same clk object). No
`clk_set_rate failed for byte_clk/pixel_clk` error ever appears in dmesg
for either index, meaning the common clock framework reports success for
whatever rate was requested on idx=1, yet the resulting hardware rate is
still 0. **This is the real mystery now**: a reported-successful
`clk_set_rate()` call that doesn't produce a locked PLL, specifically on
the second DSI link's clock only.

Also added a `clk_get_rate()` readback right after the `pixel_clk`
`clk_set_rate()` call in the same function, to directly compare requested
vs. achieved rate per index in one pass. **This is suspected of causing a
hard crash** — see below — and was reverted before being tested
successfully. If picked back up: re-add it, but first check whether
`dsi_link_hs_clk_set_rate()` runs under any lock/context where
`clk_get_rate()` (which takes `clk_prepare_lock` and can sleep) would be
unsafe.

### New blocker: three consecutive hard-crash redeploys after the %u fix

After fixing the printk to `%u` (readback still included), redeploying hit
the target kernel fine ONE time, but it turned out that was actually the
*first* instrumented (buggy-%llu) build rather than this one — the
confusion is logged here deliberately so it isn't repeated. The next three
redeploy attempts in a row (two with the readback included, one after
reverting the readback) **all failed the same way**: `insmod ... execute=1`
returns with no error (the normal success signature — the jump happens),
but the target kernel never answers on `10.42.0.2` within 30s, and Android
comes back **on its own**, not via our `qkx_bootdone` warm-reset path. Each
time, the *next* `insmod` attempt in that same Android boot instance fails
immediately with `insmod: failed to load ...: Exec format error`, even
though the vermagic string matches exactly
(`4.19.325-cip128-st12-g4b63ca9fd613 SMP preempt mod_unload modversions
aarch64`) and the `.ko`'s ELF header is valid (`xxd` confirmed `7f 45 4c 46
02 01 01 00`, `e_type=ET_REL`, `e_machine=EM_AARCH64`) — so it's not file
corruption or a build mismatch. Conclusion: **each Android boot instance
only tolerates one `insmod quest_kexec.ko ... execute=1` attempt**; a
second attempt in the same instance reliably fails with `ENOEXEC`
(plausibly a stuck/partially-torn-down module-table entry from the first
attempt, since our manual boot procedure never calls `rmmod` and the
project's warm "return to Android" path is a DRAM-preserving PMIC reset,
not a real kernel module-table reinit).

A full `adb reboot` (real cold boot, confirmed via `Power-off reason:
PS_HOLD` in fresh dmesg) reliably clears this and allows exactly one more
clean `insmod` attempt — but that attempt then also failed to bring up the
target kernel, three times in a row, with no diagnostic information
reaching us (the target kernel's own dmesg is unreachable whenever it
fails to come up before Android's own watchdog brings stock Android back).

Two live theories, not yet distinguished:
1. The added printk/readback in `dsi_link_hs_clk_set_rate()` genuinely
   destabilizes something (e.g. `clk_get_rate()` called from an unsafe
   context — see above). Weakest point: the *first* instrumented build
   (same printk location, `%llu` bug and all) booted and ran `kms_atomic`
   successfully multiple times with no instability, which was already
   calling into this exact function on every commit.
2. Cumulative hypervisor/secure-memory state exhaustion from today's very
   high number of kexec round trips. The project's own `tools/run.sh` calls
   `ion_secmap.ko` (and optionally `qkx_giveback.ko`) on every single cycle
   specifically because stock Android hyp-assigns secure ION pages to other
   VMs that **stay locked after kexec**, and accessing them from the next
   kernel hangs the CPU. Our manual boot procedure (documented earlier in
   this file) **skips this step entirely** — it was fine for many cycles,
   but may not be sustainable indefinitely without ever giving pages back.

Next step if this is picked up again: try the `ion_secmap.ko`/`GIVEBACK`
path from `tools/run.sh` instead of the bare manual insmod, OR bisect by
reflashing the exact pre-printk Image (saved results from the prior
session, topology/encoder findings) to confirm it still boots reliably
before re-adding instrumentation one line at a time.

Stopped here rather than continuing to burn `adb reboot` cycles blind, since
three failures with zero diagnostic return is a sign to change approach,
not to keep repeating the same action.

## 2026-10-10, resumed after a charging break: requested rates confirmed identical, new suspect isolated

Headset was at low battery; took a break to charge, came back at 34%
(charging). First redeploy after the break worked **immediately** —
target kernel reachable in 2s, no crash — which weakens (doesn't fully
rule out) the "cumulative hyp/secure-memory exhaustion" theory from the
previous entry, since nothing else changed except time-to-charge. Battery
level is at least a plausible contributing factor for that earlier streak
of 3 failures, even though it was never proven directly.

Got clean, correctly-formatted `QKXDBG` output from the (now `%u`-fixed)
`dsi_link_hs_clk_set_rate()` printk:

```
set_rate idx=0 cont_splash=0 byte_clk=... pixel_clk=... byte_rate=143311680 pix_rate=191082240
set_rate idx=1 cont_splash=0 byte_clk=... pixel_clk=... byte_rate=143311680 pix_rate=191082240
```

**Both controllers request identical, correct rates.** This rules out a
rate-computation bug for idx=1 — the requested byte_clk/pixel_clk values
are exactly right, matching idx=0 exactly (as they should for a symmetric
dual-DSI panel), and `clk_set_rate()` returns success for both with no
error logged. The clock framework's own bookkeeping says everything is
fine for idx=1. Yet `/sys/kernel/debug/clk/`'s `dsi1pll_vco_clk` tree still
reads rate=0 (confirmed earlier this session, repeatedly). **The gap is
between "clk core reports success" and "hardware actually locks," and it's
silent — no error surfaces anywhere in dmesg for this path.**

Traced into the PLL driver itself (`techpack/display/pll/dsi_pll_7nm.c`,
`vco_7nm_set_rate()` — the actual `clk_ops.set_rate` callback the VCO
clock's `clk_set_rate()` call chain bottoms out at). Found a real
candidate: the function opens with

```c
if (rsc->pll_on)
    return 0;
```

i.e. if the PLL resource's own `pll_on` flag (distinct from
`dsi_display`'s `is_cont_splash_enabled` — this is a lower-level,
per-PLL-resource flag, set `true` only after a successful lock-status
check elsewhere in this file, per `grep`) is already `true`, this function
**skips every register write** (`dsi_pll_init_val`, `dsi_pll_commit`, etc.)
and still returns 0/success. If something upstream marks `pll_on=true` for
ndx=1 without the PLL ever actually being configured and locked (e.g. a
continuous-splash-style early handoff that only really powers up PLL0 for
the boot logo, but still flags PLL1's resource struct as "on" for
bookkeeping reasons), this would be an exact, silent explanation for
everything observed: requested rate correct, call succeeds, hardware never
locks, zero error anywhere.

Added a minimal single-line `pr_err` at the top of `vco_7nm_set_rate()`
printing `rsc->index`, `rate`, and `rsc->pll_on` to directly confirm or
rule this out. **This build failed to come up twice in a row** (first
attempt: `insmod` itself failed with the now-familiar post-warm-reset
`Exec format error`; retry after re-pushing the module reported success
but the target kernel never answered on `10.42.0.2` within 30s, and
Android came back on its own via a clean software-triggered `PS_HOLD`
reset). Battery was confirmed stable at 33% both before and after this
failure (ruling out brownout for *this specific* failure, unlike the
unresolved earlier streak). This narrows the suspect list: **this specific
printk, in this specific function, is the leading suspect** for
destabilizing boot — plausibly because `vco_7nm_set_rate()` can be called
very early (PLL resource init / cont-splash handoff during probe, before
the console/printk path downstream of `pr_err`/`DRM_DEV_ERROR` is
necessarily safe to call from, or from a locking context the previous
`dsi_clk_manager.c` printk never touched), unlike the `dsi_link_hs_clk_set_rate()`
printk which only fires during an actual userspace-triggered modeset and
caused zero instability across many runs.

**Reverted the `vco_7nm_set_rate()` printk** and rebuilt; left the build
tree and `build/out-target/Image` / `build/out-payload-simple/payload/` at
this reverted, previously-stable state (same as the `dsi_link_hs_clk_set_rate()`
%u-fixed version that's already been confirmed to boot and run `kms_atomic`
cleanly). Did not redeploy/retest this revert before stopping — next step
if picked up again is to confirm the revert alone restores reliable boot,
then re-add the `pll_on` check more carefully: either use
`printk_ratelimited`/a static key guard, or — better — don't add a printk
inside the PLL driver's hot path at all. Instead, read `rsc->pll_on`
indirectly: check whether `mdss_pll_resources` (or an equivalent per-PLL
state) is exposed anywhere under the existing `/sys/kernel/debug/clk/`
`dsi1pll_vco_clk` directory (common clock framework debugfs usually
exposes `clk_prepare_count`/`clk_enable_count`, which are a *different*
flag from this driver-internal `pll_on`, but prepare_count being nonzero
while rate stays 0 would be an equally strong confirmation of the same
theory without touching kernel source at all) — check that first, since
it's zero-risk (pure userspace debugfs read, no new printk, no rebuild,
no reboot needed) before risking another crash cycle.

**Follow-up, same session:** retried with exactly that reverted build (the
one already proven stable earlier — identical to the version that worked
immediately after the charging break). **It also failed to come up** —
same symptom, target kernel never answered within 30s, Android came back
on its own. This is an important negative result: it rules out the
`vco_7nm_set_rate()` printk as the sole/primary cause, since this build
doesn't have it.

Checked battery across this whole sequence: 34% → 33% → 32% → 31%,
**continuously dropping despite `status` reporting `Charging`** the whole
time. That's the real signal — the charge current isn't keeping up with
whatever the kexec cycle draws, so effective power is still degrading
even while nominally "charging." Combined with the fact that the *only*
thing that changed between "worked immediately" (first retry after the
break, at 34%) and "failed repeatedly" (34% down to 31%) is battery level
continuing to fall, **power marginality is now the leading explanation for
this entire instability streak**, not any of the kernel-source changes
made today. Both printk additions (the stable `dsi_clk_manager.c` one and
the reverted `dsi_pll_7nm.c` one) are plausibly innocent.

Stopped here. Recommend before resuming: get the headset to a genuinely
high charge level with the USB-ECM cable *disconnected* during charging
(so there's no concurrent data-link current draw), confirm capacity is
climbing (not just "Charging" status) before the next kexec attempt, and
re-run the `vco_7nm_set_rate()` `pll_on` diagnostic from a clean, well-charged
state as the actual next investigative step — it was never genuinely
disproven, just untested under reliable power.

## 2026-10-10, resumed again: redeploy flakiness was a stale-module-table issue, NOT battery — and the clock investigation is CLOSED (negative result, but definitive)

User pointed out 34% (and even 30-31%, where several of the failures
happened) is plenty — earlier in the session things worked fine at lower
levels too — and asked to just retry. Did: got `insmod: Invalid argument`
(a third, *different* error signature from the same `Exec format error`
seen before — no `qkx:`-prefixed message in dmesg at all, meaning
`insmod` never even reached the module's own init code). A plain `adb
reboot` (full cold boot, confirmed via `Power-off reason: PS_HOLD`) at
**30% battery** fixed it immediately — one clean `insmod`, target kernel
reachable in 6 seconds. **This retires the battery theory entirely.** The
real, consistent cause of every redeploy failure this whole session was
stale Android-kernel module-table state after repeated warm
(`qkx_bootdone`) resets — a full `adb reboot` is the reliable fix, every
time it's been tried. Correction for next time: don't chase battery
levels; if `insmod` ever fails on a warm-reset cycle (`Exec format error`,
`Invalid argument`, or similar with no module-side dmesg output), the fix
is `adb reboot`, not more pushes/retries or battery concerns.

With a clean boot, redid the zero-risk debugfs check recommended above —
and it overturned the entire clock-PLL investigation. While `kms_atomic`'s
commit was held, the full indented `/sys/kernel/debug/clk/clk_summary`
tree showed:

```
dsi0pll_pclk_src  (locked, 191082240 Hz)
   dsi0_phy_pll_out_dsiclk
      disp_cc_mdss_pclk0_clk_src  (locked, 191082240 Hz, this feeds INTF0)
      disp_cc_mdss_pclk1_clk_src  (locked, 191082240 Hz, this feeds INTF1!)
dsi0pll_byteclk_src  (locked, 143311680 Hz)
   dsi0_phy_pll_out_byteclk   [enable_count=2 — two children below]
      disp_cc_mdss_byte0_clk_src  (locked, 143311680 Hz)
      disp_cc_mdss_byte1_clk_src  (locked, 143311680 Hz, this feeds INTF1!)
dsi1pll_vco_clk  (still 0, still fully unused)
```

**The clocks that actually drive DSI interface 1's transmit logic
(`disp_cc_mdss_byte1_clk`, `disp_cc_mdss_pclk1_clk`) are children of
DSI0's PLL, not DSI1's — and they're correctly locked, enabled
(`clk_prepare_count=1`, `clk_enable_count=1` each), and running at the
exact right frequency.** `dsi1pll_vco_clk` reading 0 the entire time
(confirmed obsessively across many sessions and commits) is real but
**irrelevant** — that PLL is simply unused hardware for this topology.

Root cause in source, found by tracing callers of `dsi_ctrl_set_clock_source()`
(`dsi_display.c` around line 2336-2360): there's an explicit comment —
*"In case of split DSI usecases, the clock for master controller should
be enabled before the other controller. Master controller in the clock
context refers to the controller that sources the clock."* — followed by
code that calls `dsi_ctrl_set_clock_source(ctrl->ctrl, &display->clock_info.mux_clks)`
for **every** non-master controller using the exact same shared
`mux_clks` as the master. **This is intentional, documented split-DSI
behavior**: one physical PLL is designed to drive both links' byte/pixel
clocks via the `disp_cc` RCG mux network. The second DSI PLL sitting
completely unlocked/unused for the whole session was correct and
by-design, not a misconfiguration.

**Conclusion: the entire clock-focused investigation (this session and
the prior one) is closed with a definitive negative result.** The clock
feeding both DSI interfaces is fine. Whatever makes `intf2` never show
vsync activity is **not a clock problem** — it's back in the
hardware/register-level territory the prior session had already started
exploring before pivoting to clocks: INTF timing-generator enable,
encoder-to-hardware-block binding at the physical-register level, or the
DSI command/video-mode start sequence specifically for the second
interface. The previously-flagged "do not trust this 0x6b800/0x6c000
register comparison" caveat still applies — that offset math needs to be
correctly re-derived from `sde_hw_intf_init()`'s own calculation before
drawing conclusions from direct register reads, which is the right next
starting point now that clocks are definitively ruled out.

## 2026-10-10, continued again: real root-cause progress on the INTF/register side — several theories falsified with hard evidence, one genuine new lead

Re-derived the register addressing from source instead of guessing.
`sde_hw_intf_init()` → `_intf_offset()` in `sde_hw_intf.c` confirmed the
real formula: hardware address = `base_off + blk_off + reg_off`, where
`blk_off` is exactly the per-INTF catalog value from DT (`0x6b800` for
intf1, `0x6c000` for intf2 — the same values used in the prior session's
"unreliable" comparison). **The base offsets were never the problem.**

The actual bug in the prior comparison: the debugfs `*_off` write handler
(`sde_dbg_reg_base_offset_write` in `msm/sde_dbg.c`) parses with
`sscanf(buf, "%5x %x", &off, &cnt)` — **no `0x` prefix accepted, and the
offset field is capped at 5 hex characters.** Writing `"0x6b800 4"` (with
the `0x` prefix) silently fails/misparses; the correct form is `"6b800 4"`
(bare hex, no prefix). This single formatting detail is why every past
attempt at direct register comparison produced garbage. **Documenting
this clearly so it is never repeated:** `echo <bare-hex-offset> <len> >
.../sde_off` then `cat .../sde_reg` — offset must be ≤5 hex digits (so
any computed address like `base+reg_off` needs to be summed as actual hex
arithmetic, not string-concatenated — made exactly this mistake once this
session and had to redo the comparison after catching it).

With addressing finally correct, read `INTF_TIMING_ENGINE_EN` (offset
`0x000` within each INTF block) during a held `kms_atomic` commit:

```
intf1 (0x6b800): 0x00000001  — enabled
intf2 (0x6c000): 0x00000000  — disabled
```

A clean, unambiguous, directly-reproducible register-level confirmation
that intf2's timing-generator master-enable bit is never set. Traced the
exact source of this: `sde_encoder_phys_vid_handle_post_kickoff()` in
`sde_encoder_phys_vid.c` only calls
`phys_enc->hw_intf->ops.enable_timing(phys_enc->hw_intf, 1)`
`if (sde_encoder_phys_vid_is_master(phys_enc))` — for the slave physical
encoder this call is skipped entirely, with `enable_state` still marked
`SDE_ENC_ENABLED` regardless (so nothing looks wrong from software's
perspective; this explains why no error is ever logged for intf2). Master
vs. slave role comes from `num_of_h_tiles`/`h_tile_instance` in
`sde_encoder.c` (`i==0` → `ENC_ROLE_MASTER`, else `ENC_ROLE_SLAVE`) — this
is genuine Qualcomm "h-tile split" dual-DSI-controller architecture (two
real separate `dsi-ctrl-0`/`dsi-ctrl-1` + two real PLLs, matching
`qcom,mdss_dsi_jdi_4k_120Hz_dsc_video` — "jdi_4k" = one single very-high-res
JDI panel, bandwidth-split left/right across 2 DSI links, **not** two
independent per-eye displays). This also explains the shared-PLL clock
finding from earlier: one clock master is expected to drive both
link's byte/pixel clocks, by design, for exactly this kind of split
panel.

**Tested the obvious fix directly: patched `handle_post_kickoff` to call
`enable_timing(..., 1)` unconditionally for both master and slave.**
Rebuilt, redeployed, ran `kms_atomic`. Register-level result: confirmed
— `intf2`'s `INTF_TIMING_ENGINE_EN` now reads `0x00000001`. **But this did
not fix the display, and produced real hardware errors**:

```
dsi-ctrl-1: dsi FIFO OVERFLOW error: 0xf0000      (rate-limited: "128467 callbacks suppressed")
dsi-ctrl-1: dsi FIFO UNDERFLOW error: 0xff0000    (rate-limited: "11527 callbacks suppressed")
dsi-ctrl-1: Detected spurious interrupts on dsi ctrl
```

Thousands of FIFO overflow/underflow errors on `dsi-ctrl-1` (the second
DSI controller) specifically, the instant both timing engines ran
independently and simultaneously. The user independently confirmed via
direct visual observation on the headset: with this patch applied, the
display behaves **exactly the same as it always has** — both halves
individually can light up, but never stay on at the same time; they
flicker/alternate. This is a very informative negative result: it means
the master-only `enable_timing` gate is **not a bug to patch around** —
it's there because the hardware genuinely cannot tolerate two
independent, freely-running timing engines on a panel whose two physical
links share one clock source. **Reverted this patch.** (Also means the
user's repeated real-world observation of "both sides work individually,
just never simultaneously" was actually a more accurate description of
the failure mode than this session's own earlier framing of "intf2 never
activates" — worth remembering: direct visual/behavioral reports from the
person who can actually see the headset are a sharper signal than any
single register snapshot.)

Checked the next layer down: **is the CTL (control-path) block correctly
told to drive both interfaces?** DT `qcom,sde-ctl-off` gives 6 CTL block
bases (`0x2000, 0x2200, 0x2400, 0x2600, 0x2800, 0x2a00`); `CTL_INTF_ACTIVE`
is at `+0xF4` within each (`sde_hw_ctl.c`). Read CTL0's (`0x20f4`):
**`0x00000006`** = `BIT(1)|BIT(2)` — both relevant INTF bits are correctly
marked active in the control path. CTL1's equivalent register read `0`
(unused for this topology). **This rules out a CTL-level
dual-interface-activation bug** — the control path genuinely is set up to
trigger/flush both physical interfaces together as one synchronized
domain, consistent with a true h-tile hardware design where one CTL block
fans a single trigger out to two INTF blocks.

Checked the layer below that: **does the slave INTF actually get its full
timing configuration programmed, or just the enable bit skipped?**
Compared every meaningful `sde_hw_intf.c` timing register
(`INTF_CONFIG`, `INTF_HSYNC_CTL`, `INTF_VSYNC_PERIOD_F0`,
`INTF_DISPLAY_V_START/END_F0`, `INTF_ACTIVE_V_START/END_F0`,
`INTF_DISPLAY_HCTL`, `INTF_ACTIVE_HCTL`) between intf1 (`0x6b800+off`) and
intf2 (`0x6c000+off`) during a live commit. **Every single one read
byte-for-byte identical between the two interfaces** — real, correct,
non-zero timing parameters, not garbage or defaults. So
`sde_hw_intf_setup_timing_engine()` (the function that programs all of
these) clearly **does** run fully for the slave with correct parameters —
only the final master-enable bit is withheld.

**Where this leaves it, with much higher confidence than before:** the
clock domain is fine (shared by design), the CTL trigger/activation path
is fine (both bits set), and the slave INTF's timing configuration is
fully and correctly programmed. The one piece that's intentionally
withheld (slave's own independent timing-engine enable) is withheld for a
real, confirmed hardware reason (enabling it causes FIFO chaos), not a
bug. **This strongly suggests the actual missing piece is not at the
INTF/CTL/clock level at all, but one layer further up: whether intf2's
physical link is actually receiving live pixel data from its layer mixer
(LM1) / SSPP fetch path.** The DSI FIFO *underflow* errors specifically
(not just overflow) are consistent with "the link is being clocked/
triggered but has no real data arriving" — exactly what you'd see if the
hardware's own internal data-routing from a single shared framebuffer to
the second physical link (needed for a true h-tile split single panel)
isn't actually wired up, even though the pure timing/trigger plumbing
checked out at every level examined so far.

**Next concrete step, not yet started:** investigate `sde_crtc.c`/
`sde_rm.c`'s layer-mixer assignment and SSPP (fetch unit) binding for a
dual-LM h-tile topology — specifically whether a single-plane, single-CRTC
atomic commit (as `kms_atomic` does, and presumably as any real compositor
for this display must do, since the h-tile split is supposed to be
transparent to userspace) correctly results in the kernel driving both
LM0 and LM1 from split left/right halves of that one source buffer, or
whether LM1's fetch configuration is itself incomplete/never bound for
this specific panel. Also worth checking `sde_rm_reserve_dsc`/DSC-encoder
binding per-LM, since this topology's `num_comp_enc=2` (confirmed earlier)
implies two independent DSC compressors that must each receive the
correct half of the source image — a mismatch there would also manifest
as "intf2 fed with no/garbage data" exactly like what's observed.

## 2026-10-10, continued: tested switching to DUALPIPE_3DMERGE_DSC topology — clean negative result, reverted

Found the SDE topology table in `sde_rm.c` (`g_top_table`): entries are
`{ name, num_lm, num_comp_enc, num_intf, num_ctl, needs_split_display }`.
Our panel's DT (`qcom,display-topology = <2 2 2>` at
`/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_jdi_4k_120Hz_dsc_video/
qcom,mdss-dsi-display-timings/timing@0`) selects `DUALPIPE_DSC`
(`{2, 2, 2, 2, true}` — `needs_split_display=true`, genuinely requires 2
independent interfaces). `DUALPIPE_3DMERGE_DSC` (`{2, 2, 1, 1, false}`)
merges both layer mixers into a single interface in hardware instead.
Hypothesis: since both DSI links are confirmed to share one PLL by design
(earlier finding), maybe the DT is requesting the wrong (independent-pipe)
topology for hardware that's actually wired for a merged single-output
config.

Patched the DTB directly (`fdtput -t x ... qcom,display-topology 2 1 1`,
same byte size, no kexec staging-window changes needed) and tested. After
several failed reachability attempts (5 in a row at one point — **ruled
out as being caused by the DTB patch**: reverted to the stock `<2 2 2>`
DTB mid-session and it *also* failed once before succeeding on retry,
confirming this was the same ordinary probabilistic early-boot flakiness
documented throughout this file, not something wrong with the patch
itself), got it running:

- Topology selection confirmed changed as intended via the `QKXDBG`
  printk: `top=6 num_lm=2 num_comp_enc=1 num_intf=1` (topology enum
  value 6 = `DUALPIPE_3DMERGE_DSC`, matching the table exactly).
- The atomic commit itself reported success (`ATOMIC commit succeeded`).
- **Visually: neither side of the panel lit up at all** — strictly worse
  than the baseline, which always got at least one side (intf1)
  genuinely working.
- dmesg showed the real cause: `[drm:sde_encoder_phys_vid_enable:940]
  invalid hw_intf 0 hw_ctl 0 hw_pp 1` — the resource manager allocated a
  "pp" (ping-pong/merge) block but **never bound a valid hw_intf or
  hw_ctl to the encoder at all**. Followed by `dsi-ctrl-0` FIFO
  overflow/underflow (notably on **ctrl-0** this time, not ctrl-1),
  `intf2 line count is less, line_cnt=0` repeated many times (literally
  zero lines ever scanned), and `intf2 pp-1 encoder is disabled`.

**Conclusion: `DUALPIPE_3DMERGE_DSC` is falsified as a one-property fix.**
Changing only `qcom,display-topology` is not sufficient — the rest of the
device tree (DSI `h_tile_instance`/controller-count properties, and
possibly other SDE catalog fields) still reflects the 2-independent-
controller configuration and conflicts with what the merge topology's
resource-allocation code expects, producing a broken binding rather than
a working merged output. This was flagged as a risk before testing and
the result confirms it. **Reverted the DTB back to the original
`<2 2 2>`** (`boot.dtb.bak` in the scratchpad build dir has the pristine
original; `build/out-payload-simple/payload/boot.dtb` is restored to it)
and confirmed the baseline (intf1 working, intf2 dark) still boots and
runs normally. Device left in this known-good state.

If this topology-switch idea is revisited, it would need a much more
thorough DT surgery — updating `h_tile_instance`/DSI controller-count
properties and any other fields tied to the 2-controller assumption
consistently alongside `qcom,display-topology`, not just the one
property — which is a substantially bigger change than what was tried
here and wasn't attempted this session.

## 2026-10-10, continued: gave ctrl1 a genuinely independent clock source — real progress, but a new side effect found

Found that the display device's `clock-names` DT property
(`/soc/qcom,mdss_mdp@ae00000/qcom,dsi-display-primary`) **already has real
phandles** for a full parallel clock set: `mux_byte_clk1`,
`mux_pixel_clk1`, `src_byte_clk1`, `src_pixel_clk1`, `cphy_byte_clk1`,
`cphy_pixel_clk1`, `shadow_byte_clk1`, `shadow_pixel_clk1`, etc. — the
hardware/clock-tree wiring for an independent ctrl1 clock path exists and
is fully provisioned. It's just never *requested*: the panel's
`qcom,dsi-select-clocks` property only ever lists `"mux_byte_clk0",
"mux_pixel_clk0"`, and even if requested, `dsi_display.c`'s clock-parsing
code only had one shared `mux_clks` struct with no slot for a second,
confirmed via grep across the whole driver — `dsi_ctrl_set_clock_source()`
is only ever called with the **same** `&display->clock_info.mux_clks` for
every controller in `dsi_display.c`'s enable loop (~line 2341-2360), both
master and slave.

**Patched this properly:**
- `dsi_display.h`: added `mux_clks_sec` field to `struct
  dsi_display_clk_info` alongside the existing `mux_clks`.
- `dsi_display.c`'s `dsi_display_clocks_init()`: added exact-match checks
  for `"mux_byte_clk1"`/`"mux_pixel_clk1"` (checked *before* the existing
  substring-based `mux_byte`/`mux_pixel` prefix checks, which would
  otherwise also match the `_clk1` names and clobber the shared struct)
  that route into the new `mux_clks_sec` field instead.
- `dsi_display.c`'s per-controller enable loop: now uses
  `&display->clock_info.mux_clks_sec` for the slave controller when its
  `byte_clk`/`pixel_clk` are non-NULL (falls back to the shared master
  clocks otherwise, so this degrades gracefully if the DT doesn't have
  the `_clk1` names selected). Added a `QKXDBG` printk right at this
  decision point.
- DTB: added `"mux_byte_clk1"`, `"mux_pixel_clk1"` to the panel's
  `qcom,dsi-select-clocks` property (now 4 entries instead of 2).

**Result — genuine, measurable, first-of-the-session progress:**
`dmesg` confirmed `QKXDBG: using independent mux_clks_sec for ctrl idx=1`
(the DT clocks resolved and routed correctly). Far more importantly,
`/sys/kernel/debug/clk/clk_summary` showed **`dsi1pll_vco_clk` genuinely
locked for the first time all session**: `1146493440 Hz`,
`enable_count=1`, `prepare_count=1` — identical to `dsi0pll_vco_clk`.
Every single check of this clock all session (single reads, 10-second
continuous polling) had shown it at a flat `0`. Ctrl1 now has its own
independently running PLL, separate from ctrl0's, for the first time.

**But: a new, real side effect.** With only this clock change (the
slave's `INTF_TIMING_ENGINE_EN` is still **not** force-enabled in this
build — confirmed still reading `0x0`), `dmesg` showed FIFO
overflow/underflow errors on **both** `dsi-ctrl-0` and `dsi-ctrl-1` —
notably including ctrl-0, which has been completely stable and
error-free in every single test this entire session until now. This
means the act of reparenting `disp_cc_mdss_byte1_clk_src`/
`disp_cc_mdss_pclk1_clk_src` away from `dsi0pll` (where they'd always
been, shared) and onto `dsi1pll` introduces some instability on ctrl0's
previously-rock-solid clock path — plausibly a brief glitch during the
`clk_set_parent()` transition itself (disp_cc_mdss_byte0_clk_src's shared
parent node, `dsi0_phy_pll_out_byteclk`, had `enable_count=2` before this
change since both ctrl0 and ctrl1 derived from it — dropping to 1 as
part of the reparent could plausibly cause a brief hiccup if it happens
while ctrl0 is actively clocking real DSI traffic, rather than before
enable).

Asked the user to visually confirm; their report: "they both work like I
said they just alternate rapidly" — consistent with their long-standing
description of the panel's behavior from earlier in this session (the
video-analysis entry), not something visibly different/worse. Given that
match, this new FIFO-error side effect may not be user-visible as a
*new* problem, but it's a real regression from the previously
perfectly-clean ctrl0 behavior and should be tracked as such rather than
assumed harmless.

**Reverted to the known-stable baseline** (`build/out-payload-simple/payload/Image`
and `/boot.dtb`, pre-clock-patch) to leave the device in a safe state.
The clock-independence source changes themselves are **kept** in the
kernel source tree (not reverted) since they represent real, verified
progress — `dsi1pll` now genuinely runs independently, which was the
whole first half of what the "intf2 needs both an independent clock AND
an independent timing engine" theory called for. `build/out-target/Image`
and `boot_clk1exp.dtb` (in the scratchpad `build/out-payload-simple/payload/`
dir) hold this clock-independence build if picked up again.

**Next step, not yet attempted:** combine this independent-clock build
with the independent-timing-engine-enable patch (the one tested earlier
this session and reverted after it caused the original FIFO storm on
ctrl-1 alone) — now that ctrl1 genuinely has its own clock, enabling its
own timing engine independently might finally not conflict the way it
did when both engines were racing off a shared clock. This is the
natural, well-motivated next experiment, though the newly-discovered
ctrl0-side clock-reparenting glitch should be kept in mind and ideally
investigated/fixed first (e.g. by ordering the reparent to happen before
any controller is actively enabled, rather than interleaved with the
current enable sequence) since it affects the previously-reliable side
too.

## 2026-10-10, continued: combined both patches — clean negative result, the clock-sharing theory is now disproven as *the* cause

Re-applied the independent-timing-engine-enable patch (removing the
`is_master` gate in `sde_encoder_phys_vid_handle_post_kickoff()`, same as
tested earlier and reverted) on top of the independent-clock-source
build from the previous entry. Rebuilt, redeployed, tested.

**Confirmed via dmesg that both halves of the fix were active
simultaneously**: `QKXDBG: using independent mux_clks_sec for ctrl idx=1`
(clock routing) fired as expected, both `dsi_link_hs_clk_start` calls for
idx=0 and idx=1 showed correct identical rates. This was a genuine test
of "independent clock + independent timing engine together," not a
partial one.

**Result: no visible improvement.** User's direct report: "it's doing the
same flashing" — identical to every prior test, not better, not worse.
dmesg confirmed this wasn't just a visual-perception coincidence:
**the exact same FIFO overflow/underflow/spurious-interrupt storm
persisted on both `dsi-ctrl-0` and `dsi-ctrl-1`**, in essentially the
same pattern as when the clock was still shared. After the 15s test
window, both `INTF_TIMING_ENGINE_EN` registers read back `0x0` (torn
down, as expected post-commit).

**This is an important, clarifying negative result: the shared-clock-
domain theory, while a real and independently-verified fact about the
hardware (`dsi1pll` genuinely locks now, confirmed via clk_summary), was
NOT the actual root cause of the FIFO conflict when both timing engines
run simultaneously.** Something else — not clock-domain sharing — is
responsible for the instability when two independent SDE timing engines
run concurrently on this panel/SoC/driver combination. The two most
concrete, well-reasoned hypotheses available this session (the
is_master-only enable gate, and the shared-clock-domain explanation for
why forcing it broke things) have both now been tested directly and
neither one, alone or combined, produces a working display.

**Reverted both patches' *effects* back to the safe baseline for the
live device** (stock `<2 2 2>` DTB, pre-clock-patch Image) — the
independent-clock and independent-timing-engine source changes remain in
the kernel tree (not deleted) since they're verified-correct,
reusable building blocks, but neither is deployed right now.

**Where this honestly leaves things:** every lever this session could
identify and directly test from first principles (clock sharing vs.
independent, timing-engine enable gate) has been tried, individually and
combined, with concrete register/dmesg evidence each time — and none of
them closes the gap. What's left is lower-level than anything reachable
through the SDE encoder/CTL/INTF/clock abstractions examined so far:
most plausibly DSI PHY-level lane timing/calibration synchronization
between the two physical links (a layer below everything checked this
session — `dsi_phy_hw_v4_0.c` and the PHY's own lane-timing-calibration
sequence, not yet investigated at all), or this exact
`DUALPIPE_DSC`-with-independent-pipes configuration may simply be a
genuinely incomplete/never-validated code path in this driver revision
for this panel, not something fixable by configuration or small patches
alone. A pragmatic alternative worth considering, if full dual-interface
output continues to resist diagnosis: deliberately configuring for
`SINGLEPIPE_DSC` (one LM, one INTF, cropped to half the panel) as an
interim "something real and stable on screen" milestone, trading full
resolution for a working, non-flickering single-eye image — a
fundamentally different and much simpler target than true dual-pipe
operation, and something that should be far more likely to just work
given intf1 alone has been rock-solid in every single test this entire
session.

## 2026-10-10, continued: tried SINGLEPIPE_DSC — real DT/driver groundwork laid, but the visual symptom persisted even with only one DSI link active, which rules out the entire "two interfaces conflicting" framing

User correctly pointed out the Quest 2 uses **one single physical panel**
(~3664×1920 combined, not two separate per-eye panels) split across two
DSI links purely for bandwidth — confirmed this matches the native mode
exactly. This means "intf1 vs intf2" has always meant "left half vs right
half of one panel," not "left eye panel vs right eye panel." Worth
remembering for any future session.

Patched `qcom,display-topology` to `<1 1 1>` (`SINGLEPIPE_DSC`, no kernel
rebuild needed — pure DT change). First attempt: commit succeeded, but
SDE picked **intf2** (the broken one) as the single active interface —
nothing visible, consistent with every other intf2-only test this
session. Root cause: `qcom,dsi-ctrl-num`/`qcom,dsi-phy-num` on the panel
node still listed both controllers (`0 1`), so which physical encoder won
the single-intf reservation was arbitrary/order-dependent in
`sde_rm.c`'s `_sde_rm_reserve_intf_related_hw()`.

Fixed properly: reduced `qcom,dsi-ctrl-num` and `qcom,dsi-phy-num` (panel
node) to `<0>` only, and `qcom,dsi-phy`/`qcom,dsi-ctrl` (on
`/soc/qcom,dsi-display-primary`) to single phandles. First attempt at
this broke `dsi_display_init()` entirely (`different ctrl and phy
counts`, `rc=-19`, no `/dev/dri/card0` at all) — turned out `phy_count`
in that specific check comes from `qcom,dsi-phy-num` (count of u32
elements), **not** from `qcom,dsi-phy` (phandle list) as initially
assumed; had to reduce `qcom,dsi-phy-num` specifically, confirmed via
direct source read of `dsi_display_parse_dt()`. Once all four properties
were consistently reduced to one entry, the DRM device probed cleanly and
**the commit correctly bound to intf1** this time (`mode=960x3664`,
confirmed via dmesg; only `dsi_link_hs_clk_start idx=0` fired, no idx=1 —
ctrl1 is now genuinely, fully absent from the running kernel's display
stack, not just logically disabled).

Checked live DSC hardware config registers (`sde_hw_dsc.c` offsets,
4 DSC blocks at `0x81000/81400/81800/81c00` from `qcom,sde-dsc-off`) to
rule out a slice/chunk mismatch: `DSC0` (the only active one — `DSC1`
read all-zero, confirming clean single-block operation) showed
`PICTURE=960x3664`, `SLICE=480x16`, `CHUNK_SIZE=480` — all internally
consistent and matching the DT's `qcom,mdss-dsc-*` values exactly. No
DSC-level misconfiguration found; this theory from earlier in the session
is now also ruled out.

**And yet:** dmesg still showed DSI FIFO underflow and `Command transfer
failed` errors on `dsi-ctrl-0` — the *only* controller now active — and
the user, watching live, still described the same "alternating" flashing
between lens positions as every single prior test this session,
including ones with two active controllers. Confirmed via direct
`clk_summary`/`INTF_TIMING_ENGINE_EN` register reads during this exact
run that `ctrl1`/`intf2`/`dsi1pll` were all **completely dormant** — zero
clock activity, timing engine off, nothing. With only one physical DSI
link electrically active and driven, two-interfaces-racing is
**categorically impossible** as an explanation for what was observed.

**This is the most important finding of the entire session, and it's a
negative one:** every software-configurable layer this session identified
and could test — clock sharing vs. independence, CTL dual-interface
activation, INTF timing-engine enable gating, DSC slice/chunk
configuration, and now full single-vs-dual-controller topology — has been
individually verified correct via direct register/dmesg evidence, in
every combination tried, and **none of them has ever changed the visual
symptom**. The flashing/alternating/non-stable behavior appears to be
invariant across every SDE-level and DSI-display-level configuration
tested. That invariance is itself strong evidence the actual root cause
sits below all of it — most plausibly DSI PHY-level lane timing/
calibration/synchronization (`techpack/display/msm/dsi/dsi_phy_hw_v4_0.c`
and the PHY's own init/calibration sequence — not investigated at all
this session, a genuinely different and much lower layer than anything
checked so far), or a hardware/electrical characteristic of this specific
panel assembly that isn't reachable through kernel configuration at all
within the constraints of a kexec'd (not full cold-boot) bring-up.

**Reverted to the known-stable baseline** (original unmodified `<2 2 2>`
DTB, original Image) to leave the device in its safest, most functional
state. All experimental DTBs (`boot_singlepipe.dtb`,
`boot_clk1exp.dtb`) and the independent-clock-source kernel source
changes remain available in the scratchpad/build tree if a future session
wants to pick this up, but nothing experimental is currently deployed.

**Honest overall assessment after this very thorough session:** the
kexec boot itself, Arch Linux ARM, and Holo Core userlands are all
confirmed solid and reproducible (documented earlier in this file). Full
dual-eye graphics remain unsolved — not from lack of trying, but because
every lever reachable from kernel-source-level configuration has now been
tested individually and in combination, with concrete hardware evidence
each time, and the symptom has not moved. Further progress most likely
requires either DSI PHY lane-level investigation (a genuinely new, deeper
area) or accepting this may be a hardware/firmware-level limitation of
running this exact panel via kexec on this specific unit that isn't
fixable through DT/driver patches alone.

## 2026-10-10, continued: found and fixed a genuine, real bug (panel DCS init was being skipped entirely) — but even combined with every other confirmed fix, the visual symptom is unchanged. This is the session's real conclusion.

User asked how the original quest-kexec repo author (Quest Pro target)
got display working without anywhere near this amount of struggle.
Re-checked their `v1.0.1-qpro` release tag (already in this local clone):
their kernel patch set (`kernel/patches/0001-...patch`) touches
`dsi_ctrl.c` only for `IS_ERR_OR_NULL` cleanup-path NULL-safety and an
earlier `ctrl->pdev` assignment — genuinely nothing SDE/DSI-topology
related. Their extensive `docs/KEXEC_ANDROID_WORKING_SETUP.md` logbook is
almost entirely about Android **userspace** bring-up (VINTF manifests,
composer startup races, HALs) — never kernel-level display pipeline
work. Conclusion: Quest Pro's panel topology must be simpler (or
genuinely already fully supported) and never forced them to touch any of
what this session has been fighting. No shortcut found there, but it
reinforced that the kexec approach itself is sound.

Went back to first principles: checked whether the panel's real DCS
("Display Command Set") init sequence — sleep-out, display-on, DSC
config, the stuff in `qcom,mdss-dsi-on-command` — was ever actually being
sent during our test commits. Added a direct printk to
`dsi_panel_enable()` (`dsi_panel.c`) logging on entry and after
`dsi_panel_tx_cmd_set(DSI_CMD_SET_ON)`'s return code. **It never fired,
not once, across the entire rest of this session's testing.**

Traced why: `dsi_display_enable()` (`dsi_display.c`) opens with:

```c
/* Engine states and panel states are populated during splash
 * resource init and hence we return early */
if (display->is_cont_splash_enabled) {
    ...
    display->panel->panel_initialized = true;
    return 0;   // skips dsi_panel_enable() ENTIRELY
}
```

This assumes the bootloader already sent valid DCS commands before
kernel boot (true for a real cold boot) and that the kernel just needs to
"take over" cleanly without re-initializing the panel. **This assumption
does not hold under kexec** — confirmed via dmesg that `dsi_phy_driver_probe`/
`dsi_ctrl_dev_probe` run completely fresh every single kexec boot, meaning
our new kernel instance's own DSI_CTRL/PHY hardware state is freshly
re-probed, not a true continuation of whatever the live Android instance
had going. Patched this function to always fall through to the real
`dsi_panel_enable()` call regardless of the cont-splash flag (clean up
splash resources if flagged, but never skip the real enable).

**Rebuilt and redeployed — but the fix alone changed nothing visible**,
because of a second, independent issue: the CRTC was *already* marked
active (inherited from cont-splash) by the time our test tool committed
`ACTIVE=1`, which DRM core treats as a no-op transition — the
bridge/encoder `enable()` callback chain (where `dsi_display_enable()`
lives) never even runs for a commit that doesn't represent a genuine
0→1 transition. Fixed by modifying the test tool itself
(`drm-test/kms_atomic.c` → new `kms_atomic2`, cross-compiled via the same
Docker toolchain used for the kernel, no kernel rebuild needed for this
part): added an explicit `ACTIVE=0`/`MODE_ID=0` disable commit
immediately before the real enable commit, forcing a genuine transition.

**This worked, confirmed via dmesg, for the first time all session:**

```
dsi_panel_enable called, panel=jdi 4k 120Hz video mode dsc dsi panel
dsi_panel_tx_cmd_set(DSI_CMD_SET_ON) returned rc=0
```

The real panel DCS init sequence genuinely sent, successfully, through
our kexec'd kernel's own hardware state, for the first time this entire
project. This is a real, confirmed, previously-undiscovered bug fix —
independent of whether it turns out to be the full answer.

**But: visually, nothing changed.** User's report after this fix alone:
"I saw alternating flashes" — identical to every single prior test.
Combined this fix with the other two independently-confirmed-real fixes
from earlier sessions today (independent `mux_clks_sec` clock routing for
ctrl1, and the `is_master` gate removed in
`sde_encoder_phys_vid_handle_post_kickoff()` so both physical encoders'
timing engines enable independently) — all three together, rebuilt,
redeployed, confirmed via dmesg that **all three fixes fired correctly
simultaneously** (`using independent mux_clks_sec for ctrl idx=1`,
`dsi_panel_enable called`, `tx_cmd_set(DSI_CMD_SET_ON) returned rc=0`,
all in the same boot). **Visual result, asked for and received twice:
"alternating" both times — identical to every other configuration tested
today.**

**This is the real, load-bearing finding of the whole investigation.**
The exact same "alternating flash" visual symptom has now been observed,
completely unchanged, across every one of these genuinely different
software configurations tested today:
- Shared clock domain (original baseline)
- Independent clock domain (`mux_clks_sec`, confirmed `dsi1pll` locks)
- Independent timing engine forced on (alone — caused FIFO storm)
- Independent clock + independent timing engine combined
- Single-interface-only (`SINGLEPIPE_DSC`, ctrl0 alone, ctrl1 fully
  removed from the DT and kernel's DSI display entirely — ctrl1
  confirmed via live register reads to be 100% electrically dormant)
- Real panel DCS init forced, alone
- Real panel DCS init + independent clock + independent timing engine,
  all three combined

**That invariance across so many genuinely different software states is
itself the conclusion: this is very unlikely to be fixable by any further
SDE/DSI kernel configuration or driver patch.** The most likely remaining
explanation is something at the power-sequencing/hardware level that a
kexec transition — which deliberately avoids a full power cycle of the
panel/backlight assembly to prevent visible flicker during the jump —
does not replicate correctly, even though the DSI host-side driver stack
is now (after today's fixes) doing everything it should. This could be:
panel power-rail sequencing timing, backlight/regulator state specific to
a true cold boot, or a hardware reset line that only a genuine power
cycle asserts. None of this is reachable or verifiable through kernel
source changes alone from where this investigation currently stands.

**Final state:** reverted to the clean, stable, known-working baseline
(stock `<2 2 2>` DTB, original Image — the one with intf1 alone reliably
lighting, no errors). All experimental source changes (`mux_clks_sec`
clock routing, the `is_master` gate removal, the forced-real-panel-enable
patch, and the various diagnostic printks) remain in the kernel source
tree for a future session, along with `kms_atomic2`
(`drm-test/kms_atomic2`, source in `kms_atomic.c`) as a reusable,
confirmed-working disable-then-enable test tool. `boot_clk1exp.dtb`
(scratchpad `build/out-payload-simple/payload/`) has the clock selection
needed for the independent-clock fix if revisited.

## 2026-10-10, continued: the "missing second plane" lead was a false trail — full signal path now traced end to end

Back on the restored working baseline (`<2 2 2>`, `DUALPIPE_DSC`). Dumped
`/sys/kernel/debug/dri/0/crtc129/status` live during a held `kms_atomic`
commit:

```
mixer:0 ctl:0 width:960 height:3664
mixer:1 ctl:0 width:960 height:3664

plane:58 stage:1 ... src_w:1920 src_h:3664 dst_w:1920 dst_h:3664
multirect: mode: 0 index: 0
```

Both mixers correctly split to 960px each (half of 1920) — the CRTC-level
horizontal split is genuinely set up right. Only one plane object exists,
spanning the full 1920px width. Initially suspected this meant the
second half-mixer had no real fetch source bound to it. **Traced this
down and it's a false lead**: `_sde_crtc_check_plane_layout()`
(`sde_crtc.c`) — the function that would auto-split a single wide plane
into left/right halves via `pstate->layout` — opens with
`if (!sde_rm_topology_is_group(&kms->rm, crtc_state,
SDE_RM_TOPOLOGY_GROUP_QUADPIPE)) return 0;`. **This entire mechanism is
QUADPIPE-only** (4-pipe topologies). For `DUALPIPE_DSC` it's a no-op.
Meaning: a single full-width plane spanning both mixers *is* the
correct, expected configuration for this topology — this is Qualcomm's
classic single-SSPP-feeds-both-layer-mixers split-display architecture
(one fetch unit's output bus wired to both LMs simultaneously; each LM's
own hardware cropping/addressing picks its half). Confirmed via
`_sde_crtc_blend_setup_mixer()`'s inner loop (seen earlier this session)
genuinely configuring blend-stage info for the *same* plane against
*both* mixers in turn — not a bug, by design. No missing second plane,
no missing SSPP allocation.

**This means the full signal path from clock to pixel is now traced,
confirmed correct, end to end, for both halves — except exactly one
step:**

1. Both DSI links' clocks: shared PLL, intentional, confirmed locked ✓
2. CTL block: `CTL_INTF_ACTIVE = 0x6`, both interfaces marked active ✓
3. Both LMs: correctly split 960px each, single shared SSPP source ✓
   (QUADPIPE-only auto-split doesn't apply here — not needed)
4. Both INTFs: full timing configuration (HSYNC/VSYNC/active-area)
   byte-for-byte identical and correct ✓
5. **Only the final per-INTF `INTF_TIMING_ENGINE_EN` bit** — intf1 gets
   it, intf2 never does (by an explicit `is_master`-only gate in
   `sde_encoder_phys_vid_handle_post_kickoff()`) ✗

And critically: **forcing step 5 for both (tested earlier this session)
does not work either** — it produces real DSI FIFO overflow/underflow
errors rather than a working display. Given steps 1-4 are now all
independently confirmed correct and identical for both halves, the
remaining gap isn't a missing register write, a missing plane, or a
missing clock — it's that **two independently-triggered SDE INTF timing
engines cannot coexist on two DSI links that share one physical PLL/clock
source**, which is exactly what `DUALPIPE_DSC`'s topology model
(`needs_split_display=true`, implying 2 real independent interfaces)
requires. This looks like a genuine architectural mismatch between the
SDE topology layer (expects 2 independent timing engines) and the DSI
clock-management layer (provides only 1 shared clock domain) for this
specific panel + SoC + driver-revision combination — not a simple
register/config bug reachable by flipping one more bit.

**Where this leaves the investigation, honestly, after a very thorough
session:** every individually-testable layer has been checked and is
correct. The one thing that would close the gap — getting intf2 a
*genuinely independent* clock domain (not just an independent enable bit)
to go with its independent timing engine — hasn't been attempted and
would be a much deeper change (likely requires either DT-level per-
controller clock source assignment distinct from the shared `mux_clks`
path found earlier in `dsi_display.c`, or discovering an
undocumented "dual-link broadcast" hardware mode distinct from both
`DUALPIPE_DSC` and `DUALPIPE_3DMERGE_DSC` that this driver revision may or
may not actually support for this panel). This is the honest, most
promising next avenue if picked up again — not another register check,
but an attempt at giving intf2 its own real, independent clock source
alongside its independent timing engine, rather than either forcing just
one or the other (both already tried, both failed in informative ways).

Build state: kernel source has been rebuilt back to the clean,
non-FIFO-storm-causing state (slave `enable_timing` patch reverted,
`build/out-target/Image` and `build/out-payload-simple/payload/`
regenerated to match) — safe to redeploy as-is. The `dsi_link_hs_clk_set_rate()`
`%u`-fixed diagnostic printk in `dsi_clk_manager.c` is still present and
harmless (confirmed stable across many runs this session); everything
else experimental has been cleanly reverted.

## 2026-10-10, continued: video evidence reframes the whole bug — it's periodic/dynamic, not static

User provided a 5s/30fps phone video (`20261010_110150.mp4`, recorded
during the slave-`enable_timing`-forced-on patched build, while the
FIFO-overflow/underflow storm on `dsi-ctrl-1` was active) of the headset
lenses. Extracted all 150 frames and measured per-frame brightness in
fixed crop regions over each lens (`video_frames/` in the scratchpad, two
representative frames saved as `f30_082.png`/`f30_084.png` in this repo
root for reference) rather than relying on verbal description alone.

**Finding: each lens lights up in brief, mostly-isolated single-frame
flashes (~33ms, one video frame at 30fps — likely even shorter given
120Hz panel refresh), not sustained on-periods.** This matches the user's
own description exactly ("both individually flicker one after another,
they just don't stay on at the same time") and is a materially different
picture than this session's own earlier framing of "intf2 is statically
disabled." A static-disabled interface should never flash at all, even
briefly — these flashes are real, brief, successful frame outputs on
*both* sides, just not reliably/simultaneously.

**The left lens's flashes are periodic, not random**: frames 5, 35, 65
(1-indexed, 30fps) — gaps of **exactly 30 frames = 1.00 second, twice in a
row**. That's far too clean to be coincidental and strongly implies a
software timer/retry loop, not a continuous hardware race. The right
lens's flashes are less clean (frame gaps 11/9/17/37 ≈ 0.3–1.2s) but still
clearly episodic.

Checked the obvious candidate — SDE's built-in ESD (panel-health) status
check timer (`sde_connector.c`, `STATUS_CHECK_INTERVAL_MS`) — **it's
hardcoded to 5000ms**, which does not match the observed ~1s period, so
it's very likely *not* this specific mechanism (though the existence of a
whole ESD-recovery subsystem — `esd_recovery_pending`,
`esd_check_underway`, `dsi_display_set_ctrl_esd_check_flag()` in
`dsi_display.c` — confirms this class of periodic retry/recovery
machinery does exist in this driver and is worth checking further;
the exact 1Hz-ish source has not yet been identified).

**Follow-up, same session: ran the actual continuous-sampling experiment
this suggested, and it overturns the "periodic retry" theory above.**
Redeployed the clean reverted build (no FIFO-storm patch active) and
polled `INTF_TIMING_ENGINE_EN` for both interfaces every 0.1s for a full
10 seconds while `kms_atomic` held a commit (100 samples each,
`date +%s` timestamps confirmed real 10s elapsed, `%N` nanoseconds not
supported by this busybox `date` but second-granularity was enough).
Result: **completely static, zero toggling for the entire window.**
`intf1` (`0x6b800`) goes `0→1` within the first ~0.2s and then stays `1`
for all remaining ~98 samples. `intf2` (`0x6c000`) reads `0` on **every
single one** of the 100 samples, the entire 10 seconds, no exceptions.

This means the ~1 Hz periodic flicker seen in the video is **not** a
general property of the stock driver — it was specific to the
FIFO-overflow-storm situation caused by the (already-reverted)
slave-`enable_timing` patch (two independent timing engines racing/
beating against each other on a shared clock, or the resulting
overflow/underflow errors triggering some error-handling cadence). Under
actual stock/unpatched behavior, `intf2`'s timing engine is **statically,
consistently never enabled** — confirmed now by both the original
single-snapshot read and this 10-second continuous-sampling run. The
"it's a dynamic retry loop, single snapshots are unreliable" framing from
the previous entry was **wrong** and is retracted here; the original
static-disable finding stands and is now more solidly confirmed than
before. (The user's general real-world impression that "it always
flickers" most likely reflects watching many separate ~15s `kms_atomic`
test invocations start and stop over the course of this whole project —
each one legitimately turning the display on then off — rather than an
in-hardware toggle happening *within* one continuous commit.)

**Net effect: back to the original, simpler, next concrete step** (now on
firmer ground than before the video detour): since clocks are fine, CTL
correctly marks both interfaces active, and the slave INTF's full timing
*configuration* is correctly programmed (all confirmed earlier this
session) — and now also confirmed the master-only enable gate is a real,
static, intentional-looking single point of control, not a symptom of
something flickering elsewhere — the actual next place to look is
`sde_crtc.c`/`sde_rm.c`'s layer-mixer (LM1) and SSPP fetch-path binding
for the second physical pipe, to understand whether/how a single-plane,
single-CRTC atomic commit is supposed to result in real pixel data
reaching intf2's side at all, independent of whether its own timing
engine ever gets enabled.

## Redeploy reliability notes (separate from the display investigation)

This session re-confirmed and extended the established pattern: after a
warm `qkx_bootdone` reset, the next `insmod quest_kexec.ko ... execute=1`
frequently fails with one of several generic kernel-module-loader errors
(`Exec format error`, `Invalid argument`, `Device or resource busy`) with
**zero module-side dmesg output** in every case (failure happens in the
kernel's generic module loader before our module's own init runs). A full
`adb reboot` reliably clears this. Separately, genuinely fresh boots
(confirmed via `uptime` showing `up 0 min`) still occasionally fail to
bring up the target kernel on the first `insmod` attempt and fall back to
Android on their own — this is the same probabilistic early-boot
flakiness documented earlier in this file, unrelated to module-table
state, and a same-boot retry (no reboot needed) has resolved it every
time it recurred this session. Battery level (tested as low as ~20-24%,
confirmed net-discharging via `healthd` current readings since the host
Mac's USB port can't actually charge this device while it's also serving
as the data link) was ruled out as a cause of any of these specific
failures — a full reboot or a same-boot retry fixed every instance
regardless of battery state at the time.

## PHY-level cont-splash lead — RULED OUT

Followed up on the external-research lead (Qualcomm-documented bug class:
PHY analog/digital reset must happen before PLL lock, else lane jitter) by
adding diagnostic printks directly inside `dsi_phy_enable()` in
`dsi_phy.c`, logging `is_cont_splash_enabled` and whether
`dsi_phy_enable_hw()` actually gets called, per DSI index.

After a fresh kexec boot + `kms_atomic2` atomic commit, captured (via a
`while true; do dmesg -c >> file; sleep 0.2; done &` polling loop on the
target, since the ring buffer itself gets completely flooded/wrapped
within ~1s by the pre-existing `dsi_display_handle_fifo_overflow` spam —
plain `dmesg | grep` after the fact returns nothing useful, lesson learned
for future diagnostics on this target: always capture to a file
concurrently, never rely on the live ring buffer once the panel is
mid-flash):

```
DSI_0: QKXDBG: dsi_phy_enable idx=0 is_cont_splash_enabled=0
DSI_0: QKXDBG: dsi_phy_enable_hw called for idx=0
DSI_1: QKXDBG: dsi_phy_enable idx=1 is_cont_splash_enabled=0
DSI_1: QKXDBG: dsi_phy_enable_hw called for idx=1
```

Both controllers show `is_cont_splash_enabled=0` and both call
`dsi_phy_enable_hw()` (the real PHY analog/digital reset). So this lead is
**ruled out** — the PHY hardware reset is not being skipped by a stale
cont-splash flag for either interface. Combined with the already-confirmed
fixes (independent `mux_clks_sec` PLL clock routing, `is_master` gate
removed from the SDE timing-engine enable, forced real `dsi_panel_enable()`
DCS command path, and confirmed-working panel GPIO reset/regulator
power-on), every software-driver-layer lead investigated this session now
checks out as individually correct, yet the alternating-flash symptom
reportedly persists. This strengthens the case that the remaining problem
is not in the DSI host driver's per-link configuration at all, and is
either in `loader.c`'s kexec-transition power-domain handling (deliberately
skips a full display power-domain reset to avoid visible flicker during
the warm transition — not yet investigated) or in a lower layer entirely
(SDE layer-mixer/SSPP fetch-path binding for the second physical pipe, per
the note above).

## loader.c inspected directly — theory disproven; three more cont-splash gates found and also ruled out

Read `quest-kexec/module/loader.c` in full: it has zero display-specific
code — no DSI/MDSS/SDE/"splash" references anywhere. It only suspends
GPU (`adreno_pm_suspend`), optionally syncboss/DWC3-gadget/QMP, and the
watchdog, then jumps. So the "loader.c skips a display power-domain reset"
theory doesn't hold up on inspection — whatever display hardware state is
inherited is simply however Android was driving the panel at the instant
of freeze, nothing deliberately preserved/skipped by this module.

Pivoted to one more pass over is_cont_splash_enabled gates in
dsi_display.c, specifically looking for anything that could explain the
FIFO overflow/underflow storm itself (the concrete, measurable anomaly
present in every test this session, dual-pipe or single-pipe) rather than
more cont-splash theorizing. Found three previously-unexamined gated
call sites:
- dsi_display_ctrl_host_enable() — skips dsi_ctrl_set_host_engine_state(ON)
  entirely under cont-splash.
- dsi_display_phy_sw_reset() — skips dsi_ctrl_phy_sw_reset() for all
  controllers under cont-splash.
- dsi_post_clkon_cb() — skips dsi_display_toggle_resync_fifo() (which
  calls dsi_phy_toggle_resync_fifo() per controller, operating directly
  on the PHY's retime/resync FIFO hardware — mechanistically the most
  plausible of the three given the symptom) under cont-splash, specifically
  on every DSI_LINK_HS_CLK change.

That last one looked like a strong, mechanistically on-point lead: it's
the literal hardware FIFO sync operation, gated by the same stale-flag
pattern already confirmed real twice elsewhere. Patched all three to
force through unconditionally (with QKXDBG printks logging
is_cont_splash_enabled at each site), rebuilt, and tested after an
unusually long streak of boot flakiness tonight (2 cycles of warm adb
reboot to clear stale module-table state, then a 3rd attempt that never
reached the target at all, then — at the user's suggestion — a genuine
cold power-cycle of the headset, which was needed this time since su
stayed inaccessible for 110+ seconds after the second warm reboot in a way
plain adb reboot retries weren't resolving; the cold cycle fixed it
immediately).

Result: clean negative. All three printks fired with
is_cont_splash_enabled=0 already — meaning the flag had already
transitioned to false by the time any of these three sites ran, so none
of them were actually being skipped in the live sequence to begin with.
Forcing them through unconditionally changed nothing, as expected once the
flag reads false. Captured the entire enable sequence this time via a
continuous `dmesg -c` polling loop (not just a grep after the fact) to
rule out ring-buffer wraparound hiding anything: 46,296
"fifo_overflow snapshot" lines across the ~15s hold window — same storm,
undiminished.

Where this leaves the is_cont_splash_enabled theory overall: every
gated call site reachable in the display-enable path that this session
could find (dsi_display_enable's panel DCS skip — real bug, fixed;
dsi_phy_enable's PHY HW reset — already false, not skipped;
dsi_display_ctrl_host_enable, dsi_display_phy_sw_reset,
dsi_post_clkon_cb's resync-FIFO toggle — all already false, not skipped)
has now been checked. One real bug was found and fixed this session
(the panel DCS command skip); everything else gated by this flag turns out
to already be evaluating correctly on its own by the time it matters. The
FIFO storm's root cause is not a stale cont-splash flag anywhere in the
DSI display driver. This specific investigative angle is exhausted —
further progress needs to look at the actual clock-rate/timing-parameter
values being programmed (pixel clock vs. byte-clock/lane-rate match for
this panel's mode) rather than more cont-splash-skip hunting.

## Session close-out: clock-rate/timing-value theory also re-confirmed dead; PHY enable register sequence read in full, unconditional and clean

User correctly pushed back on investigating clock-rate/timing-parameter
values as a "fresh" lead: the exact pclk/byte-clk frequencies were already
directly confirmed correct via `clk_summary` earlier this session
(191082240 Hz / 143311680 Hz, both locked, both feeding the right
children), and single-pipe-only (ctrl0 alone) already flickered too, so a
rate-mismatch theory doesn't fit either.

Read `dsi_phy_hw_v4_0_dphy_enable()`/`dsi_phy_hw_v4_0_enable()` in
`dsi_phy_hw_v4_0.c` directly (the actual register-level PHY enable
sequence underneath the already-confirmed `dsi_phy_enable_hw()` call) to
check for any calibration-cache-reuse or status-bit short-circuit that
might silently skip real reconfiguration independent of
`is_cont_splash_enabled`. Found none: every lane/timing/PLL/LDO/rescode
register is written unconditionally from `cfg->timing`/`cfg->lane_map` on
every single call, no shortcuts of any kind.

**This closes out the entire investigative line this session could
pursue through code reading and register/dmesg verification.** Full
tally of what's been individually checked and confirmed either correct
or genuinely fixed:
- Independent `mux_clks_sec` PLL clock routing for ctrl1 — correct, real improvement, kept.
- `is_master` timing-engine-enable gate removed — correct, real improvement, kept.
- `dsi_display_enable()`'s panel-DCS-command cont-splash skip — **real bug, fixed**.
- Panel GPIO reset / regulator power-on — confirmed firing correctly, was never broken.
- `dsi_phy_enable()`'s PHY HW reset cont-splash skip — checked, already false, not a live skip.
- `dsi_display_ctrl_host_enable`/`dsi_display_phy_sw_reset`/resync-FIFO-toggle cont-splash skips — checked, already false, not live skips.
- Exact pclk/byte-clk frequencies — confirmed correct via direct `clk_summary` register read.
- DSC slice/chunk/picture config — confirmed correct via direct register read, matches DT exactly.
- Dual-pipe vs. single-pipe topology — ruled out categorically (single-pipe-only still flickers).
- `loader.c` kexec-transition handling — read in full, zero display-specific code, theory doesn't apply.
- PHY HW enable register sequence (`dsi_phy_hw_v4_0_dphy_enable`) — read in full, fully unconditional, no shortcuts.

One real, confirmed, kept fix landed this session (panel DCS command
path). Every other avenue reachable through source reading plus
register/dmesg verification has been exhausted without finding another
live bug. The flashing/alternating symptom is invariant across every
single one of these configurations, which is itself the strongest
available evidence that whatever remains is not fixable through Linux
driver configuration alone — most plausibly a genuine electrical/analog
PHY characteristic specific to never getting a truly cold (power-on, not
kexec-warm) bring-up, which would need oscilloscope/protocol-analyzer-level
signal inspection to diagnose further, not available in this setup.
Stopping the live-device investigation here for tonight.

## 2026-10-11, new session: bus/RSC bandwidth-vote theory tested directly and disproven; baseline symptom re-confirmed; one new decoded register, inconclusive

**New session, picking up from the "Full tally."** Before touching hardware,
re-read the entire display investigation and formed a new candidate theory:
none of the exhausted avenues (clocks, INTF, CTL, DSC, cont-splash, PHY HW
enable sequence) touch memory-bus/interconnect bandwidth voting for the
display path, and FIFO underflow (observed even in the single-controller
`SINGLEPIPE_DSC` test) is the textbook signature of bus bandwidth starvation,
not a clock/encoder/PHY problem. Cloned the exact kernel source
(`facebookincubator/oculus-linux-kernel` @ `6e428607`) into a scratch dir to
verify this against real code before testing on hardware (not just
speculation): confirmed `techpack/display/msm/sde/sde_core_perf.c` genuinely
routes display bandwidth votes either straight to ICC or through the SDE RSC
hardware sequencer (`sde_rsc_client_vote()`), and found that quest-kexec's
own upstream patch (`kernel/patches/0001-...patch`) already adds bisection
instrumentation to the **legacy `msm_bus` AP-side QoS-programming path**
(`msm_bus_dev_init_qos()`), with a comment describing a real, confirmed
kexec-specific hard-crash bug on one bus node (stale inherited clock-gating
from the prior kernel causing a NoC QoS write to hard-reset the SoC) — this
is genuine, directly-relevant prior evidence that kexec-inherited
bus/interconnect state is a known failure class on this exact SoC family,
not a stretch.

**Deployed the already-built, already-DEBUG_FS/DYNAMIC_DEBUG-enabled
`build/out-target/Image`** (confirmed via the docker volume's `.config`
directly — the committed `build/out-target/target.config` snapshot in this
repo is stale/out of date and should not be trusted; the real build config
lives in the `qkx-target-out` docker volume). Regenerated the boot payload
fresh via `tools/prepare.py` into `build/out-payload-fresh/` (the
`build/out-payload/` dir in this repo predates the final target kernel build
and should not be reused as-is).

**Checked the display bus-bandwidth path directly, two ways:**
1. `/sys/kernel/debug/msm-bus-dbg/client-data/{disp_rsc_ebi,disp_rsc_mnoc_llcc,disp_rsc_reg}`
   — the real RSC-mediated bandwidth-vote client logs for the display path.
2. Cross-referenced DT (`captured-quest2/runtime.dts`): the display's own
   fabric nodes (`fab_gem_noc_display`, `fab_mmss_noc_display`) are marked
   `qcom,bypass-qos-prg` — **by design**, the display bandwidth path skips
   the legacy AP-side QoS-programming mechanism entirely (it's RSC-owned
   instead). So the kexec-crash-prone `msm_bus_dev_init_qos()` path found
   above, while real and interesting, **does not apply to the display path
   at all** — a dead end for this specific theory, confirmed by DT, not by
   assumption.

**With that ruled out, checked the actual RSC-vote delivery path directly.**
While holding a `kms_atomic2` commit: `disp_rsc_ebi`/`disp_rsc_mnoc_llcc`
showed real, sane, non-zero bandwidth requests going active (16.6 GB/s EBI,
8.3+8.3 GB/s mNOC — plausible numbers for this panel's mode, not zero/
garbage), and dmesg showed the underlying RPMh commit
(`qkxbus: commit rsc=8000 writes done ret=0 n=3/5`, `qkxbus: commit rsc
invalidate`) firing repeatedly and succeeding throughout the hold. **This
disproves the "no bandwidth vote happens" theory by direct measurement** —
software requests real bandwidth and the RPMh commit reports success,
repeatedly, same as every prior "software says success" finding this project
has checked. (Could not rule out the deeper "vote succeeds in software but
isn't honored in hardware" version of this theory — same unfalsifiable-
without-a-scope shape as the panel-power-sequencing theory already on
record — no tooling available to check that here.)

**Baseline symptom re-confirmed, correlated live with the user watching:**
ran `kms_atomic2` fresh (note: `/tmp` does not survive a return-to-Android +
re-kexec cycle — re-`wget` the binary from the Mac's `python -m http.server`
every fresh boot, forgot this once this session and got a false "dark"
result from the test tool being missing, not a real symptom). User's live
report: "alternation" — same as every prior session. dmesg showed the same
FIFO overflow storm on both `dsi-ctrl-0`/`dsi-ctrl-1` as always.

**One new thing decoded, inconclusive:** the existing `QKXDBG: fifo_overflow
snapshot ctrl_idx=%d lane_status1=0x%08x` printk (`scratch_patch/dsi_display.c`,
reads DSI PHY offset `0x14C` = `DSIPHY_CMN_LANE_STATUS1` directly) has been
showing `0x1f` on both controllers every time this session but was never
decoded before. Per `dsi_phy_hw_v4_0.c`'s own `stop_state_mask` usage,
`0x1f` = `BIT(0)|BIT(1)|BIT(2)|BIT(3)|BIT(4)` = all 4 data lanes + the clock
lane simultaneously in "stop state" (LP-11/idle) at the instant this handler
runs. Plausibly just the expected post-fault settled state rather than
anything diagnostic of the root cause, since this printk fires from a
workqueue-scheduled handler, not the raw ISR — by the time it runs,
real hardware recovery (`dsi_phy_hw_v4_0_lane_reset()` forces this exact
same `0x1f` pattern via `DSIPHY_CMN_LANE_CTRL3` as part of normal recovery)
may have already happened. Not pursued further; flagging so it isn't
re-discovered as if new without this caveat.

**Also hit and resolved, unrelated to display:** after a real `adb reboot`
(needed once this session when insmod failed with "Invalid argument" and
zero module-side dmesg, matching the documented stale-module-table pattern),
the on-device `/data/local/tmp/qkx-Image` file was found zeroed out at its
header (`ARM\x64` magic missing, all zero) — the kexec module's own
`QKXDBG: fail at kernel header validation` printk confirmed this cleanly.
Root cause not fully pinned down (plausibly: `adb push`'s write wasn't
flushed to flash before `adb reboot`, which may not do a full graceful
sync of all filesystems) — re-pushing the exact same (verified-intact
locally) file fixed it immediately. Worth remembering: after any
`adb reboot` in this project, re-verify payload file headers on-device
before the next `insmod`, don't assume a previously-pushed file survived.

**Net result: another real, evidence-based negative result** (bus/RSC
bandwidth voting checks out as actually working, same as every other
software layer checked across this whole investigation), plus two small
reusable findings (the `bypass-qos-prg` DT fact, and the decoded-but-
inconclusive `lane_status1=0x1f` meaning) for whoever picks this up next.
The actual root cause remains open.

## 2026-10-11, continued: cross-checked against a real working dual-DSI device (Quest Pro) at the DT level — one real difference found and tested, clean negative result

User pushed back on the "Quest Pro must just have a simpler panel" dismissal
from the earlier session (search above for "how the original quest-kexec
repo author... got display working") — rightly: never actually verified,
just inferred from the kernel patch set alone. Checked properly this time by
pulling Quest Pro's own board DTS directly
(`facebookincubator/oculus-linux-kernel`, branch
`oculus-quest-pro-kernel-master`, files `kona-xrfusion.dtsi` /
`dsi-panel-xrsmrtvwr-jdi-dual-video.dtsi`).

**Quest Pro is not simpler — if anything it's a more complex topology**:
`qcom,display-topology = <4 4 2>` (4 layer mixers, 4 DSC encoders, 2 physical
INTFs) vs our `<2 2 2>`, same `qcom,dsi-ctrl-num = <0 1>` (both real DSI
controllers active), same `qcom,dsi-select-clocks = "mux_byte_clk0",
"mux_pixel_clk0"` (shared single clock source for both links, exactly our
baseline, not independent). So Quest Pro genuinely drives two real DSI
controllers off one shared PLL via kexec and it works — this directly
undercuts the entire "shared clock / two controllers can't coexist" framing
this project's own investigation had been converging on. Something other
than the shared-dual-link architecture itself must differ.

Diffed the full set of DT property *names* on both panels' nodes (ignoring
values, since timings/resolution differ). Two meaningfully different
properties found on Quest Pro's panel, absent on ours:
- `qcom,cmd-sync-wait-broadcast` — checked the driver
  (`dsi_panel.c`→`panel->sync_broadcast_en`, used in `dsi_display.c`'s
  `dsi_display_status_reg_read()`): only gates whether the **ESD
  periodic-health-check status read** verifies the slave controller too.
  Not part of the actual command-broadcast path (that one,
  `dsi_display_broadcast_cmd()`, is unconditional for any `ctrl_count > 1`
  panel, ours included) — narrower than it first looked, not tested live.
- `qcom,mdss-dsi-tx-eot-append` — a real DSI protocol-level setting
  (`dsi_panel.c`→`host->append_tx_eot`→`dsi_ctrl_hw_cmn_host_setup()`→
  `DSI_EOT_PACKET_CTRL` register, bit 0) controlling whether an EOT packet
  is appended to DSI command transmissions. Plausible candidate for a
  command-transfer/FIFO-adjacent issue, cheap to test (single boolean DT
  property, no kernel rebuild).

Also checked `dsi_split_link_setup()` (called right above the EOT register
write in the same function) — this is Qualcomm's unrelated "Split Display
Link" feature (one controller's lanes split across two PHYs). Zero DT usage
anywhere in the entire Quest Pro *or* reference vendor tree. Not what either
device uses; ruled out immediately, not a real lead.

**Tested `qcom,mdss-dsi-tx-eot-append` live.** First attempt gave a false
read: the pushed `boot.dtb` and the file actually staged by the kexec module
didn't match (`sha256sum` on-device vs. local disagreed) — same
"`adb push` not flushed before the next operation" issue as the Image-file
corruption earlier this session, now confirmed to also hit the DTB. **New
standing rule for this project: after any `adb push` of a payload file,
`adb shell sync` and `sha256sum` both the local and on-device copy before
`insmod`, every time — don't trust push success output alone.** Once
verified matching (and the property confirmed actually present at
`/proc/device-tree/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_jdi_4k_120Hz_dsc_video/qcom,mdss-dsi-tx-eot-append`
on the live target, not just in the pushed file), ran `kms_atomic2` with the
user watching live: **"alternating" — identical to baseline.** dmesg showed
the same FIFO overflow storm (14k+ lines) as every other test. Clean,
trustworthy negative result this time (unlike the first attempt).

**Where this leaves it:** the two real DT-level differences found by diffing
against a confirmed-working dual-DSI-via-kexec device are now both accounted
for — one (`tx-eot-append`) tested directly and disproven, the other
(`cmd-sync-wait-broadcast`) understood well enough via source to know it
isn't in the critical path (ESD health-check only). The "maybe it's
something trivially different in the DT" angle, motivated by a real working
reference device rather than guesswork, is now mostly closed off too. Still
open: a full line-by-line value diff (not just property-name diff) of the
two panels' shared properties, and the DSI PHY timing-calibration values
layer, both still unread. Returned device to Android cleanly at session end.

## 2026-10-11 — BREAKTHROUGH: first real image on BOTH halves of the panel

**User, watching live:** "I SAW IT ON BOTH SIDES" — the green test
framebuffer visible across both halves of the panel for the first time in
this project. Still visibly imperfect ("looked like a bunch of compression
... on the right side there was a green strip"), i.e. DSC-style artifacts
remain — but the alternating-flash symptom is gone and real pixels reach
both DSI links.

**Exact working state — saved, reproducible:**
- Kernel display patch: `quest-kexec/kernel/display-patches/0001-display-first-image-both-halves-2026-10-11.patch`
  (`git diff techpack/display` in the `qkx-target-ksrc` volume, on top of
  kexec patch HEAD `e53ffe93b2` "qkx: nothing parked by default..."). Contains
  the DCS/cont-splash force-enable fix, the (inactive unless DT selects it)
  `mux_clks_sec` routing, the forced cont-splash gates, and QKXDBG printks.
  **Does NOT contain the `is_master` timing-engine-gate removal** — that is
  the change reverted to get here (see below).
- Exact boot payload: `build/working-display-2026-10-11/` (Image
  `2e6caeb6…`, initramfs `372e484c…`, boot.dtb `9e8fdb55…` = stock DTB, see
  `SHA256SUMS`; `build/` is gitignored, so the patch above is the durable
  record — rebuild with the normal target-Image command after applying it).
- Test command on target: `QKX_HZ=90 QKX_CRTC=1 QKX_HOLD=30 /tmp/kms_atomic3`
  (`drm-test/kms_atomic.c` now takes `QKX_HZ`/`QKX_CRTC`/`QKX_HOLD` env
  knobs; defaults unchanged). Result: **0 FIFO overflows, 0 underflows,
  0 command-transfer failures** (vs ~12,000 overflows per run before).

**How it was found — golden-reference register diff against stock Android
(the method this project had never used):** every earlier comparison was
inside the broken kernel (intf1 vs intf2, dsi0 vs dsi1). Wrote
`drm-test/regdump/qkx_regdump.c`, a read-only module exposing
`/proc/qkx_regdump` (one `block+offset = value` line for CTL0-3, LM0-3,
INTF1/2, PP0-3, DSC0-3, DSI ctrl 0/1, DSI PHY 0/1 incl. PLL; offsets from
the captured DT). Built for both kernels (Android: `qkx-ksrc`/`qkx-out` with
`LOCALVERSION=-g4b63ca9fd613`; target: `qkx-target-ksrc`/`qkx-target-out`).
**Only read while the panel is actively scanning** (unclocked block reads
can reset the SoC). Three dumps, all in `drm-test/regdump/dumps/`:
1. `android_bootloader_state_90hz.txt` — stock Android as booted (display
   state is the bootloader's, adopted via cont-splash).
2. `android_kernel_reinit_90hz.txt` — stock Android after `input keyevent 26`
   twice (display fully off then on), so **Android's own kernel** reprograms
   everything through the same driver code we run. This control is the key:
   any difference between (1) and (2) is just bootloader-vs-kernel
   programming and is proven harmless, since the panel works after (2).
3. `kexec_ismaster_gate_removed_fifo_storm.txt` — our kernel mid-commit.

(2) vs (3) showed the smoking gun: **on stock Android only INTF1's
`INTF_TIMING_ENGINE_EN` is set; INTF2's stays 0** — the split panel is
driven by the master INTF's timing engine alone. Our build still carried
the earlier experiment that removed the `is_master` gate in
`sde_encoder_phys_vid_handle_post_kickoff()` so both engines run. That was
the direct cause of the FIFO overflow storm, and the overflow-recovery
handler then soft-reset the DSI controllers over and over (hence our dump's
`DSI_CTRL` video-mode bit off, `DSI_CLK_CTRL=0`, `DSI_CLK_STATUS=0x1`, all
lanes idle in stop state `DSI_LANE_STATUS=0x1f1f` vs Android's HS
`0x1f00`). Reverting just `sde_encoder_phys_vid.c` to stock → storm gone,
DSI ctrl/PHY state matches Android's kernel-reinit state (only the
content-dependent MISR differs) → image on both halves.

**Corrections to earlier entries in this file (they were wrong):**
- The `is_master` gate is **correct stock behaviour**, not a bug. "intf2
  vsync:0" / "intf2 never engages" was normal: on this hardware the slave
  INTF is never independently enabled, the master's engine drives both.
  The 2026-10-10 entries treating the gate as the problem (and keeping the
  removal "as a real improvement") led the investigation astray from that
  point on.
- Earlier tonight's "pushed file got zeroed/corrupted" explanation is wrong.
  What actually happens: the kexec jump (and the unclean reset back to
  Android) **drops page-cache writes that were not yet flushed**, so the
  file reverts to its *previous* contents (that's why the "bad" DTB had
  exactly the stock DTB's checksum). Rule stands, cause corrected: after any
  `adb push`, run `adb shell sync` and compare `sha256sum` on both sides
  before `insmod ... execute=1`.

**Proven benign by the control (Android's own kernel programs them the
same way and the panel works) — don't chase these:** `DSI_LANE_SWAP_CTRL=1`,
`DSI_EOT_PACKET_CTRL=0` (consistent with the negative `tx-eot-append` test),
DSI PLL SSC registers, PLL fractional divider, `DSI_CLKOUT_TIMING_CTRL`.
Also equivalent: PHY `CMN_CLK_CFG0/1` pixel-clock path (Android mux=1 then
÷3, ours mux=0 then ÷6 — same output).

**Other facts established this session:**
- Stock Android runs the panel at **90 Hz** on **`sde-crtc-1`**
  (`/sys/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/card0/sde-crtc-1/measured_fps`),
  backlight 255. All 61 modes (60-120 Hz) share one 382164 kHz clock (DFPS
  via vertical front porch).
- Stock Android logs **zero** DSI FIFO overflow/underflow; it does log the
  occasional `dsi-ctrl-0: Command transfer failed`, so that line is normal
  noise, not a symptom.
- JDI panel backlight = two BLUs (left/right) strobed at programmable
  scanlines via DCS `0xB9` in `dsi_panel_jdi_update_backlight()`.

**Remaining known differences vs Android (next leads for the
"compression"/green-strip artifacts):** `PP0/PP1+0x0a8` (Android 3, ours 0;
unnamed in `sde_hw_pingpong.c`, sits between `PP_DSC_MODE` 0x0a0 and
`PP_DCE_DATA_IN_SWAP` 0x0ac), `INTF1/2+0x08c` (Android 0x11, ours 0),
`INTF1/2+0x1c4` (Android 0x200, ours 0x600), `DSC0/1+0x128..0x130`, and
TOP `+0x008/+0x010/+0x014` (likely interrupt masks for Android's second,
virtual-display CRTC). Diff command:
`join -j1 <(sed 's/ = / /' A|sort) <(sed 's/ = / /' B|sort) | awk '$2!=$3'`.

## 2026-10-11, later: root cause of the FIFO storm found — pixel clock runs at 2x after kexec

**Measured, not inferred.** Register dump from a storming boot vs the Boot A
dump where the image showed on both halves: the only meaningful difference
is the DSI PHY pixel-clock path. `PHY_CMN_CLK_CFG0` (phy+0x010) pclk divider
`[7:4]` and `PHY_CMN_CLK_CFG1` (phy+0x014) `dsiclk_sel` `[1:0]`:
- Android: ÷3 with sel=1 (bitclk/2) → bitclk/6 ✓
- Boot A (worked): ÷6 with sel=0 (bitclk) → bitclk/6 ✓
- every storming boot: **÷3 with sel=0 → bitclk/3 = 2x pixel clock** ✗
INTF pushes pixels twice as fast as the DSI link drains → overflow every
frame. `clk_summary` still reports the correct rate (the clock framework's
view is right, the hardware isn't), which is why all earlier clock checks
passed.

**Mechanism (dsi_pll_7nm.c):** `vco_7nm_unprepare()` caches `cached_cfg0`
from hardware whenever not in a cont-splash handoff, but caches `cached_cfg1`
only during a handoff. After kexec, prepare restores the bootloader's ÷3
divider from `cached_cfg0` while `dsi_pll_enable()` writes the never-set
`cached_cfg1` (0) into dsiclk_sel. Boot A likely escaped because Android's
screen had timed out before the jump (PLL off, nothing stale to restore).

**Live proof:** `drm-test/regdump/qkx_pclkfix.c` (one-shot module) rewrote
both PHYs `0x31 → 0x61` mid-commit: overflows dropped 12253 → ~1800 per 5 s
(not zero, because the recovery handler had already reset the DSI
controllers by then).

**Fix written, NOT yet verified:**
`quest-kexec/kernel/display-patches/0002-dsi-pll-7nm-cache-pclk-mux-with-divider.patch`
(cache cfg1 alongside cfg0 in unprepare, master and slave). Applied in the
`qkx-target-ksrc` volume and built (Image sha `90ce4f83…`), but 5 kexec
attempts in a row failed to come up and pstore kept no log, so it's unknown
whether the patch or ordinary early-boot flakiness is to blame. Next: boot
the known-good `build/working-display-2026-10-11/` payload to separate the
two, then retry the patched build.

**Ruled out this session (with measurements):** panel bias rails
(`lcdb_ldo`/`lcdb_ncp`, `vddio`, `blu-mcu` are all enabled during commits;
an earlier "disabled" reading was taken after the hold ended),
`display_panel_avdd` (GPIO fixed regulator, no consumer; holding it on via
`drm-test/regdump/qkx_regon.c` did not stop the storm), framebuffer content
(solid vs pattern), and commit timing after boot.

## 2026-10-11 (late): pclk mux fix CONFIRMED on a cold boot; remaining symptom is garbage picture with one correct strip

**Fixed and verified:** `dsi_pll_7nm.c` `pclk_mux_read_sel()` now syncs
`cached_cfg1` (and the slave's) with the hardware mux value the clock framework
just read. Before, `dsi_pll_enable()` wrote the never-initialised `cached_cfg1`
(0) into `PHY_CMN_CLK_CFG1`, switching pclk to undivided bitclk while the
bootloader's /3 divider stayed in `CFG0` -> pixel clock x2 -> FIFO overflow
storm. (The earlier `vco_7nm_unprepare` caching in patch 0002 was NOT the real
fix; the cold-boot trace showed no PLL prepare/unprepare before the first
commit.) Verified on a cold `adb reboot` + kexec, first commit after boot:
enable-pre `cfg1=0x0 cached=0x1` -> enable-post `cfg1=0x1`; **0 FIFO
overflow/underflow, 0 command failures**. Saved as
`quest-kexec/kernel/display-patches/0003-...patch` (full `techpack/display`
diff incl. QKXDBG/QKXPLL prints); payload in `build/working-clockfix-2026-10-11/`;
dump `drm-test/regdump/dumps/kexec_clockfix_cold_boot_90hz.txt`.
Also: a hung `kms_atomic3` (RCU stall on CPU2) happened once and needed a
return-to-Android; cold reboot before kexec remains the reliable procedure.

**Remaining symptom (user, watching):** static/blocks everywhere plus a thin
strip at the far right of **constant width** that shows the correct fill colour
(green fill -> green strip, red fill -> red strip). Rest is garbage not following
the fill. `kms_atomic.c` gained `QKX_COLOR=<hex ARGB>` (built as
`drm-test/kms_atomic4`).

**Compared against Android's own kernel re-init dump:** all DSC0/1 encoder
registers identical (slice 480, 4 slices/line); DSI ctrl registers identical
except MISR; INTF/PP diffs are `INTF+0x1c4` (= `INTF_INTR_STATUS`, status),
`INTF+0x08c`, `PP+0x0a8` (never written by the driver), PHY PLL readback
0x6b8-0x798 (likely calibration status). Panel power/reset/DCS ON sequence runs
with rc=0. So the SoC-side config matches; suspect the panel-side DSC decode
(PPS/init timing) or something not visible in these registers.

**Refresh-rate experiment (solid red, `QKX_COLOR=FFFF0000`, user watching, all 0 FIFO errors):**
the correct-colour strip at the far right gets **wider at 60 Hz, narrower at 120 Hz**
(90 Hz in between, constant width run to run); the garbage blocks are unchanged.
All modes share one 382164 kHz pixel clock and differ only in vertical front porch,
so the good area grows with vertical blanking time. Suggests the panel's decode
degrades during the active scan and recovers in blanking (or similar frame-timing
dependence), not a static config error. Ruled out as causes this round: panel PPS
(DCS 0xE6 in DSI_CMD_SET_ON) matches the SoC DSC encoder exactly; DSC0/1 and DSI ctrl
registers match Android's kernel re-init; PHY0 PLL registers 0x6b8-0x798 differ even
between Android's bootloader state and Android's re-init (calibration status); TOP
0x008/0x010/0x014/0x304 are interrupt status; CTL1 diffs are Android's virtual display.

## 2026-10-11: BREAKTHROUGH — Dual-Plane Hardware Mixer Discovery & Clean Display (Zero Glitches/Static)

### The Root Cause:
The Quest 2 display topology uses dual hardware layer mixers (LM0 and LM1) driving dual DSI interfaces (INTF1 and INTF2) through dual DSC encoders (480-pixel slices, 4 slices per line total = 1920 total horizontal resolution).

When committing a single wide plane (`src_w=1920, dst_w=1920`) via standard DRM atomic modeset:
* The kernel driver attempted to bind a single plane across both layer mixers, but the DSC timing and slice packing degraded during scanout across the boundary, causing the entire left eye and most of the screen to break into static/compression failure, leaving only a thin sliver on the far right where the second pipe recovered during blanking.
* **Stock Android's Secret:** Investigating Android's active plane allocations revealed Android's HWComposer does NOT use a single plane. Instead, it assigns **two distinct hardware DMA planes**:
  * **Plane 101:** Source `[0, 0, 960, 3664]`, Destination `[0, 0, 960, 3664]` (feeding LM0 / left eye)
  * **Plane 98:** Source `[960, 0, 960, 3664]`, Destination `[960, 0, 960, 3664]` (feeding LM1 / right eye)
  * Both planes reference the same framebuffer and are committed simultaneously in a single atomic request to CRTC 129.

### The Fix (`drm-test/kms_atomic_dual.c`):
We implemented `kms_atomic_dual`:
1. Binds **Plane 101** to `CRTC 129` with source rect `[0, 0, 960, 3664]` and dest rect `[0, 0, 960, 3664]`.
2. Binds **Plane 98** to `CRTC 129` with source rect `[960, 0, 960, 3664]` and dest rect `[960, 0, 960, 3664]`.
3. Commits all 32 atomic properties across CRTC 129, Connector 29, Plane 101, and Plane 98 in one atomic transaction (`DRM_MODE_ATOMIC_ALLOW_MODESET`).

### Result:
* Tested in both stock Android (SurfaceFlinger suspended) and pure **kexec Linux**.
* **Confirmed through lenses:** ZERO static, ZERO artifacts, ZERO green strips, and 100% clean solid colors across both eyes!

### Backup Locations:
* Directory: `build/working-dualplane-clean-2026-10-11/`
* Tarball: `/Users/waffles/quest2-kexec-backups/working-dualplane-clean-20261011-230409.tar.gz`
* Git Commit: `f08eefeb8d8127027740710d06ef4277bb1031db`

## 2026-10-12: System Protection Stripping & Clean kexec Transition Procedure

To eliminate Android crash loops, watchdog barking panics (`Panic triggered due to watchdog bark!`), and early aborts prior to jumping into Linux:

### 1. Mandatory Pre-Jump Protection Stripping
Always execute the following on Android before `insmod quest_kexec.ko`:
```bash
# 1. Disable kernel panic escalation on non-fatal warnings
echo 0 > /proc/sys/kernel/panic
echo 0 > /proc/sys/kernel/panic_on_oops
echo 0 > /proc/sys/kernel/panic_on_rcu_stall
echo 0 > /proc/sys/kernel/panic_on_warn

# 2. Set SELinux to permissive
setenforce 0

# 3. Kill background crash reporters & camera watchdogs
for s in cameraserver virtual_camera bootanim bugreportservice crashreporterd; do
    setprop ctl.stop $s
done
kill -9 $(pidof ovr_crashreporter64 ovr_crashuploader64 cameraserver) 2>/dev/null || true

# 4. Cleanly stop TrustZone / TEE listeners (prevents stale buffer locks across kexec)
for s in vendor.oculus.hardware.attestation-service \
         vendor.oculus.devicecert-hal-1-0 \
         gatekeeperd gatekeeper-1-0 \
         vendor.keymaster-4-1 \
         vendor.spdaemon vendor.qseecomd; do
    setprop ctl.stop $s
done

# 5. Drop page caches to free RAM
sync
echo 3 > /proc/sys/vm/drop_caches
```

### 2. Dual-Plane Video Player Execution (Stereo VR Side-by-Side)
In pure kexec Linux, the video player binary `/bin/kms_play_vr` decodes and displays stereo video containers directly to the dual layer mixers (Plane 101 for Left Eye + Plane 98 for Right Eye):

> [!IMPORTANT]
> The Meta Quest 2 uses a single physical 3664×1920 panel mounted in portrait orientation (`1920` wide × `3664` high).
> Video frames must be formatted as side-by-side stereo (Left: [0..1832], Right: [1832..3664] on a 3664×1920 landscape canvas), and then rotated 90° clockwise into 1920×3664.
> KMS Planes:
> - Plane 101 (Left Eye): bounds `[0, 0, 960, 3664]`
> - Plane 98 (Right Eye): bounds `[960, 0, 960, 3664]`
> Launching without killing previous KMS master processes will result in `errno=13` (`EACCES`). Always kill active players first.

```bash
killall -9 kms_play_vr kms_play kms_atomic_dual 2>/dev/null || true
/bin/kms_play_vr /tmp/testvideo_vr.qkxv
```
To run fastfetch over the network gadget on host PC:
```bash
telnet 10.42.0.2 23
fastfetch
```

### 3. Early-Boot RCU Stalls (Crypto / SPMI / RCU)
Early kexec boot can occasionally hit a non-fatal scheduling delay during `cryptomgr_notify` / `aes_init` / `spmi_cleanup_irq`. With `panic=0` and non-fatal watchdog handling, the kernel boots through. A clean retry after reboot reliably succeeds.

---

## Verified Backup Snapshot: Stereo Dual-Plane Video (2026-10-12)

Verified working configuration where pure kexec Linux boots, Arch Linux ARM fastfetch renders, and full stereo VR video plays smoothly across both eyes.

* **Tarball Archive**: `/Users/waffles/quest2-kexec-backups/working-video-dualplane-20261012-081950.tar.gz` (55 MB)
* **Standalone Backup**: `/Users/waffles/quest2-kexec-backups/working-video-dualplane-20261012-081950/`
* **Repository Mirror**: `build/working-video-dualplane-2026-10-12/`

### Verified SHA256 Checksums:
```text
9189f98f0920e524bb1e19d5cde0ad469480091beff8a8af2bb75967fd7b04db  boot.dtb
684a8462c7bb61ccfd0da1f635e77425fe5d97ff7dc150709491ec6a00dd1539  Image
881f4cd91096663d9219e2829b9d163bc6c737e7af3c68766a882156f1dd2b14  initramfs
e1ec6fcf697c09291a466a14f48c84651faba9609499aa5d7bd5d88b618061bf  quest_kexec.ko
b1adc405633727ec01aa469f3529991b635da4892aef3246337b7f54029ac1b2  kms_play_vr
a10cca5bb020e183361ad40bd4fd3e5498599b22ddff90542a09d99aae806712  testvideo_vr.qkxv
```

## 2026-10-12/13: Holo desktop on panel + tracking bring-up (IMU/controllers)

### DE: Stage-1 Holo desktop WORKS on the panel (verified in-headset)
- Xvfb 3664x1920 + openbox + per-eye xterms, presented via new
  `drm-test/kms_dual_image` (XWD snapshot -> 90deg rotate -> proven
  dual-plane atomic commit, planes 101/98). Refreshes on file mtime.
- Rotation ground truth (user-measured, corners A/B/C/D card): CCW, no
  swap (`QKX_ROT=1`). The video path's transpose=1 was never orientation-
  verified; trust eyes, not history.
- Per-eye visible bands measured with a numbered ruler card: left eye
  sees landscape x 400-1200, right eye 2400-3600, ~1200px bridge gap
  between (nose bridge + lens falloff). Keep all content inside bands;
  single-window-spanning-the-seam does NOT fuse. Two contained terminals
  (verified via xwininfo + snapshots) is the correct Stage-1 layout.
- X clients render +770px right of tree position (constant, virgin
  server, no WM; root-caused empirically with 4-marker test, mechanism
  unknown). Compensate placement by -770 (positive coords only; negative
  geometry hits right-anchor semantics).
- Input: `drm-test/xtest_dl` (dlopen libX11/libXtst, XTEST inject from a
  command file) verified live with xev (keycode 46) + typed text on
  screen. Raw-protocol xtest_inject kept for reference (XTestFakeInput
  key/button = 8 words, motion = 9; Xvfb rejects wide GetKeyboardMapping).
- fastfetch renders on-panel (llvmpipe, 10.42.0.2, overlay).
- Plasma 6.5 stack installs (plasma-desktop/workspace, kwin, konsole,
  dolphin) but kwin+plasmashell together OOM-wedge the target twice.
  Lean path (kwin_x11 manually, no session/autostart) assembled but
  untested. Plasma needs small-batch installs + immediate pkg-cache
  cleans (tmpfs!) and `yes ""` for provider prompts (--noconfirm alone
  does not answer number prompts).
- Infra: 4G /tmp + 1G zram swap on target; Mac NAT (pf + forwarding)
  for ECM internet; IPv6 off on usb0 + gai.conf (pacman stalls on AAAA
  otherwise); pacman SigLevel Optional (no keyring dance on Holo);
  static IPv4 http server binds (IPv6-only socket fails ECM clients).
- Tools: `tools/qkx-save.sh` + `qkx-save-pull.sh` (overlay-upper save/
  restore over ECM HTTP, SHA256SUMS). Full FS backup pulled 2.5GB/100k
  files to backups dir. `persist` partition (64M) backed up (serials +
  IPD/lens/display calibration!) -- kept OUT of git (unit PII).
- NAND verdict: userdata FBE-encrypted (superblock verified random),
  hardware-bound keys, unmountable from kexec. Repartitioning refused
  (brick risk, locked bootloader, no EDL). sda7 (xros_b, 128M) reads
  all zeros (candidate scratch, unidentified -- NO writes). persist as
  boot-kit is the safe NAND use. USB-OTG SSD is the real persistent
  distro path (needs a dongle).

### Tracking: full map, IMU one message away
- Controllers = standard BT HID gamepads (MODALIAS b0006, gamepad
  keys/hats/6 ABS axes) via Oculus runtime virtual input (uinput-style,
  creator closes fds; BT link bypasses BlueDroid tracking). Live
  BTN_TR/TR2 events captured. BT HID pairing viable IF HCI comes up;
  HCI blocked on QCA6390 power sequencing (rfkill stuck, no slim
  slaves, btfm silent; needs Android power-sequence capture).
- HMD IMU = ICM42688 behind syncboss MCU (SPI), NOT direct Linux bus.
  Factory + online + temperature calibration + all 4x OV7251 camera
  calibrations CAPTURED from sensorservice (backups dir, not git).
- kexec link proven live end to end: GENI-SPI narrow re-enable (serial/
  I2C stay off -- dodges the SMMU boot hang), nsync DT enable, syncboss
  probe clean, /dev nodes up, MCU wakes in 184ms, timesync traffic
  flows, correct 248B framing (Android uses 248, not 512).
- libsyncboss.so RE'd: IMU = packet type 80; NO subscribe write in the
  enable path (pure userspace state + stream reads); stream filter
  ioctl documented; powerstate replays event on open; ENABLE_TIMESYNC
  (type 8) sent with no observed effect.
- Syncboss API inventory: syncboss_{init,imu_enable[_id],
  imu_set_data_rate,imu_get_reg,stream_open/decode/filter},
  powerstate_{init,wait}, direct-channel dma (needs DT node absent
  everywhere incl. stock; driver needed 4.19 backports: dma-resv shim,
  uaccess include, DMA_BIT_MASK, MODULE_IMPORT_NS guard -- all in tree).
- Direct-channel kernel build HANGS dark twice (GENI image boots fine);
  cause undetermined (pstore overwritten by Android both times).
  Suspect the probe path, not the DT node (absent node = dormant).
- OPEN: the one trigger message for IMU flow. Candidates: HAL
  subscribe bytes (needs strace/LD_PRELOAD on Android -- static strace
  6.1 cross-built and ready in /tmp/strace-build), nsync-lock gating
  (nsync IRQ registration fails with flags mismatch), direct-channel
  config (needs DT node + working build).
- ARTIFACTS: /tmp/vrhal (sensors-service, vrsensorsctl, libsyncboss*.so,
  disassembly), /tmp/sbhal (syncboss driver sources), static strace
  pushed to /data/local/tmp/strace on-device. Kernel trees exported
  post-session (docker-volumes/*-20261013.tar).

## 2026-10-13: userdata FBE keys extracted live (Quest 2 NAND rootfs path)

Goal: mount encrypted userdata (sda9, 54G) from kexec kernel. Result:
**all 4 raw master keys captured from the running kernel.**

- Root was lost after the hang-test reboot (slot _a, stock boot); re-rooted
  via Magisk app, `su -c id` = uid 0 again.
- Recon (rootless+root): FBE v1, `filenames_mode=aes-256-cts`,
  `uses_fs_ioc_add_encryption_key=true`; /data = dm-49 = plain linear 1:1
  over sda9 (name=userdata, single slave, no dm-crypt layer). Metadata/content
  encryption is inline (mount opt `inlinecrypt`, contents mode 127=PRIVATE).
- /proc/keys: 4x `._fscrypt` master keys (in `fscrypt-dm-49` keyring) + 4x
  `fscrypt-provisioning`. No `keyctl` binary on stock; Magisk busybox lacks
  the applet. Wrote `tools/kdump.c` (raw syscalls, static) instead.
- keyctl READ is refused by design: provisioning keys EOPNOTSUPP (no .read),
  `._fscrypt` keys EACCES (poss=SEARCH only). A `keyperm.ko` perm-flip (via
  `key_lookup` address from /proc/kallsyms after lowering kptr_restrict;
  kallsyms_lookup_name is NOT exported on stock) moved EACCES->EOPNOTSUPP,
  proving `._fscrypt` also has no .read (payload = kernel struct pointer).
- `tools/keyperm/kextract.c` dumps `mk_secret` directly (exact layouts via
  the real `fs/crypto/fscrypt_private.h`; RANDSTRUCT off, configs verified
  identical). Identifier-vs-description match validated every key.
- Finding: **all 4 secrets size=64, is_hw_wrapped=0 = RAW v1 keys, no TEE
  dependency.** Raw 64B blobs + map in
  `/Users/waffles/quest2-kexec-backups/fbe-keys-20261013/` (600, NOT in git):
  - `ab9b06a9...` device DE, `ff56a2ac...` CE user0, `ecf433dd...` DE user0,
    `1335229a...` per_boot ephemeral (init log confirms).
  - Policies v1, contents PRIVATE(127)/filenames CTS(4) (`tools/kpolicy.c`).
- Still open: target needs `CONFIG_F2FS_FS_ENCRYPTION` (currently unset;
  FS_ENCRYPTION/F2FS/INLINE_CRYPT present) + ICE inline-crypt path for
  contents mode 127; per-boot bundling script (extract->initramfs->ADD_KEY
  ioctl->mount sda9) not yet written. Keys must be re-extracted per boot.
- Committed: `tools/kdump.c`, `tools/kpolicy.c`, `tools/keyperm/` (d0a1452).

### Corroboration (independent path, same session)
- `tools/kcap.c` (ptrace tracer, no kernel module) captured vold's
  ADD_ENCRYPTION_KEY for CE-user0 at unlock: desc ff56a2ac60835a6c,
  raw `d56dcaab...fbe89b` -- BYTE-IDENTICAL to the kextract line above.
  Two independent methods, same key. Saved (0600) also at
  `backups/ce-user0-key/` (descriptor.txt/key.bin/key.hex).
- Lock/unlock recipe that worked: set PIN (`cmd lock_settings set-pin`),
  soft-reboot left CE invalidated, `cmd lock_settings verify --old`
  reinstalls under trace. Oculus lockscreen never calls lockUserKey;
  ILockSettings (A14) has no lock/unlock -- use `mount`-service 64/65
  and IVold lockCeStorage=45/unlockCeStorage=44/getUnlockedUsers=43.
- WARNING: `service call vold 36` (initUser0) hangs the caller and
  soft-reboots system_server. Otherwise harmless (keys reinstall).
- Target kernel gap CONFIRMED from qkx-target-out/.config:
  `# CONFIG_F2FS_FS_ENCRYPTION is not set` (FS_ENCRYPTION=y,
  BLK_INLINE_ENCRYPTION=y + FALLBACK=y, FS_ENCRYPTION_INLINE_CRYPT=y
  all present). Needs a target rebuild with F2FS_FS_ENCRYPTION=y
  before sda9 mount can decrypt. Device-side state restored: PIN
  cleared, guest users gone, 30G free on userdata.

## 2026-10-13 (cont.): NAND path staged end to end, pending live kexec test

Screenshot thesis ("grab the keys while Android is booted, mount data from
the next kernel") is exactly this path; all host-side pieces are now built
and individually verified, live kexec mount test still open.

- Target kernel gap CLOSED: `qkx-target-out/.config` now has
  `CONFIG_F2FS_FS_ENCRYPTION=y` (plus `UFS_CRYPTO=y`, `CRYPTO_QTI=y`,
  `BLK_DEV_NBD=y`). `build/out-target/Image-nand` refreshed from the live
  docker build (sha `ff273d85...`, same as `Image-nbd` — one kernel covers
  both NAND and NBD paths now).
- `quest-kexec/initramfs/init-nand.sh` rewritten: fetches multi-key
  `nand-keys.txt` (`DESC RAW` per line) with fallback to legacy single-key
  `nand-key.txt`; mounts sda9 ro, ADD_KEYs every key via `/bin/kadd`,
  proves decrypt via `/userdata/media/0/qkx-root/bin/sh`; `qkx_nand=rw`
  now bind-mounts dev/proc/sys + tmpfs tmp/run before
  `switch_root /newroot /sbin/init` (previous version bound only the
  rootfs, which switch_root would have choked on).
- New tooling (all in `tools/`, committed): `build-initramfs-nand.sh`
  (reproducible pack via `initramfs/build.py --extra kadd:bin/kadd`),
  `nand-extract-keys.sh` (live `kextract.ko` -> dmesg scrape -> 0600 keys
  file in /tmp, never in git), `nand-serve-keys.sh` (ECM HTTP on :8000,
  serves both `nand-keys.txt` and legacy `nand-key.txt` derived from the
  CE-user0 line).
- Verified live tonight (read-only, no reboot): fresh extraction returns
  the same 3 persistent raws as yesterday byte-for-byte (device-DE
  `ab9b...`, DE-user0 `ecf4...`, CE-user0 `ff56...` — stable, no lock
  change rotated them) plus a 4th live descriptor `fb2aea5de1ecf5a2`
  (scope unidentified; not on any of /data /data/media* /data/misc
  /data/system /data/data /data/user* — carried along harmlessly).
  `kpolicy` confirms `/data/media/0` + `qkx-root` are both CE-user0
  (v0, contents 127 PRIVATE/inlinecrypt, filenames 4 CTS), so the single
  CE key unlocks the rootfs path; `kadd` re-ADD of the live CE key
  returns ok. Staged rootfs is plain Arch ARM 2.0G at
  `/data/media/0/qkx-root` (writable f2fs — solves the squashfs
  read-only limitation).
- Boot payload prepared (not booted): `build/out-payload-nand/` (gitignored)
  via `tools/prepare.py` with `qkx_nand=ro` (safe default: prove decrypt,
  stay in telnet shell). Test recipe: `nand-extract-keys.sh`,
  `nand-serve-keys.sh`, push Image/initramfs/boot.dtb + protection-strip,
  `insmod ... execute=1`, expect `DECRYPT OK` over telnet, then retry with
  `qkx_nand=rw` for the UFS Arch switch_root. Device at 33% + Charging
  (USB) at stage time.

### Live NAND ro-proof attempt #1 (2026-10-13 eve): target did not come up
- Pre-flight: payload + module sha256-verified both sides after `adb sync`
  (Image `ff273d85...`, initramfs `254e9fbd...`, boot.dtb `c6ea2272...`,
  ko `26ef219d...`), protection-strip applied, key server on :8000 serving
  fresh-extracted 4-key file (extraction re-verified byte-identical to
  yesterday). `uname -r` confirmed stock `g4b63ca9fd613`, slot `_a`.
- `insmod ... execute=1 preserve_watchdog=1 watchdog_recovery=0
  core_hang_control=2 flush_rpmh=1 suspend_syncboss=1 disconnect_qmp=0
  phase_delay_ms=300` → adb dropped (rc 255), expected.
- After 18s + retries: no adb, no ping on 10.42.0.2, NO key fetch on the
  HTTP server (target never reached init-nand.sh's wget). ~1 min later adb
  reappeared on stock Android (`uname` = stock, uptime 0) → early-target
  crash → warm reset. Root lost (Magisk app present, daemon inactive —
  same as the earlier hang-test reboot; needs app re-root).
- NOT yet diagnosed (needs root for pstore/ramoops). Same shape as the
  documented Oct-11 flakiness (new Image failing to come up repeatedly
  with no pstore). Retry plan after re-root: pull pstore, confirm module
  table clean (fresh boot, so no ENOEXEC expected), re-push + sync +
  re-verify (page-cache revert rule), retry insmod once or twice.

### Live NAND ro-proof attempt #2: same early failure, root lost again
- Same verified payload, re-stripped, `insmod` again (module absent from
  lsmod beforehand, so no ENOEXEC concern). Same outcome: no ping, no key
  fetch (both HTTP hits were localhost self-checks), warm reset to stock,
  root lost again.
- pstore from attempt #1 (pulled after re-root) is the real find: the
  target (kernel #41, docker build Oct-14 01:32 UTC) panics at ~10.4s in
  `do_initcalls`, BEFORE any initramfs init runs:
  `msm_bus_debugfs_init+0x348 → debugfs_create_file → start_creating →
  strlen` on garbage (`x0=0x42180`, "user address but active_mm is
  swapper") → `Kernel panic - not syncing: Attempted to kill init!`
  Full log in `/tmp/ramoops-nand1.txt` (Mac, not committed).
- Cmdline red herring resolved: the log's `Kernel command line` shows
  stock args only because console-ramoops truncates the ~1425-char line
  mid-word (`androidboot.seri...`); the staged DTB was verified to contain
  `qkx_nand=ro rdinit=/init ...` via both raw offset search and `fdtget`.
  Loader passes DTB through untouched (no bootargs code in loader.c).
- Prime suspect is kernel-build delta, not the NAND userspace: Oct-09
  volume .config had `# CONFIG_BLK_DEV_NBD is not set` +
  `# CONFIG_F2FS_FS_ENCRYPTION is not set` (but DEBUG_FS/DYNAMIC_DEBUG
  already y — same as now). Current build adds at least NBD +
  F2FS_ENCRYPTION; full .config diff vs the last known-good boot kernel
  (Oct-12 working-video Image, 26617872 B) not yet done. The crash site
  (bus NoC debugfs with a garbage node name) also fits the documented
  stale-RPMh/bus-state flakiness class (`ldoa15 rpmh_write rc=-110`
  timeout in the same log), so state-dependence isn't ruled out — but two
  identical failures in a row points at the build first.
- Next (needs re-root + approval): (a) pull attempt-#2 pstore, confirm
  same crash; (b) sanity-boot the Oct-12 known-good Image+initramfs to
  prove the kexec path/device-state is healthy (no NAND decrypt expected
  — old kernel lacks F2FS_ENCRYPTION — shell-only signal); (c) rebuild
  target with minimal delta (F2FS_ENCRYPTION only) and bisect if needed.

### NAND attempts #1-3: GENI regression found via .config archaeology
- Attempt #2 pstore (pulled after re-root): DIFFERENT crash than #1 —
  target reached 40s, UFS probed (`sda: sda1..sda9` visible!), but PID 1
  stuck in `deferred_probe_initcall → flush_work`, RCU stalls from 31s,
  QKX watchdog reset at 40s. kworker spinning in `async_run_entry_fn`.
  The GENI stack was all active (qupv3 SE probes, spi_geni completed,
  syncboss/cm710x probing) — the exact Oct-10 hang signature.
- Attempt #3 (identical payload): same no-ECM outcome; pstore not yet
  pulled (root lost again). 0/3 → stopped retrying, fixed the build.
- `.config` diff across volume backups nails it: Oct-09 (known-good era)
  had `SPI_QCOM_GENI`/`QCOM_GENI_SE` OFF; the Oct-12/13 tracking work
  turned both ON (for the syncboss/IMU narrow re-enable) and never
  reverted. Oct-10 NOTES disabled exactly these two for the GENI-SMMU
  deferred-probe hang. Full functional delta was small: GENI x2 (the
  bug), NBD + F2FS_ENCRYPTION (wanted), SENSORS_SSC + SYNCBOSS_DEBUG +
  DIRECTCHANNEL (tracking; DIRECTCHANNEL already back off).
- Fix applied in `qkx-target-out/.config` via `scripts/config --disable
  SPI_QCOM_GENI --disable QCOM_GENI_SE` + olddefconfig (verified both
  `is not set`; ION/SERIAL_GENI/I2C_GENI confirmed still off;
  F2FS_ENCRYPTION + NBD kept; SENSORS_SSC kept as suspect #2 if the hang
  persists). Target Image rebuilding in docker; will stage as new
  Image-nand + fresh payload and re-attempt (needs re-root).

### GENI-fix kernel built (Image `3f4410fb...`), payload staged
- Docker build clean (BUILD_RC=0). Old GENI-enabled Image kept as
  `build/out-target/Image-nand-geni` (gitignored) for bisection reference.
  Fresh `build/out-payload-nand/` prepared with `qkx_nand=ro`.
  Awaiting re-root for attempt #4.

### Attempt #4 (GENI-fix kernel `3f4410fb`): BOOTS, ECM works, UFS reads wrong
- Target booted clean past initcalls (no msm_bus panic, no deferred hang —
  GENI revert fixed the boot). ECM up (ping 0.7ms), fetched `nand-keys.txt`
  from Mac (server log `GET 200` from 10.42.0.2), `sda: sda1..sda9` probed.
- `mount -t f2fs -o ro /dev/sda9` FAILS: `Magic Mismatch, valid(0xf2f52010)
  - read(0xd57d911f)` (both superblocks). Keys never got tried — block
  content is wrong before FBE matters.
- Read forensics (live target shell): sda9 size OK (56575275008 B ≈ 54G),
  sda9@0x400 = deterministic random; re-mount after `drop_caches` gives
  BYTE-IDENTICAL garbage (stable-wrong, not flaky transport). sda7 = zeros
  (matches Android), sda8@0x400 = structured non-random, but sda LBA1 (GPT
  header) + LBA2 (entries) = ALL ZEROS — yet the kernel parsed 9 partitions
  at probe. No UFS/SCSI errors in dmesg. LUN geometry sane (sda 63GB = only
  big LUN; `sde` 574MB; no reorder suspect).
- Working theories (need Android ground truth): (a) partitions came from
  DT, not a live GPT read (stale/shifted map → wrong region, stable
  garbage); (b) UFS ICE decrypting with stale keyslots (deterministic
  garbage on encrypted regions; GPT zeros unexplained). Next: after
  re-root, dump Android's sda9@0x400 + LBA1 + `/sys/block/sda/sda9/start` +
  `dmsetup table dm-49`, compare byte-for-byte with tonight's reads.
- Warm-reset via `echo r > /proc/qkx_bootdone` (~12s back to Android).
  Root lost again (expected). Evidence: server log + this entry; no pstore
  needed (no crash — init ran to the telnet loop).

### Attempt #4 post-mortem: sda9 raw is ciphertext — dm-49 is dm-default-key
- Android ground truth (rooted): sda9 start/size IDENTICAL both kernels
  (13274304/110498584) — map theory dead. Decisive: stock Android reads
  the SAME garbage at sda9@0x400 (`1f 91 7d d5...`) and zeros at LBA1.
  The kexec kernel reads perfectly; the flash genuinely holds ciphertext.
- `dm-49@0x400` = valid f2fs magic `10 20 f5 f2`. dm-49 (name=userdata,
  size == sda9 size, single slave sda9) TRANSFORMS: the Oct-13
  "linear 1:1, no dm-crypt layer" recon was wrong.
- New tool `tools/dmstat.c` (static, DM_TABLE_STATUS ioctl, read-only):
  dm-49 type = **`default-key`** (Google inline-crypto target, params
  empty in status). fstab confirms: `fileencryption=ice:aes-256-cts:v1,
  keydirectory=/metadata/vold/metadata_encryption` — metadata crypto is
  ICE inline, no software dm-crypt.
- Read the driver (`drivers/md/dm-default-key.c` in qkx-ksrc): legacy
  table `AES-256-XTS <keyhex> <dev> <start>` auto-expands to
  `aes-xts-plain64 ... sector_size:4096 iv_large_sectors`, byte-swaps the
  key per-u32, and (partitioned dev) adds `start_sect` to iv_offset.
  Offline XTS test vs live sda9 bytes: **MAGIC-MATCH** with the backup
  `metadata-key/key.bin` (be32-swapped, DUN=(13274304+0)>>3=1659288,
  crypto-sector 0, magic at +1024). So key.bin IS the live metadata key
  (all prior plain64/plain XTS attempts failed only on endianness+DUN).
- Target already has `CONFIG_DM_DEFAULT_KEY=y`. New tool `tools/dmcreate.c`
  (DEV_CREATE+TABLE_LOAD+resume) is now bundled in initramfs-nand alongside
  dmstat; init-nand.sh recreates `userdata` mapping from a fetched
  `nand-metadata-key.txt`, mounts the MAPPED device, then kadds file keys.
  `nand-serve-keys.sh` serves the metadata hex too. Rebuilt initramfs
  (2091810 B). Next: fresh payload + attempt #5 (needs re-root).

### Attempt #6 (same dm payload): ✔ DECRYPT OK — UFS rootfs path proven
- Roulette passed this time (same Image/initramfs as #5: early-boot hangs
  are state-dependent, GENI config irrelevant — #4 booted GENI-on, #5 hung
  GENI-off, #6 booted GENI-off).
- Target log: `sda9 present` → dm mapping created → **`/dev/dm-0 mounted
  ro` → `added 4 key(s)` → `DECRYPT OK: qkx-root readable` →
  `os-release present`**, `nand_ok=1 mode=ro`. Both key files fetched
  (server log 200s from 10.42.0.2). Live shell confirms: qkx-root has
  os-release + executable sbin/init, and /userdata shows real Android
  content (app, app-asec, ...) — full userdata decrypt, not just the CE
  path. device-DE + DE-user0 + CE-user0 (+fb2a) all ADD_KEY'd.
- This is the screenshot thesis working end to end: keys grabbed while
  Android booted (kextract + metadata unwrap), data partition mounted
  from the kexec'd kernel via recreated dm-default-key + fscrypt keys.
- Next: `qkx_nand=rw` payload → remount rw → switch_root to Arch systemd
  on UFS. Needs return-to-Android + re-kexec (can't switch_root from the
  ro shell — not PID 1).
