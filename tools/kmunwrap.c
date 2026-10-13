/* kmunwrap: unwrap a Keystore-wrapped key via Keymaster 4.0 HIDL (hwbinder),
 * replicating vold's decryptWithKeystoreKey (AES-256-GCM, appId-bound).
 * Usage: kmunwrap <keymaster_blob.bin> <appid.hex> <nonce.hex> <ct.hex> <tag.hex>
 * Prints raw key hex on stdout. Static build:
 *   aarch64-linux-gnu-gcc -static -O2 -o kmunwrap kmunwrap.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define BINDER_WRITE_READ_NR ((3u<<30)|(48u<<16)|((unsigned)'b'<<8)|1u)
#define BINDER_VERSION_NR    ((3u<<30)|(4u<<16)|((unsigned)'b'<<8)|9u)

#define _IOWU(t,n,s) ((1u<<30)|((unsigned)(s)<<16)|((unsigned)(t)<<8)|(n))
#define _IORU(t,n,s) ((2u<<30)|((unsigned)(s)<<16)|((unsigned)(t)<<8)|(n))
#define _IOU(t,n)    (((unsigned)(t)<<8)|(n))
#define BC_TRANSACTION _IOWU('c',0,64)
#define BC_TRANSACTION_SG _IOWU('c',17,72)
#define BC_REPLY _IOWU('c',1,64)
#define BC_FREE_BUFFER _IOWU('c',3,8)
#define BR_REPLY _IORU('r',3,64)
#define BR_TRANSACTION_COMPLETE _IOU('r',6)
#define BR_FAILED_REPLY _IOU('r',17)
#define BR_NOOP _IOU('r',12)
#define BR_OKK _IOU('r',1)
#define BR_DEAD_REPLY _IOU('r',5)

#define B_PACK(c0,c1,c2,c3) (((uint32_t)(c0)<<24)|((uint32_t)(c1)<<16)|((uint32_t)(c2)<<8)|(uint32_t)(c3))
#define BINDER_TYPE_HANDLE (B_PACK('s','h','*',0x85))
#define BINDER_TYPE_PTR    (B_PACK('p','t','*',0x85))
#define HAS_PARENT 0x01

/* IKeymasterDevice 4.0 transact codes (declaration order, 1-based) */
#define KM_BEGIN 16
#define KM_UPDATE 17
#define KM_FINISH 18
#define KM_GET_HW_INFO 1

static int bfd = -1;
static uint8_t *bmap = NULL;
#define BMAP_SIZE (256*1024)

struct __attribute__((packed)) bwr {
    uint64_t write_size, write_consumed, write_buffer;
    uint64_t read_size, read_consumed, read_buffer;
};
struct __attribute__((packed)) trdata {
    uint32_t handle; uint32_t _p0;
    uint64_t cookie, code, flags; /* code/flags u32 in 8B slots? no: u32+u32, pid+euid */
};
/* exact 64B binder_transaction_data */
struct __attribute__((packed)) txn {
    uint32_t handle; uint32_t pad0;
    uint64_t cookie;
    uint32_t code; uint32_t flags;
    uint32_t pid; uint32_t euid;
    uint64_t data_size, offsets_size, buf_ptr, off_ptr;
};

static void die(const char *m) { perror(m); exit(1); }

/* ---------- parcel builder ---------- */
typedef struct {
    uint8_t *buf; size_t len, cap;
    /* vec records in write order: struct pos, data, parent info */
    struct { size_t struct_pos; uint8_t *data; ssize_t boff; size_t dlen; int parent_slot; size_t parent_off; } *vecs;
    int nvecs, vec_cap;
    uint64_t *obj_offs; int nobjs, obj_cap; /* offsets array (positions of buffer objects) */
    /* buffer object structs appended at end; each tied to a vec by index */
} Parcel;

static void p_init(Parcel *p) { memset(p, 0, sizeof *p); p->cap = 1024; p->buf = malloc(p->cap); }
static void p_reserve(Parcel *p, size_t n) {
    while (p->len + n > p->cap) { p->cap *= 2; p->buf = realloc(p->buf, p->cap); }
}
static size_t p_pos(Parcel *p) { return p->len; }
static void p_align(Parcel *p, size_t a) { while (p->len % a) { p_reserve(p,1); p->buf[p->len++] = 0; } }
static void p_u32(Parcel *p, uint32_t v) { p_align(p,4); p_reserve(p,4); memcpy(p->buf+p->len,&v,4); p->len+=4; }
static void p_u64(Parcel *p, uint64_t v) { p_align(p,8); p_reserve(p,8); memcpy(p->buf+p->len,&v,8); p->len+=8; }
static void p_bytes(Parcel *p, const void *d, size_t n) { p_reserve(p,n); memcpy(p->buf+p->len,d,n); p->len+=n; }

