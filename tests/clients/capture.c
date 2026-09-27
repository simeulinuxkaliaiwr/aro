/* capture: grab one frame of the first output ("output") or of window APP_ID ("window APP_ID"); print a pixel inside it */
#define _GNU_SOURCE /* memfd_create */

#include "common.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct globals g;
static struct wl_output *output;
static struct ext_output_image_capture_source_manager_v1 *out_src;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *top_src;
static struct ext_image_copy_capture_manager_v1 *copy;
static struct ext_foreign_toplevel_list_v1 *toplevels;
static struct ext_foreign_toplevel_handle_v1 *wanted;
static const char *wanted_app;
static uint32_t width, height, shm_format = UINT32_MAX;
static int constraints_done, frame_state;       /* 1 ready, -1 failed */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name))
		out_src = wl_registry_bind(r, name, &ext_output_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
		top_src = wl_registry_bind(r, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name))
		copy = wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name))
		toplevels = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void h_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }
static void h_done(void *d, struct ext_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }
static void h_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *t) { (void)d; (void)h; (void)t; }
static void h_app_id(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *a)
{
	(void)d;
	if (wanted_app && !strcmp(a, wanted_app))
		wanted = h;
}
static void h_identifier(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *i) { (void)d; (void)h; (void)i; }

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	h_closed, h_done, h_title, h_app_id, h_identifier,
};

static void l_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l, struct ext_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)l;
	ext_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, NULL);
}
static void l_finished(void *d, struct ext_foreign_toplevel_list_v1 *l) { (void)d; (void)l; }

static const struct ext_foreign_toplevel_list_v1_listener list_listener = { l_toplevel, l_finished };

static void s_size(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t w, uint32_t h)
{ (void)d; (void)s; width = w; height = h; }
static void s_shm(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f)
{
	(void)d; (void)s;
	if (getenv("CAPTURE_DEBUG"))
		fprintf(stderr, "offered shm format %08x\n", f);
	if (shm_format == UINT32_MAX || f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888)
		shm_format = f;
}
static void s_dmabuf_dev(void *d, struct ext_image_copy_capture_session_v1 *s, struct wl_array *a) { (void)d; (void)s; (void)a; }
static void s_dmabuf_fmt(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f, struct wl_array *m) { (void)d; (void)s; (void)f; (void)m; }
static void s_done(void *d, struct ext_image_copy_capture_session_v1 *s) { (void)d; (void)s; constraints_done = 1; }
static void s_stopped(void *d, struct ext_image_copy_capture_session_v1 *s) { (void)d; (void)s; fail("capture session stopped"); }

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	s_size, s_shm, s_dmabuf_dev, s_dmabuf_fmt, s_done, s_stopped,
};

static void f_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) { (void)d; (void)f; (void)t; }
static void f_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void f_time(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{ (void)d; (void)f; (void)a; (void)b; (void)c; }
static void f_ready(void *d, struct ext_image_copy_capture_frame_v1 *f) { (void)d; (void)f; frame_state = 1; }
static uint32_t fail_reason;
static void f_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t r)
{ (void)d; (void)f; fail_reason = r; frame_state = -1; }

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	f_transform, f_damage, f_time, f_ready, f_failed,
};

int main(int argc, char **argv)
{
	int window = argc > 2 && !strcmp(argv[1], "window");
	wanted_app = window ? argv[2] : NULL;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!copy || !out_src)
		fail("aro does not offer ext-image-copy-capture");

	struct ext_image_capture_source_v1 *src;
	if (window) {
		if (!top_src || !toplevels)
			fail("window capture not offered");
		ext_foreign_toplevel_list_v1_add_listener(toplevels, &list_listener, NULL);
		roundtrip(&g);
		roundtrip(&g);
		if (!wanted)
			fail("no window with app_id %s", wanted_app);
		src = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(top_src, wanted);
	} else {
		src = ext_output_image_capture_source_manager_v1_create_source(out_src, output);
	}

	struct ext_image_copy_capture_session_v1 *session =
		ext_image_copy_capture_manager_v1_create_session(copy, src, 0);
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
	/* a window that is resizing changes its size under us: take the new one and retry */
	uint32_t *px = NULL;
	for (int attempt = 0; attempt < 5; attempt++) {
		/* up to two seconds: a resizing window sends its size when it next draws */
		for (int i = 0; i < 100 && !constraints_done; i++) {
			roundtrip(&g);
			if (!constraints_done)
				usleep(20000);
		}
		if (!constraints_done || !width || !height || shm_format == UINT32_MAX)
			fail("no buffer constraints from aro");

		int stride = width * 4, size = stride * height;
		int fd = memfd_create("capture", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, size) < 0)
			fail("no shared memory");
		px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		struct wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, size);
		struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, shm_format);
		wl_shm_pool_destroy(pool);
		close(fd);

		frame_state = 0;
		struct ext_image_copy_capture_frame_v1 *frame =
			ext_image_copy_capture_session_v1_create_frame(session);
		ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
		ext_image_copy_capture_frame_v1_attach_buffer(frame, buf);
		ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, width, height);
		ext_image_copy_capture_frame_v1_capture(frame);
		for (int i = 0; i < 50 && !frame_state; i++) {
			roundtrip(&g);
			usleep(20000);
		}
		ext_image_copy_capture_frame_v1_destroy(frame);
		if (frame_state == 1)
			break;
		if (fail_reason != EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS)
			fail("frame failed: %u", fail_reason);
		constraints_done = 0;   /* aro sends the new size; wait for it */
		usleep(100000);
	}
	if (frame_state != 1)
		fail("no frame captured");

	/* a quarter across, half down: inside a window in every layout, never in a gap */
	uint32_t p = px[(height / 2) * width + width / 4];
	if (shm_format == WL_SHM_FORMAT_XBGR8888 || shm_format == WL_SHM_FORMAT_ABGR8888)
		p = (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff);
	printf("captured %ux%u, middle pixel %06x\n", width, height, p & 0xffffff);
	return 0;
}
