#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#define __user
#include "drm.h"
#include "drm_mode.h"

int main(int argc, char **argv)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct drm_set_client_cap cap = {
        .capability = DRM_CLIENT_CAP_ATOMIC,
        .value = 1,
    };
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        perror("SET_CLIENT_CAP atomic");
    }

    uint32_t plane_ids[] = {98, 101};
    for (int i = 0; i < 2; i++) {
        uint32_t pid = plane_ids[i];
        printf("=== Plane %u ===\n", pid);
        struct drm_mode_obj_get_properties props = {0};
        props.obj_id = pid;
        props.obj_type = DRM_MODE_OBJECT_PLANE;
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);

        uint32_t *pids = calloc(props.count_props, sizeof(uint32_t));
        uint64_t *pvals = calloc(props.count_props, sizeof(uint64_t));
        props.props_ptr = (uint64_t)(uintptr_t)pids;
        props.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);

        for (uint32_t j = 0; j < props.count_props; j++) {
            struct drm_mode_get_property prop = {0};
            prop.prop_id = pids[j];
            ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
            printf("  %s: %lld (0x%llx)\n", prop.name, (long long)pvals[j], (unsigned long long)pvals[j]);
        }
        free(pids); free(pvals);
    }

    // Also check CRTC 129 properties!
    printf("=== CRTC 129 ===\n");
    struct drm_mode_obj_get_properties cprops = {0};
    cprops.obj_id = 129;
    cprops.obj_type = DRM_MODE_OBJECT_CRTC;
    ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &cprops);
    uint32_t *cpids = calloc(cprops.count_props, sizeof(uint32_t));
    uint64_t *cpvals = calloc(cprops.count_props, sizeof(uint64_t));
    cprops.props_ptr = (uint64_t)(uintptr_t)cpids;
    cprops.prop_values_ptr = (uint64_t)(uintptr_t)cpvals;
    ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &cprops);
    for (uint32_t j = 0; j < cprops.count_props; j++) {
        struct drm_mode_get_property prop = {0};
        prop.prop_id = cpids[j];
        ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
        printf("  %s: %lld (0x%llx)\n", prop.name, (long long)cpvals[j], (unsigned long long)cpvals[j]);
    }

    // Also check Connector 29 properties!
    printf("=== Connector 29 ===\n");
    struct drm_mode_obj_get_properties conprops = {0};
    conprops.obj_id = 29;
    conprops.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &conprops);
    uint32_t *conpids = calloc(conprops.count_props, sizeof(uint32_t));
    uint64_t *conpvals = calloc(conprops.count_props, sizeof(uint64_t));
    conprops.props_ptr = (uint64_t)(uintptr_t)conpids;
    conprops.prop_values_ptr = (uint64_t)(uintptr_t)conpvals;
    ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &conprops);
    for (uint32_t j = 0; j < conprops.count_props; j++) {
        struct drm_mode_get_property prop = {0};
        prop.prop_id = conpids[j];
        ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
        printf("  %s: %lld (0x%llx)\n", prop.name, (long long)conpvals[j], (unsigned long long)conpvals[j]);
    }

    close(fd);
    return 0;
}
