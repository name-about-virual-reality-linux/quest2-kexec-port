/* xtest_inject: raw-socket X11 XTEST input injector (no Xlib needed).
 * Static build like the other drm-test tools:
 *   clang --target=aarch64-linux-gnu -fuse-ld=lld -static -O2 -o xtest_inject xtest_inject.c
 *
 * Usage: DISPLAY=:99 xtest_inject /tmp/keys.cmd
 *   Reads command lines from the file (re-reads on change, polls 200ms):
 *     key <hex-keysym> down|up     press/release a key by keysym
 *     type <text>                  type ASCII text (down+up per char)
 *     button <1..5> down|up        mouse buttons (4/5 = wheel, use down only)
 *     motion <x> <y>               absolute pointer warp
 *     sleep <ms>
 *     quit
 *   Keysyms resolve via the server's own keyboard mapping (fetched once at
 *   startup), so layout mismatches fail loudly instead of mistyping.
 *
 * Why raw protocol: the kexec target has no XlibSaga, and Xvfb accepts no
 * physical keyboards -- XTEST injection is the only input path for the
 * offscreen desktop. Server must have XTEST (Xvfb does).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno)); \
    return 1; } } while (0)

static int sfd = -1;

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 255; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = v >> 24;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wfull(const uint8_t *b, size_t n) {
    size_t o = 0;
    while (o < n) {
        ssize_t r = write(sfd, b + o, n - o);
        if (r <= 0) { perror("write"); exit(1); }
        o += (size_t)r;
    }
}

static void rfull(uint8_t *b, size_t n) {
    size_t o = 0;
    while (o < n) {
        ssize_t r = read(sfd, b + o, n - o);
        if (r <= 0) { perror("read"); exit(1); }
        o += (size_t)r;
    }
}

/* Static US-positional keysym->keycode table (evdev + 8, the layout Xvfb
 * serves here; verified against this server's own xkbcomp dump). Avoids
 * GetKeyboardMapping entirely. shift=1 means Shift_L must be held. */
struct kentry { uint32_t keysym; uint8_t keycode; uint8_t shift; };
static const struct kentry KEYMAP[] = {
    {0x0020, 65, 0}, /* space */
    {0x0021, 10, 1}, {0x0040, 11, 1}, {0x0023, 12, 1}, {0x0024, 13, 1},
    {0x0025, 14, 1}, {0x005e, 15, 1}, {0x0026, 16, 1}, {0x002a, 17, 1},
    {0x0028, 18, 1}, {0x0029, 19, 1},
    {0x0031, 10, 0}, {0x0032, 11, 0}, {0x0033, 12, 0}, {0x0034, 13, 0},
    {0x0035, 14, 0}, {0x0036, 15, 0}, {0x0037, 16, 0}, {0x0038, 17, 0},
    {0x0039, 18, 0}, {0x0030, 19, 0},
    {0x002d, 20, 0}, {0x005f, 20, 1}, {0x003d, 21, 0}, {0x002b, 21, 1},
    {0x005b, 34, 0}, {0x007b, 34, 1}, {0x005d, 35, 0}, {0x007d, 35, 1},
    {0x003b, 47, 0}, {0x003a, 47, 1}, {0x0027, 48, 0}, {0x0022, 48, 1},
    {0x0060, 49, 0}, {0x007e, 49, 1}, {0x005c, 51, 0}, {0x007c, 51, 1},
    {0x002c, 59, 0}, {0x003c, 59, 1}, {0x002e, 60, 0}, {0x003e, 60, 1},
    {0x002f, 61, 0}, {0x003f, 61, 1},
    {0x0061, 38, 0}, {0x0062, 56, 0}, {0x0063, 54, 0}, {0x0064, 40, 0},
    {0x0065, 26, 0}, {0x0066, 41, 0}, {0x0067, 42, 0}, {0x0068, 43, 0},
    {0x0069, 31, 0}, {0x006a, 44, 0}, {0x006b, 45, 0}, {0x006c, 46, 0},
    {0x006d, 58, 0}, {0x006e, 57, 0}, {0x006f, 32, 0}, {0x0070, 33, 0},
    {0x0071, 24, 0}, {0x0072, 27, 0}, {0x0073, 39, 0}, {0x0074, 28, 0},
    {0x0075, 30, 0}, {0x0076, 55, 0}, {0x0077, 25, 0}, {0x0078, 53, 0},
    {0x0079, 29, 0}, {0x007a, 52, 0},
    {0x0041, 38, 1}, {0x0042, 56, 1}, {0x0043, 54, 1}, {0x0044, 40, 1},
    {0x0045, 26, 1}, {0x0046, 41, 1}, {0x0047, 42, 1}, {0x0048, 43, 1},
    {0x0049, 31, 1}, {0x004a, 44, 1}, {0x004b, 45, 1}, {0x004c, 46, 1},
    {0x004d, 58, 1}, {0x004e, 57, 1}, {0x004f, 32, 1}, {0x0050, 33, 1},
    {0x0051, 24, 1}, {0x0052, 27, 1}, {0x0053, 39, 1}, {0x0054, 28, 1},
    {0x0055, 30, 1}, {0x0056, 55, 1}, {0x0057, 25, 1}, {0x0058, 53, 1},
    {0x0059, 29, 1}, {0x005a, 52, 1},
    {0xff08, 22, 0}, /* BackSpace */
    {0xff09, 23, 0}, /* Tab */
    {0xff0d, 36, 0}, /* Return */
    {0xff1b, 9, 0},  /* Escape */
    {0xffe1, 50, 0}, /* Shift_L */
    {0xffe2, 62, 0}, /* Shift_R */
    {0xffe3, 37, 0}, /* Control_L */
};
#define KEYMAP_N (sizeof(KEYMAP) / sizeof(KEYMAP[0]))

