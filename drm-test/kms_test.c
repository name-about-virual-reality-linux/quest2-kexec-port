/* Minimal KMS dumb-buffer modeset test. No libdrm, no dependencies beyond
 * the kernel DRM UAPI headers copied alongside this file. Opens /dev/dri/
 * card0, enumerates connectors, creates a solid-color framebuffer, and
 * drives SETCRTC on the first connector that has any modes at all
 * (ignores "connection" status, since embedded DSI panels here report
 * "unknown" rather than "connected" with no EDID hotplug detection).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#ifndef __user
#define __user
#endif
#include "drm.h"
#include "drm_mode.h"

#define CHECK(cond, msg) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno)); \
	return 1; } } while (0)

int main(int argc, char **argv)
{
	const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
	int fd = open(card, O_RDWR);
	CHECK(fd >= 0, "open card");

	struct drm_mode_card_res res = {0};
	CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES (sizes)");
	fprintf(stderr, "counts: fb=%u crtc=%u conn=%u enc=%u\n",
		res.count_fbs, res.count_crtcs, res.count_connectors, res.count_encoders);

	uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
	uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
	uint32_t *enc_ids = calloc(res.count_encoders, sizeof(uint32_t));
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
	res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;
	res.fb_id_ptr = 0;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES (ids)");

	uint32_t chosen_conn = 0, chosen_enc = 0;
	struct drm_mode_modeinfo mode = {0};
	int have_mode = 0;

	for (uint32_t i = 0; i < res.count_connectors && !have_mode; i++) {
		struct drm_mode_get_connector c = {0};
		c.connector_id = conn_ids[i];
		CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR (sizes)");
		fprintf(stderr, "connector %u: type=%u conn_status=%u modes=%u encoders=%u\n",
			conn_ids[i], c.connector_type, c.connection, c.count_modes, c.count_encoders);
		if (c.count_modes == 0)
			continue;
		struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
		uint32_t *encs = calloc(c.count_encoders, sizeof(uint32_t));
		c.modes_ptr = (uint64_t)(uintptr_t)modes;
		c.encoders_ptr = (uint64_t)(uintptr_t)encs;
		c.props_ptr = 0; c.prop_values_ptr = 0; c.count_props = 0;
		CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR (full)");
		mode = modes[0];
		chosen_conn = conn_ids[i];
		chosen_enc = c.count_encoders ? encs[0] : 0;
		have_mode = 1;
		fprintf(stderr, "picked mode: %ux%u@%u clock=%u\n",
			mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock);
		free(modes); free(encs);
	}
	CHECK(have_mode, "no connector with modes found");

	uint32_t chosen_crtc = crtc_ids[0];
	if (chosen_enc) {
		struct drm_mode_get_encoder e = {0};
		e.encoder_id = chosen_enc;
		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0 && e.crtc_id)
			chosen_crtc = e.crtc_id;
	}
	fprintf(stderr, "using connector=%u crtc=%u\n", chosen_conn, chosen_crtc);

	struct drm_mode_create_dumb create = {0};
	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "CREATE_DUMB");
	fprintf(stderr, "dumb buffer: handle=%u pitch=%u size=%llu\n",
		create.handle, create.pitch, (unsigned long long)create.size);

	uint32_t fb_id = 0;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &(struct drm_mode_fb_cmd){
		.width = create.width, .height = create.height,
		.pitch = create.pitch, .bpp = 32, .depth = 24,
		.handle = create.handle, .fb_id = 0,
	}) == 0 || (fb_id = 0, 1), "ADDFB");
	{
		struct drm_mode_fb_cmd fbcmd = {0};
		fbcmd.width = create.width; fbcmd.height = create.height;
		fbcmd.pitch = create.pitch; fbcmd.bpp = 32; fbcmd.depth = 24;
		fbcmd.handle = create.handle;
		CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd) == 0, "ADDFB (real)");
		fb_id = fbcmd.fb_id;
	}
	fprintf(stderr, "fb_id=%u\n", fb_id);

	struct drm_mode_map_dumb map = {0};
	map.handle = create.handle;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "MAP_DUMB");
	void *buf = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	CHECK(buf != MAP_FAILED, "mmap");

	/* Solid bright color, easy to spot on a headset screen. ARGB8888-ish. */
	uint32_t color = 0xFFFF2020; /* bright red-orange */
	for (size_t i = 0; i < create.size / 4; i++)
		((uint32_t *)buf)[i] = color;
	fprintf(stderr, "framebuffer filled with 0x%08x\n", color);

	struct drm_mode_crtc setcrtc = {0};
	setcrtc.crtc_id = chosen_crtc;
	setcrtc.fb_id = fb_id;
	setcrtc.x = 0; setcrtc.y = 0;
	setcrtc.set_connectors_ptr = (uint64_t)(uintptr_t)&chosen_conn;
	setcrtc.count_connectors = 1;
	setcrtc.mode = mode;
	setcrtc.mode_valid = 1;
	int ret = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &setcrtc);
	if (ret != 0) {
		fprintf(stderr, "SETCRTC failed: errno=%d %s\n", errno, strerror(errno));
		return 1;
	}
	fprintf(stderr, "SETCRTC succeeded. Holding for 15s so the screen can be checked...\n");
	sleep(15);
	fprintf(stderr, "done.\n");
	return 0;
}
