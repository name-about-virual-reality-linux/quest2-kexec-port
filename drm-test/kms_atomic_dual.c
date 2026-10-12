#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#define __user
#include "drm.h"
#include "drm_mode.h"

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno)); \
    return 1; } } while (0)

#define MAX_PROPS 64

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

int main(int argc, char **argv) {
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
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

    /* Enumerate planes compatible with this CRTC */
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
    /* Stock Android uses Plane 101 and Plane 98 (two DMA planes) */
    uint32_t plane_left = avail_planes[0];
    uint32_t plane_right = avail_planes[1];
    /* Check if 101 and 98 exist specifically */
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

    /* Allocate Dumb Framebuffer */
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

    /* Fill pattern */
    uint32_t half_w = mode.hdisplay / 2;
    uint32_t pitch_px = create.pitch / 4;
    const char *pat = getenv("QKX_PATTERN");
    int pattern_num = pat ? atoi(pat) : 4; /* Default: Clean split RED / GREEN */

    if (pattern_num == 4) {
        for (uint32_t y = 0; y < mode.vdisplay; y++) {
            for (uint32_t x = 0; x < mode.hdisplay; x++) {
                pixels[y * pitch_px + x] = (x < half_w) ? 0xFFFF0000 : 0xFF00FF00;
            }
        }
        fprintf(stderr, "FB filled: Left RED, Right GREEN (Pattern 4)\n");
    } else if (pattern_num == 2) {
        static const uint32_t band[8] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF,
                                          0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF, 0xFFFF8000 };
        for (uint32_t y = 0; y < mode.vdisplay; y++) {
            uint32_t c = band[(y * 8) / mode.vdisplay];
            for (uint32_t x = 0; x < mode.hdisplay; x++)
                pixels[y * pitch_px + x] = c;
        }
        fprintf(stderr, "FB filled: 8 row bands (Pattern 2)\n");
    } else {
        uint32_t color = 0xFF20FF40;
        if (getenv("QKX_COLOR"))
            color = (uint32_t)strtoul(getenv("QKX_COLOR"), NULL, 16);
        for (size_t i = 0; i < create.size / 4; i++)
            pixels[i] = color;
        fprintf(stderr, "FB filled: solid 0x%08x\n", color);
    }

    /* Mode Blob */
    struct drm_mode_create_blob blob = {
        .data = (uint64_t)(uintptr_t)&mode,
        .length = sizeof(mode),
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) == 0, "CREATEPROPBLOB");

    /* Property Discovery */
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

    /* Atomic DISABLE pass first to force genuine OFF->ON transition if needed */
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

    /* Build Dual-Plane Atomic Request */
    uint32_t objs[4] = { chosen_crtc, chosen_conn, plane_left, plane_right };
    uint32_t counts[4] = { 0, 1, 0, 0 };
    uint32_t props[64];
    uint64_t vals[64];
    int n = 0;

    // CRTC
    props[n] = p_crtc_active; vals[n++] = 1; counts[0]++;
    props[n] = p_crtc_mode_id; vals[n++] = blob.blob_id; counts[0]++;
    if (p_crtc_core_clk) { props[n] = p_crtc_core_clk; vals[n++] = 445798080; counts[0]++; }
    if (p_crtc_core_ab)  { props[n] = p_crtc_core_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (p_crtc_llcc_ab)  { props[n] = p_crtc_llcc_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (p_crtc_dram_ab)  { props[n] = p_crtc_dram_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (p_crtc_dram_ib)  { props[n] = p_crtc_dram_ib; vals[n++] = 1600000000ULL; counts[0]++; }

    // Connector
    props[n] = p_conn_crtc_id; vals[n++] = chosen_crtc;

    // Left Plane (X: 0 .. half_w)
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

    // Right Plane (X: half_w .. 2*half_w)
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

    fprintf(stderr, "committing ENABLE dual-plane atomic request (%d props across 4 objects)...\n", n);
    int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atom);
    if (ret != 0) {
        fprintf(stderr, "ATOMIC commit failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    int hold = getenv("QKX_HOLD") ? atoi(getenv("QKX_HOLD")) : 20;
    fprintf(stderr, "SUCCESS! Dual-plane commit accepted! Holding for %d seconds...\n", hold);
    sleep(hold);
    fprintf(stderr, "done.\n");
    close(fd);
    return 0;
}
