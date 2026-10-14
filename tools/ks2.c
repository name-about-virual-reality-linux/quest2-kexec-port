/* ks2: unwrap a Keystore-wrapped key via Keystore2 AIDL (framework binder).
 * Flow: SM.getService(keystore2) -> getSecurityLevel(TEE) ->
 *   createOperation(BLOB desc, GCM-DECRYPT params) -> update(ct) -> finish(tag)
 * Usage: ks2 <keymaster_blob.bin> <appid_hex> <nonce_hex> <ct_hex> <tag_hex>
 * Static build: aarch64-linux-gnu-gcc -static -O2 -o ks2 ks2.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define WRR ((3u<<30)|(48u<<16)|((unsigned)'b'<<8)|1u)
#define VERN ((3u<<30)|(4u<<16)|((unsigned)'b'<<8)|9u)
#define _IOWU(t,n,s) ((1u<<30)|((unsigned)(s)<<16)|((unsigned)(t)<<8)|(n))
#define _IORU(t,n,s) ((2u<<30)|((unsigned)(s)<<16)|((unsigned)(t)<<8)|(n))
#define _IOU(t,n) (((unsigned)(t)<<8)|(n))
#define BC_TR _IOWU('c',0,64)
#define BC_SG _IOWU('c',17,72)
#define BC_FB _IOWU('c',3,8)
#define BR_RP _IORU('r',3,64)
#define BR_TC _IOU('r',6)
#define BR_NOOP _IOU('r',12)
#define BR_OKK _IOU('r',1)
#define B_HDL ((((uint32_t)'s')<<24)|(((uint32_t)'h')<<16)|(((uint32_t)'*')<<8)|0x85u)
#define B_PTR ((((uint32_t)'p')<<24)|(((uint32_t)'t')<<16)|(((uint32_t)'*')<<8)|0x85u)

static int bfd = -1;
struct __attribute__((packed)) bwr { uint64_t ws,wc,wb,rs,rc,rb; };
struct __attribute__((packed)) txn {
    uint32_t h; uint32_t p0; uint64_t cookie;
    uint32_t code; uint32_t flags; uint32_t pid; uint32_t euid;
    uint64_t ds, os, bp, op;
};
static void die(const char *m) { perror(m); exit(1); }

/* ---------- builder ---------- */
static uint8_t *B; static size_t BL, BC;
static void b_init(void) { BC = 4096; B = malloc(BC); BL = 0; }
static void b_res(size_t n) { while (BL + n > BC) { BC *= 2; B = realloc(B, BC); } }
static void b_al(size_t a) { while (BL % a) { b_res(1); B[BL++] = 0; } }
static void b_u32(uint32_t v) { b_al(4); b_res(4); memcpy(B+BL,&v,4); BL+=4; }
static void b_u64(uint64_t v) { b_al(8); b_res(8); memcpy(B+BL,&v,8); BL+=8; }
static void b_by(const void *d, size_t n) { b_res(n); memcpy(B+BL,d,n); BL+=n; }
static void b_hdr(void) {
    b_u32(0x80000000u); b_u32(0xFFFFFFFFu);
    b_by("TSYS", 4);
}
static void b_tok(const char *s) {
    /* AIDL interface token = String16 */
    size_t n = strlen(s);
    b_u32((uint32_t)n);
    for (size_t i = 0; i < n; i++) { b_res(2); B[BL++] = s[i]; B[BL++] = 0; }
    b_res(2); B[BL++] = 0; B[BL++] = 0;
    while (BL % 4) { b_res(1); B[BL++] = 0; }
}
static void b_str(const char *s) { /* AIDL String16, same as token */
    size_t n = strlen(s);
    b_u32((uint32_t)n);
    for (size_t i = 0; i < n; i++) { b_res(2); B[BL++] = s[i]; B[BL++] = 0; }
    b_res(2); B[BL++] = 0; B[BL++] = 0;
    while (BL % 4) { b_res(1); B[BL++] = 0; }
}
static void b_bytes(const uint8_t *d, size_t n) { /* byte[]: len + bytes + pad */
    b_u32((uint32_t)n);
    b_by(d, n);
    while (BL % 4) { b_res(1); B[BL++] = 0; }
}
static void b_bytes_null(void) { b_u32(0xFFFFFFFFu); } /* @nullable null */
/* parcelable scope: returns pos of size field; call b_pend() to fill */
static size_t b_pstart(void) { b_al(4); size_t p = BL; b_u32(0); return p; }
static void b_pend(size_t p) {
    uint32_t sz = (uint32_t)(BL - (p + 4));
    memcpy(B + p, &sz, 4);
}
/* KeyParameter parcelable: [present=1][size=4+content][tag][1][union][value].
   value u32 (scalar) or [len][data+pad] (blob). content = tag+1+union+value. */
