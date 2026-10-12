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

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct drm_mode_get_plane_res pres = {0};
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres);
    printf("Plane count: %u\n", pres.count_planes);

    uint32_t *plane_ids = calloc(pres.count_planes, sizeof(uint32_t));
    pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres);

    for (uint32_t i = 0; i < pres.count_planes; i++) {
        struct drm_mode_get_plane p = {0};
        p.plane_id = plane_ids[i];
        ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p);

        // Get plane properties
        struct drm_mode_obj_get_properties props = {0};
        props.obj_id = plane_ids[i];
        props.obj_type = DRM_MODE_OBJECT_PLANE;
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);
        if (props.count_props > 0) {
            uint32_t *prop_ids = calloc(props.count_props, sizeof(uint32_t));
            uint64_t *prop_vals = calloc(props.count_props, sizeof(uint64_t));
            props.props_ptr = (uint64_t)(uintptr_t)prop_ids;
            props.prop_values_ptr = (uint64_t)(uintptr_t)prop_vals;
            ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);

            if (p.fb_id != 0) {
                printf("Plane ID %u: crtc_id=%u fb_id=%u possible_crtcs=0x%x\n",
                       p.plane_id, p.crtc_id, p.fb_id, p.possible_crtcs);
                for (uint32_t j = 0; j < props.count_props; j++) {
                    struct drm_mode_get_property prop = {0};
                    prop.prop_id = prop_ids[j];
                    ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
                    if (prop_vals[j] != 0 || strstr(prop.name, "CRTC") || strstr(prop.name, "SRC"))
                        printf("   prop %s (%u) = %llu (0x%llx)\n", prop.name, prop_ids[j],
                               (unsigned long long)prop_vals[j], (unsigned long long)prop_vals[j]);
                }
            }
            free(prop_ids); free(prop_vals);
        }
    }
    close(fd);
    return 0;
}