/* reserve a hidl_vec struct (24B: buffer u64, size u64, owns u8+pad), record data for later */
static int p_vec(Parcel *p, const uint8_t *data, size_t dlen, int parent_slot, size_t parent_off) {
    p_align(p, 8);
    size_t spos = p->len;
    uint64_t zero = 0;
    p_bytes(p, &zero, 8);           /* mBuffer placeholder */
    p_u64(p, (uint64_t)dlen);
    p_reserve(p, 8); p->buf[p->len++] = 0; memset(p->buf+p->len, 0, 7); p->len += 7; /* owns+pad */
    if (p->nvecs == p->vec_cap) {
        p->vec_cap = p->vec_cap ? p->vec_cap*2 : 8;
        p->vecs = realloc(p->vecs, p->vec_cap * sizeof *p->vecs);
    }
    int idx = p->nvecs++;
    p->vecs[idx].struct_pos = spos;
    p->vecs[idx].data = (uint8_t*)data;
    p->vecs[idx].boff = -1;
    p->vecs[idx].dlen = dlen;
    p->vecs[idx].parent_slot = parent_slot;
    p->vecs[idx].parent_off = parent_off;
    return idx;
}
/* hidl_string: {buffer u64, size u32, owns u32} 16B */
static int p_string(Parcel *p, const char *s) {
    p_align(p, 8);
    size_t slen = strlen(s) + 1;
    size_t spos = p->len;
    uint64_t zero = 0;
    p_bytes(p, &zero, 8);
    p_u32(p, (uint32_t)slen);
    p_u32(p, 0);
    if (p->nvecs == p->vec_cap) {
        p->vec_cap = p->vec_cap ? p->vec_cap*2 : 8;
        p->vecs = realloc(p->vecs, p->vec_cap * sizeof *p->vecs);
    }
    int idx = p->nvecs++;
    p->vecs[idx].struct_pos = spos;
    p->vecs[idx].data = (uint8_t*)s;
    p->vecs[idx].boff = -1;
    p->vecs[idx].dlen = slen;
    p->vecs[idx].parent_slot = -1;
    p->vecs[idx].parent_off = 0;
    return idx;
}
static void p_token(Parcel *p, const char *iface) {
    /* String16: int32 len(chars) + utf16le */
    size_t n = strlen(iface);
    p_u32(p, (uint32_t)n);
    for (size_t i = 0; i < n; i++) { p_reserve(p,2); p->buf[p->len++] = iface[i]; p->buf[p->len++] = 0; }
}
/* KeyParameter element (40B). Returns element offset; caller notes blob struct pos = el+16. */
static size_t p_keyparam(Parcel *p, uint32_t tag, uint64_t f) {
    p_align(p, 8);
    size_t el = p->len;
    p_u32(p, tag); p_u32(p, 0); p_u64(p, f);
    /* blob vec struct inline (parent set by caller via p_vec_at) */
    uint64_t zero = 0;
    p_bytes(p, &zero, 8); p_u64(p, 0);
    p_reserve(p, 8); p->buf[p->len++] = 0; memset(p->buf+p->len, 0, 7); p->len += 7;
    return el;
}
/* attach vec data to an already-written vec struct at struct_pos */
static int p_vec_at(Parcel *p, size_t struct_pos, const uint8_t *data, size_t dlen,
                    int parent_slot, size_t parent_off) {
    if (p->nvecs == p->vec_cap) {
        p->vec_cap = p->vec_cap ? p->vec_cap*2 : 8;
        p->vecs = realloc(p->vecs, p->vec_cap * sizeof *p->vecs);
    }
    int idx = p->nvecs++;
    p->vecs[idx].struct_pos = struct_pos;
    p->vecs[idx].data = (uint8_t*)data;
    p->vecs[idx].boff = -1;
    p->vecs[idx].dlen = dlen;
    p->vecs[idx].parent_slot = parent_slot;
    p->vecs[idx].parent_off = parent_off;
    /* patch size field */
    memcpy(p->buf + struct_pos + 8, &dlen, 8);
    return idx;
}

