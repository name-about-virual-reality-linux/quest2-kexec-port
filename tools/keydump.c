/* keydump: read fscrypt master keys from the kernel keyring (root only).
 * Uses raw syscalls (no libkeyutils needed). Usage: keydump [id-hex ...]
 * With no args, lists candidate fscrypt keys from /proc/keys then reads them.
 * Static build: aarch64-linux-gnu-gcc -static -O2 -o keydump keydump.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

#define KEYCTL_READ 11
#define KEYCTL_DESCRIBE 6
#define KEYCTL_SEARCH 10

static long kctl(int op, unsigned long a2, unsigned long a3,
                 unsigned long a4, unsigned long a5) {
    return syscall(__NR_keyctl, op, a2, a3, a4, a5);
}

static void dump_one(unsigned id) {
    char desc[512];
    long n = kctl(KEYCTL_DESCRIBE, id, (unsigned long)desc, sizeof(desc) - 1, 0);
    if (n < 0) {
        printf("%08x: describe failed: %s\n", id, strerror(errno));
        return;
    }
    desc[n < 511 ? n : 511] = 0;
    /* payload size probe */
    long sz = kctl(KEYCTL_READ, id, 0, 0, 0);
    if (sz < 0) {
        printf("%08x: [%s] read-size failed: %s\n", id, desc, strerror(errno));
        return;
    }
    unsigned char *buf = malloc(sz > 0 ? sz : 1);
    long got = kctl(KEYCTL_READ, id, (unsigned long)buf, sz, 0);
    if (got < 0) {
        printf("%08x: [%s] read failed: %s\n", id, desc, strerror(errno));
        free(buf);
        return;
    }
    printf("%08x: [%s] %ld bytes: ", id, desc, got);
    for (long i = 0; i < got; i++)
        printf("%02x", buf[i]);
    printf("\n");
    free(buf);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            dump_one((unsigned)strtoul(argv[i], NULL, 16));
        return 0;
    }
    FILE *f = fopen("/proc/keys", "r");
    if (!f) { perror("open /proc/keys"); return 1; }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned id;
        char type[64] = "", desc[256] = "";
        if (sscanf(line, "%x %*s %*s %*s %*s %*s %63s %255s", &id, type, desc) < 3)
            continue;
        if (strstr(desc, "fscrypt") || strstr(type, "fscrypt") ||
            strstr(desc, "_fs") || !strcmp(type, "logon"))
            dump_one(id);
    }
    fclose(f);
    return 0;
}
