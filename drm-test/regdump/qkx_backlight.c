// SPDX-License-Identifier: GPL-2.0
/*
 * Live test module to adjust Quest 2 JDI rolling backlight:
 * Sends DCS command 0xB9 with custom (start_r, period_r, start_l, period_l)
 * to test continuous illumination across the full active frame.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <video/mipi_display.h>
#include <drm/drm_mipi_dsi.h>

#include "dsi_defs.h"
#include "dsi_panel.h"
#include "dsi_display.h"

static unsigned int start_r = 0;
static unsigned int period_r = 4914;
static unsigned int start_l = 0;
static unsigned int period_l = 4914;
static int execute = 0;

module_param(start_r, uint, 0644);
module_param(period_r, uint, 0644);
module_param(start_l, uint, 0644);
module_param(period_l, uint, 0644);
module_param(execute, int, 0644);

typedef int (*fn_get_active)(void **displays, u32 count);
typedef int (*fn_dcs_write)(struct mipi_dsi_device *dsi, u8 cmd, const void *data, size_t len);

/* Exact live symbols from /proc/kallsyms on running kernel */
static fn_get_active get_active_fn = (fn_get_active)0xffffff8008ea3628ULL;
static fn_dcs_write dcs_write_fn   = (fn_dcs_write)0xffffff80088331e4ULL;

static int __init qkx_backlight_init(void)
{
	struct dsi_display *disp = NULL;
	struct dsi_panel *panel = NULL;
	u8 reg = 0xB9;
	u8 payload[16] = {0};
	int count, rc;

	count = get_active_fn((void **)&disp, 1);
	if (!count || !disp) {
		pr_err("qkx_backlight: no active dsi_display found (count=%d disp=%p)\n", count, disp);
		return -ENODEV;
	}

	panel = disp->panel;
	if (!panel) {
		pr_err("qkx_backlight: disp->panel is NULL\n");
		return -ENODEV;
	}

	pr_info("qkx_backlight: found panel '%s' (type '%s')\n",
		panel->name ? panel->name : "unknown",
		panel->type ? panel->type : "unknown");

	/*
	 * Payload layout for JDI DCS 0xB9:
	 * bytes 0..1:   start_r (BE)
	 * bytes 2..3:   period_r (BE)
	 * bytes 4..7:   reserved (0)
	 * bytes 8..9:   start_l (BE)
	 * bytes 10..11: period_l (BE)
	 * bytes 12..15: reserved (0)
	 */
	payload[0]  = (start_r >> 8) & 0xff;
	payload[1]  = start_r & 0xff;
	payload[2]  = (period_r >> 8) & 0xff;
	payload[3]  = period_r & 0xff;

	payload[8]  = (start_l >> 8) & 0xff;
	payload[9]  = start_l & 0xff;
	payload[10] = (period_l >> 8) & 0xff;
	payload[11] = period_l & 0xff;

	pr_info("qkx_backlight: params: R(start=%u, period=%u) L(start=%u, period=%u) execute=%d\n",
		start_r, period_r, start_l, period_l, execute);

	if (!execute) {
		pr_info("qkx_backlight: dry-run mode (pass execute=1 to send DCS 0xB9)\n");
		return -EAGAIN;
	}

	mutex_lock(&panel->panel_lock);
	rc = dcs_write_fn(&panel->mipi_device, reg, payload, sizeof(payload));
	mutex_unlock(&panel->panel_lock);

	pr_info("qkx_backlight: mipi_dsi_dcs_write(0xB9) returned rc=%d\n", rc);
	return -EAGAIN; /* One-shot module */
}

module_init(qkx_backlight_init);
MODULE_LICENSE("GPL");
