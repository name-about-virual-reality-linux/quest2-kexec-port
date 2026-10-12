#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "drm.h"
#include "drm_mode.h"

int main(int argc, char **argv)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    struct drm_mode_card_res res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES");
        return 1;
    }
    printf("Resources: count_fbs=%u count_crtcs=%u count_connectors=%u count_encoders=%u\n",
           res.count_fbs, res.count_crtcs, res.count_connectors, res.count_encoders);

    uint32_t *fb_ids = calloc(res.count_fbs, sizeof(uint32_t));
    uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    uint32_t *enc_ids = calloc(res.count_encoders, sizeof(uint32_t));

    res.fb_id_ptr = (uint64_t)(uintptr_t)fb_ids;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES ids");
        return 1;
    }

    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_crtc crtc = {0};
        crtc.crtc_id = crtc_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0) {
            printf("CRTC ID %u: fb_id=%u x=%u y=%u gamma_size=%u mode_valid=%u\n",
                   crtc.crtc_id, crtc.fb_id, crtc.x, crtc.y, crtc.gamma_size, crtc.mode_valid);
            if (crtc.mode_valid) {
                printf("  mode: %s %ux%u @ %uHz (clock %u, flags 0x%x)\n",
                       crtc.mode.name, crtc.mode.hdisplay, crtc.mode.vdisplay,
                       crtc.mode.vrefresh, crtc.mode.clock, crtc.mode.flags);
            }
        }
    }

    for (uint32_t i = 0; i < res.count_encoders; i++) {
        struct drm_mode_get_encoder enc = {0};
        enc.encoder_id = enc_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
            printf("Encoder ID %u: type=%u crtc_id=%u possible_crtcs=0x%x possible_clones=0x%x\n",
                   enc.encoder_id, enc.encoder_type, enc.crtc_id, enc.possible_crtcs, enc.possible_clones);
        }
    }

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn = {0};
        conn.connector_id = conn_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0) {
            printf("Connector ID %u: type=%u status=%u encoder_id=%u count_modes=%u count_props=%u\n",
                   conn.connector_id, conn.connector_type, conn.connection, conn.encoder_id,
                   conn.count_modes, conn.count_props);
        }
    }

    close(fd);
    return 0;
}
