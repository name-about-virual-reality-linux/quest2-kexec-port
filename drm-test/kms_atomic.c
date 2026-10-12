/* Atomic KMS modeset test, no libdrm. Same card/connector/mode discovery as
 * kms_test.c, but drives the panel through DRM_IOCTL_MODE_ATOMIC instead of
 * the legacy SETCRTC path, which this downstream SDE driver may only
 * partially support (Android's compositor always uses atomic, never
 * legacy). Sets CRTC MODE_ID+ACTIVE, connector CRTC_ID, and plane
 * FB_ID, CRTC_ID, and the SRC/CRTC geometry props, in one atomic commit
 * with ALLOW_MODESET.
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

#define MAX_PROPS 32

struct obj_props {
	uint32_t obj_id, obj_type;
	uint32_t prop_id[MAX_PROPS];
	char name[MAX_PROPS][DRM_PROP_NAME_LEN];
	int count;
};

static int load_props(int fd, struct obj_props *o)
{
	struct drm_mode_obj_get_properties g = {0};
	g.obj_id = o->obj_id;
	g.obj_type = o->obj_type;
	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) != 0)
		return -1;
	uint32_t *ids = calloc(g.count_props, sizeof(uint32_t));
	uint64_t *vals = calloc(g.count_props, sizeof(uint64_t));
	g.props_ptr = (uint64_t)(uintptr_t)ids;
	g.prop_values_ptr = (uint64_t)(uintptr_t)vals;
	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) != 0) {
		free(ids); free(vals);
		return -1;
	}
	o->count = g.count_props > MAX_PROPS ? MAX_PROPS : g.count_props;
	for (int i = 0; i < o->count; i++) {
		o->prop_id[i] = ids[i];
		struct drm_mode_get_property p = {0};
		p.prop_id = ids[i];
		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0)
			memcpy(o->name[i], p.name, DRM_PROP_NAME_LEN);
		else
			o->name[i][0] = 0;
	}
	free(ids); free(vals);
	return 0;
}

static uint32_t find_prop(struct obj_props *o, const char *name)
{
	for (int i = 0; i < o->count; i++)
		if (!strcmp(o->name[i], name))
			return o->prop_id[i];
	fprintf(stderr, "WARN: property '%s' not found on obj %u (type %u)\n",
		name, o->obj_id, o->obj_type);
	return 0;
}

int main(int argc, char **argv)
{
	const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
	int fd = open(card, O_RDWR);
	CHECK(fd >= 0, "open card");

	struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
	CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0, "SET_CLIENT_CAP universal planes");
	cap.capability = DRM_CLIENT_CAP_ATOMIC; cap.value = 1;
	CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0, "SET_CLIENT_CAP atomic");

	struct drm_mode_card_res res = {0};
	CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES (sizes)");
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
	int have_mode = 0, chosen_crtc_index = 0;

	for (uint32_t i = 0; i < res.count_connectors && !have_mode; i++) {
		struct drm_mode_get_connector c = {0};
		c.connector_id = conn_ids[i];
		CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR (sizes)");
		if (c.count_modes == 0)
			continue;
		struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
		uint32_t *encs = calloc(c.count_encoders, sizeof(uint32_t));
		c.modes_ptr = (uint64_t)(uintptr_t)modes;
		c.encoders_ptr = (uint64_t)(uintptr_t)encs;
		c.props_ptr = 0; c.prop_values_ptr = 0; c.count_props = 0;
		CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0, "GETCONNECTOR (full)");
		mode = modes[0];
		/* QKX_HZ: pick the first mode with this vrefresh (stock Android runs 90). */
		const char *want_hz = getenv("QKX_HZ");
		if (want_hz) {
			uint32_t hz = (uint32_t)atoi(want_hz);
			for (uint32_t m = 0; m < c.count_modes; m++)
				if (modes[m].vrefresh == hz) {
					mode = modes[m];
					break;
				}
		}
		chosen_conn = conn_ids[i];
		chosen_enc = c.count_encoders ? encs[0] : 0;
		have_mode = 1;
		fprintf(stderr, "connector=%u conn_status=%u picked mode: %ux%u@%u\n",
			chosen_conn, c.connection, mode.hdisplay, mode.vdisplay, mode.vrefresh);
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
	/* QKX_CRTC: force a CRTC index (stock Android drives the panel from sde-crtc-1). */
	const char *want_crtc = getenv("QKX_CRTC");
	if (want_crtc && (uint32_t)atoi(want_crtc) < res.count_crtcs)
		chosen_crtc = crtc_ids[atoi(want_crtc)];
	for (uint32_t i = 0; i < res.count_crtcs; i++)
		if (crtc_ids[i] == chosen_crtc)
			chosen_crtc_index = i;
	fprintf(stderr, "using connector=%u crtc=%u (index %d)\n",
		chosen_conn, chosen_crtc, chosen_crtc_index);

	/* Find a plane whose possible_crtcs includes our chosen CRTC. */
	struct drm_mode_get_plane_res pres = {0};
	CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0, "GETPLANERESOURCES (size)");
	uint32_t *plane_ids = calloc(pres.count_planes, sizeof(uint32_t));
	pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0, "GETPLANERESOURCES (ids)");
	uint32_t chosen_plane = 0;
	for (uint32_t i = 0; i < pres.count_planes; i++) {
		struct drm_mode_get_plane pl = {0};
		pl.plane_id = plane_ids[i];
		CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl) == 0, "GETPLANE");
		fprintf(stderr, "plane %u: possible_crtcs=0x%x fmt_count=%u\n",
			plane_ids[i], pl.possible_crtcs, pl.count_format_types);
		if (pl.possible_crtcs & (1u << chosen_crtc_index) && !chosen_plane)
			chosen_plane = plane_ids[i];
	}
	CHECK(chosen_plane, "no usable plane found for chosen crtc");
	fprintf(stderr, "using plane=%u\n", chosen_plane);

	struct drm_mode_create_dumb create = {0};
	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "CREATE_DUMB");

	struct drm_mode_fb_cmd fbcmd = {0};
	fbcmd.width = create.width; fbcmd.height = create.height;
	fbcmd.pitch = create.pitch; fbcmd.bpp = 32; fbcmd.depth = 24;
	fbcmd.handle = create.handle;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd) == 0, "ADDFB");
	uint32_t fb_id = fbcmd.fb_id;
	fprintf(stderr, "fb_id=%u size=%llu\n", fb_id, (unsigned long long)create.size);

	struct drm_mode_map_dumb map = {0};
	map.handle = create.handle;
	CHECK(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "MAP_DUMB");
	void *buf = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	CHECK(buf != MAP_FAILED, "mmap");
	uint32_t color = 0xFF20FF40; /* bright green this time, to tell apart from the earlier red/white flash */
	if (getenv("QKX_COLOR"))
		color = (uint32_t)strtoul(getenv("QKX_COLOR"), NULL, 16); /* QKX_COLOR=FFFF0000 etc. */
	if (getenv("QKX_PATTERN")) {
		/* Quadrants TL red, TR green, BL blue, BR white, black grid every 120px:
		 * shows link/half geometry, and DSC damage lines up with 480px slices. */
		static const uint32_t quad[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };
		for (uint32_t y = 0; y < create.height; y++) {
			uint32_t *row = (uint32_t *)((char *)buf + (size_t)y * create.pitch);
			for (uint32_t x = 0; x < create.width; x++) {
				int q = (x >= create.width / 2) + 2 * (y >= create.height / 2);
				row[x] = (x % 120 < 2 || y % 120 < 2) ? 0xFF000000 : quad[q];
			}
		}
		fprintf(stderr, "framebuffer filled with quadrant test pattern\n");
	} else {
		for (size_t i = 0; i < create.size / 4; i++)
			((uint32_t *)buf)[i] = color;
		fprintf(stderr, "framebuffer filled with 0x%08x\n", color);
	}

	/* Create the MODE_ID blob. */
	struct drm_mode_create_blob blob = {0};
	blob.data = (uint64_t)(uintptr_t)&mode;
	blob.length = sizeof(mode);
	CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) == 0, "CREATEPROPBLOB");
	fprintf(stderr, "mode blob_id=%u\n", blob.blob_id);

	struct obj_props crtc_p = { .obj_id = chosen_crtc, .obj_type = DRM_MODE_OBJECT_CRTC };
	struct obj_props conn_p = { .obj_id = chosen_conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR };
	struct obj_props plane_p = { .obj_id = chosen_plane, .obj_type = DRM_MODE_OBJECT_PLANE };
	CHECK(load_props(fd, &crtc_p) == 0, "load crtc props");
	CHECK(load_props(fd, &conn_p) == 0, "load connector props");
	CHECK(load_props(fd, &plane_p) == 0, "load plane props");

	uint32_t p_mode_id = find_prop(&crtc_p, "MODE_ID");
	uint32_t p_active = find_prop(&crtc_p, "ACTIVE");
	uint32_t p_crtc_id_conn = find_prop(&conn_p, "CRTC_ID");
	uint32_t p_fb_id = find_prop(&plane_p, "FB_ID");
	uint32_t p_crtc_id_plane = find_prop(&plane_p, "CRTC_ID");
	uint32_t p_src_x = find_prop(&plane_p, "SRC_X");
	uint32_t p_src_y = find_prop(&plane_p, "SRC_Y");
	uint32_t p_src_w = find_prop(&plane_p, "SRC_W");
	uint32_t p_src_h = find_prop(&plane_p, "SRC_H");
	uint32_t p_crtc_x = find_prop(&plane_p, "CRTC_X");
	uint32_t p_crtc_y = find_prop(&plane_p, "CRTC_Y");
	uint32_t p_crtc_w = find_prop(&plane_p, "CRTC_W");
	uint32_t p_crtc_h = find_prop(&plane_p, "CRTC_H");
	CHECK(p_mode_id && p_active && p_crtc_id_conn && p_fb_id && p_crtc_id_plane &&
	      p_src_x && p_src_y && p_src_w && p_src_h && p_crtc_x && p_crtc_y &&
	      p_crtc_w && p_crtc_h, "all required properties found");

	/* Build the atomic request: 3 objects (crtc, connector, plane). */
	uint32_t objs[3] = { chosen_crtc, chosen_conn, chosen_plane };
	uint32_t counts[3] = { 2, 1, 10 };
	uint32_t props[2 + 1 + 10];
	uint64_t vals[2 + 1 + 10];
	int n = 0;
	props[n] = p_mode_id; vals[n++] = blob.blob_id;
	props[n] = p_active;  vals[n++] = 1;
	props[n] = p_crtc_id_conn; vals[n++] = chosen_crtc;
	props[n] = p_fb_id;       vals[n++] = fb_id;
	props[n] = p_crtc_id_plane; vals[n++] = chosen_crtc;
	props[n] = p_src_x; vals[n++] = 0;
	props[n] = p_src_y; vals[n++] = 0;
	props[n] = p_src_w; vals[n++] = ((uint64_t)mode.hdisplay) << 16;
	props[n] = p_src_h; vals[n++] = ((uint64_t)mode.vdisplay) << 16;
	props[n] = p_crtc_x; vals[n++] = 0;
	props[n] = p_crtc_y; vals[n++] = 0;
	props[n] = p_crtc_w; vals[n++] = mode.hdisplay;
	props[n] = p_crtc_h; vals[n++] = mode.vdisplay;

	/* Force a genuine OFF->ON transition: if the CRTC is already marked
	 * active (inherited from cont-splash through the kexec jump), a
	 * commit that just re-requests ACTIVE=1 is a no-op transition from
	 * DRM core's point of view and the bridge/encoder enable() callback
	 * (which sends the panel's real DCS init commands) never fires. So
	 * explicitly disable first, then enable. */
	{
		uint32_t disable_objs[1] = { chosen_crtc };
		uint32_t disable_counts[1] = { 2 };
		uint32_t disable_props[2] = { p_mode_id, p_active };
		uint64_t disable_vals[2] = { 0, 0 };
		struct drm_mode_atomic disable_atomic = {0};
		disable_atomic.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
		disable_atomic.count_objs = 1;
		disable_atomic.objs_ptr = (uint64_t)(uintptr_t)disable_objs;
		disable_atomic.count_props_ptr = (uint64_t)(uintptr_t)disable_counts;
		disable_atomic.props_ptr = (uint64_t)(uintptr_t)disable_props;
		disable_atomic.prop_values_ptr = (uint64_t)(uintptr_t)disable_vals;
		fprintf(stderr, "committing DISABLE (crtc ACTIVE=0, MODE_ID=0) first...\n");
		int dret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &disable_atomic);
		fprintf(stderr, "disable commit ret=%d (errno=%d %s)\n",
			dret, dret ? errno : 0, dret ? strerror(errno) : "ok");
	}

	struct drm_mode_atomic atomic = {0};
	atomic.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	atomic.count_objs = 3;
	atomic.objs_ptr = (uint64_t)(uintptr_t)objs;
	atomic.count_props_ptr = (uint64_t)(uintptr_t)counts;
	atomic.props_ptr = (uint64_t)(uintptr_t)props;
	atomic.prop_values_ptr = (uint64_t)(uintptr_t)vals;

	fprintf(stderr, "committing ENABLE atomic request (%d total props across 3 objects)...\n", n);
	int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
	if (ret != 0) {
		fprintf(stderr, "ATOMIC commit failed: errno=%d %s\n", errno, strerror(errno));
		return 1;
	}
	int hold = getenv("QKX_HOLD") ? atoi(getenv("QKX_HOLD")) : 15;
	fprintf(stderr, "ATOMIC commit succeeded (mode %ux%u@%u, clock %u). Holding for %ds so the screen can be checked...\n",
		mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock, hold);
	sleep(hold);
	fprintf(stderr, "done.\n");
	return 0;
}
