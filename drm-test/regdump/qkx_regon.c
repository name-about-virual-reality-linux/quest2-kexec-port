// SPDX-License-Identifier: GPL-2.0
/*
 * Claim and enable a regulator by name and hold it until unload.
 * Used to keep display_panel_avdd (GPIO fixed regulator, no consumer in the
 * DT) from being switched off by the regulator core's unused-rail cleanup.
 */
#include <linux/module.h>
#include <linux/regulator/consumer.h>

static char *name = "display_panel_avdd";
module_param(name, charp, 0444);

static struct regulator *reg;

static int __init qkx_regon_init(void)
{
	int rc;

	reg = regulator_get(NULL, name);
	if (IS_ERR(reg)) {
		pr_err("qkx_regon: regulator_get(%s) failed: %ld\n", name, PTR_ERR(reg));
		return PTR_ERR(reg);
	}
	rc = regulator_enable(reg);
	pr_info("qkx_regon: enable(%s) rc=%d is_enabled=%d\n", name, rc,
		regulator_is_enabled(reg));
	if (rc) {
		regulator_put(reg);
		return rc;
	}
	return 0;
}

static void __exit qkx_regon_exit(void)
{
	regulator_disable(reg);
	regulator_put(reg);
}

module_init(qkx_regon_init);
module_exit(qkx_regon_exit);
MODULE_LICENSE("GPL");