static void b_kp(uint32_t tag, int vtag, const void *v, size_t vlen, int vint) {
    b_u32(1); /* present */
    if (vtag == 14) {
        uint32_t content = 4 + 4 + 4 + 4 + (uint32_t)vlen;
        while ((content % 4) != 0) content++;
        b_u32(4 + content); /* size incl itself */
        b_u32(tag);
        b_u32(1);
        b_u32((uint32_t)vtag);
        b_bytes(v, vlen);
    } else {
        b_u32(4 + 4 + 4 + 4 + 4); /* size incl itself = 20 */
        b_u32(tag);
        b_u32(1);
        b_u32((uint32_t)vtag);
        b_u32((uint32_t)vint);
    }
}

/* ---------- transact ---------- */
static uint8_t rbuf[65536];
static int do_tr(uint32_t handle, uint32_t code, uint8_t *data, size_t dlen,
                 uint8_t **rout, size_t *rlen, uint64_t **robj, int *rnobj) {
    struct txn td; memset(&td, 0, sizeof td);
    td.h = handle; td.code = code;
    td.ds = dlen; td.os = 0; td.bp = (uint64_t)data; td.op = 0;
    uint8_t wcmd[4 + 64];
    uint32_t bc = BC_TR;
    memcpy(wcmd, &bc, 4); memcpy(wcmd + 4, &td, 64);
    struct bwr rw; memset(&rw, 0, sizeof rw);
    rw.ws = sizeof wcmd; rw.wb = (uint64_t)wcmd; rw.rs = sizeof rbuf; rw.rb = (uint64_t)rbuf;
    if (ioctl(bfd, WRR, &rw) < 0) { perror("ioctl"); return -1; }
    size_t pos = 0;
    static uint8_t reply[65536]; static uint64_t ro[64];
    while (pos + 4 <= rw.rc) {
        uint32_t c; memcpy(&c, rbuf + pos, 4); pos += 4;
        if (c == BR_TC || c == BR_NOOP || c == BR_OKK) continue;
        if (c == BR_RP) {
            struct txn *r = (struct txn *)(rbuf + pos); pos += 64;
            size_t ds = (size_t)r->ds; int no = (int)(r->os / 8);
            if (ds > sizeof reply) { fprintf(stderr, "reply too big\n"); return -1; }
            memcpy(reply, (void*)(uintptr_t)r->bp, ds);
            uint64_t *oa = (uint64_t*)(uintptr_t)r->op;
            for (int i = 0; i < no && i < 64; i++) ro[i] = oa[i];
            if (!getenv("KS_NOFREE")) {
            uint8_t fc[12]; uint32_t f = BC_FB;
            memcpy(fc, &f, 4); memcpy(fc + 4, &r->bp, 8);
            struct bwr w2; memset(&w2, 0, sizeof w2);
            w2.ws = 12; w2.wb = (uint64_t)fc;
            ioctl(bfd, WRR, &w2);
            }
            *rout = reply; *rlen = ds; *robj = ro; *rnobj = no;
            return 0;
        }
        fprintf(stderr, "unexpected %08x\n", c); return -1;
    }
    fprintf(stderr, "no reply\n"); return -1;
}
/* reply reader */
typedef struct { uint8_t *d; size_t len, cur; uint64_t *o; int no, oc; } R;
static void r_ini(R *r, uint8_t *d, size_t len, uint64_t *o, int no) {
    memset(r, 0, sizeof *r); r->d = d; r->len = len; r->o = o; r->no = no;
}
static uint32_t r_u32(R *r) { r->cur = (r->cur + 3) & ~(size_t)3; uint32_t v = 0; if (r->cur + 4 <= r->len) memcpy(&v, r->d + r->cur, 4); r->cur += 4; return v; }
static uint64_t r_u64(R *r) { r->cur = (r->cur + 7) & ~(size_t)7; uint64_t v = 0; if (r->cur + 8 <= r->len) memcpy(&v, r->d + r->cur, 8); r->cur += 8; return v; }
static void r_skipstr(R *r) { /* String16: len + (len+1) units + pad */
    uint32_t n = r_u32(r);
    r->cur += (size_t)(n + 1) * 2;
    r->cur = (r->cur + 3) & ~(size_t)3;
}
static int32_t r_ex(R *r) { /* exception; on service-specific, print code+msg */
    int32_t e = (int32_t)r_u32(r);
    if (e != 0) {
        int32_t code = -9999;
        if (r->cur + 4 <= r->len) code = (int32_t)r_u32(r);
        fprintf(stderr, "service exception ex=%d code=%d rlen=%zu\n", e, code, r->len);
        if (r->cur + 4 <= r->len) {
            uint32_t ml = 0; memcpy(&ml, r->d + r->cur, 4);
            fprintf(stderr, "msglen=%u\n", ml);
            if (ml < 500) {
                r_u32(r); char msg[500]; size_t k;
                for (k = 0; k < ml && r->cur + 2 <= r->len; k++) { msg[k] = (char)r->d[r->cur]; r->cur += 2; }
                msg[k < 500 ? k : 499] = 0;
                fprintf(stderr, "msg: %s\n", msg);
            } else {
                fprintf(stderr, "raw: ");
                for (size_t i = r->cur; i < r->len && i < r->cur + 64; i++) fprintf(stderr, "%02x", r->d[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    return e;
}
static uint32_t r_handle(R *r) { /* next object must be HANDLE */
    if (r->oc >= r->no) { fprintf(stderr, "no object\n"); exit(1); }
    uint64_t o = r->o[r->oc++];
    uint32_t t = 0, h = 0;
    memcpy(&t, r->d + o, 4); memcpy(&h, r->d + o + 8, 4);
    if (t != B_HDL) { fprintf(stderr, "not a handle (type %08x)\n", t); exit(1); }
    return h;
}
static int transact(uint32_t h, uint32_t code, R *r) {
    if (getenv("KS_DUMP")) fprintf(stderr, "TX h=%u code=%u len=%zu\n", h, code, BL);
    uint8_t *rd; size_t rl; uint64_t *ro; int rn;
    int rc = do_tr(h, code, B, BL, &rd, &rl, &ro, &rn);
    if (rc) return -1;
    r_ini(r, rd, rl, ro, rn);
    return 0;
}

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

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "replay")) {
        /* replay <parcel.bin> <code>: SM -> seclevel, then send parcel bytes */
        bfd = open("/dev/binder", O_RDWR | O_CLOEXEC);
        if (bfd < 0) die("open binder");
        int32_t ver = 0;
        if (ioctl(bfd, VERN, &ver) < 0) die("ver");
        uint8_t *m = mmap(NULL, 262144, PROT_READ, MAP_PRIVATE, bfd, 0);
        if (m == MAP_FAILED) die("mmap");
        (void)m;
        R r;
        size_t plen;
        uint8_t *pbytes = readfile(argv[2], &plen);
        uint32_t target = (uint32_t)atoi(argv[3]);
        { b_init(); b_hdr();
          b_tok("android.os.IServiceManager");
          b_str("android.system.keystore2.IKeystoreService/default");
          if (transact(0, 1, &r)) return 1;
          if (r_ex(&r)) return 1; }
        uint32_t ks2h = r_handle(&r);
        { b_init(); b_hdr();
          b_tok("android.system.keystore2.IKeystoreService");
          b_u32(1);
          if (transact(ks2h, 1, &r)) return 1;
          if (r_ex(&r)) return 1;
          uint32_t csz = r_u32(&r);
          (void)csz; }
        uint32_t sec = r_handle(&r);
        fprintf(stderr, "seclevel handle=%u, replaying %zu bytes to code %u\n", sec, plen, target);
        struct txn td; memset(&td, 0, sizeof td);
        td.h = sec; td.code = target; td.ds = plen; td.os = 0;
        td.bp = (uint64_t)pbytes; td.op = 0;
        uint8_t wcmd[68]; uint32_t bc = BC_TR;
        memcpy(wcmd, &bc, 4); memcpy(wcmd + 4, &td, 64);
        struct bwr rw; memset(&rw, 0, sizeof rw);
        rw.ws = sizeof wcmd; rw.wb = (uint64_t)wcmd; rw.rs = 65536;
        static uint8_t rb2[65536]; rw.rb = (uint64_t)rb2;
        if (ioctl(bfd, WRR, &rw) < 0) die("wrr");
        size_t pos = 0;
        while (pos + 4 <= rw.rc) {
            uint32_t c; memcpy(&c, rb2 + pos, 4); pos += 4;
            if (c == BR_TC || c == BR_NOOP || c == BR_OKK) continue;
            if (c == BR_RP) {
                uint64_t ds = 0, os = 0; memcpy(&ds, rb2 + pos + 32, 8); memcpy(&os, rb2 + pos + 40, 8);
                uint64_t db = 0; memcpy(&db, rb2 + pos + 48, 8);
                uint64_t ob = 0; memcpy(&ob, rb2 + pos + 56, 8);
                fprintf(stderr, "reply dsz=%llu osz=%llu\n", (unsigned long long)ds, (unsigned long long)os);
                uint8_t *d = (uint8_t*)(uintptr_t)db;
                uint32_t st = 0; if (ds >= 4) memcpy(&st, d, 4);
                fprintf(stderr, "status=%d\n", (int32_t)st);
                size_t non = (size_t)(os / 8);
                for (size_t i = 0; i < non && i < 10; i++) {
                    uint64_t oo = 0; memcpy(&oo, (uint8_t*)(uintptr_t)ob + i*8, 8);
                    uint32_t ty = 0; memcpy(&ty, d + oo, 4);
                    fprintf(stderr, "robj%zu: off=%llu type=%08x\n", i, (unsigned long long)oo, ty);
                }
                break;
            }
            fprintf(stderr, "unexpected %08x\n", c); break;
        }
        return 0;
    }
    if (argc != 6) { fprintf(stderr, "usage: %s <blob.bin> <appid_hex> <nonce_hex> <ct_hex> <tag_hex>\n", argv[0]); return 2; }
    size_t blen, alen, nlen, clen, tlen;
    uint8_t *blob = readfile(argv[1], &blen);
    uint8_t *appid = unhex(argv[2], &alen);
    uint8_t *nonce = unhex(argv[3], &nlen);
    uint8_t *ct = unhex(argv[4], &clen);
    uint8_t *tag = unhex(argv[5], &tlen);

    bfd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (bfd < 0) die("open binder");
    int32_t ver = 0;
    if (ioctl(bfd, VERN, &ver) < 0) die("ver");
    uint8_t *m = mmap(NULL, 262144, PROT_READ, MAP_PRIVATE, bfd, 0);
    if (m == MAP_FAILED) die("mmap");
    (void)m;
    R r;

    /* 1. SM.getService(keystore2) */
    uint32_t ks2 = 0;
    { b_init(); b_hdr();
      b_tok("android.os.IServiceManager");
      b_str("android.system.keystore2.IKeystoreService/default");
      if (transact(0, 1, &r)) { fprintf(stderr, "sm fail\n"); return 1; }
      { fprintf(stderr, "SMRAW len=%zu no=%d\n", r.len, r.no);
        for (int i = 0; i < r.no && i < 3; i++) fprintf(stderr, "SMRAW off[%d]=%llu\n", i, (unsigned long long)r.o[i]);
        for (size_t i = 0; i < r.len && i < 48; i++) fprintf(stderr, "%02x%s", r.d[i], (i+1)%16 ? "" : "\n");
        fprintf(stderr, "\n"); }
      if (getenv("KS_DUMP")) {
          fprintf(stderr, "sm reply len=%zu nobjs=%d\n", r.len, r.no);
          for (int i = 0; i < r.no && i < 4; i++) {
              uint32_t t = 0, h = 0; uint64_t c = 0;
              memcpy(&t, r.d + r.o[i], 4); memcpy(&h, r.d + r.o[i] + 8, 4); memcpy(&c, r.d + r.o[i] + 16, 8);
              fprintf(stderr, "sm obj%d: type=%08x h=%u cookie=%llx\n", i, t, h, (unsigned long long)c);
          }
      }
      if (r_ex(&r)) return 1;
      ks2 = r_handle(&r);
      fprintf(stderr, "keystore2 handle=%u\n", ks2);
      if (getenv("KS_SLEEP")) {
          fprintf(stderr, "sleeping...\n");
          /* probe: empty transact to ks2 handle with bogus code */
          { struct txn td2; memset(&td2, 0, sizeof td2);
            td2.h = ks2; td2.code = 999; td2.ds = 0; td2.os = 0; td2.bp = (uint64_t)B; td2.op = 0;
            uint8_t w2[68]; uint32_t bc2 = BC_TR;
            memcpy(w2, &bc2, 4); memcpy(w2+4, &td2, 64);
            struct bwr rw2; memset(&rw2, 0, sizeof rw2);
            rw2.ws = sizeof w2; rw2.wb = (uint64_t)w2; rw2.rs = sizeof rbuf; rw2.rb = (uint64_t)rbuf;
            fprintf(stderr, "probe h=%u...\n", ks2);
            if (ioctl(bfd, WRR, &rw2) < 0) { perror("probe ioctl"); }
            else {
                size_t p2 = 0;
                while (p2 + 4 <= rw2.rc) {
                    uint32_t c2; memcpy(&c2, rbuf+p2, 4); p2 += 4;
                    if (c2 == BR_TC || c2 == BR_NOOP || c2 == BR_OKK) continue;
                    if (c2 == BR_RP) {
                        uint64_t dsz = 0; memcpy(&dsz, rbuf+p2+32, 8);
                        uint64_t db2 = 0; memcpy(&db2, rbuf+p2+48, 8);
                        uint32_t st2 = 0xdeadbeef;
                        if (dsz >= 4) memcpy(&st2, (uint8_t*)(uintptr_t)db2, 4);
                        fprintf(stderr, "probe reply dsz=%llu status=%d\n", (unsigned long long)dsz, (int32_t)st2);
                        break;
                    }
                    fprintf(stderr, "probe unexpected %08x\n", c2); break;
                }
            } }
          sleep(15);
      } }

    /* 2. getSecurityLevel(TEE=1) -> seclevel handle */
    uint32_t sec = 0;
    { b_init(); b_hdr();
      if (getenv("KS_DUMP")) fprintf(stderr, "after hdr BL=%zu\n", BL);
      b_tok("android.system.keystore2.IKeystoreService");
      if (getenv("KS_DUMP")) fprintf(stderr, "after tok BL=%zu\n", BL);
      b_u32(1); /* TRUSTED_ENVIRONMENT */
      if (getenv("KS_DUMP")) fprintf(stderr, "after u32 BL=%zu\n", BL);
      if (transact(ks2, 1, &r)) { fprintf(stderr, "gsl fail\n"); return 1; }
      if (r_ex(&r)) return 1;
      sec = r_handle(&r);
      fprintf(stderr, "seclevel handle=%u\n", sec); }

    /* 3. createOperation(BLOB desc, GCM params, forced=false) */
    uint32_t iop = 0;
    { b_init(); b_hdr();
      b_tok("android.system.keystore2.IKeystoreSecurityLevel");
      /* KeyDescriptor: [present=1][size incl itself][domain][nspace][alias][blob] */
      b_u32(1); /* present */
      { size_t sizepos = BL; b_u32(0);
        b_u32(3); /* Domain.BLOB */
        int64_t ns = -1;
        if (getenv("KS_NS")) ns = (int64_t)atoll(getenv("KS_NS"));
        { uint64_t un = (uint64_t)ns; b_al(8); b_res(8); memcpy(B+BL,&un,8); BL+=8; }
        if (getenv("KS_ALIAS")) b_str(getenv("KS_ALIAS"));
        else b_u32(0xFFFFFFFFu); /* alias null */
        b_bytes(blob, blen);
        { uint32_t tot = (uint32_t)(BL - sizepos); memcpy(B + sizepos, &tot, 4); } }
      int nparams = 6;
      if (getenv("KS_NPARAMS")) nparams = atoi(getenv("KS_NPARAMS"));
      b_u32((uint32_t)nparams); /* KeyParameter[] count */
      if (nparams > 0) b_kp(0x20000001u, 7, NULL, 0, 1); /* PURPOSE=DECRYPT */
      if (nparams > 1) b_kp(0x90000259u, 14, appid, alen, 0);
      if (nparams > 2) b_kp(0x20000004u, 2, NULL, 0, 32);
      if (nparams > 3) b_kp(0x20000006u, 3, NULL, 0, 1);
      if (nparams > 4) b_kp(0x900003E9u, 14, nonce, nlen, 0);
      if (nparams > 5) b_kp(0x300003EBu, 11, NULL, 0, 128);
      b_u32(0); /* forced=false */
      if (transact(sec, 1, &r)) { fprintf(stderr, "createOp xact fail\n"); return 1; }
      { static int onetime = 0;
        if (!onetime) { onetime = 1;
          fprintf(stderr, "CREPLY len=%zu nobjs=%d\n", r.len, r.no);
          for (size_t i = 0; i < r.len; i++) fprintf(stderr, "%02x%s", r.d[i], (i+1)%32 ? "" : "\n");
          fprintf(stderr, "\n"); } }
      if (r_ex(&r)) return 1;
      /* CreateOperationResponse parcel: [size][binder][...skip rest] */
      uint32_t csz = r_u32(&r);
      (void)csz;
      iop = r_handle(&r);
      fprintf(stderr, "iop handle=%u\n", iop); }

    /* 4. update(...) */
    { b_init(); b_hdr();
      b_tok("android.system.keystore2.IKeystoreOperation");
      int split = 0;
      if (getenv("KS_SPLIT")) split = atoi(getenv("KS_SPLIT"));
      if (split == 0 || split == 3 || split == 4) {
          uint8_t *cttag = malloc(clen + tlen);
          memcpy(cttag, ct, clen); memcpy(cttag + clen, tag, tlen);
          b_bytes(cttag, clen + tlen);
          free(cttag);
      } else {
          b_bytes(ct, clen);
      }
      if (transact(iop, 2, &r)) { fprintf(stderr, "update xact fail\n"); return 1; }
      if (r_ex(&r)) return 1;
      /* @nullable byte[] output: the raw key on success */
      uint32_t ol = r_u32(&r);
      fprintf(stderr, "update out len=%d rlen=%zu\n", (int32_t)ol, r.len);
      { size_t i; for (i = 0; i < r.len; i++) fprintf(stderr, "%02x%s", r.d[i], (i+1)%32 ? "" : "\n"); fprintf(stderr, "\n"); }
      if ((int32_t)ol > 0) {
          for (uint32_t i = 0; i < ol && r.cur + i < r.len; i++)
              printf("%02x", r.d[r.cur + i]);
          printf("\n");
      } }

    /* 5. finish(...) */
    { b_init(); b_hdr();
      b_tok("android.system.keystore2.IKeystoreOperation");
      int split2 = 0;
      if (getenv("KS_SPLIT")) split2 = atoi(getenv("KS_SPLIT"));
      if (split2 == 1) { b_bytes_null(); b_bytes(tag, tlen); }
      else if (split2 == 2) { b_bytes(tag, tlen); b_bytes_null(); }
      else if (split2 == 3) { /* update was ct+tag; finish EMPTY (non-null) */
          uint8_t ee = 0; b_bytes(&ee, 0); b_bytes_null(); }
      else if (split2 == 4) { /* finish EMPTY, EMPTY */
          uint8_t ee = 0; b_bytes(&ee, 0); b_bytes(&ee, 0); }
      else { b_bytes_null(); b_bytes_null(); }
      if (transact(iop, 3, &r)) { fprintf(stderr, "finish xact fail\n"); return 1; }
      if (r_ex(&r)) return 1;
      uint32_t ol = r_u32(&r);
      fprintf(stderr, "finish out len=%d rlen=%zu\n", (int32_t)ol, r.len);
      { size_t i; for (i = 0; i < r.len; i++) fprintf(stderr, "%02x%s", r.d[i], (i+1)%32 ? "" : "\n"); fprintf(stderr, "\n"); } }
    return 0;
}
