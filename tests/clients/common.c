/* common.c: what every test client needs from a running aro */
#define _GNU_SOURCE /* memfd_create */

#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Noreturn void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
	struct globals *g = data;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name))
		g->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		g->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		g->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(g->wm, &wm_listener, NULL);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		g->layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(iface, ext_workspace_manager_v1_interface.name))
		g->workspaces = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
}

static void reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

void connect_globals(struct globals *g)
{
	memset(g, 0, sizeof *g);
	g->dpy = wl_display_connect(NULL);
	if (!g->dpy)
		fail("cannot connect to aro");
	wl_registry_add_listener(wl_display_get_registry(g->dpy), &reg_listener, g);
	roundtrip(g);
	if (!g->compositor || !g->shm || !g->wm || !g->layer_shell)
		fail("aro is missing a basic global");
}

void roundtrip(struct globals *g)
{
	if (wl_display_roundtrip(g->dpy) < 0)
		fail("aro went away");
}

struct wl_buffer *solid_buffer(struct globals *g, int w, int h, uint32_t argb)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("aro-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		fail("no shared memory");
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		fail("mmap failed");
	for (int i = 0; i < w * h; i++)
		px[i] = argb;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(g->shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
	                                                WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}
