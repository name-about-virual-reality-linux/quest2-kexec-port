// SPDX-License-Identifier: GPL-2.0
/*
 * Live test for the DSI PHY pixel-clock mismatch: if a PHY has
 * dsiclk_sel = 0 (bitclk) in CMN_CLK_CFG1 but the pclk divider in
 * CMN_CLK_CFG0[7:4] is the bootloader's 3 (meant for dsiclk_sel = 1,
 * bitclk/2), the pixel clock runs at twice the intended rate. Double the
 * divider so the output matches. Only load while the panel is scanning.
 */
#include <linux/module.h>
#include <linux/io.h>

#define PHY_CMN_CLK_CFG0 0x010
#define PHY_CMN_CLK_CFG1 0x014

static const phys_addr_t phys[] = { 0xae94400, 0xae96400 };

static int __init qkx_pclkfix_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(phys); i++) {
		void __iomem *base = ioremap(phys[i], 0x20);
		u32 cfg0, cfg1, div;

		if (!base)
			return -ENOMEM;
		cfg0 = readl_relaxed(base + PHY_CMN_CLK_CFG0);
		cfg1 = readl_relaxed(base + PHY_CMN_CLK_CFG1);
		div = (cfg0 >> 4) & 0xf;
		if ((cfg1 & 0x3) == 0 && div == 3) {
			writel_relaxed((cfg0 & ~0xf0) | (6 << 4), base + PHY_CMN_CLK_CFG0);
			wmb();
		}
		pr_info("qkx_pclkfix: phy%d cfg0 0x%02x -> 0x%02x cfg1 0x%02x\n", i,
			cfg0, readl_relaxed(base + PHY_CMN_CLK_CFG0), cfg1);
		iounmap(base);
	}
	return -EAGAIN; /* one-shot: don't stay loaded */
}

module_init(qkx_pclkfix_init);
MODULE_LICENSE("GPL");
