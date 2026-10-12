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
            printf("  %s: %llu (0x%llx)\n", prop.name, (unsigned long long)pvals[j], (unsigned long long)pvals[j]);
        }
        free(pids); free(pvals);
    }
    close(fd);
    return 0;
}