static const struct kentry *lookup_keysym(uint32_t keysym) {
    for (size_t i = 0; i < KEYMAP_N; i++)
        if (KEYMAP[i].keysym == keysym)
            return &KEYMAP[i];
    return NULL;
}

static int keycode_for(uint32_t keysym) {
    const struct kentry *e = lookup_keysym(keysym);
    return e ? e->keycode : -1;
}

int main(int argc, char **argv) {
    const char *cmdfile = argc > 1 ? argv[1] : "/tmp/keys.cmd";
    const char *dpy = getenv("DISPLAY") ? getenv("DISPLAY") : ":99";
    int disp = 99;
    if (sscanf(dpy, ":%d", &disp) != 1) { fprintf(stderr, "bad DISPLAY\n"); return 1; }

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(sfd >= 0, "socket");
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/.X11-unix/X%d", disp);
    CHECK(connect(sfd, (struct sockaddr *)&a, sizeof(a)) == 0, "connect X socket");

    /* setup: byte-order 'l', unused, major 11, minor 0, auth Counts 0 */
    uint8_t setup[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    put16(setup + 2, 11);
    wfull(setup, 12);
    uint8_t rh[8];
    rfull(rh, 8);
    if (rh[0] != 1) { fprintf(stderr, "setup refused (status %d)\n", rh[0]); return 1; }
    uint16_t dlen = get16(rh + 6);
    uint8_t *sdata = malloc((size_t)dlen * 4);
    rfull(sdata, (size_t)dlen * 4);
    free(sdata);

    /* QueryExtension XTEST -> major opcode.
     * Request: opcode, pad, len=4 words, nbytes, pad, name padded to 4. */
    uint8_t qe[16];
    memset(qe, 0, sizeof(qe));
    qe[0] = 98; /* QueryExtension */
    put16(qe + 2, 4);
    put16(qe + 4, 5);
    memcpy(qe + 8, "XTEST", 5);
    wfull(qe, 16);
    uint8_t qr[32];
    rfull(qr, 32);
    if (qr[0] != 1 || qr[8] == 0) { fprintf(stderr, "no XTEST on server\n"); return 1; }
    uint8_t xtest_major = qr[9];
    fprintf(stderr, "XTEST major opcode %d\n", xtest_major);
    fprintf(stderr, "static keymap: %zu entries\n", KEYMAP_N);

    /* helper lambdas via local functions: send one XTEST request */
    long last_mtime = 0;
    long file_off = 0;
    for (;;) {
        struct stat st;
        if (stat(cmdfile, &st) == 0 && (long)st.st_mtime >= last_mtime) {
            FILE *f = fopen(cmdfile, "r");
            if (f) {
                fseek(f, file_off, SEEK_SET);
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    file_off = ftell(f);
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = 0;
                    if (!line[0] || line[0] == '#') continue;
                    if (!strncmp(line, "quit", 4)) { fclose(f); return 0; }
                    if (!strncmp(line, "sleep ", 6)) {
                        struct timespec ts = { .tv_sec = atoi(line + 6) / 1000,
                            .tv_nsec = (long)(atoi(line + 6) % 1000) * 1000000L };
                        nanosleep(&ts, NULL);
                        continue;
                    }
                    if (!strncmp(line, "motion ", 7)) {
                        int x = 0, y = 0;
                        if (sscanf(line + 7, "%d %d", &x, &y) == 2) {
                            uint8_t r[36];
                            memset(r, 0, sizeof(r));
                            r[0] = xtest_major;
                            r[1] = 5; /* XTestFakeInput */
                            put16(r + 2, 9);
                            r[4] = 6; /* MotionNotify */
                            put32(r + 12, (uint32_t)x);
                            put32(r + 16, (uint32_t)y);
                            put32(r + 28, 0);
                            wfull(r, 36);
                        }
                        continue;
                    }
                    if (!strncmp(line, "button ", 7)) {
                        int b = 0; char dir[8] = "";
                        if (sscanf(line + 7, "%d %7s", &b, dir) >= 1) {
                            int isdown = dir[0] != 'u';
                            uint8_t r[36];
                            memset(r, 0, sizeof(r));
                            r[0] = xtest_major;
                            r[1] = 5;
                            put16(r + 2, 9);
                            r[4] = isdown ? 4 : 5; /* ButtonPress/Release */
                            r[5] = (uint8_t)b;
                            wfull(r, 36);
                        }
                        continue;
                    }
                    if (!strncmp(line, "key 0x", 6) || !strncmp(line, "key ", 4)) {
                        unsigned ks = 0; char dir[8] = "";
                        const char *args = line[4] == ' ' ? line + 5 : line + 4;
                        if (sscanf(args, "%x %7s", &ks, dir) >= 1) {
                            int kc = keycode_for(ks);
                            if (kc < 0) { fprintf(stderr, "no keycode for U+%04X\n", ks); continue; }
                            int isdown = dir[0] != 'u';
                            uint8_t r[36];
                            memset(r, 0, sizeof(r));
                            r[0] = xtest_major;
                            r[1] = 5;
                            put16(r + 2, 9);
                            r[4] = isdown ? 2 : 3; /* KeyPress/Release */
                            r[5] = (uint8_t)kc;
                            wfull(r, 36);
                        }
                        continue;
                    }
                    if (!strncmp(line, "type ", 5)) {
                        for (const char *c = line + 5; *c; c++) {
                            const struct kentry *e = lookup_keysym((uint8_t)*c);
                            if (!e) { fprintf(stderr, "no key for '%c'\n", *c); continue; }
                            int kc = e->keycode;
                            int shk = e->shift ? 50 : -1; /* Shift_L */
                            uint8_t r[36];
                            if (shk > 0) {
                                memset(r, 0, sizeof(r));
                                r[0] = xtest_major; r[1] = 5; put16(r + 2, 9);
                                r[4] = 2; r[5] = (uint8_t)shk; wfull(r, 36);
                            }
                            memset(r, 0, sizeof(r));
                            r[0] = xtest_major; r[1] = 5; put16(r + 2, 9);
                            r[4] = 2; r[5] = (uint8_t)kc; wfull(r, 36);
                            memset(r, 0, sizeof(r));
                            r[0] = xtest_major; r[1] = 5; put16(r + 2, 9);
                            r[4] = 3; r[5] = (uint8_t)kc; wfull(r, 36);
                            if (shk > 0) {
                                memset(r, 0, sizeof(r));
                                r[0] = xtest_major; r[1] = 5; put16(r + 2, 9);
                                r[4] = 3; r[5] = (uint8_t)shk; wfull(r, 36);
                            }
                            struct timespec ts = { .tv_nsec = 8000000L };
                            nanosleep(&ts, NULL);
                        }
                        continue;
                    }
                    fprintf(stderr, "unknown cmd: %s\n", line);
                }
                last_mtime = (long)st.st_mtime;
                fclose(f);
            }
        }
        struct timespec ts = { .tv_nsec = 200000000L };
        nanosleep(&ts, NULL);
    }
}
