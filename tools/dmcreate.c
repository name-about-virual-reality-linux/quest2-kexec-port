// dmcreate: create + load + resume a device-mapper mapping from one table line.
// Usage: ./dmcreate <name> <start> <len> <target> <params...>
//   e.g.: ./dmcreate userdata 0 110498584 default-key "AES-256-XTS <hex> /dev/sda9 0"
// Static build (docker qkx-build):
//   aarch64-linux-gnu-gcc -static -O2 -o dmcreate dmcreate.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

static int ctl(int fd, unsigned long req, struct dm_ioctl *d, size_t size) {
    d->version[0] = 4; d->version[1] = 0; d->version[2] = 0;
    d->data_size = size;
    return ioctl(fd, req, d);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <name> <start> <len> <target> <params...>\n", argv[0]);
        return 2;
    }
    const char *name = argv[1];
    unsigned long long start = strtoull(argv[2], 0, 0);
    unsigned long long len = strtoull(argv[3], 0, 0);
    const char *target = argv[4];

    char params[4096] = "";
    for (int i = 5; i < argc; i++) {
        if ((strlen(params) + strlen(argv[i]) + 2) >= sizeof params) {
            fprintf(stderr, "params too long\n");
            return 2;
        }
        if (i > 5) strcat(params, " ");
        strcat(params, argv[i]);
    }

    int fd = open("/dev/mapper/control", O_RDWR);
    if (fd < 0) { perror("open /dev/mapper/control"); return 1; }

    static unsigned char buf[16384];
    struct dm_ioctl *d = (struct dm_ioctl *)buf;

    /* DEV_CREATE */
    memset(buf, 0, sizeof buf);
    strncpy(d->name, name, sizeof d->name - 1);
    if (ctl(fd, DM_DEV_CREATE, d, sizeof buf) < 0) { perror("DEV_CREATE"); return 1; }
    printf("created %s dev=%llu\n", name, (unsigned long long)d->dev);

    /* TABLE_LOAD */
    memset(buf, 0, sizeof buf);
    strncpy(d->name, name, sizeof d->name - 1);
    struct dm_target_spec *t = (struct dm_target_spec *)(buf + sizeof *d);
    d->data_start = sizeof *d;
    t->sector_start = start;
    t->length = len;
    t->status = 0;
    strncpy(t->target_type, target, sizeof t->target_type - 1);
    strcpy((char *)t + sizeof *t, params);
    t->next = sizeof *t + strlen(params) + 1;
    d->target_count = 1;
    if (ctl(fd, DM_TABLE_LOAD, d, sizeof buf) < 0) { perror("TABLE_LOAD"); return 1; }
    printf("table loaded: %s %s\n", target, params[0] ? "(params ok)" : "(empty)");

    /* DEV_SUSPEND (resume): set no_flush? plain resume = suspend flag 0 */
    memset(buf, 0, sizeof buf);
    strncpy(d->name, name, sizeof d->name - 1);
    if (ctl(fd, DM_DEV_SUSPEND, d, sizeof buf) < 0) { perror("RESUME"); return 1; }
    printf("resumed %s\n", name);
    close(fd);
    return 0;
}
