/* kms_dual_image: present a landscape XWD screenshot (3664x1920) on the
 * Quest 2 portrait panel (1920x3664) via the proven dual-plane atomic
 * commit (planes 101/98 sharing one FB, same as kms_atomic_dual).
 *
 * Usage: kms_dual_image /tmp/frame.xwd
 *   Refreshes whenever the file's mtime changes (poll QKX_DT_MS, default
 *   1000ms); commits once at startup regardless. QKX_HOLD bounds runtime
 *   (0 = run until killed). QKX_HZ/QKX_CRTC/QKX_NODISABLE as in
 *   kms_atomic_dual. Falls back to the RED/GREEN pattern if the file is
 *   missing or has unexpected geometry (never leaves the panel dark).
 *
 * Static, raw ioctls only -- same build as the other kms_* tools:
 *   clang --target=aarch64-linux-gnu -fuse-ld=lld -static
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define __user
#include "drm.h"
#include "drm_mode.h"

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno)); \
    return 1; } } while (0)

#define MAX_PROPS 64
#define SRC_W 3664u
#define SRC_H 1920u

struct obj_props {
    uint32_t obj_id, obj_type;
    uint32_t prop_id[MAX_PROPS];
    char name[MAX_PROPS][DRM_PROP_NAME_LEN];
    int count;
};

static int load_props(int fd, struct obj_props *o) {
    struct drm_mode_obj_get_properties g = {.obj_id = o->obj_id, .obj_type = o->obj_type};
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) != 0) return -1;
    uint32_t *ids = calloc(g.count_props, sizeof(uint32_t));
    uint64_t *vals = calloc(g.count_props, sizeof(uint64_t));
    g.props_ptr = (uint64_t)(uintptr_t)ids;
    g.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) != 0) {
        free(ids); free(vals); return -1;
    }
    o->count = g.count_props > MAX_PROPS ? MAX_PROPS : g.count_props;
    for (int i = 0; i < o->count; i++) {
        o->prop_id[i] = ids[i];
        struct drm_mode_get_property p = {.prop_id = ids[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0)
            memcpy(o->name[i], p.name, DRM_PROP_NAME_LEN);
        else
            o->name[i][0] = 0;
    }
    free(ids); free(vals);
    return 0;
}

static uint32_t find_prop(struct obj_props *o, const char *name) {
    for (int i = 0; i < o->count; i++) {
        if (!strcmp(o->name[i], name))
            return o->prop_id[i];
    }
    return 0;
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Load an XWD file (xwd -root output, 32bpp TrueColor) into raw u32 pixels.
 * Returns 0 on success with w and h set; leaves px untouched on failure. */
static int load_xwd(const char *path, uint8_t **px, uint32_t *w, uint32_t *h) {
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    struct stat st;
    if (fstat(f, &st) != 0 || st.st_size < 100) { close(f); return -1; }
    size_t n = (size_t)st.st_size;
    uint8_t *buf = malloc(n);
    if (!buf) { close(f); return -1; }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(f, buf + got, n - got);
        if (r <= 0) { free(buf); close(f); return -1; }
        got += (size_t)r;
    }
    close(f);
    uint32_t hdr_size = rd32be(buf + 0);
    uint32_t depth = rd32be(buf + 12);
    uint32_t width = rd32be(buf + 16);
    uint32_t height = rd32be(buf + 20);
    uint32_t bpp = rd32be(buf + 44);
    uint32_t bytes_per_line = rd32be(buf + 48);
    uint32_t ncolors = (hdr_size >= 80) ? rd32be(buf + 76) : 0;
    size_t data_offset = hdr_size + (size_t)ncolors * 12;
    if (data_offset > n || depth != 24 || bpp != 32 || width == 0 || height == 0 ||
        bytes_per_line < width * 4 || data_offset + (size_t)bytes_per_line * height > n) {
        free(buf);
        return -1;
    }
    uint8_t *out = malloc((size_t)width * height * 4);
    if (!out) { free(buf); return -1; }
    /* aarch64/x86 xwd dumps are LSBFirst: bytes already [B,G,R,pad] matching
     * our little-endian 0xAARRGGBB framebuffer; just force opaque alpha. */
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = buf + data_offset + (size_t)y * bytes_per_line;
        uint8_t *dst = out + (size_t)y * width * 4;
        memcpy(dst, src, (size_t)width * 4);
        for (uint32_t x = 0; x < width; x++)
            dst[x * 4 + 3] = 0xFF;
    }
    free(buf);
    *px = out;
    *w = width;
    *h = height;
    return 0;
}

