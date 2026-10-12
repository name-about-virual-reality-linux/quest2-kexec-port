#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define __user
#include "drm.h"
#include "drm_mode.h"

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR);
    struct drm_mode_crtc crtc = {.crtc_id = 129};
    ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc);
    printf("CRTC 129 mode:\n");
    printf("  name: %s\n", crtc.mode.name);
    printf("  clock: %u kHz\n", crtc.mode.clock);
    printf("  hdisplay: %u\n", crtc.mode.hdisplay);
    printf("  hsync_start: %u\n", crtc.mode.hsync_start);
    printf("  hsync_end: %u\n", crtc.mode.hsync_end);
    printf("  htotal: %u\n", crtc.mode.htotal);
    printf("  hskew: %u\n", crtc.mode.hskew);
    printf("  vdisplay: %u\n", crtc.mode.vdisplay);
    printf("  vsync_start: %u\n", crtc.mode.vsync_start);
    printf("  vsync_end: %u\n", crtc.mode.vsync_end);
    printf("  vtotal: %u\n", crtc.mode.vtotal);
    printf("  vscan: %u\n", crtc.mode.vscan);
    printf("  vrefresh: %u\n", crtc.mode.vrefresh);
    printf("  flags: 0x%x\n", crtc.mode.flags);
    printf("  type: 0x%x\n", crtc.mode.type);
    close(fd);
    return 0;
}
