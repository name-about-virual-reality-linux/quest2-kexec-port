#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#define __user
#include "drm.h"
#include "drm_mode.h"

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR);
    struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);

    uint32_t objs[] = {129, 29, 101, 98};
    uint32_t types[] = {DRM_MODE_OBJECT_CRTC, DRM_MODE_OBJECT_CONNECTOR, DRM_MODE_OBJECT_PLANE, DRM_MODE_OBJECT_PLANE};

    for (int i = 0; i < 4; i++) {
        printf("=== OBJ %u ===\n", objs[i]);
        struct drm_mode_obj_get_properties p = {.obj_id = objs[i], .obj_type = types[i]};
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p);
        uint32_t *pids = calloc(p.count_props, sizeof(uint32_t));
        uint64_t *pvals = calloc(p.count_props, sizeof(uint64_t));
        p.props_ptr = (uint64_t)(uintptr_t)pids;
        p.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p);
        for (uint32_t j = 0; j < p.count_props; j++) {
            struct drm_mode_get_property prop = {.prop_id = pids[j]};
            ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
            printf("  %s = ID %u (val %llu)\n", prop.name, pids[j], (unsigned long long)pvals[j]);
        }
        free(pids); free(pvals);
    }
    close(fd);
    return 0;
}
