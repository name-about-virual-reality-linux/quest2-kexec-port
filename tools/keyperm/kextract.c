// kextract: snapshot fscrypt master-key secrets from live kernel RAM.
// Our own device, volatile reads only. Validates layout by comparing
// mk_spec identifier bytes against the key's own description string.
// Usage: insmod kextract.ko lookup_addr=0x< key_lookup> keyids=12837a62,...
// Output: dmesg lines tagged KXKEY (pull with dmesg | grep KXKEY).
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/rwsem.h>
#include "fscrypt_private.h"

static char *keyids = "";
module_param(keyids, charp, 0644);

static unsigned long lookup_addr = 0;
module_param(lookup_addr, ulong, 0644);

static void print_hex(const char *tag, const u8 *buf, unsigned int len)
{
	/* 96 bytes max -> 192 hex chars; keep each line < 256 */
	char line[256];
	unsigned int i, o = 0;

	for (i = 0; i < len && o + 2 < sizeof(line) - 1; i++)
		o += snprintf(line + o, sizeof(line) - o, "%02x", buf[i]);
	line[o] = 0;
	pr_info("KXKEY %s=%s\n", tag, line);
}

static int __init kextract_init(void)
{
	struct key *(*lk)(key_serial_t);
	char *p, *tok;
	char buf[256];

	if (!lookup_addr) {
		pr_err("kextract: need lookup_addr=<key_lookup>\n");
		return -EINVAL;
	}
	lk = (void *)lookup_addr;
	strscpy(buf, keyids, sizeof(buf));
	p = buf;
	while ((tok = strsep(&p, ",")) != NULL) {
		unsigned long id;
		struct key *k;
		struct fscrypt_master_key *mk;
		char ident[33];
		unsigned int i;

		if (!*tok)
			continue;
		if (kstrtoul(tok, 16, &id)) {
			pr_err("kextract: bad id '%s'\n", tok);
			continue;
		}
		k = lk((key_serial_t)id);
		if (IS_ERR(k)) {
			pr_err("KXKEY id=%08lx lookup failed: %ld\n",
			       id, PTR_ERR(k));
			continue;
		}
		mk = (struct fscrypt_master_key *)k->payload.data[0];
		if (!mk) {
			pr_info("KXKEY id=%08lx desc=%s EMPTY (no payload)\n",
				id, k->description);
			key_put(k);
			continue;
		}
		for (i = 0; i < FSCRYPT_KEY_IDENTIFIER_SIZE; i++)
			snprintf(ident + 2 * i, 3, "%02x",
				 mk->mk_spec.u.identifier[i]);
		ident[32] = 0;
		pr_info("KXKEY id=%08lx desc=%s ident=%s match=%d size=%u wrapped=%d\n",
			id, k->description, ident,
			!strcmp(k->description, ident),
			mk->mk_secret.size, (int)mk->mk_secret.is_hw_wrapped);
		if (down_read_trylock(&mk->mk_secret_sem)) {
			print_hex("raw", mk->mk_secret.raw,
				  mk->mk_secret.size <= FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE ?
				  mk->mk_secret.size : FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE);
			up_read(&mk->mk_secret_sem);
		} else {
			pr_info("KXKEY id=%08lx secret busy\n", id);
		}
		key_put(k);
	}
	return 0;
}

static void __exit kextract_exit(void)
{
	pr_info("kextract: unload\n");
}

module_init(kextract_init);
module_exit(kextract_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("snapshot fscrypt master key secrets for kexec project");
