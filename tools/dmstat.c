// dmstat: print a mapped device's live target table (type + params).
// Read-only (TABLE_STATUS); never changes mappings.
// Usage: ./dmstat [name]   (default: userdata)
// Static build (docker qkx-build):
//   aarch64-linux-gnu-gcc -static -O2 -o dmstat dmstat.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

int main(int argc, char **argv) {
    const char *want = argc > 1 ? argv[1] : "userdata";
    int fd = open("/dev/device-mapper", O_RDWR);
    if (fd < 0) { perror("open /dev/device-mapper"); return 1; }

    static unsigned char buf[16384];
    struct dm_ioctl *d = (struct dm_ioctl *)buf;

    /* 1. list devices, find the target's dev number (not strictly needed) */
    memset(buf, 0, sizeof buf);
    d->version[0] = 4; d->version[1] = 0; d->version[2] = 0;
    d->data_size = sizeof buf;
    if (ioctl(fd, DM_LIST_DEVICES, d) < 0) { perror("LIST_DEVICES"); return 1; }
    struct dm_name_list *nl = (struct dm_name_list *)(buf + d->data_start);
    int found = 0;
    for (;;) {
        if (!strcmp(nl->name, want)) { found = 1; break; }
        if (!nl->next) break;
        nl = (struct dm_name_list *)((char *)nl + nl->next);
    }
    if (!found) { fprintf(stderr, "device '%s' not mapped\n", want); return 1; }

    /* 2. live table status (includes full target params incl. key) */
    memset(buf, 0, sizeof buf);
    d->version[0] = 4; d->version[1] = 0; d->version[2] = 0;
    d->data_size = sizeof buf;
    strncpy(d->name, want, sizeof d->name - 1);
    if (ioctl(fd, DM_TABLE_STATUS, d) < 0) { perror("TABLE_STATUS"); return 1; }
    printf("targets=%u flags=%x\n", d->target_count, d->flags);
    char *p = (char *)buf + d->data_start;
    for (unsigned i = 0; i < d->target_count; i++) {
        struct dm_target_spec *t = (struct dm_target_spec *)p;
        char *params = p + sizeof *t;
        printf("target%u: start=%llu len=%llu type=%s\n  params: %s\n",
               i, (unsigned long long)t->sector_start,
               (unsigned long long)t->length, t->target_type, params);
        if (!t->next) break;
        p += t->next;
    }
    /* NOTE: for crypt targets the params include the raw key. This tool
       prints it (needed to recreate the mapping after kexec). Do not save
       output to git; treat like key material (0600, /tmp only). */
    close(fd);
    return 0;
}
