/* window: opens N windows, keeps them for HOLD seconds, then closes them one by one */
#define _DEFAULT_SOURCE /* usleep */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_WINDOWS 16

struct win {
	struct wl_surface *surf;
	struct xdg_surface *xs;
	struct xdg_toplevel *top;
	int w, h;
	int drawn;
};

static struct globals g;
static struct win wins[MAX_WINDOWS];

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	struct win *w = data;
	xdg_surface_ack_configure(xs, serial);
	int bw = w->w > 0 ? w->w : 400, bh = w->h > 0 ? w->h : 300;
	wl_surface_attach(w->surf, solid_buffer(&g, bw, bh, 0xff203040), 0, 0);
	wl_surface_commit(w->surf);
	w->drawn = 1;
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *states)
{
	struct win *win = data;
	(void)t; (void)states;
	win->w = w;
	win->h = h;
}

static void top_close(void *data, struct xdg_toplevel *t) { (void)data; (void)t; }

static const struct xdg_toplevel_listener top_listener = {
	.configure = top_configure,
	.close = top_close,
};

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 3;
	int hold = argc > 2 ? atoi(argv[2]) : 3;
	if (n < 1 || n > MAX_WINDOWS)
		fail("usage: window N HOLD, N from 1 to %d", MAX_WINDOWS);
	connect_globals(&g);

	for (int i = 0; i < n; i++) {
		struct win *w = &wins[i];
		w->surf = wl_compositor_create_surface(g.compositor);
		w->xs = xdg_wm_base_get_xdg_surface(g.wm, w->surf);
		xdg_surface_add_listener(w->xs, &xs_listener, w);
		w->top = xdg_surface_get_toplevel(w->xs);
		xdg_toplevel_add_listener(w->top, &top_listener, w);
		xdg_toplevel_set_app_id(w->top, "aro-test");
		wl_surface_commit(w->surf);
		roundtrip(&g);
	}
	for (int tries = 0; tries < 20; tries++) {
		int all = 1;
		for (int i = 0; i < n; i++)
			all &= wins[i].drawn;
		if (all)
			break;
		roundtrip(&g);
	}
	printf("%d windows open\n", n);
	fflush(stdout);

	for (int i = 0; i < hold * 10; i++) {
		roundtrip(&g);
		usleep(100000);
	}

	for (int i = n - 1; i >= 0; i--) {
		xdg_toplevel_destroy(wins[i].top);
		xdg_surface_destroy(wins[i].xs);
		wl_surface_destroy(wins[i].surf);
		roundtrip(&g);
		usleep(100000);
	}
	puts("windows closed");
	return 0;
}
