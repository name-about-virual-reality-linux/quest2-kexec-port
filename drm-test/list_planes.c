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
    struct drm_mode_get_plane_res pres = {0};
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres);
    uint32_t *pids = calloc(pres.count_planes, sizeof(uint32_t));
    pres.plane_id_ptr = (uint64_t)(uintptr_t)pids;
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres);

    for (uint32_t i = 0; i < pres.count_planes; i++) {
        struct drm_mode_get_plane p = {.plane_id = pids[i]};
        ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p);
        printf("Plane %u: possible_crtcs=0x%x crtc_id=%u fb_id=%u\n",
               p.plane_id, p.possible_crtcs, p.crtc_id, p.fb_id);
    }
    close(fd);
    return 0;
}
