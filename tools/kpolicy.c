// kpolicy: dump fscrypt policy (version + descriptor) of directories.
// Usage: ./kpolicy /data /data/misc /data/media/0 /data/data /data/system /data/user/0 ...
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <linux/fs.h>
/* Exact numbers from kernel include/uapi/linux/fscrypt.h */
#define FS_IOC_GET_ENCRYPTION_POLICY_NR _IOW('f', 21, char[12])
#define FS_IOC_GET_ENCRYPTION_POLICY_EX_NR _IOWR('f', 22, char[9])

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        unsigned char buf[64];
        int fd = open(argv[i], O_RDONLY | O_DIRECTORY);
        if (fd < 0) { printf("%s: open ERR %d\n", argv[i], errno); continue; }
        memset(buf, 0, sizeof buf);
        if (ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX_NR, buf) < 0 &&
            ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_NR, buf) < 0) {
            printf("%s: policy ERR %d (%s)\n", argv[i], errno, strerror(errno));
        } else {
            int j;
            printf("%s: version=%u contents_mode=%u filenames_mode=%u flags=%02x desc=",
                   argv[i], buf[0], buf[1], buf[2], buf[3]);
            for (j = 0; j < 8; j++) printf("%02x", buf[4 + j]);
            printf(" rest=");
            for (j = 12; j < 24; j++) printf("%02x", buf[j]);
            printf("\n");
        }
        close(fd);
    }
    return 0;
}
