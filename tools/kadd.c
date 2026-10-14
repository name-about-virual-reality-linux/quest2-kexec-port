// kadd: install one raw fscrypt v1 key into a mounted filesystem.
// Usage: ./kadd <dir-on-target-fs> <descriptor-hex16> <raw-hex>
//   e.g.: ./kadd /mnt/userdata ab9b06a9b17d657c <128-hex-chars>
// Static build:
//   clang --target=aarch64-linux-gnu -fuse-ld=lld -static -O2 -o kadd kadd.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define FS_IOC_ADD_ENCRYPTION_KEY _IOWR('f', 23, char[80])

struct key_spec {
    uint32_t type;
    uint32_t reserved;
    union {
        uint8_t reserved2[32];
        uint8_t descriptor[8];
        uint8_t identifier[16];
    } u;
};

static int hex2bin(const char *hex, uint8_t *out, int max) {
    int n = strlen(hex) / 2, i;
    if ((int)strlen(hex) % 2 || n > max || n <= 0) return -1;
    for (i = 0; i < n; i++) {
        unsigned int b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) return -1;
        out[i] = b;
    }
    return n;
}

int main(int argc, char **argv) {
    uint8_t arg[80 + 64];
    uint8_t desc[8], raw[64];
    int fd, raw_len;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <dir> <descriptor-hex16> <raw-hex>\n", argv[0]);
        return 2;
    }
    if (hex2bin(argv[2], desc, 8) != 8) { fprintf(stderr, "bad descriptor\n"); return 2; }
    raw_len = hex2bin(argv[3], raw, 64);
    if (raw_len <= 0) { fprintf(stderr, "bad raw key\n"); return 2; }

    fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (fd < 0) { perror("open dir"); return 1; }

    memset(arg, 0, sizeof arg);
    /* struct fscrypt_add_key_arg: spec(40) + raw_size(4) + key_id(4) + rsvd[7](28) + flags(4) + raw[] */
    arg[0] = 1; /* FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR */
    memcpy(arg + 8, desc, 8);
    arg[40] = raw_len;
    memcpy(arg + 80, raw, raw_len);
    memset(arg + 44, 0, 32);

    if (ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, arg) < 0) {
        printf("ADD_KEY %s: ERR %d (%s)\n", argv[2], errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("ADD_KEY %s: ok (%d bytes)\n", argv[2], raw_len);
    close(fd);
    return 0;
}