/* finalize: returns malloc'd contiguous request (caller frees); fills *out_offs (malloc'd) */
static uint8_t *p_finalize(Parcel *p, size_t *out_len, uint64_t **out_offs, int *out_n) {
    /* buffer object struct: type u32, flags u32, buffer u64, length u64, parent u64, parent_off u64 = 40B */
    p_align(p, 8);
    size_t obj_base = p->len;
    for (int i = 0; i < p->nvecs; i++) {
        uint32_t t = BINDER_TYPE_PTR;
        uint32_t fl = (p->vecs[i].parent_slot >= 0) ? HAS_PARENT : 0;
        p_bytes(p, &t, 4); p_bytes(p, &fl, 4);
        uint64_t z = 0; p_bytes(p, &z, 8); /* buffer: patched below */
        uint64_t ln = p->vecs[i].dlen; p_bytes(p, &ln, 8);
        uint64_t par = p->vecs[i].parent_slot >= 0 ? (uint64_t)p->vecs[i].parent_slot : 0;
        p_bytes(p, &par, 8);
        uint64_t po = p->vecs[i].parent_off; p_bytes(p, &po, 8);
    }
    /* patch buffer addresses: need stable base address -> copy to fixed malloc first */
    uint8_t *req = malloc(p->len);
    memcpy(req, p->buf, p->len);
    uint64_t base = (uint64_t)req;
    (void)base;
    /* data blobs: we point at CALLER-owned buffers (must stay alive during ioctl) */
    for (int i = 0; i < p->nvecs; i++) {
        size_t opos = obj_base + (size_t)i * 40;
        uint64_t addr;
        if (p->vecs[i].boff >= 0) addr = (uint64_t)req + (uint64_t)p->vecs[i].boff;
        else addr = p->vecs[i].dlen ? (uint64_t)p->vecs[i].data : 0;
        memcpy(req + opos + 8, &addr, 8);
    }
    uint64_t *offs = malloc(sizeof(uint64_t) * (size_t)(p->nvecs ? p->nvecs : 1));
    for (int i = 0; i < p->nvecs; i++) offs[i] = (uint64_t)(obj_base + (size_t)i * 40);
    *out_len = p->len; *out_offs = offs; *out_n = p->nvecs;
    return req;
}

/* ---------- binder transact ---------- */
static uint8_t rbuf[65536];
static int do_tr(uint32_t handle, uint32_t code, uint8_t *data, size_t dlen,
                 uint64_t *offs, int noff, uint64_t extra, uint8_t **rout, size_t *rlen,
                 uint64_t **robj, int *rnobj) {
    struct txn td;
    memset(&td, 0, sizeof td);
    td.handle = handle; td.code = code; td.flags = 0;
    td.data_size = dlen; td.offsets_size = (uint64_t)noff * 8;
    td.buf_ptr = (uint64_t)data; td.off_ptr = (uint64_t)offs;
    uint8_t wcmd[4 + 72];
    uint32_t bc = extra ? BC_TRANSACTION_SG : BC_TRANSACTION;
    memcpy(wcmd, &bc, 4); memcpy(wcmd + 4, &td, sizeof td);
    size_t wlen = 4 + sizeof td;
    if (extra) { memcpy(wcmd + 4 + sizeof td, &extra, 8); wlen += 8; }
    struct bwr rw;
    memset(&rw, 0, sizeof rw);
    rw.write_size = wlen; rw.write_buffer = (uint64_t)wcmd;
    rw.read_size = sizeof rbuf; rw.read_buffer = (uint64_t)rbuf;
    if (ioctl(bfd, BINDER_WRITE_READ_NR, &rw) < 0) { perror("ioctl W_R"); return -1; }
    /* parse read buffer */
    size_t pos = 0;
    static uint8_t reply[65536]; static uint64_t reply_offs[256];
    int got_reply = 0;
    while (pos + 4 <= rw.read_consumed) {
        uint32_t cmd; memcpy(&cmd, rbuf + pos, 4); pos += 4;
        if (cmd == BR_TRANSACTION_COMPLETE) continue;
        if (cmd == BR_NOOP || cmd == BR_OKK) continue;
        if (cmd == BR_REPLY) {
            struct txn *r = (struct txn *)(rbuf + pos); pos += sizeof *r;
            size_t ds = (size_t)r->data_size;
            int no = (int)(r->offsets_size / 8);
            /* reply data lives in our mmap at r->buf_ptr */
            if (ds > sizeof reply) { fprintf(stderr, "reply too big\n"); return -1; }
            memcpy(reply, (void*)(uintptr_t)r->buf_ptr, ds);
            uint64_t *oarr = (uint64_t*)(uintptr_t)r->off_ptr;
            for (int i = 0; i < no && i < 256; i++) reply_offs[i] = oarr[i];
            /* free the kernel buffer */
            uint8_t fc[4 + 8]; uint32_t fcmd = BC_FREE_BUFFER;
            memcpy(fc, &fcmd, 4); memcpy(fc + 4, &r->buf_ptr, 8);
            struct bwr rw2; memset(&rw2, 0, sizeof rw2);
            rw2.write_size = sizeof fc; rw2.write_buffer = (uint64_t)fc;
            ioctl(bfd, BINDER_WRITE_READ_NR, &rw2);
            *rout = reply; *rlen = ds; *robj = reply_offs; *rnobj = no;
            got_reply = 1;
            break;
        } else if (cmd == BR_FAILED_REPLY || cmd == BR_DEAD_REPLY) {
            fprintf(stderr, "binder reply error %u\n", cmd);
            return -1;
        } else {
            fprintf(stderr, "unexpected binder cmd %u\n", cmd);
            return -1;
        }
    }
    return got_reply ? 0 : -1;
}

