// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only MDSS/DSI register dumper: /proc/qkx_regdump prints one
 * "block+offset = value" line per 32-bit register, so a dump from stock
 * Android and one from the kexec kernel can be diffed directly.
 * Only read while the display is actively scanning: touching a block whose
 * clock/GDSC is off can hang or reset the SoC.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

struct qkx_region {
	const char *name;
	phys_addr_t pa;
	u32 size;
};

/* Offsets from the captured kona/hollywood DT (mdss_mdp@ae00000, dsi ctrl/phy). */
static const struct qkx_region regions[] = {
	{ "top",   0xae01000, 0x494 },
	{ "ctl0",  0xae02000, 0x1dc }, { "ctl1",  0xae02200, 0x1dc },
	{ "ctl2",  0xae02400, 0x1dc }, { "ctl3",  0xae02600, 0x1dc },
	{ "lm0",   0xae45000, 0x320 }, { "lm1",   0xae46000, 0x320 },
	{ "lm2",   0xae47000, 0x320 }, { "lm3",   0xae48000, 0x320 },
	{ "intf1", 0xae6b800, 0x2b8 }, { "intf2", 0xae6c000, 0x2b8 },
	{ "pp0",   0xae71000, 0xd4 },  { "pp1",   0xae71800, 0xd4 },
	{ "pp2",   0xae72000, 0xd4 },  { "pp3",   0xae72800, 0xd4 },
	{ "dsc0",  0xae81000, 0x140 }, { "dsc1",  0xae81400, 0x140 },
	{ "dsc2",  0xae81800, 0x140 }, { "dsc3",  0xae81c00, 0x140 },
	{ "dsi0",  0xae94000, 0x400 }, { "dsi1",  0xae96000, 0x400 },
	{ "phy0",  0xae94400, 0x7c0 }, { "phy1",  0xae96400, 0x7c0 },
};

static int qkx_regdump_show(struct seq_file *m, void *v)
{
	int i;
	u32 off;

	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		void __iomem *base = ioremap(regions[i].pa, regions[i].size);

		if (!base) {
			seq_printf(m, "%s ioremap failed\n", regions[i].name);
			continue;
		}
		for (off = 0; off < regions[i].size; off += 4)
			seq_printf(m, "%s+0x%03x = 0x%08x\n", regions[i].name,
				   off, readl_relaxed(base + off));
		iounmap(base);
	}
	return 0;
}

static int qkx_regdump_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, qkx_regdump_show, NULL, 256 * 1024);
}

static const struct file_operations qkx_regdump_fops = {
	.owner = THIS_MODULE,
	.open = qkx_regdump_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init qkx_regdump_init(void)
{
	return proc_create("qkx_regdump", 0400, NULL, &qkx_regdump_fops) ? 0 : -ENOMEM;
}

static void __exit qkx_regdump_exit(void)
{
	remove_proc_entry("qkx_regdump", NULL);
}

module_init(qkx_regdump_init);
module_exit(qkx_regdump_exit);
MODULE_LICENSE("GPL");