/* Rotation/mirroring (runtime: QKX_ROT=0 CW like the video path,
 * QKX_ROT=1 CCW; QKX_SWAP=1 mirrors portrait horizontally, i.e. swaps
 * which eye sees which half). Defaults preserve the verified video path.
 * CW:  portrait (px,py) shows landscape (lx=py, ly=1919-px).
 * CCW: portrait (px,py) shows landscape (lx=3663-py, ly=px). */
static int g_rot_ccw = 0;
static int g_swap = 0;
/* Look-around pan (landscape px, applied in source space before rotation).
 * Driven live by QKX_PAN_X/QKX_PAN_Y env (re-read every frame) or a pose
 * file (QKX_POSE_FILE, two ints "x y"). Positive X looks right. */
static int g_pan_x = 0;
static int g_pan_y = 0;
static const char *g_pose_file = NULL;

static void blit_poll_pan(void) {
    const char *e;
    if (g_pose_file) {
        FILE *f = fopen(g_pose_file, "r");
        if (f) {
            int x = 0, y = 0;
            if (fscanf(f, "%d %d", &x, &y) == 2) {
                g_pan_x = x;
                g_pan_y = y;
            }
            fclose(f);
            return;
        }
    }
    e = getenv("QKX_PAN_X");
    if (e) g_pan_x = atoi(e);
    e = getenv("QKX_PAN_Y");
    if (e) g_pan_y = atoi(e);
}

static void blit_rotated(uint32_t *dst, uint32_t dst_pitch_px,
                         const uint8_t *src, uint32_t dst_w, uint32_t dst_h) {
    const uint32_t *s = (const uint32_t *)src;
    for (uint32_t py = 0; py < dst_h; py++) {
        for (uint32_t px = 0; px < dst_w; px++) {
            uint32_t dx = g_swap ? (dst_w - 1 - px) : px;
            int lx, ly;
            if (!g_rot_ccw) {
                lx = (int)py + g_pan_x;
                ly = (int)(SRC_H - 1 - dx) + g_pan_y;
            } else {
                lx = (int)(SRC_W - 1 - py) + g_pan_x;
                ly = (int)dx + g_pan_y;
            }
            uint32_t v = 0xFF000000; /* letterbox */
            if (lx >= 0 && lx < (int)SRC_W && ly >= 0 && ly < (int)SRC_H)
                v = s[(uint32_t)ly * SRC_W + (uint32_t)lx];
            dst[py * dst_pitch_px + px] = v;
        }
    }
}

static void fill_pattern(uint32_t *pixels, uint32_t pitch_px, uint32_t w, uint32_t h) {
    uint32_t half_w = w / 2;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            pixels[y * pitch_px + x] = (x < half_w) ? 0xFFFF0000 : 0xFF00FF00;
}