/* ---------- reply parsing (order-based vec resolution) ---------- */
typedef struct { uint8_t *d; size_t len; uint64_t *objs; int nobj; size_t cur; int objcur;
                 /* resolved buffers in order */
                 struct { uint64_t addr; uint64_t len; } *bufs; int nbufs; } R;
static void r_init(R *r, uint8_t *d, size_t len, uint64_t *objs, int nobj) {
    memset(r, 0, sizeof *r);
    r->d = d; r->len = len; r->objs = objs; r->nobj = nobj;
    r->bufs = malloc(sizeof *r->bufs * (size_t)(nobj + 1));
    for (int i = 0; i < nobj; i++) {
        uint64_t opos = objs[i];
        uint32_t type; memcpy(&type, d + opos, 4);
        if (type == BINDER_TYPE_HANDLE) {
            uint32_t h; memcpy(&h, d + opos + 8, 4);
            r->bufs[i].addr = 0; r->bufs[i].len = ((uint64_t)h << 32) | 0x48414e44u;
        } else if (type == BINDER_TYPE_PTR) {
            uint64_t a, l; memcpy(&a, d + opos + 8, 8); memcpy(&l, d + opos + 16, 8);
            r->bufs[i].addr = a; r->bufs[i].len = l;
        } else { r->bufs[i].addr = 0; r->bufs[i].len = 0; }
    }
    r->nbufs = nobj;
}
static uint32_t r_u32(R *r) { r->cur = (r->cur + 3) & ~(size_t)3; uint32_t v; memcpy(&v, r->d + r->cur, 4); r->cur += 4; return v; }
static uint64_t r_u64(R *r) { r->cur = (r->cur + 7) & ~(size_t)7; uint64_t v; memcpy(&v, r->d + r->cur, 8); r->cur += 8; return v; }
/* consume next vec object; returns index into bufs */
static int r_nextbuf(R *r) { if (r->objcur >= r->nbufs) { fprintf(stderr, "obj overrun\n"); exit(1); } return r->objcur++; }
/* skip a vec<KeyParameter> (each element 40B + nested blobs consume objects in order) */
static void r_skip_kpvec(R *r) {
    r->cur = (r->cur + 7) & ~(size_t)7; r->cur += 24; /* vec struct */
    int bi = r_nextbuf(r);
    /* array bytes live at bufs[bi]; count unknown -> derive: elements follow? We can't know count
       from struct alone... count = bufs[bi].len / 40 */
    size_t nel = r->bufs[bi].len / 40;
    /* copy element bytes for blob scanning */
    uint8_t *arr = (uint8_t*)(uintptr_t)r->bufs[bi].addr;
    for (size_t i = 0; i < nel; i++) {
        /* every element blob has exactly one object, empty or not */
        r_nextbuf(r);
        (void)arr;
    }
}
/* read a vec<uint8_t> into malloc'd buffer */
static uint8_t *r_vec(R *r, size_t *out_len) {
    r->cur = (r->cur + 7) & ~(size_t)7; r->cur += 24;
    int bi = r_nextbuf(r);
    size_t n = (size_t)r->bufs[bi].len;
    uint8_t *o = malloc(n ? n : 1);
    if (n) memcpy(o, (void*)(uintptr_t)r->bufs[bi].addr, n);
    *out_len = n;
    return o;
}
static int32_t r_status(R *r) { return (int32_t)r_u32(r); }

