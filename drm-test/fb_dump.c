#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define __user
#include "drm.h"
#include "drm_mode.h"

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR);
    struct drm_mode_fb_cmd fb = {.fb_id = 298};
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb) == 0) {
        printf("FB 298: width=%u height=%u pitch=%u bpp=%u depth=%u handle=%u\n",
               fb.width, fb.height, fb.pitch, fb.bpp, fb.depth, fb.handle);
    } else {
        perror("GETFB 298");
    }
    close(fd);
    return 0;
}
