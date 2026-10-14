#!/usr/bin/env bash
# Regenerate the NAND boot payloads (ro + rw) from current build outputs.
# Usage: tools/build-payload-nand.sh
# Bakes the standard NAND cmdline (incl. initcall_debug for hang forensics).
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -f captured-quest2/runtime.dtb ] || [ ! -f captured-quest2/iomem ]; then
  echo "missing captured-quest2 artifacts" >&2; exit 1
fi
BASE="$(sed -e 's/softdog.soft_panic=1//' captured-quest2/cmdline)"
BASE="$BASE rdinit=/init nokaslr printk.devkmsg=on qkx_qmp_adopt=1"
BASE="$BASE initcall_debug ignore_loglevel initcall_blacklist=virtual_sensor_driver_init,msm_bus_debugfs_init"
# The kexec transition leaves some hardware (camera stack, syncboss MCU
# link) in states that wedge deferred probes and stall PID 1 forever
# (boot roulette). These drivers are irrelevant to the NAND Arch path
# (no camera/IMU needed for a shell/distro), so keep them unregistered
# here. Display tests use their own payload/cmdline and are unaffected
# (mdss/dsi/sde are separate drivers, NOT blacklisted).
BASE="$BASE,cam_cci_init_module,cam_cci_late_init,cam_req_mgr_late_init"
BASE="$BASE,cam_a5_init_module,cam_actuator_driver_init,cam_bps_init_module"
BASE="$BASE,cam_cdm_intf_init_module,cam_cpas_dev_init_module,cam_csiphy_init_module"
BASE="$BASE,cam_custom_csid_driver_init,cam_custom_dev_init_module,cam_custom_hw_sub_module_init"
BASE="$BASE,cam_eeprom_driver_init,cam_fd_dev_init_module,cam_fd_hw_init_module"
BASE="$BASE,cam_flash_init_module,cam_hw_cdm_init_module,cam_icp_init_module"
BASE="$BASE,cam_ife_csid17x_init_module,cam_ife_csid_lite_init_module,cam_ipe_init_module"
BASE="$BASE,cam_isp_dev_init_module,cam_jpeg_dev_init_module,cam_jpeg_dma_init_module"
BASE="$BASE,cam_jpeg_enc_init_module,cam_lrme_dev_init_module,cam_lrme_hw_init_module"
BASE="$BASE,cam_ois_driver_init,cam_req_mgr_init,cam_res_mgr_init"
BASE="$BASE,syncboss_direct_channel_init,syncboss_miscfifo_init,syncboss_nsync_init"
BASE="$BASE,syncboss_powerstate_init,syncboss_init,syncboss_timesync_init"
BASE="$BASE,cam_sensor_driver_init,cam_smmu_init_module,cam_sync_init,cam_vfe_init_module"
cd quest-kexec
python3 tools/prepare.py --live-dtb ../captured-quest2/runtime.dtb \
  --iomem ../captured-quest2/iomem \
  --kernel-image ../build/out-target/Image-nand \
  --initramfs out/initramfs-nand.gz \
  --output ../build/out-payload-nand \
  --cmdline "$BASE qkx_nand=ro" > /dev/null
python3 tools/prepare.py --live-dtb ../captured-quest2/runtime.dtb \
  --iomem ../captured-quest2/iomem \
  --kernel-image ../build/out-target/Image-nand \
  --initramfs out/initramfs-nand.gz \
  --output ../build/out-payload-nand-rw \
  --cmdline "$BASE qkx_nand=rw" > /dev/null
ls -l ../build/out-payload-nand/ ../build/out-payload-nand-rw/ | grep -E "initramfs|dtb"
echo "payloads regenerated (initcall_debug on)"