/* ---------- file helpers ---------- */
static uint8_t *readfile(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s + 1); if (fread(b, 1, (size_t)s, f) != (size_t)s) exit(1);
    fclose(f); *n = (size_t)s; return b;
}
static uint8_t *unhex(const char *h, size_t *n) {
    size_t l = strlen(h);
    while (l && (h[l-1] == '\n' || h[l-1] == ' ' || h[l-1] == '\r')) l--;
    uint8_t *o = malloc(l / 2 + 1);
    for (size_t i = 0; i < l / 2; i++) { unsigned b; sscanf(h + 2*i, "%2x", &b); o[i] = (uint8_t)b; }
    *n = l / 2; return o;
}

/* emit vec record for an already-written struct */
static void rec_vec(Parcel *p, size_t struct_pos, const uint8_t *data, size_t dlen,
                    int parent_slot, size_t parent_off) {
    if (p->nvecs == p->vec_cap) {
        p->vec_cap = p->vec_cap ? p->vec_cap*2 : 16;
        p->vecs = realloc(p->vecs, p->vec_cap * sizeof *p->vecs);
    }
    int idx = p->nvecs++;
    p->vecs[idx].struct_pos = struct_pos;
    p->vecs[idx].data = (uint8_t*)data;
    p->vecs[idx].boff = -1;
    p->vecs[idx].dlen = dlen;
    p->vecs[idx].parent_slot = parent_slot;
    p->vecs[idx].parent_off = parent_off;
    memcpy(p->buf + struct_pos + 8, &dlen, 8);
}

/* send one transact; returns 0 with reply parsed into R */
static int transact(uint32_t handle, uint32_t code, Parcel *p, R *r) {
    size_t dl; uint64_t *of; int no;
    uint8_t *req = p_finalize(p, &dl, &of, &no);
    { const char *dd = getenv("KM_DUMP");
      if (dd && dd[0]=='1') {
        fprintf(stderr, "parcel len=%zu nobjs=%d\n", dl, no);
        for (size_t i = 0; i < dl && i < 208; i++) {
            fprintf(stderr, "%02x%s", req[i], (i+1)%32 ? "" : "\n");
        }
        fprintf(stderr, "\noffs:");
        for (int i = 0; i < no; i++) fprintf(stderr, " %llu", (unsigned long long)of[i]);
        fprintf(stderr, "\n");
      } }
    uint8_t *rd; size_t rl; uint64_t *ro; int rn;
    uint64_t extra = 0;
    for (int i = 0; i < p->nvecs; i++) extra += (p->vecs[i].dlen + 7) & ~(size_t)7;
    int rc = do_tr(handle, code, req, dl, of, no, extra, &rd, &rl, &ro, &rn);
    free(req); free(of);
    if (rc < 0) return -1;
    r_init(r, rd, rl, ro, rn);
    return 0;
}

static const char *sm_token = "android.hidl.manager@1.0::IServiceManager";
static uint32_t sm_getcode = 1;

/* IBase::interfaceDescriptor probe: transact handle-0, parse status+string */
static int ibasedesc(const char *token) {
    bfd = open("/dev/hwbinder", O_RDWR | O_CLOEXEC);
    if (bfd < 0) die("open hwbinder");
    int32_t ver = 0;
    if (ioctl(bfd, BINDER_VERSION_NR, &ver) < 0) die("version");
    bmap = mmap(NULL, BMAP_SIZE, PROT_READ, MAP_PRIVATE, bfd, 0);
    if (bmap == MAP_FAILED) die("mmap");
    Parcel p; p_init(&p);
    p_token(&p, token);
    p.nvecs = 0;
    R r;
    if (transact(0, 256136003u, &p, &r) < 0) { fprintf(stderr, "ibasedesc transact failed\n"); return 1; }
    int32_t st = r_status(&r);
    fprintf(stderr, "status=%d (0x%x)\n", st, (unsigned)st);
    if (st) return 1;
    r.cur = (r.cur + 7) & ~(size_t)7; r.cur += 16;
    int bi = r_nextbuf(&r);
    size_t n = (size_t)r.bufs[bi].len;
    uint8_t *s = (uint8_t*)(uintptr_t)r.bufs[bi].addr;
    printf("descriptor(%zu): %.*s\n", n, (int)n, s);
    return 0;
}

