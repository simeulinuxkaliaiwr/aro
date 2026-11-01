/* selfmove: one window that asks aro to move it on its first press, as SDL's hit test does, and prints every button it sees ("press N", "release N") for HOLD seconds */
#define _DEFAULT_SOURCE /* usleep */

#include "common.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct globals g;
static struct wl_seat *seat;
static struct wl_surface *surf;
static struct xdg_toplevel *top;
static int drawn, presses, releases;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                      wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)serial; (void)s; (void)x; (void)y; }

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)p; (void)serial; (void)s; }

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; (void)x; (void)y; }

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
                       uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)t; (void)button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		printf("press %d\n", ++presses);
		if (presses == 1)
			xdg_toplevel_move(top, seat, serial);
	} else {
		printf("release %d\n", ++releases);
	}
	fflush(stdout);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }

/* a version 1 seat sends only these */
static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis,
};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(surf, solid_buffer(&g, 400, 300, 0xff203040), 0, 0);
	wl_surface_commit(surf);
	drawn = 1;
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

int main(int argc, char **argv)
{
	int hold = argc > 1 ? atoi(argv[1]) : 5;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!seat)
		fail("aro does not offer a seat");
	wl_pointer_add_listener(wl_seat_get_pointer(seat), &ptr_listener, NULL);

	surf = wl_compositor_create_surface(g.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g.wm, surf);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_set_app_id(top, "aro-test");
	wl_surface_commit(surf);
	for (int tries = 0; tries < 20 && !drawn; tries++)
		roundtrip(&g);
	printf("open\n");
	fflush(stdout);

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= hold)
			break;
		wl_display_flush(g.dpy);
		struct pollfd pfd = { wl_display_get_fd(g.dpy), POLLIN, 0 };
		if (poll(&pfd, 1, 100) > 0 && wl_display_dispatch(g.dpy) < 0)
			fail("aro went away");
		wl_display_dispatch_pending(g.dpy);
	}
	return 0;
}
