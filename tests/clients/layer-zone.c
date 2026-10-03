/* layer-zone: a bar along the bottom, then a red gadget anchored to the bottom of the
 * layer under it, which must sit on top of the bar rather than behind it */
#define _DEFAULT_SOURCE /* usleep */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static struct globals g;
static struct wl_surface *bar, *gadget;
static int bar_up, gadget_up;

static void bar_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                          uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	wl_surface_attach(bar, solid_buffer(&g, w ? (int)w : 1280, h ? (int)h : 40, 0xff404040), 0, 0);
	wl_surface_commit(bar);
	bar_up = 1;
}

static void gadget_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                             uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	wl_surface_attach(gadget, solid_buffer(&g, w ? (int)w : 40, h ? (int)h : 40, 0xffff0000), 0, 0);
	wl_surface_commit(gadget);
	gadget_up = 1;
}

static void closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data; (void)ls;
	fail("aro closed a layer surface");
}

static const struct zwlr_layer_surface_v1_listener bar_listener = { bar_configure, closed };
static const struct zwlr_layer_surface_v1_listener gadget_listener = { gadget_configure, closed };

int main(int argc, char **argv)
{
	int hold = argc > 1 ? atoi(argv[1]) : 3;
	connect_globals(&g);

	bar = wl_compositor_create_surface(g.compositor);
	struct zwlr_layer_surface_v1 *bl = zwlr_layer_shell_v1_get_layer_surface(
		g.layer_shell, bar, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "test-taskbar");
	zwlr_layer_surface_v1_add_listener(bl, &bar_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(bl, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(bl, 0, 40);
	zwlr_layer_surface_v1_set_exclusive_zone(bl, 40);
	wl_surface_commit(bar);
	for (int i = 0; i < 20 && !bar_up; i++)
		roundtrip(&g);

	/* made after the bar, as desktop gadgets usually are */
	gadget = wl_compositor_create_surface(g.compositor);
	struct zwlr_layer_surface_v1 *gl = zwlr_layer_shell_v1_get_layer_surface(
		g.layer_shell, gadget, NULL, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "test-gadget");
	zwlr_layer_surface_v1_add_listener(gl, &gadget_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(gl, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_size(gl, 40, 40);
	wl_surface_commit(gadget);
	for (int i = 0; i < 20 && !gadget_up; i++)
		roundtrip(&g);
	if (!bar_up || !gadget_up)
		fail("the bar or the gadget was never configured");
	roundtrip(&g);
	puts("both drawn");
	fflush(stdout);

	for (int i = 0; i < hold * 10; i++) {
		roundtrip(&g);
		usleep(100000);
	}
	return 0;
}