static uint32_t sm_get(const char *fqname, const char *instance) {
    Parcel p; p_init(&p);
    p_token(&p, sm_token);
    int s1 = p_string(&p, fqname);
    int s2 = p_string(&p, instance);
    (void)s1; (void)s2;
    size_t dl; uint64_t *of; int no;
    uint8_t *req = p_finalize(&p, &dl, &of, &no);
    { const char *dd = getenv("KM_DUMP");
      if (dd && dd[0]=='1') {
        FILE *mf = fopen("/proc/self/maps", "r");
        if (mf) { char lb[256]; int nl = 0;
          while (fgets(lb, sizeof lb, mf) && nl < 8) { fprintf(stderr, "map: %s", lb); nl++; }
          fclose(mf); }
        for (int i = 0; i < no; i++) {
          size_t opos = (size_t)of[i];
          uint64_t av = 0; memcpy(&av, req + opos + 8, 8);
          fprintf(stderr, "obj%d addr=%llx len_field_off=%zu\n", i,
                  (unsigned long long)av, opos);
        }
      } }
    uint8_t *rd; size_t rl; uint64_t *ro; int rn;
    /* IServiceManager::get */
    uint64_t extra = 0;
    for (int i = 0; i < p.nvecs; i++) extra += (p.vecs[i].dlen + 7) & ~(size_t)7;
    if (do_tr(0, sm_getcode, req, dl, of, no, extra, &rd, &rl, &ro, &rn) < 0) { fprintf(stderr, "sm get failed\n"); exit(1); }
    R r; r_init(&r, rd, rl, ro, rn);
    int32_t st = r_status(&r);
    if (st) { fprintf(stderr, "sm status %d\n", st); exit(1); }
    if (rn < 1) { fprintf(stderr, "sm: no object (service missing?)\n"); exit(1); }
    uint32_t type; memcpy(&type, rd + ro[0], 4);
    if (type != BINDER_TYPE_HANDLE) { fprintf(stderr, "sm: not a handle\n"); exit(1); }
    uint32_t h; memcpy(&h, rd + ro[0] + 8, 4);
    free(req); free(of);
    return h;
}
static const uint8_t ZEROS[56] = {0};
static const uint8_t EMPTY1[1] = {0};

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "desc")) return ibasedesc(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "ping")) {
        /* IBase::ping on handle 0 with given token */
        bfd = open("/dev/hwbinder", O_RDWR | O_CLOEXEC);
        if (bfd < 0) die("open hwbinder");
        int32_t ver = 0;
        if (ioctl(bfd, BINDER_VERSION_NR, &ver) < 0) die("version");
        bmap = mmap(NULL, BMAP_SIZE, PROT_READ, MAP_PRIVATE, bfd, 0);
        if (bmap == MAP_FAILED) die("mmap");
        Parcel p; p_init(&p);
        const char *ptok = getenv("KM_NOTOKEN") ? NULL : argv[2];
        if (ptok) p_token(&p, ptok);
        p.nvecs = 0;
        R r;
        uint32_t code = getenv("KM_CODE") ? (uint32_t)atoi(getenv("KM_CODE")) : 256921159u;
        if (transact(0, code, &p, &r) < 0) { fprintf(stderr, "ping transact failed\n"); return 1; }
        { size_t i; fprintf(stderr, "reply len=%zu nobjs=%d\n", r.len, r.nbufs);
          for (i = 0; i < r.len && i < 64; i++) fprintf(stderr, "%02x%s", r.d[i], (i+1)%16 ? "" : "\n");
          fprintf(stderr, "\n"); }
        int32_t st = r_status(&r);
        fprintf(stderr, "ping status=%d (0x%x)\n", st, (unsigned)st);
        return st ? 1 : 0;
    }
    if (argc != 6 && argc != 8) { fprintf(stderr, "usage: %s <blob.bin> <appid_hex> <nonce_hex> <ct_hex> <tag_hex> [sm_token sm_code]\n", argv[0]); return 2; }
    if (argc == 8) { sm_token = argv[6]; sm_getcode = (uint32_t)atoi(argv[7]); }
    size_t blen, alen, nlen, clen, tlen;
    uint8_t *blob = readfile(argv[1], &blen);
    uint8_t *appid = unhex(argv[2], &alen);
    uint8_t *nonce = unhex(argv[3], &nlen);
    uint8_t *ct = unhex(argv[4], &clen);
    uint8_t *tag = unhex(argv[5], &tlen);
    fprintf(stderr, "blob=%zu appid=%zu nonce=%zu ct=%zu tag=%zu\n", blen, alen, nlen, clen, tlen);

    bfd = open("/dev/hwbinder", O_RDWR | O_CLOEXEC);
    if (bfd < 0) die("open hwbinder");
    int32_t ver = 0;
    if (ioctl(bfd, BINDER_VERSION_NR, &ver) < 0) die("version");
    fprintf(stderr, "binder version=%d\n", ver);
    bmap = mmap(NULL, BMAP_SIZE, PROT_READ, MAP_PRIVATE, bfd, 0);
    if (bmap == MAP_FAILED) die("mmap");

    uint32_t km = sm_get("android.hardware.keymaster@4.0::IKeymasterDevice", "default");
    fprintf(stderr, "keymaster handle=%u\n", km);

    const uint32_t TAG_APP_ID = 0x90000259u, TAG_BLOCKMODE = 0x20000004u,
                   TAG_PADDING = 0x20000006u, TAG_NONCE = 0x900003E9u,
                   TAG_MACLEN = 0x300003EBu;

    /* ---- begin(DECRYPT, blob, params, emptyAuth) ---- */
    Parcel p; p_init(&p);
    p_token(&p, "android.hardware.keymaster@4.0::IKeymasterDevice");
    p_u32(&p, 1); /* DECRYPT */
    p_align(&p, 8);
    size_t s_blob = p_pos(&p);
    { uint64_t z=0; p_bytes(&p,&z,8); p_u64(&p,0); p_reserve(&p,8); p.buf[p.len++]=0; memset(p.buf+p.len,0,7); p.len+=7; }
    size_t s_params = p_pos(&p);
    { uint64_t z=0; p_bytes(&p,&z,8); p_u64(&p,0); p_reserve(&p,8); p.buf[p.len++]=0; memset(p.buf+p.len,0,7); p.len+=7; }
    p_align(&p, 8);
    size_t apos = p_pos(&p);
    p_bytes(&p, ZEROS, 56); /* authToken */
    size_t epos[5];
    epos[0] = p_keyparam(&p, TAG_APP_ID, 0);
    epos[1] = p_keyparam(&p, TAG_BLOCKMODE, 32);
    epos[2] = p_keyparam(&p, TAG_PADDING, 1);
    epos[3] = p_keyparam(&p, TAG_NONCE, 0);
    epos[4] = p_keyparam(&p, TAG_MACLEN, 128);
    p.nvecs = 0; /* manual vec order from here */
    rec_vec(&p, s_blob, blob, blen, -1, 0);
    int v_params = p.nvecs;
    p.vecs[v_params].struct_pos = s_params;
    p.vecs[v_params].data = NULL;
    p.vecs[v_params].boff = (ssize_t)epos[0];
    p.vecs[v_params].dlen = 5 * 40;
    p.vecs[v_params].parent_slot = -1;
    p.vecs[v_params].parent_off = 0;
    p.nvecs++;
    memcpy(p.buf + s_params + 8, &(uint64_t){5*40}, 8);
    rec_vec(&p, epos[0]+16, appid, alen, v_params, 0*40+16);
    rec_vec(&p, epos[1]+16, EMPTY1, 0, v_params, 1*40+16);
    rec_vec(&p, epos[2]+16, EMPTY1, 0, v_params, 2*40+16);
    rec_vec(&p, epos[3]+16, nonce, nlen, v_params, 3*40+16);
    rec_vec(&p, epos[4]+16, EMPTY1, 0, v_params, 4*40+16);
    rec_vec(&p, apos+40, EMPTY1, 0, -1, 0);

    R r;
    if (transact(km, KM_BEGIN, &p, &r) < 0) { fprintf(stderr, "begin transact failed\n"); return 1; }
    if (r_status(&r)) { fprintf(stderr, "begin status!=0\n"); return 1; }
    int32_t err = (int32_t)r_u32(&r);
    fprintf(stderr, "begin error=%d\n", err);
    if (err) return 1;
    r_skip_kpvec(&r);
    uint64_t op = r_u64(&r);
    fprintf(stderr, "op=%llu\n", (unsigned long long)op);

    /* ---- update(op, [], ct, auth, ver) ---- */
    Parcel q; p_init(&q);
    p_token(&q, "android.hardware.keymaster@4.0::IKeymasterDevice");
    p_u64(&q, op);
    p_align(&q, 8);
    size_t q_params = p_pos(&q);
    { uint64_t z=0; p_bytes(&q,&z,8); p_u64(&q,0); p_reserve(&q,8); q.buf[q.len++]=0; memset(q.buf+q.len,0,7); q.len+=7; }
    size_t q_input = p_pos(&q);
    { uint64_t z=0; p_bytes(&q,&z,8); p_u64(&q,0); p_reserve(&q,8); q.buf[q.len++]=0; memset(q.buf+q.len,0,7); q.len+=7; }
    p_align(&q, 8);
    size_t q_auth = p_pos(&q); p_bytes(&q, ZEROS, 56);
    size_t q_ver = p_pos(&q); p_bytes(&q, ZEROS, 48);
    q.nvecs = 0;
    rec_vec(&q, q_params, EMPTY1, 0, -1, 0);
    rec_vec(&q, q_input, ct, clen, -1, 0);
    rec_vec(&q, q_auth+40, EMPTY1, 0, -1, 0);
    rec_vec(&q, q_ver+40, EMPTY1, 0, -1, 0);
    R r2;
    if (transact(km, KM_UPDATE, &q, &r2) < 0) { fprintf(stderr, "update transact failed\n"); return 1; }
    if (r_status(&r2)) { fprintf(stderr, "update status!=0\n"); return 1; }
    err = (int32_t)r_u32(&r2);
    fprintf(stderr, "update error=%d\n", err);
    if (err) return 1;
    uint32_t consumed = r_u32(&r2);
    fprintf(stderr, "consumed=%u\n", consumed);
    r_skip_kpvec(&r2);
    { size_t olen; uint8_t *o = r_vec(&r2, &olen); fprintf(stderr, "update output=%zu\n", olen); free(o); }

    /* ---- finish(op, [], tag, auth, ver) -> raw key ---- */
    Parcel f; p_init(&f);
    p_token(&f, "android.hardware.keymaster@4.0::IKeymasterDevice");
    p_u64(&f, op);
    p_align(&f, 8);
    size_t f_params = p_pos(&f);
    { uint64_t z=0; p_bytes(&f,&z,8); p_u64(&f,0); p_reserve(&f,8); f.buf[f.len++]=0; memset(f.buf+f.len,0,7); f.len+=7; }
    size_t f_input = p_pos(&f);
    { uint64_t z=0; p_bytes(&f,&z,8); p_u64(&f,0); p_reserve(&f,8); f.buf[f.len++]=0; memset(f.buf+f.len,0,7); f.len+=7; }
    p_align(&f, 8);
    size_t f_auth = p_pos(&f); p_bytes(&f, ZEROS, 56);
    size_t f_ver = p_pos(&f); p_bytes(&f, ZEROS, 48);
    f.nvecs = 0;
    rec_vec(&f, f_params, EMPTY1, 0, -1, 0);
    rec_vec(&f, f_input, tag, tlen, -1, 0);
    rec_vec(&f, f_auth+40, EMPTY1, 0, -1, 0);
    rec_vec(&f, f_ver+40, EMPTY1, 0, -1, 0);
    R r3;
    if (transact(km, KM_FINISH, &f, &r3) < 0) { fprintf(stderr, "finish transact failed\n"); return 1; }
    if (r_status(&r3)) { fprintf(stderr, "finish status!=0\n"); return 1; }
    err = (int32_t)r_u32(&r3);
    fprintf(stderr, "finish error=%d\n", err);
    if (err) return 1;
    r_skip_kpvec(&r3);
    { size_t olen; uint8_t *o = r_vec(&r3, &olen);
      for (size_t i = 0; i < olen; i++) printf("%02x", o[i]);
      printf("\n");
      fprintf(stderr, "output=%zu bytes\n", olen); }
    return 0;
}
