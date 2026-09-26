/* layer-popup: a bar on the left edge with a red tooltip, held so it can be screenshotted */
#define _DEFAULT_SOURCE /* usleep */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static struct globals g;
static struct wl_surface *bar, *tip;
static int bar_h, drawn;

static void bar_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                          uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	bar_h = h ? (int)h : 720;
	wl_surface_attach(bar, solid_buffer(&g, w ? (int)w : 30, bar_h, 0xff404040), 0, 0);
	wl_surface_commit(bar);
}

static void bar_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data; (void)ls;
	fail("aro closed the bar");
}

static const struct zwlr_layer_surface_v1_listener bar_listener = { bar_configure, bar_closed };

static void tip_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(tip, solid_buffer(&g, 200, 120, 0xffff0000), 0, 0);
	wl_surface_commit(tip);
	drawn = 1;
}

static const struct xdg_surface_listener tip_listener = { tip_configure };

static void pop_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)d; (void)p;
	printf("tooltip placed at %d,%d, %dx%d\n", x, y, w, h);
}

static void pop_done(void *d, struct xdg_popup *p)
{
	(void)d; (void)p;
	fail("aro dismissed the tooltip");
}

static void pop_repositioned(void *d, struct xdg_popup *p, uint32_t token)
{
	(void)d; (void)p; (void)token;
}

static const struct xdg_popup_listener pop_listener = { pop_configure, pop_done, pop_repositioned };

static struct xdg_popup *tooltip(struct zwlr_layer_surface_v1 *ls, struct wl_surface *surf,
                                 struct xdg_surface **xs_out)
{
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g.wm, surf);
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(g.wm);
	xdg_positioner_set_size(pos, 200, 120);
	xdg_positioner_set_anchor_rect(pos, 0, bar_h - 80, 30, 40);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_RIGHT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_RIGHT);
	struct xdg_popup *p = xdg_surface_get_popup(xs, NULL, pos);
	zwlr_layer_surface_v1_get_popup(ls, p);
	xdg_positioner_destroy(pos);
	*xs_out = xs;
	return p;
}

int main(int argc, char **argv)
{
	int hold = argc > 1 ? atoi(argv[1]) : 3;
	connect_globals(&g);

	bar = wl_compositor_create_surface(g.compositor);
	struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(
		g.layer_shell, bar, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "test-bar");
	zwlr_layer_surface_v1_add_listener(ls, &bar_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
	zwlr_layer_surface_v1_set_size(ls, 30, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, 30);
	wl_surface_commit(bar);
	roundtrip(&g);
	roundtrip(&g);

	/* a few tooltips that come and go, like hovering along a bar */
	for (int i = 0; i < 3; i++) {
		struct wl_surface *s = wl_compositor_create_surface(g.compositor);
		struct xdg_surface *xs;
		struct xdg_popup *p = tooltip(ls, s, &xs);
		wl_surface_commit(s);
		roundtrip(&g);
		xdg_popup_destroy(p);
		xdg_surface_destroy(xs);
		wl_surface_destroy(s);
		roundtrip(&g);
	}

	/* the one that stays, in red, for the screenshot */
	tip = wl_compositor_create_surface(g.compositor);
	struct xdg_surface *xs;
	struct xdg_popup *p = tooltip(ls, tip, &xs);
	xdg_surface_add_listener(xs, &tip_listener, NULL);
	xdg_popup_add_listener(p, &pop_listener, NULL);
	wl_surface_commit(tip);
	for (int i = 0; i < 20 && !drawn; i++)
		roundtrip(&g);
	if (!drawn)
		fail("the tooltip was never configured");
	roundtrip(&g);
	puts("tooltip drawn");
	fflush(stdout);

	for (int i = 0; i < hold * 10; i++) {
		roundtrip(&g);
		usleep(100000);
	}
	return 0;
}
