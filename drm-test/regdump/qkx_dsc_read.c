// SPDX-License-Identifier: GPL-2.0
/*
 * qkx_dsc_read.c - Safe read-only dump of DSC and CTL status registers
 */
#include <linux/module.h>
#include <linux/io.h>

static int __init qkx_dsc_read_init(void)
{
	void __iomem *dsc0 = ioremap(0xae81000, 0x140);
	void __iomem *dsc1 = ioremap(0xae81400, 0x140);
	void __iomem *ctl0 = ioremap(0xae02000, 0x1dc);
	void __iomem *ctl1 = ioremap(0xae02200, 0x1dc);

	if (!dsc0 || !dsc1 || !ctl0 || !ctl1) {
		pr_err("qkx_dsc_read: ioremap failed\n");
		goto out;
	}

	pr_info("qkx_dsc_read: ctl0+0xe4(merge3d)=0x%08x +0xe8(dsc)=0x%08x +0xec(wb)=0x%08x +0xf4(intf)=0x%08x\n",
		readl_relaxed(ctl0 + 0xe4), readl_relaxed(ctl0 + 0xe8),
		readl_relaxed(ctl0 + 0xec), readl_relaxed(ctl0 + 0xf4));
	pr_info("qkx_dsc_read: ctl1+0xe4(merge3d)=0x%08x +0xe8(dsc)=0x%08x +0xec(wb)=0x%08x +0xf4(intf)=0x%08x\n",
		readl_relaxed(ctl1 + 0xe4), readl_relaxed(ctl1 + 0xe8),
		readl_relaxed(ctl1 + 0xec), readl_relaxed(ctl1 + 0xf4));
	pr_info("qkx_dsc_read: dsc0+0x128=0x%08x dsc0+0x12c=0x%08x dsc0+0x130=0x%08x\n",
		readl_relaxed(dsc0 + 0x128), readl_relaxed(dsc0 + 0x12c),
		readl_relaxed(dsc0 + 0x130));
	pr_info("qkx_dsc_read: dsc1+0x128=0x%08x dsc1+0x12c=0x%08x dsc1+0x130=0x%08x\n",
		readl_relaxed(dsc1 + 0x128), readl_relaxed(dsc1 + 0x12c),
		readl_relaxed(dsc1 + 0x130));

out:
	if (dsc0) iounmap(dsc0);
	if (dsc1) iounmap(dsc1);
	if (ctl0) iounmap(ctl0);
	if (ctl1) iounmap(ctl1);
	return -EAGAIN;
}

module_init(qkx_dsc_read_init);
MODULE_LICENSE("GPL");
