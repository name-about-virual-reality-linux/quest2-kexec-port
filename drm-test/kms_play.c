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
#include <signal.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define __user
#include "drm.h"
#include "drm_mode.h"

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno)); \
    fflush(stderr); \
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

struct QkxxHeader {
    char magic[4];       /* "QKXV" */
    uint32_t num_frames;
    uint32_t fps;
    uint32_t width;
    uint32_t height;
};

static volatile int g_running = 1;
static void handle_sig(int s) { (void)s; g_running = 0; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    const char *videopath = argc > 1 ? argv[1] : "/testvideo.qkxv";
    int fd_vid = open(videopath, O_RDONLY);
    CHECK(fd_vid >= 0, "open video file (.qkxv)");

    struct stat st;
    CHECK(fstat(fd_vid, &st) == 0, "fstat video file");
    size_t vid_size = st.st_size;

    uint8_t *vid_data = mmap(NULL, vid_size, PROT_READ, MAP_SHARED, fd_vid, 0);
    CHECK(vid_data != MAP_FAILED, "mmap video file");

    struct QkxxHeader *hdr = (struct QkxxHeader *)vid_data;
    if (memcmp(hdr->magic, "QKXV", 4) != 0) {
        fprintf(stderr, "Invalid magic, not a QKXV file!\n");
        return 1;
    }
    fprintf(stderr, "QKXV Video: %u frames, %u fps, native %ux%u, size %.2f MB\n",
            hdr->num_frames, hdr->fps, hdr->width, hdr->height, vid_size / (1024.0 * 1024.0));

    uint32_t *frame_sizes = (uint32_t *)(vid_data + sizeof(struct QkxxHeader));
    uint8_t *frame_data_ptr = (uint8_t *)(frame_sizes + hdr->num_frames);

    /* Index frame offsets */
    uint8_t **frame_ptrs = malloc(sizeof(uint8_t *) * hdr->num_frames);
    uint8_t *curr = frame_data_ptr;
    for (uint32_t i = 0; i < hdr->num_frames; i++) {
        frame_ptrs[i] = curr;
        curr += frame_sizes[i];
    }

    const char *card = "/dev/dri/card0";
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
        c.props_ptr = 0; c.prop_values_ptr = 0; c.count_props = 0;
        CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR modes");
        mode = modes[0];
        const char *want_hz = getenv("QKX_HZ");
        if (want_hz) {
            uint32_t hz = (uint32_t)atoi(want_hz);
            for (uint32_t m = 0; m < c.count_modes; m++) {
                if (modes[m].vrefresh == hz) { mode = modes[m]; break; }
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
        if (crtc_ids[i] == chosen_crtc) chosen_crtc_index = i;
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
                if (avail_count < 16) avail_planes[avail_count++] = all_planes[i];
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

    /* Clear framebuffer to black */
    memset(pixels, 0, create.size);

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
    uint32_t pl_crtc_x = find_prop(&pl_p, "CRTC_X");
    uint32_t pl_crtc_y = find_prop(&pl_p, "CRTC_Y");
    uint32_t pl_crtc_w = find_prop(&pl_p, "CRTC_W");
    uint32_t pl_crtc_h = find_prop(&pl_p, "CRTC_H");
    uint32_t pl_src_x = find_prop(&pl_p, "SRC_X");
    uint32_t pl_src_y = find_prop(&pl_p, "SRC_Y");
    uint32_t pl_src_w = find_prop(&pl_p, "SRC_W");
    uint32_t pl_src_h = find_prop(&pl_p, "SRC_H");
    uint32_t pl_zpos = find_prop(&pl_p, "zpos");
    uint32_t pl_blend_op = find_prop(&pl_p, "blend_op");

    uint32_t pr_fb_id = find_prop(&pr_p, "FB_ID");
    uint32_t pr_crtc_id = find_prop(&pr_p, "CRTC_ID");
    uint32_t pr_crtc_x = find_prop(&pr_p, "CRTC_X");
    uint32_t pr_crtc_y = find_prop(&pr_p, "CRTC_Y");
    uint32_t pr_crtc_w = find_prop(&pr_p, "CRTC_W");
    uint32_t pr_crtc_h = find_prop(&pr_p, "CRTC_H");
    uint32_t pr_src_x = find_prop(&pr_p, "SRC_X");
    uint32_t pr_src_y = find_prop(&pr_p, "SRC_Y");
    uint32_t pr_src_w = find_prop(&pr_p, "SRC_W");
    uint32_t pr_src_h = find_prop(&pr_p, "SRC_H");
    uint32_t pr_zpos = find_prop(&pr_p, "zpos");
    uint32_t pr_blend_op = find_prop(&pr_p, "blend_op");

    /* Step 1: Commit DISABLE pass to reset CRTC state cleanly */
    {
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

    /* Step 2: Build and Commit Dual-Plane ENABLE Atomic Request */
    uint32_t half_w = mode.hdisplay / 2;
    uint32_t objs[4] = { chosen_crtc, chosen_conn, plane_left, plane_right };
    uint32_t counts[4] = { 0, 1, 0, 0 };
    uint32_t props[64];
    uint64_t vals[64];
    int n = 0;

    props[n] = p_crtc_active;  vals[n++] = 1; counts[0]++;
    props[n] = p_crtc_mode_id; vals[n++] = blob.blob_id; counts[0]++;
    if (p_crtc_core_clk) { props[n] = p_crtc_core_clk; vals[n++] = 445798080; counts[0]++; }
    if (p_crtc_core_ab)  { props[n] = p_crtc_core_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (p_crtc_llcc_ab)  { props[n] = p_crtc_llcc_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (p_crtc_dram_ab)  { props[n] = p_crtc_dram_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (p_crtc_dram_ib)  { props[n] = p_crtc_dram_ib; vals[n++] = 1600000000ULL; counts[0]++; }

    props[n] = p_conn_crtc_id; vals[n++] = chosen_crtc;

    props[n] = pl_fb_id;   vals[n++] = fb_id; counts[2]++;
    props[n] = pl_crtc_id; vals[n++] = chosen_crtc; counts[2]++;
    props[n] = pl_crtc_x;  vals[n++] = 0; counts[2]++;
    props[n] = pl_crtc_y;  vals[n++] = 0; counts[2]++;
    props[n] = pl_crtc_w;  vals[n++] = half_w; counts[2]++;
    props[n] = pl_crtc_h;  vals[n++] = mode.vdisplay; counts[2]++;
    props[n] = pl_src_x;   vals[n++] = 0; counts[2]++;
    props[n] = pl_src_y;   vals[n++] = 0; counts[2]++;
    props[n] = pl_src_w;   vals[n++] = ((uint64_t)half_w) << 16; counts[2]++;
    props[n] = pl_src_h;   vals[n++] = ((uint64_t)mode.vdisplay) << 16; counts[2]++;
    if (pl_zpos) { props[n] = pl_zpos; vals[n++] = 0; counts[2]++; }
    if (pl_blend_op) { props[n] = pl_blend_op; vals[n++] = 1; counts[2]++; }

    props[n] = pr_fb_id;   vals[n++] = fb_id; counts[3]++;
    props[n] = pr_crtc_id; vals[n++] = chosen_crtc; counts[3]++;
    props[n] = pr_crtc_x;  vals[n++] = half_w; counts[3]++;
    props[n] = pr_crtc_y;  vals[n++] = 0; counts[3]++;
    props[n] = pr_crtc_w;  vals[n++] = half_w; counts[3]++;
    props[n] = pr_crtc_h;  vals[n++] = mode.vdisplay; counts[3]++;
    props[n] = pr_src_x;   vals[n++] = ((uint64_t)half_w) << 16; counts[3]++;
    props[n] = pr_src_y;   vals[n++] = 0; counts[3]++;
    props[n] = pr_src_w;   vals[n++] = ((uint64_t)half_w) << 16; counts[3]++;
    props[n] = pr_src_h;   vals[n++] = ((uint64_t)mode.vdisplay) << 16; counts[3]++;
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
    CHECK(ret == 0, "ATOMIC modeset commit");
    fprintf(stderr, "SUCCESS! Dual-plane atomic commit accepted!\n");

    int scale2x = (hdr->width <= 480) ? 1 : 0;
    int disp_w = scale2x ? (hdr->width * 2) : hdr->width;
    int disp_h = scale2x ? (hdr->height * 2) : hdr->height;
    int x_left = (half_w - disp_w) / 2;
    int x_right = half_w + x_left;
    int y_start = (mode.vdisplay - disp_h) / 2;
    uint32_t pitch_px = create.pitch / 4;

    uint32_t *frame_rgba = malloc(hdr->width * hdr->height * 4);
    CHECK(frame_rgba != NULL, "alloc frame_rgba");

    fprintf(stderr, "Playing QKXV video: %u frames at %u fps (displaying %dx%d per eye, scale2x=%d)\n",
            hdr->num_frames, hdr->fps, disp_w, disp_h, scale2x);

    uint64_t frame_ns = 1000000000ULL / hdr->fps;
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    uint32_t cur_frame = 0;
    uint64_t total_played = 0;

    while (g_running) {
        /* Decode current JPEG frame */
        int j_w = 0, j_h = 0, j_comp = 0;
        uint8_t *decoded = stbi_load_from_memory(frame_ptrs[cur_frame], frame_sizes[cur_frame],
                                                 &j_w, &j_h, &j_comp, 4);
        if (decoded) {
            /* Convert RGBA to ARGB/XRGB8888 */
            uint32_t npixels = j_w * j_h;
            for (uint32_t i = 0; i < npixels; i++) {
                uint8_t r = decoded[i * 4 + 0];
                uint8_t g = decoded[i * 4 + 1];
                uint8_t b = decoded[i * 4 + 2];
                frame_rgba[i] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
            stbi_image_free(decoded);

            /* Blit frame to both left and right eye in mmap'd framebuffer */
            if (scale2x) {
                for (int y = 0; y < j_h; y++) {
                    uint32_t *src_row = &frame_rgba[y * j_w];
                    uint32_t *dst_l1 = &pixels[(y_start + y * 2) * pitch_px + x_left];
                    uint32_t *dst_l2 = &pixels[(y_start + y * 2 + 1) * pitch_px + x_left];
                    uint32_t *dst_r1 = &pixels[(y_start + y * 2) * pitch_px + x_right];
                    uint32_t *dst_r2 = &pixels[(y_start + y * 2 + 1) * pitch_px + x_right];
                    for (int x = 0; x < j_w; x++) {
                        uint32_t p = src_row[x];
                        dst_l1[x * 2] = p;     dst_l1[x * 2 + 1] = p;
                        dst_l2[x * 2] = p;     dst_l2[x * 2 + 1] = p;
                        dst_r1[x * 2] = p;     dst_r1[x * 2 + 1] = p;
                        dst_r2[x * 2] = p;     dst_r2[x * 2 + 1] = p;
                    }
                }
            } else {
                for (int y = 0; y < j_h; y++) {
                    uint32_t *src_row = &frame_rgba[y * j_w];
                    uint32_t *dst_l = &pixels[(y_start + y) * pitch_px + x_left];
                    uint32_t *dst_r = &pixels[(y_start + y) * pitch_px + x_right];
                    memcpy(dst_l, src_row, j_w * 4);
                    memcpy(dst_r, src_row, j_w * 4);
                }
            }
        }

        cur_frame++;
        total_played++;
        if (cur_frame >= hdr->num_frames) {
            cur_frame = 0; /* Loop! */
        }

        /* Frame pacing */
        next_frame.tv_nsec += frame_ns;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec += 1;
            next_frame.tv_nsec -= 1000000000L;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t diff_ns = ((int64_t)(next_frame.tv_sec - now.tv_sec)) * 1000000000LL +
                          (next_frame.tv_nsec - now.tv_nsec);
        if (diff_ns > 0) {
            struct timespec req = { .tv_sec = diff_ns / 1000000000LL, .tv_nsec = diff_ns % 1000000000LL };
            nanosleep(&req, NULL);
        } else {
            clock_gettime(CLOCK_MONOTONIC, &next_frame);
        }
    }

    fprintf(stderr, "kms_play finished. Rendered %llu frames.\n", (unsigned long long)total_played);
    free(frame_rgba);
    free(frame_ptrs);
    munmap(vid_data, vid_size);
    close(fd_vid);
    close(fd);
    return 0;
}