int main(int argc, char **argv) {
    const char *img = argc > 1 ? argv[1] : "/tmp/frame.xwd";
    const char *card = getenv("QKX_CARD") ? getenv("QKX_CARD") : "/dev/dri/card0";
    int dt_ms = getenv("QKX_DT_MS") ? atoi(getenv("QKX_DT_MS")) : 1000;
    int hold_s = getenv("QKX_HOLD") ? atoi(getenv("QKX_HOLD")) : 0;
    g_rot_ccw = getenv("QKX_ROT") ? atoi(getenv("QKX_ROT")) : 0;
    g_swap = getenv("QKX_SWAP") ? atoi(getenv("QKX_SWAP")) : 0;
    g_pose_file = getenv("QKX_POSE_FILE");
    fprintf(stderr, "blit: rot=%s swap=%d pose=%s\n",
            g_rot_ccw ? "CCW" : "CW", g_swap,
            g_pose_file ? g_pose_file : "(env)");
    if (dt_ms < 100) dt_ms = 100;

    int fd = open(card, O_RDWR);
    CHECK(fd >= 0, "open card");

    struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
    CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0, "cap universal planes");
    cap.capability = DRM_CLIENT_CAP_ATOMIC; cap.value = 1;
    CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0, "cap atomic");
    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

    struct drm_mode_card_res res = {0};
    CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES size");
    uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    uint32_t *enc_ids = calloc(res.count_encoders, sizeof(uint32_t));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;
    CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES ids");

    uint32_t chosen_conn = 0, chosen_enc = 0;
    struct drm_mode_modeinfo mode = {0};
    int have_mode = 0, chosen_crtc_index = 0;

    for (uint32_t i = 0; i < res.count_connectors && !have_mode; i++) {
        struct drm_mode_get_connector c = { .connector_id = conn_ids[i] };
        CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR size");
        if (c.count_modes == 0) continue;
        struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
        uint32_t *encs = calloc(c.count_encoders, sizeof(uint32_t));
        c.modes_ptr = (uint64_t)(uintptr_t)modes;
        c.encoders_ptr = (uint64_t)(uintptr_t)encs;
        c.props_ptr = 0;
        c.prop_values_ptr = 0;
        c.count_props = 0;
        CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR modes");
        mode = modes[0];
        const char *want_hz = getenv("QKX_HZ");
        if (want_hz) {
            uint32_t hz = (uint32_t)atoi(want_hz);
            for (uint32_t m = 0; m < c.count_modes; m++) {
                if (modes[m].vrefresh == hz) {
                    mode = modes[m];
                    break;
                }
            }
        }
        chosen_conn = conn_ids[i];
        chosen_enc = c.count_encoders ? encs[0] : 0;
        have_mode = 1;
        fprintf(stderr, "connector=%u picked mode: %s %ux%u@%u clock=%u\n",
                chosen_conn, mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock);
        free(modes); free(encs);
    }
    CHECK(have_mode, "no connector with modes found");
    CHECK(mode.hdisplay == 1920 && mode.vdisplay == 3664, "panel must be 1920x3664");

    uint32_t chosen_crtc = crtc_ids[0];
    if (chosen_enc) {
        struct drm_mode_get_encoder e = { .encoder_id = chosen_enc };
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0 && e.crtc_id)
            chosen_crtc = e.crtc_id;
    }
    const char *want_crtc = getenv("QKX_CRTC");
    if (want_crtc && (uint32_t)atoi(want_crtc) < res.count_crtcs)
        chosen_crtc = crtc_ids[atoi(want_crtc)];
    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        if (crtc_ids[i] == chosen_crtc)
            chosen_crtc_index = i;
    }
    fprintf(stderr, "using connector=%u crtc=%u (index %d)\n", chosen_conn, chosen_crtc, chosen_crtc_index);

    struct drm_mode_get_plane_res pres = {0};
    CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0, "GETPLANERESOURCES size");
    uint32_t *all_planes = calloc(pres.count_planes, sizeof(uint32_t));
    pres.plane_id_ptr = (uint64_t)(uintptr_t)all_planes;
    CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0, "GETPLANERESOURCES ids");

    uint32_t avail_planes[16];
    int avail_count = 0;
    for (uint32_t i = 0; i < pres.count_planes; i++) {
        struct drm_mode_get_plane pl = { .plane_id = all_planes[i] };
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl) == 0) {
            if (pl.possible_crtcs & (1u << chosen_crtc_index)) {
                if (avail_count < 16)
                    avail_planes[avail_count++] = all_planes[i];
            }
        }
    }
    CHECK(avail_count >= 2, "need at least 2 planes for dual-pipe DSC modeset");
    uint32_t plane_left = avail_planes[0];
    uint32_t plane_right = avail_planes[1];
    int has_101 = 0, has_98 = 0;
    for (int i = 0; i < avail_count; i++) {
        if (avail_planes[i] == 101) has_101 = 1;
        if (avail_planes[i] == 98) has_98 = 1;
    }
    if (has_101 && has_98) {
        plane_left = 101;
        plane_right = 98;
    }
    fprintf(stderr, "using plane_left=%u plane_right=%u (out of %d avail)\n",
            plane_left, plane_right, avail_count);

    struct drm_mode_create_dumb create = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .bpp = 32,
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "CREATE_DUMB");

    struct drm_mode_fb_cmd fbcmd = {
        .width = create.width,
        .height = create.height,
        .pitch = create.pitch,
        .bpp = 32,
        .depth = 24,
        .handle = create.handle,
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd) == 0, "ADDFB");
    uint32_t fb_id = fbcmd.fb_id;
    fprintf(stderr, "created FB %u (%ux%u pitch=%u size=%llu)\n",
            fb_id, create.width, create.height, create.pitch, (unsigned long long)create.size);

    struct drm_mode_map_dumb map = { .handle = create.handle };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "MAP_DUMB");
    uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    CHECK(pixels != MAP_FAILED, "mmap");
    uint32_t pitch_px = create.pitch / 4;
    uint32_t half_w = mode.hdisplay / 2;

    struct drm_mode_create_blob blob = {
        .data = (uint64_t)(uintptr_t)&mode,
        .length = sizeof(mode),
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) == 0, "CREATEPROPBLOB");

    struct obj_props crtc_p = { .obj_id = chosen_crtc, .obj_type = DRM_MODE_OBJECT_CRTC };
    struct obj_props conn_p = { .obj_id = chosen_conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR };
    struct obj_props pl_p = { .obj_id = plane_left, .obj_type = DRM_MODE_OBJECT_PLANE };
    struct obj_props pr_p = { .obj_id = plane_right, .obj_type = DRM_MODE_OBJECT_PLANE };
    CHECK(load_props(fd, &crtc_p) == 0, "load crtc props");
    CHECK(load_props(fd, &conn_p) == 0, "load conn props");
    CHECK(load_props(fd, &pl_p) == 0, "load plane_left props");
    CHECK(load_props(fd, &pr_p) == 0, "load plane_right props");

    uint32_t p_crtc_active = find_prop(&crtc_p, "ACTIVE");
    uint32_t p_crtc_mode_id = find_prop(&crtc_p, "MODE_ID");
    uint32_t p_crtc_core_clk = find_prop(&crtc_p, "core_clk");
    uint32_t p_crtc_core_ab = find_prop(&crtc_p, "core_ab");
    uint32_t p_crtc_llcc_ab = find_prop(&crtc_p, "llcc_ab");
    uint32_t p_crtc_dram_ab = find_prop(&crtc_p, "dram_ab");
    uint32_t p_crtc_dram_ib = find_prop(&crtc_p, "dram_ib");
    uint32_t p_conn_crtc_id = find_prop(&conn_p, "CRTC_ID");
    uint32_t pl_fb_id = find_prop(&pl_p, "FB_ID");
    uint32_t pl_crtc_id = find_prop(&pl_p, "CRTC_ID");
    uint32_t pl_src_x = find_prop(&pl_p, "SRC_X");
    uint32_t pl_src_y = find_prop(&pl_p, "SRC_Y");
    uint32_t pl_src_w = find_prop(&pl_p, "SRC_W");
    uint32_t pl_src_h = find_prop(&pl_p, "SRC_H");
    uint32_t pl_crtc_x = find_prop(&pl_p, "CRTC_X");
    uint32_t pl_crtc_y = find_prop(&pl_p, "CRTC_Y");
    uint32_t pl_crtc_w = find_prop(&pl_p, "CRTC_W");
    uint32_t pl_crtc_h = find_prop(&pl_p, "CRTC_H");
    uint32_t pl_zpos = find_prop(&pl_p, "zpos");
    uint32_t pl_blend_op = find_prop(&pl_p, "blend_op");
    uint32_t pr_fb_id = find_prop(&pr_p, "FB_ID");
    uint32_t pr_crtc_id = find_prop(&pr_p, "CRTC_ID");
    uint32_t pr_src_x = find_prop(&pr_p, "SRC_X");
    uint32_t pr_src_y = find_prop(&pr_p, "SRC_Y");
    uint32_t pr_src_w = find_prop(&pr_p, "SRC_W");
    uint32_t pr_src_h = find_prop(&pr_p, "SRC_H");
    uint32_t pr_crtc_x = find_prop(&pr_p, "CRTC_X");
    uint32_t pr_crtc_y = find_prop(&pr_p, "CRTC_Y");
    uint32_t pr_crtc_w = find_prop(&pr_p, "CRTC_W");
    uint32_t pr_crtc_h = find_prop(&pr_p, "CRTC_H");
    uint32_t pr_zpos = find_prop(&pr_p, "zpos");
    uint32_t pr_blend_op = find_prop(&pr_p, "blend_op");

    if (getenv("QKX_NODISABLE") == NULL) {
        uint32_t d_objs[1] = { chosen_crtc };
        uint32_t d_counts[1] = { 2 };
        uint32_t d_props[2] = { p_crtc_mode_id, p_crtc_active };
        uint64_t d_vals[2] = { 0, 0 };
        struct drm_mode_atomic d_atom = {
            .flags = DRM_MODE_ATOMIC_ALLOW_MODESET,
            .count_objs = 1,
            .objs_ptr = (uint64_t)(uintptr_t)d_objs,
            .count_props_ptr = (uint64_t)(uintptr_t)d_counts,
            .props_ptr = (uint64_t)(uintptr_t)d_props,
            .prop_values_ptr = (uint64_t)(uintptr_t)d_vals,
        };
        fprintf(stderr, "committing DISABLE pass...\n");
        int dret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &d_atom);
        fprintf(stderr, "disable pass ret=%d (errno=%d)\n", dret, dret ? errno : 0);
    }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long last_mtime = -1;
    int last_pan_x = 0x7fffffff, last_pan_y = 0x7fffffff;
    int frames = 0;
    for (;;) {
        struct stat st;
        int have_img = 0;
        uint8_t *raw = NULL;
        uint32_t iw = 0, ih = 0;
        blit_poll_pan();
        int pan_changed = (g_pan_x != last_pan_x || g_pan_y != last_pan_y);
        if (stat(img, &st) == 0 && (long)st.st_mtime != last_mtime) {
            if (load_xwd(img, &raw, &iw, &ih) == 0 && iw == SRC_W && ih == SRC_H) {
                blit_rotated(pixels, pitch_px, raw, mode.hdisplay, mode.vdisplay);
                free(raw);
                have_img = 1;
                last_mtime = (long)st.st_mtime;
            } else {
                if (raw) free(raw);
                if (frames == 0) fill_pattern(pixels, pitch_px, mode.hdisplay, mode.vdisplay);
            }
        } else if (frames == 0) {
            fill_pattern(pixels, pitch_px, mode.hdisplay, mode.vdisplay);
        }
        if (frames > 0 && !have_img && (long)st.st_mtime == last_mtime && !pan_changed) {
            /* unchanged since last commit: just wait */
        } else {
            uint32_t objs[4] = { chosen_crtc, chosen_conn, plane_left, plane_right };
            uint32_t counts[4] = { 0, 1, 0, 0 };
            uint32_t props[64];
            uint64_t vals[64];
            int n = 0;
            props[n] = p_crtc_active; vals[n++] = 1; counts[0]++;
            props[n] = p_crtc_mode_id; vals[n++] = blob.blob_id; counts[0]++;
            if (p_crtc_core_clk) { props[n] = p_crtc_core_clk; vals[n++] = 445798080; counts[0]++; }
            if (p_crtc_core_ab)  { props[n] = p_crtc_core_ab; vals[n++] = 3396556800ULL; counts[0]++; }
            if (p_crtc_llcc_ab)  { props[n] = p_crtc_llcc_ab; vals[n++] = 3396556800ULL; counts[0]++; }
            if (p_crtc_dram_ab)  { props[n] = p_crtc_dram_ab; vals[n++] = 3396556800ULL; counts[0]++; }
            if (p_crtc_dram_ib)  { props[n] = p_crtc_dram_ib; vals[n++] = 1600000000ULL; counts[0]++; }
            props[n] = p_conn_crtc_id; vals[n++] = chosen_crtc;
            props[n] = pl_fb_id; vals[n++] = fb_id; counts[2]++;
            props[n] = pl_crtc_id; vals[n++] = chosen_crtc; counts[2]++;
            props[n] = pl_crtc_x; vals[n++] = 0; counts[2]++;
            props[n] = pl_crtc_y; vals[n++] = 0; counts[2]++;
            props[n] = pl_crtc_w; vals[n++] = half_w; counts[2]++;
            props[n] = pl_crtc_h; vals[n++] = mode.vdisplay; counts[2]++;
            props[n] = pl_src_x;  vals[n++] = 0; counts[2]++;
            props[n] = pl_src_y;  vals[n++] = 0; counts[2]++;
            props[n] = pl_src_w;  vals[n++] = ((uint64_t)half_w) << 16; counts[2]++;
            props[n] = pl_src_h;  vals[n++] = ((uint64_t)mode.vdisplay) << 16; counts[2]++;
            if (pl_zpos) { props[n] = pl_zpos; vals[n++] = 0; counts[2]++; }
            if (pl_blend_op) { props[n] = pl_blend_op; vals[n++] = 1; counts[2]++; }
            props[n] = pr_fb_id; vals[n++] = fb_id; counts[3]++;
            props[n] = pr_crtc_id; vals[n++] = chosen_crtc; counts[3]++;
            props[n] = pr_crtc_x; vals[n++] = half_w; counts[3]++;
            props[n] = pr_crtc_y; vals[n++] = 0; counts[3]++;
            props[n] = pr_crtc_w; vals[n++] = half_w; counts[3]++;
            props[n] = pr_crtc_h; vals[n++] = mode.vdisplay; counts[3]++;
            props[n] = pr_src_x;  vals[n++] = ((uint64_t)half_w) << 16; counts[3]++;
            props[n] = pr_src_y;  vals[n++] = 0; counts[3]++;
            props[n] = pr_src_w;  vals[n++] = ((uint64_t)half_w) << 16; counts[3]++;
            props[n] = pr_src_h;  vals[n++] = ((uint64_t)mode.vdisplay) << 16; counts[3]++;
            if (pr_zpos) { props[n] = pr_zpos; vals[n++] = 0; counts[3]++; }
            if (pr_blend_op) { props[n] = pr_blend_op; vals[n++] = 1; counts[3]++; }
            struct drm_mode_atomic atom = {
                .flags = DRM_MODE_ATOMIC_ALLOW_MODESET,
                .count_objs = 4,
                .objs_ptr = (uint64_t)(uintptr_t)objs,
                .count_props_ptr = (uint64_t)(uintptr_t)counts,
                .props_ptr = (uint64_t)(uintptr_t)props,
                .prop_values_ptr = (uint64_t)(uintptr_t)vals,
            };
            int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atom);
            if (ret != 0) {
                fprintf(stderr, "ATOMIC commit failed: errno=%d (%s)\n", errno, strerror(errno));
                return 1;
            }
            frames++;
            last_pan_x = g_pan_x;
            last_pan_y = g_pan_y;
            fprintf(stderr, "frame %d committed (%s)\n", frames, have_img ? img : "pattern");
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = now.tv_sec - t0.tv_sec;
        if (hold_s > 0 && elapsed >= hold_s) break;
        struct timespec ts = { .tv_sec = dt_ms / 1000,
                               .tv_nsec = (long)(dt_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "done (%d frames).\n", frames);
    close(fd);
    return 0;
}
