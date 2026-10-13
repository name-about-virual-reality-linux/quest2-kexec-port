/* dumpioctl: LD_PRELOAD spy that hexdumps BINDER_WRITE_READ payloads.
 * Build (dynamic!): aarch64-linux-gnu-gcc -shared -fPIC -O2 -o dump.so dumpioctl.c -ldl
 * Use: LD_PRELOAD=/data/local/tmp/dump.so KM_DUMPFILE=/data/local/tmp/io.dump <cmd>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>

typedef int (*ioctl_fn)(int, unsigned long, ...);
#define WRR 0xC0306201u

static void dump(const uint8_t *b, size_t n, int fd) {
    char line[128];
    for (size_t i = 0; i < n; i += 16) {
        int o = 0;
        o += snprintf(line + o, sizeof(line) - o, "%04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            o += snprintf(line + o, sizeof(line) - o, "%02x ", b[i + j]);
        o += snprintf(line + o, sizeof(line) - o, "\n");
        write(fd, line, o);
    }
}

int ioctl(int fd, unsigned long req, ...) {
    static ioctl_fn real = 0;
    if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int r = real(fd, req, arg);
    if (req == WRR && r == 0) {
        /* struct bwr: 6x u64 */
        uint64_t *w = (uint64_t *)arg;
        uint64_t wsize = w[0], wbuf = w[2];
        const char *path = getenv("KM_DUMPFILE");
        int ofd = open(path ? path : "/data/local/tmp/io.dump",
                       O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (ofd >= 0) {
            char h[128];
            int hl = snprintf(h, sizeof h, "== write_size=%llu\n",
                              (unsigned long long)wsize);
            write(ofd, h, hl);
            if (wsize > 0 && wsize < 4096) {
                /* cmd u32 + txn: dump first bytes */
                uint8_t tmp[1024];
                size_t n = wsize > sizeof tmp ? sizeof tmp : (size_t)wsize;
                /* read target proc memory? No - OUR OWN memory, direct copy */
                memcpy(tmp, (void *)(uintptr_t)wbuf, n);
                /* parse cmd + txn header */
                uint32_t cmd = 0, hnd = 0, code = 0;
                uint64_t dsz = 0, osz = 0, dptr = 0, optr = 0;
                if (n >= 4) memcpy(&cmd, tmp, 4);
                if (n >= 4 + 64) {
                    memcpy(&hnd, tmp + 4, 4);
                    memcpy(&code, tmp + 4 + 16, 4);
                    memcpy(&dsz, tmp + 4 + 32, 8);
                    memcpy(&osz, tmp + 4 + 40, 8);
                    memcpy(&dptr, tmp + 4 + 48, 8);
                    memcpy(&optr, tmp + 4 + 56, 8);
                }
                hl = snprintf(h, sizeof h,
                              "cmd=%08x handle=%u code=%u dsz=%llu osz=%llu\n",
                              cmd, hnd, code,
                              (unsigned long long)dsz,
                              (unsigned long long)osz);
                write(ofd, h, hl);
                if (dsz > 0 && dsz < 2048) {
                    uint8_t db[2048];
                    memcpy(db, (void *)(uintptr_t)dptr, (size_t)dsz);
                    dump(db, (size_t)dsz, ofd);
                }
            }
            close(ofd);
        }
    }
    return r;
}
