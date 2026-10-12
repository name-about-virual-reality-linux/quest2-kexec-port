/* xtest_dl: XTEST injector via dlopen (no Xlib headers/libs at build time).
 * Links only libc/libdl; loads the TARGET's own libX11/libXtst at runtime:
 *   XOpenDisplay / XKeysymToKeycode / XTestFakeKeyEvent /
 *   XTestFakeButtonEvent / XTestFakeMotionEvent / XFlush / XCloseDisplay.
 * Same command file as xtest_inject: key/type/button/motion/sleep/quit.
 * Build: clang --target=aarch64-linux-gnu -fuse-ld=lld -static -O2
 * Runs INSIDE the Holo chroot (needs its libX11/libXtst):
 *   DISPLAY=:99 /tmp/xtest_dl /tmp/keys.cmd
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s: %s\n", msg, dlerror()); \
    return 1; } } while (0)

typedef void Display;
static Display *(*p_XOpenDisplay)(const char *);
static int (*p_XCloseDisplay)(Display *);
static int (*p_XFlush)(Display *);
static unsigned (*p_XKeysymToKeycode)(Display *, unsigned long);
static int (*p_XTestFakeKeyEvent)(Display *, unsigned, int, unsigned long);
static int (*p_XTestFakeButtonEvent)(Display *, unsigned, int, unsigned long);
static int (*p_XTestFakeMotionEvent)(Display *, int, int, int, unsigned long);

int main(int argc, char **argv) {
    const char *cmdfile = argc > 1 ? argv[1] : "/tmp/keys.cmd";
    const char *dpyname = getenv("DISPLAY") ? getenv("DISPLAY") : ":99";

    void *lx11 = dlopen("libX11.so.6", RTLD_NOW);
    if (!lx11) lx11 = dlopen("libX11.so", RTLD_NOW);
    CHECK(lx11, "dlopen libX11");
    void *lxt = dlopen("libXtst.so.6", RTLD_NOW);
    if (!lxt) lxt = dlopen("libXtst.so", RTLD_NOW);
    CHECK(lxt, "dlopen libXtst");

    *(void **)&p_XOpenDisplay = dlsym(lx11, "XOpenDisplay");
    *(void **)&p_XCloseDisplay = dlsym(lx11, "XCloseDisplay");
    *(void **)&p_XFlush = dlsym(lx11, "XFlush");
    *(void **)&p_XKeysymToKeycode = dlsym(lx11, "XKeysymToKeycode");
    *(void **)&p_XTestFakeKeyEvent = dlsym(lxt, "XTestFakeKeyEvent");
    *(void **)&p_XTestFakeButtonEvent = dlsym(lxt, "XTestFakeButtonEvent");
    *(void **)&p_XTestFakeMotionEvent = dlsym(lxt, "XTestFakeMotionEvent");
    CHECK(p_XOpenDisplay && p_XCloseDisplay && p_XFlush && p_XKeysymToKeycode &&
          p_XTestFakeKeyEvent && p_XTestFakeButtonEvent && p_XTestFakeMotionEvent,
          "resolve symbols");

    Display *d = p_XOpenDisplay(dpyname);
    CHECK(d, "XOpenDisplay");

    /* focus helper: warp pointer into a client area first (caller uses
     * motion to position; openbox focuses on click or focusNew) */
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
                    if (!strncmp(line, "quit", 4)) { p_XCloseDisplay(d); return 0; }
                    if (!strncmp(line, "sleep ", 6)) {
                        int ms = atoi(line + 6);
                        struct timespec ts = { .tv_sec = ms / 1000,
                            .tv_nsec = (long)(ms % 1000) * 1000000L };
                        nanosleep(&ts, NULL);
                        continue;
                    }
                    if (!strncmp(line, "motion ", 7)) {
                        int x = 0, y = 0;
                        if (sscanf(line + 7, "%d %d", &x, &y) == 2)
                            p_XTestFakeMotionEvent(d, -1, x, y, 0);
                        p_XFlush(d);
                        continue;
                    }
                    if (!strncmp(line, "button ", 7)) {
                        int b = 0; char dir[8] = "";
                        if (sscanf(line + 7, "%d %7s", &b, dir) >= 1) {
                            p_XTestFakeButtonEvent(d, (unsigned)b, dir[0] != 'u', 0);
                            p_XFlush(d);
                        }
                        continue;
                    }
                    if (!strncmp(line, "key 0x", 6) || !strncmp(line, "key ", 4)) {
                        unsigned long ks = 0; char dir[8] = "";
                        const char *args = line[4] == ' ' ? line + 5 : line + 4;
                        if (sscanf(args, "%lx %7s", &ks, dir) >= 1) {
                            unsigned kc = p_XKeysymToKeycode(d, ks);
                            if (!kc) { fprintf(stderr, "no keycode for U+%04lX\n", ks); continue; }
                            p_XTestFakeKeyEvent(d, kc, dir[0] != 'u', 0);
                            p_XFlush(d);
                        }
                        continue;
                    }
                    if (!strncmp(line, "type ", 5)) {
                        for (const char *c = line + 5; *c; c++) {
                            unsigned long ks = (unsigned char)*c;
                            unsigned kc = p_XKeysymToKeycode(d, ks);
                            if (!kc) {
                                /* try shifted variant for symbols */
                                fprintf(stderr, "no keycode for '%c'\n", *c);
                                continue;
                            }
                            p_XTestFakeKeyEvent(d, kc, 1, 0);
                            p_XTestFakeKeyEvent(d, kc, 0, 0);
                            p_XFlush(d);
                            struct timespec ts = { .tv_nsec = 8000000L };
                            nanosleep(&ts, NULL);
                        }
                        /* Return key after a type line is a separate cmd */
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
