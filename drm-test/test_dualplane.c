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

#define CHECK(cond, msg) do { if (!(cond)) {     fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno));     return 1; } } while (0)

static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    struct drm_mode_obj_get_properties props = {.obj_id = obj_id, .obj_type = obj_type};
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) != 0) return 0;
    uint32_t *pids = calloc(props.count_props, sizeof(uint32_t));
    uint64_t *pvals = calloc(props.count_props, sizeof(uint64_t));
    props.props_ptr = (uint64_t)(uintptr_t)pids;
    props.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
    ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);
    uint32_t ret = 0;
    for (uint32_t i = 0; i < props.count_props; i++) {
        struct drm_mode_get_property prop = {.prop_id = pids[i]};
        ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
        if (strcmp(prop.name, name) == 0) {
            ret = pids[i];
            break;
        }
    }
    free(pids); free(pvals);
    return ret;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    CHECK(fd >= 0, "open card0");

    struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
    CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0, "set atomic cap");
    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

    uint32_t crtc_id = 129;
    uint32_t conn_id = 29;
    uint32_t plane_left = 101;
    uint32_t plane_right = 98;

    // Get 90Hz mode from CRTC
    struct drm_mode_crtc crtc = {.crtc_id = crtc_id};
    CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0, "get crtc");
    struct drm_mode_modeinfo mode = crtc.mode;
    printf("Using mode: %s %ux%u@%u clock=%u flags=0x%x\n",
           mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock, mode.flags);

    // Create mode blob
    struct drm_mode_create_blob blob = {
        .data = (uint64_t)(uintptr_t)&mode,
        .length = sizeof(mode),
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) == 0, "create mode blob");

    // Create dumb framebuffer 1920x3664
    struct drm_mode_create_dumb create = {
        .width = 1920,
        .height = 3664,
        .bpp = 32,
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "create dumb");

    struct drm_mode_fb_cmd fbcmd = {
        .width = 1920,
        .height = 3664,
        .pitch = create.pitch,
        .bpp = 32,
        .depth = 24,
        .handle = create.handle,
    };
    CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd) == 0, "add fb");
    uint32_t fb_id = fbcmd.fb_id;
    printf("Created FB %u (pitch=%u, size=%llu)\n", fb_id, create.pitch, (unsigned long long)create.size);

    struct drm_mode_map_dumb map = {.handle = create.handle};
    CHECK(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "map dumb");
    uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    CHECK(pixels != MAP_FAILED, "mmap");

    // Fill pattern: Left half RED, Right half GREEN
    for (uint32_t y = 0; y < 3664; y++) {
        for (uint32_t x = 0; x < 1920; x++) {
            pixels[y * (create.pitch / 4) + x] = (x < 960) ? 0xFFFF0000 : 0xFF00FF00;
        }
    }

    // Property IDs
    uint32_t crtc_active = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t crtc_mode_id = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    uint32_t crtc_core_clk = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "core_clk");
    uint32_t crtc_core_ab = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "core_ab");
    uint32_t crtc_llcc_ab = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "llcc_ab");
    uint32_t crtc_dram_ab = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "dram_ab");
    uint32_t crtc_dram_ib = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "dram_ib");

    uint32_t conn_crtc_id = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

    uint32_t p_fb_id = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t p_crtc_id = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t p_src_x = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t p_src_y = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t p_src_w = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t p_src_h = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "SRC_H");
    uint32_t p_crtc_x = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t p_crtc_y = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t p_crtc_w = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t p_crtc_h = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint32_t p_zpos = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "zpos");
    uint32_t p_blend_op = get_prop_id(fd, plane_left, DRM_MODE_OBJECT_PLANE, "blend_op");

    uint32_t objs[4] = { crtc_id, conn_id, plane_left, plane_right };
    uint32_t counts[4] = { 0, 1, 0, 0 };

    uint32_t props[64];
    uint64_t vals[64];
    int n = 0;

    // CRTC
    props[n] = crtc_active; vals[n++] = 1; counts[0]++;
    props[n] = crtc_mode_id; vals[n++] = blob.blob_id; counts[0]++;
    if (crtc_core_clk) { props[n] = crtc_core_clk; vals[n++] = 445798080; counts[0]++; }
    if (crtc_core_ab)  { props[n] = crtc_core_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (crtc_llcc_ab)  { props[n] = crtc_llcc_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (crtc_dram_ab)  { props[n] = crtc_dram_ab; vals[n++] = 3396556800ULL; counts[0]++; }
    if (crtc_dram_ib)  { props[n] = crtc_dram_ib; vals[n++] = 1600000000ULL; counts[0]++; }

    // Connector
    props[n] = conn_crtc_id; vals[n++] = crtc_id;

    // Plane Left (101): covers X=0..960
    props[n] = p_fb_id; vals[n++] = fb_id; counts[2]++;
    props[n] = p_crtc_id; vals[n++] = crtc_id; counts[2]++;
    props[n] = p_crtc_x; vals[n++] = 0; counts[2]++;
    props[n] = p_crtc_y; vals[n++] = 0; counts[2]++;
    props[n] = p_crtc_w; vals[n++] = 960; counts[2]++;
    props[n] = p_crtc_h; vals[n++] = 3664; counts[2]++;
    props[n] = p_src_x; vals[n++] = 0; counts[2]++;
    props[n] = p_src_y; vals[n++] = 0; counts[2]++;
    props[n] = p_src_w; vals[n++] = 960ULL << 16; counts[2]++;
    props[n] = p_src_h; vals[n++] = 3664ULL << 16; counts[2]++;
    if (p_zpos) { props[n] = p_zpos; vals[n++] = 0; counts[2]++; }
    if (p_blend_op) { props[n] = p_blend_op; vals[n++] = 1; counts[2]++; }

    // Plane Right (98): covers X=960..1920
    props[n] = p_fb_id; vals[n++] = fb_id; counts[3]++;
    props[n] = p_crtc_id; vals[n++] = crtc_id; counts[3]++;
    props[n] = p_crtc_x; vals[n++] = 960; counts[3]++;
    props[n] = p_crtc_y; vals[n++] = 0; counts[3]++;
    props[n] = p_crtc_w; vals[n++] = 960; counts[3]++;
    props[n] = p_crtc_h; vals[n++] = 3664; counts[3]++;
    props[n] = p_src_x; vals[n++] = 960ULL << 16; counts[3]++;
    props[n] = p_src_y; vals[n++] = 0; counts[3]++;
    props[n] = p_src_w; vals[n++] = 960ULL << 16; counts[3]++;
    props[n] = p_src_h; vals[n++] = 3664ULL << 16; counts[3]++;
    if (p_zpos) { props[n] = p_zpos; vals[n++] = 0; counts[3]++; }
    if (p_blend_op) { props[n] = p_blend_op; vals[n++] = 1; counts[3]++; }

    struct drm_mode_atomic req = {
        .flags = DRM_MODE_ATOMIC_ALLOW_MODESET,
        .count_objs = 4,
        .objs_ptr = (uint64_t)(uintptr_t)objs,
        .count_props_ptr = (uint64_t)(uintptr_t)counts,
        .props_ptr = (uint64_t)(uintptr_t)props,
        .prop_values_ptr = (uint64_t)(uintptr_t)vals,
    };

    int hold = argc > 1 ? atoi(argv[1]) : 30;
    printf("Committing dual-plane atomic modeset with %d props (hold %ds)...\n", n, hold);
    int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &req);
    if (ret != 0) {
        printf("ATOMIC commit failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("SUCCESS! Dual-plane commit accepted! Holding for %d seconds...\n", hold);
    sleep(hold);
    close(fd);
    return 0;
}
