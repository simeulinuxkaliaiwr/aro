/* export: one frame of window APP_ID through Hyprland's toplevel export, found by its
 * wlr toplevel handle ("export APP_ID") or by aro's window id ("export id N"); print a pixel */
#define _GNU_SOURCE /* memfd_create */

#include "common.h"
#include "hyprland-toplevel-export-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct globals g;
static struct hyprland_toplevel_export_manager_v1 *exporter;
static struct zwlr_foreign_toplevel_manager_v1 *toplevels;
static struct zwlr_foreign_toplevel_handle_v1 *wanted;
static const char *wanted_app;
static uint32_t width, height, shm_format = UINT32_MAX;
static int buffers_done, frame_state;           /* 1 ready, -1 failed */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, hyprland_toplevel_export_manager_v1_interface.name))
		exporter = wl_registry_bind(r, name, &hyprland_toplevel_export_manager_v1_interface, 2);
	else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name))
		toplevels = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void h_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) { (void)d; (void)h; (void)t; }
static void h_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *a)
{
	(void)d;
	if (wanted_app && !strcmp(a, wanted_app))
		wanted = h;
}
static void h_output(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d; (void)h; (void)o; }
static void h_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a) { (void)d; (void)h; (void)a; }
static void h_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }
static void h_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
	h_title, h_app_id, h_output, h_output, h_state, h_done, h_closed, NULL,
};

static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)m;
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, NULL);
}
static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = { m_toplevel, m_finished };

static void f_buffer(void *d, struct hyprland_toplevel_export_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
	(void)d; (void)f; (void)stride;
	shm_format = fmt;
	width = w;
	height = h;
}
static void f_damage(void *d, struct hyprland_toplevel_export_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void f_flags(void *d, struct hyprland_toplevel_export_frame_v1 *f, uint32_t fl) { (void)d; (void)f; (void)fl; }
static void f_ready(void *d, struct hyprland_toplevel_export_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{ (void)d; (void)f; (void)a; (void)b; (void)c; frame_state = 1; }
static void f_failed(void *d, struct hyprland_toplevel_export_frame_v1 *f) { (void)d; (void)f; frame_state = -1; }
static void f_dmabuf(void *d, struct hyprland_toplevel_export_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{ (void)d; (void)f; (void)fmt; (void)w; (void)h; }
static void f_buffer_done(void *d, struct hyprland_toplevel_export_frame_v1 *f) { (void)d; (void)f; buffers_done = 1; }

static const struct hyprland_toplevel_export_frame_v1_listener frame_listener = {
	f_buffer, f_damage, f_flags, f_ready, f_failed, f_dmabuf, f_buffer_done,
};

int main(int argc, char **argv)
{
	int by_id = argc > 2 && !strcmp(argv[1], "id");
	wanted_app = argc > 1 && !by_id ? argv[1] : NULL;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!exporter)
		fail("aro does not offer hyprland-toplevel-export");

	struct hyprland_toplevel_export_frame_v1 *frame;
	if (by_id) {
		frame = hyprland_toplevel_export_manager_v1_capture_toplevel(exporter, 0,
			(uint32_t)strtoul(argv[2], NULL, 10));
	} else {
		if (!toplevels || !wanted_app)
			fail("usage: export APP_ID | export id N");
		zwlr_foreign_toplevel_manager_v1_add_listener(toplevels, &manager_listener, NULL);
		roundtrip(&g);
		roundtrip(&g);
		if (!wanted)
			fail("no window with app_id %s", wanted_app);
		frame = hyprland_toplevel_export_manager_v1_capture_toplevel_with_wlr_toplevel_handle(
			exporter, 0, wanted);
	}
	hyprland_toplevel_export_frame_v1_add_listener(frame, &frame_listener, NULL);
	for (int i = 0; i < 20 && !buffers_done && frame_state != -1; i++)
		roundtrip(&g);
	if (frame_state == -1)
		fail("frame failed");
	if (!buffers_done || !width || !height || shm_format == UINT32_MAX)
		fail("no buffer parameters from aro");

	int stride = width * 4, size = stride * height;
	int fd = memfd_create("export", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		fail("no shared memory");
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, shm_format);
	wl_shm_pool_destroy(pool);
	close(fd);

	hyprland_toplevel_export_frame_v1_copy(frame, buf, 1);
	for (int i = 0; i < 50 && !frame_state; i++) {
		roundtrip(&g);
		usleep(20000);
	}
	if (frame_state != 1)
		fail(frame_state ? "frame failed" : "no frame copied");

	uint32_t p = px[(height / 2) * width + width / 4];
	printf("exported %ux%u, middle pixel %06x\n", width, height, p & 0xffffff);
	return 0;
}
