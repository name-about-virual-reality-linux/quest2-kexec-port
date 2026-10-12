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
#include "drm-test/stb_image.h"

#define __user
#include "drm-test/drm.h"
#include "drm-test/drm_mode.h"

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

struct QkxxHeader {
    char magic[4];
    uint32_t num_frames;
    uint32_t fps;
    uint32_t width;
    uint32_t height;
};

static volatile int g_running = 1;
static void handle_sig(int s) { (void)s; g_running = 0; }

int main(int argc, char **argv) {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    const char *videopath = argc > 1 ? argv[1] : "/tmp/testvideo_vr.qkxv";
    int fd_vid = open(videopath, O_RDONLY);
    CHECK(fd_vid >= 0, "open video file");

    struct stat st;
    CHECK(fstat(fd_vid, &st) == 0, "fstat video file");
    size_t vid_size = st.st_size;

    uint8_t *vid_data = mmap(NULL, vid_size, PROT_READ, MAP_SHARED, fd_vid, 0);
    CHECK(vid_data != MAP_FAILED, "mmap video file");

    struct QkxxHeader *hdr = (struct QkxxHeader *)vid_data;
    CHECK(memcmp(hdr->magic, "QKXV", 4) == 0, "QKXV magic");
    fprintf(stderr, "VR Video: %u frames @ %u fps, full frame %ux%u (%.2f MB)\n",
            hdr->num_frames, hdr->fps, hdr->width, hdr->height, vid_size / (1024.0*1024.0));

    uint32_t *frame_sizes = (uint32_t *)(vid_data + sizeof(struct QkxxHeader));
    uint8_t *frame_data_ptr = (uint8_t *)(frame_sizes + hdr->num_frames);

    uint8_t **frame_ptrs = malloc(sizeof(uint8_t *) * hdr->num_frames);
    uint8_t *curr = frame_data_ptr;
    for (uint32_t i = 0; i < hdr->num_frames; i++) {
        frame_ptrs[i] = curr;
        curr += frame_sizes[i];
    }

    int fd = open("/dev/dri/card0", O_RDWR);
    CHECK(fd >= 0, "open card");

    struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    cap.capability = DRM_CLIENT_CAP_ATOMIC; cap.value = 1;
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
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
        chosen_conn = conn_ids[i];
        chosen_enc = c.encoder_id ? c.encoder_id : encs[0];
        have_mode = 1;
        free(modes); free(encs);
    }
    CHECK(have_mode, "find connector");

    uint32_t chosen_crtc = 0;
    for (uint32_t i = 0; i < res.count_encoders; i++) {
        if (enc_ids[i] == chosen_enc) {
            struct drm_mode_get_encoder e = { .encoder_id = enc_ids[i] };
            CHECK(ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0, "GETENCODER");
            for (uint32_t j = 0; j < res.count_crtcs; j++) {
                if (e.possible_crtcs & (1 << j)) {
                    chosen_crtc = crtc_ids[j];
                    chosen_crtc_index = j;
                    break;
                }
            }
            break;
        }
    }
    CHECK(chosen_crtc != 0, "find crtc");

    uint32_t plane_left = 101, plane_right = 98;
    uint32_t half_w = mode.hdisplay / 2;

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

    struct drm_mode_map_dumb map = { .handle = create.handle };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "MAP_DUMB");
    uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    CHECK(pixels != MAP_FAILED, "mmap");

    /* Decode frame 0 into pixels */
    int j_w = 0, j_h = 0, j_comp = 0;
    uint8_t *dec0 = stbi_load_from_memory(frame_ptrs[0], frame_sizes[0], &j_w, &j_h, &j_comp, 4);
    CHECK(dec0 != NULL, "decode frame 0");
    uint32_t npix = mode.hdisplay * mode.vdisplay;
    for (uint32_t i = 0; i < npix; i++) {
        uint8_t r = dec0[i * 4 + 0];
        uint8_t g = dec0[i * 4 + 1];
        uint8_t b = dec0[i * 4 + 2];
        pixels[i] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
    }
    stbi_image_free(dec0);

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

    /* Step 1: DISABLE pass */
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

    /* Step 2: ENABLE pass */
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
    int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atom);
    CHECK(ret == 0, "ATOMIC modeset commit");
    fprintf(stderr, "SUCCESS! Initial VR frame 0 accepted and visible!\n");

    /* Video Playback Loop */
    uint64_t frame_ns = 1000000000ULL / hdr->fps;
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    uint32_t cur_frame = 1;
    uint64_t total_played = 1;

    struct drm_clip_rect clip = { .x1 = 0, .y1 = 0, .x2 = mode.hdisplay, .y2 = mode.vdisplay };
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb_id,
        .flags = 0,
        .color = 0,
        .num_clips = 1,
        .clips_ptr = (uint64_t)(uintptr_t)&clip,
    };

    while (g_running) {
        int w = 0, h = 0, comp = 0;
        uint8_t *decoded = stbi_load_from_memory(frame_ptrs[cur_frame], frame_sizes[cur_frame], &w, &h, &comp, 4);
        if (decoded) {
            for (uint32_t i = 0; i < npix; i++) {
                uint8_t r = decoded[i * 4 + 0];
                uint8_t g = decoded[i * 4 + 1];
                uint8_t b = decoded[i * 4 + 2];
                pixels[i] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
            stbi_image_free(decoded);
            ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
        }

        cur_frame++;
        total_played++;
        if (cur_frame >= hdr->num_frames) {
            cur_frame = 0;
        }

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
    free(frame_ptrs);
    munmap(vid_data, vid_size);
    munmap(pixels, create.size);
    close(fd_vid);
    close(fd);
    return 0;
}
