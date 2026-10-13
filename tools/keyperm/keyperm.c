// keyperm: one-shot helper to grant READ on FBE master keys so the
// runtime keyring can be dumped for the kexec userdata-mount project.
// Our own device, volatile change only (perm bits reset on reboot).
// Usage: insmod keyperm.ko keyids="12837a62,35cdb6b4,390d2678,3bdc29fd"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/key.h>

static char *keyids = "";
module_param(keyids, charp, 0644);
MODULE_PARM_DESC(keyids, "comma-separated hex key serials to make readable");

static unsigned long lookup_addr = 0;
module_param(lookup_addr, ulong, 0644);
MODULE_PARM_DESC(lookup_addr, "address of key_lookup from /proc/kallsyms");

static int __init keyperm_init(void)
{
    struct key *(*lk)(key_serial_t);
    char *p, *tok;
    char buf[256];

    if (!lookup_addr) {
        pr_err("keyperm: need lookup_addr=<key_lookup from /proc/kallsyms>\n");
        return -EINVAL;
    }
    lk = (void *)lookup_addr;
    strscpy(buf, keyids, sizeof(buf));
    p = buf;
    while ((tok = strsep(&p, ",")) != NULL) {
        unsigned long id;
        struct key *k;
        if (!*tok)
            continue;
        if (kstrtoul(tok, 16, &id)) {
            pr_err("keyperm: bad id '%s'\n", tok);
            continue;
        }
        k = lk((key_serial_t)id);
        if (IS_ERR(k)) {
            pr_err("keyperm: lookup %08lx failed: %ld\n", id, PTR_ERR(k));
            continue;
        }
        pr_info("keyperm: key %08lx perm was %08x datalen=%u\n",
                id, k->perm, k->datalen);
        /* KEY_POS_READ | KEY_USR_READ */
        k->perm |= 0x02000000u | 0x00020000u;
        pr_info("keyperm: key %08lx perm now %08x\n", id, k->perm);
        key_put(k);
    }
    return 0;
}

static void __exit keyperm_exit(void)
{
    pr_info("keyperm: unload\n");
}

module_init(keyperm_init);
module_exit(keyperm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("grant READ on FBE keys for runtime extraction");
