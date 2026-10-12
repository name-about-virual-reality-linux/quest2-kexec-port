// SPDX-License-Identifier: GPL-2.0
/*
 * Live test to sync the remaining hardware registers that differed between
 * stock Android and kexec:
 *   intf1+0x08c: Android 0x11, ours 0x0
 *   intf2+0x08c: Android 0x11, ours 0x0
 *   pp0+0x0a8:   Android 0x3,  ours 0x0
 *   pp1+0x0a8:   Android 0x3,  ours 0x0
 * Must be loaded while the display is scanning.
 */
#include <linux/module.h>
#include <linux/io.h>

static int __init qkx_diffsync_init(void)
{
	void __iomem *intf1 = ioremap(0xae6b800, 0x100);
	void __iomem *intf2 = ioremap(0xae6c000, 0x100);
	void __iomem *pp0   = ioremap(0xae71000, 0x100);
	void __iomem *pp1   = ioremap(0xae71800, 0x100);

	if (!intf1 || !intf2 || !pp0 || !pp1) {
		pr_err("qkx_diffsync: ioremap failed\n");
		goto out;
	}

	pr_info("qkx_diffsync: BEFORE intf1+0x8c=0x%08x intf2+0x8c=0x%08x pp0+0xa8=0x%08x pp1+0xa8=0x%08x\n",
		readl_relaxed(intf1 + 0x8c), readl_relaxed(intf2 + 0x8c),
		readl_relaxed(pp0 + 0xa8), readl_relaxed(pp1 + 0xa8));

	writel_relaxed(0x11, intf1 + 0x8c);
	writel_relaxed(0x11, intf2 + 0x8c);
	writel_relaxed(0x3, pp0 + 0xa8);
	writel_relaxed(0x3, pp1 + 0xa8);
	wmb();

	pr_info("qkx_diffsync: AFTER  intf1+0x8c=0x%08x intf2+0x8c=0x%08x pp0+0xa8=0x%08x pp1+0xa8=0x%08x\n",
		readl_relaxed(intf1 + 0x8c), readl_relaxed(intf2 + 0x8c),
		readl_relaxed(pp0 + 0xa8), readl_relaxed(pp1 + 0xa8));

out:
	if (intf1) iounmap(intf1);
	if (intf2) iounmap(intf2);
	if (pp0)   iounmap(pp0);
	if (pp1)   iounmap(pp1);
	return -EAGAIN; /* One-shot module */
}

module_init(qkx_diffsync_init);
MODULE_LICENSE("GPL");
