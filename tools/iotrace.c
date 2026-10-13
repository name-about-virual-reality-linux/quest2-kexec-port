/* iotrace: dump BINDER_WRITE_READ transact payloads (requests + replies).
 * Usage: iotrace <outfile> <prog> [args...]      (fork+exec under trace)
 *        iotrace <outfile> -p <pid> [secs]       (attach to running proc)
 * Static build: aarch64-linux-gnu-gcc -static -O2 -o iotrace iotrace.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/user.h>

#define WRR 0xC0306201u
#define SG  0x40486311u
#define PLAIN 0x40406300u
#define SYS_IOCTL 29

static FILE *out;
static ssize_t rmem(int tid, unsigned long addr, void *buf, size_t len) {
    struct iovec l = { buf, len }, r = { (void *)addr, len };
    return process_vm_readv(tid, &l, 1, &r, 1, 0);
}
static void dump(const char *tag, int tid, unsigned long addr, size_t n, size_t max) {
    static uint8_t b[2048];
    if (n > max) n = max;
    if (!n) { fprintf(out, "%s: empty\n", tag); return; }
    if (rmem(tid, addr, b, n) != (ssize_t)n) { fprintf(out, "%s: readfail\n", tag); return; }
    fprintf(out, "%s: %zu bytes\n", tag, n);
    for (size_t i = 0; i < n; i += 16) {
        fprintf(out, "%04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < n; j++) fprintf(out, "%02x ", b[i + j]);
        fprintf(out, "\n");
    }
}
static int attached[512], na = 0;
static void attach_tid(int tid) {
    for (int i = 0; i < na; i++) if (attached[i] == tid) return;
    if (na < 512 && ptrace(PTRACE_ATTACH, tid, 0, 0) == 0) attached[na++] = tid;
}
static void attach_all(int pid) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid > 0) attach_tid(tid);
    }
    closedir(d);
}
/* per-tid state: entering flag + saved ioctl arg */
static unsigned long saved_arg[512];
static int in_ioctl[512];
static int slotof(int tid) {
    for (int i = 0; i < na; i++) if (attached[i] == tid) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: iotrace <outfile> <prog> [args] | iotrace <outfile> -p <pid> [secs]\n"); return 1; }
    out = fopen(argv[1], "w");
    if (!out) return 1;
    int apid = 0, secs = 0;
    pid_t c = -1;
    time_t end = 0;
    if (!strcmp(argv[2], "-p")) {
        if (argc < 4) return 1;
        apid = atoi(argv[3]); secs = argc >= 5 ? atoi(argv[4]) : 20;
        attach_all(apid);
        usleep(200000);
        for (int i = 0; i < na; i++) {
            int st; waitpid(attached[i], &st, WNOHANG);
            in_ioctl[i] = 0; saved_arg[i] = 0;
            ptrace(PTRACE_SETOPTIONS, attached[i], 0, (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK));
            ptrace(PTRACE_SYSCALL, attached[i], 0, 0);
        }
        end = time(NULL) + secs;
    } else {
        c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(17);
            execvp(argv[2], argv + 2);
            _exit(127);
        }
        int st; waitpid(c, &st, 0);
        ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACESYSGOOD);
        attached[na++] = c;
        in_ioctl[0] = 0; saved_arg[0] = 0;
        ptrace(PTRACE_SYSCALL, c, 0, 0);
    }
    for (;;) {
        if (apid && time(NULL) >= end) break;
        if (apid) attach_all(apid);
        int st;
        int tid = waitpid(-1, &st, WNOHANG);
        if (tid <= 0) { usleep(3000); continue; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (!apid && tid == c) break;
            continue;
        }
        int sig = WSTOPSIG(st);
        int ev = (st >> 16) & 0xffff;
        if (ev == PTRACE_EVENT_CLONE || ev == PTRACE_EVENT_FORK || ev == PTRACE_EVENT_VFORK) {
            unsigned long nt = 0;
            ptrace(PTRACE_GETEVENTMSG, tid, 0, &nt);
            if (nt && na < 512) {
                attached[na] = (int)nt; saved_arg[na] = 0; in_ioctl[na] = 0; na++;
                ptrace(PTRACE_SETOPTIONS, (int)nt, 0, (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK));
                ptrace(PTRACE_SYSCALL, (int)nt, 0, 0);
            }
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
            continue;
        }
        { static int nstops = 0;
          if (nstops < 40) { fprintf(out, "stop tid=%d sig=%d ev=%d\n", tid, sig, ev); fflush(out); nstops++; } }
        if (sig == (SIGTRAP | 0x80)) {
            struct user_regs_struct rg;
            struct iovec iov = { &rg, sizeof rg };
            int slot = slotof(tid);
            if (slot < 0) { ptrace(PTRACE_SYSCALL, tid, 0, 0); continue; }
            if (ptrace(PTRACE_GETREGSET, tid, (void *)1, &iov) == 0 && rg.regs[8] == SYS_IOCTL) {
                if (!in_ioctl[slot]) {
                    in_ioctl[slot] = 1;
                    unsigned long req = rg.regs[1], arg = rg.regs[2];
                    { const char *e = getenv("KM_ALL");
                      if (e && e[0]=='1') {
                        fprintf(out, "ioctl req=%lx tid=%d ws?", req, tid);
                        if ((uint32_t)req == WRR) {
                          uint64_t dws=0, dwb=0;
                          rmem(tid, arg, &dws, 8); rmem(tid, arg+16, &dwb, 8);
                          fprintf(out, " WRR ws=%llu wb=%lx", (unsigned long long)dws, (unsigned long)dwb);
                          if (dws >= 68) {
                            uint8_t h0[68];
                            if (rmem(tid, (unsigned long)dwb, h0, 68) == 68) {
                              uint32_t cc=0; memcpy(&cc, h0, 4);
                              fprintf(out, " cmd=%08x", cc);
                            } else fprintf(out, " wbreadfail");
                          }
                        }
                        fprintf(out, "\n"); fflush(out);
                      } }
                    saved_arg[slot] = 0;
                    if ((uint32_t)req == WRR) {
                        saved_arg[slot] = arg;
                        uint64_t ws = 0, wb = 0;
                        rmem(tid, arg, &ws, 8);
                        rmem(tid, arg + 16, &wb, 8);
                        static uint8_t wb0[80];
                        if (ws >= 4 && rmem(tid, (unsigned long)wb, wb0, ws > 80 ? 80 : (size_t)ws) > 0) {
                            uint32_t cmd = 0;
                            memcpy(&cmd, wb0, 4);
                            if (1) {
                                uint32_t h = 0, code = 0;
                                uint64_t dsz = 0, osz = 0, dptr = 0, optr = 0;
                                memcpy(&h, wb0 + 4, 4);
                                memcpy(&code, wb0 + 4 + 16, 4);
                                memcpy(&dsz, wb0 + 4 + 32, 8);
                                memcpy(&osz, wb0 + 4 + 40, 8);
                                memcpy(&dptr, wb0 + 4 + 48, 8);
                                memcpy(&optr, wb0 + 4 + 56, 8);
                                fprintf(out, "== %08x req handle=%u code=%u dsz=%llu osz=%llu\n", cmd,
                                        h, code, (unsigned long long)dsz, (unsigned long long)osz);
                                dump("reqdata", tid, (unsigned long)dptr, (size_t)dsz, 768);
                                /* dump offsets array */
                                if (osz && osz <= 128) {
                                    uint64_t oa[16]; size_t on = (size_t)osz / 8;
                                    if (on > 16) on = 16;
                                    if (rmem(tid, (unsigned long)optr, oa, on * 8) == (ssize_t)(on * 8)) {
                                        fprintf(out, "reqoffs:");
                                        for (size_t k = 0; k < on; k++) fprintf(out, " %llu", (unsigned long long)oa[k]);
                                        fprintf(out, "\n");
                                    }
                                }
                                fflush(out);
                            }
                        }
                    }
                } else {
                    in_ioctl[slot] = 0;
                    /* exit: dump reply */
                    unsigned long arg = saved_arg[slot];
                    if (arg) {
                        uint64_t rs = 0, rb = 0;
                        rmem(tid, arg + 24, &rs, 8);
                        rmem(tid, arg + 40, &rb, 8);
                        /* scan read buffer for BR_REPLY */
                        static uint8_t rd[65536];
                        if (rs > 0 && rs <= sizeof rd && rmem(tid, (unsigned long)rb, rd, (size_t)rs) == (ssize_t)rs) {
                            size_t p = 0;
                            while (p + 4 <= (size_t)rs) {
                                uint32_t c2 = 0; memcpy(&c2, rd + p, 4); p += 4;
                                if (c2 == 0x80407203u || c2 == 0x80407202u) { /* BR_REPLY or BR_TRANSACTION */
                                    uint64_t ds = 0, os = 0, db = 0, ob = 0;
                                    memcpy(&ds, rd + p + 32, 8);
                                    memcpy(&os, rd + p + 40, 8);
                                    memcpy(&db, rd + p + 48, 8);
                                    memcpy(&ob, rd + p + 56, 8);
                                    fprintf(out, "-- %s dsz=", c2 == 0x80407202u ? "incoming" : "reply");
                                    fprintf(out, "x dsz=%llu osz=%llu\n",
                                            (unsigned long long)ds, (unsigned long long)os);
                                    dump("repdata", tid, (unsigned long)db, (size_t)ds, 768);
                                    if (os && os <= 128) {
                                        uint64_t oa[16]; size_t on = (size_t)os / 8;
                                        if (on > 16) on = 16;
                                        if (rmem(tid, (unsigned long)ob, oa, on * 8) == (ssize_t)(on * 8)) {
                                            fprintf(out, "repoffs:");
                                            for (size_t k = 0; k < on; k++) fprintf(out, " %llu", (unsigned long long)oa[k]);
                                            fprintf(out, "\n");
                                        }
                                    }
                                    fflush(out);
                                    break;
                                } else if (c2 == 0x7206u || c2 == 0x720cu || c2 == 0x7201u) {
                                    continue; /* COMPLETE/NOOP/OK */
                                } else break;
                            }
                        }
                    }
                }
            }
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
        } else if (sig == SIGTRAP) {
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
        } else {
            ptrace(PTRACE_SYSCALL, tid, 0, (void *)(long)sig);
        }
    }
    for (int i = 0; i < na; i++) ptrace(PTRACE_DETACH, attached[i], 0, 0);
    fclose(out);
    fprintf(stderr, "IOTRACE_DONE\n");
    return 0;
}
