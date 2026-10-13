// kdump: minimal raw-syscall keyring reader for Quest 2 FBE recon.
// No libkeyutils needed. Static build:
//   clang --target=aarch64-linux-gnu -static -O2 -o kdump kdump.c
// Usage (as root): ./kdump /data/local/tmp/kkeys
// Writes <id>.bin payloads + MANIFEST (describe lines + read sizes).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#define KEYCTL_DESCRIBE 6
#define KEYCTL_SEARCH 10
#define KEYCTL_READ 11

#define KEY_SPEC_SESSION_KEYRING -3

static long kctl(int cmd, unsigned long a2, unsigned long a3,
                 unsigned long a4, unsigned long a5) {
    return syscall(SYS_keyctl, cmd, a2, a3, a4, a5);
}

static int slurp_proc_keys(char ids[][16], int max) {
    FILE *f = fopen("/proc/keys", "r");
    if (!f) { perror("fopen /proc/keys"); return -1; }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        unsigned int id;
        if (sscanf(line, "%x", &id) == 1) snprintf(ids[n++], 16, "%08x", id);
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : "/data/local/tmp/kkeys";
    mkdir(outdir, 0700);
    char manpath[512];
    snprintf(manpath, sizeof manpath, "%s/MANIFEST", outdir);
    FILE *man = fopen(manpath, "w");
    if (!man) { perror("fopen manifest"); return 1; }

    char ids[128][16];
    int n = slurp_proc_keys(ids, 128);
    fprintf(man, "nkeys=%d\n", n);

    char desc[4096], members[65536], payload[1024 * 1024];
    for (int i = 0; i < n; i++) {
        unsigned long id = strtoul(ids[i], NULL, 16);
        long dr = kctl(KEYCTL_DESCRIBE, id, (unsigned long)desc, sizeof desc - 1, 0);
        if (dr < 0) {
            fprintf(man, "%s describe: ERR %d (%s)\n", ids[i], errno, strerror(errno));
            continue;
        }
        desc[dr] = 0;
        // describe format: type;uid;gid;perm;description
        fprintf(man, "%s describe: %s", ids[i], desc);
        if (desc[strlen(desc) - 1] != '\n') fputc('\n', man);

        int is_keyring = (strncmp(desc, "keyring;", 8) == 0);
        if (is_keyring) {
            long mr = kctl(KEYCTL_READ, id, (unsigned long)members, sizeof members, 0);
            if (mr < 0) {
                fprintf(man, "%s members: ERR %d (%s)\n", ids[i], errno, strerror(errno));
            } else {
                fprintf(man, "%s members: %ld bytes:", ids[i], mr);
                for (long o = 0; o + 4 <= mr; o += 4) {
                    unsigned int mid;
                    memcpy(&mid, members + o, 4);
                    fprintf(man, " %08x", mid);
                }
                fputc('\n', man);
            }
            continue;
        }
        long pr = kctl(KEYCTL_READ, id, (unsigned long)payload, sizeof payload, 0);
        if (pr < 0) {
            fprintf(man, "%s payload: ERR %d (%s)\n", ids[i], errno, strerror(errno));
        } else {
            char ppath[512];
            snprintf(ppath, sizeof ppath, "%s/%s.bin", outdir, ids[i]);
            int fd = open(ppath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                long w = write(fd, payload, pr);
                close(fd);
                fprintf(man, "%s payload: %ld bytes -> %s.bin (wrote %ld)\n",
                        ids[i], pr, ids[i], w);
            } else {
                fprintf(man, "%s payload: %ld bytes, SAVE FAILED %d\n",
                        ids[i], pr, errno);
            }
        }
    }
    fclose(man);
    printf("done, manifest: %s\n", manpath);
    return 0;
}
