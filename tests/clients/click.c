/* click: press and release a mouse button (left, right, middle, back, forward) through a virtual pointer; "click BUTTON X Y [TOX TOY]" goes to X,Y first and drags to TOX,TOY; "click up|down X Y" turns the wheel there (layout 1280x720) */
#define _DEFAULT_SOURCE /* usleep */
#include "common.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat *seat;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

int main(int argc, char **argv)
{
	static const struct { const char *name; uint32_t code; } buttons[] = {
		{ "left", BTN_LEFT }, { "right", BTN_RIGHT }, { "middle", BTN_MIDDLE },
		{ "back", BTN_SIDE }, { "forward", BTN_EXTRA },
	};
	uint32_t button = 0;
	for (size_t i = 0; argc > 1 && i < sizeof buttons / sizeof buttons[0]; i++)
		if (!strcmp(argv[1], buttons[i].name))
			button = buttons[i].code;
	const int wheel = argc > 1 ? (!strcmp(argv[1], "down") ? 1 : !strcmp(argv[1], "up") ? -1 : 0) : 0;
	if (!button && !wheel)
		fail("usage: click left|right|middle|back|forward|up|down [X Y [TOX TOY]]");
	const int at = argc >= 4, drag = argc >= 6;

	struct globals g;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!mgr || !seat)
		fail("aro does not offer virtual pointers");

	struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	roundtrip(&g);
	if (at) {
		zwlr_virtual_pointer_v1_motion_absolute(p, 1, atoi(argv[2]), atoi(argv[3]), 1280, 720);
		zwlr_virtual_pointer_v1_frame(p);
		roundtrip(&g);
	}
	if (wheel) {
		zwlr_virtual_pointer_v1_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
		zwlr_virtual_pointer_v1_axis_discrete(p, 2, WL_POINTER_AXIS_VERTICAL_SCROLL,
		                                      wl_fixed_from_int(15 * wheel), wheel);
		zwlr_virtual_pointer_v1_frame(p);
		roundtrip(&g);
		zwlr_virtual_pointer_v1_destroy(p);
		roundtrip(&g);
		return 0;
	}
	zwlr_virtual_pointer_v1_button(p, 1, button, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(p);
	roundtrip(&g);
	/* a drag in small steps, as a hand would */
	for (int i = 1; drag && i <= 20; i++) {
		int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]);
		zwlr_virtual_pointer_v1_motion_absolute(p, 10 + i, x0 + (x1 - x0) * i / 20,
		                                        y0 + (y1 - y0) * i / 20, 1280, 720);
		zwlr_virtual_pointer_v1_frame(p);
		roundtrip(&g);
		usleep(10000);
	}
	zwlr_virtual_pointer_v1_button(p, 2, button, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(p);
	roundtrip(&g);
	zwlr_virtual_pointer_v1_destroy(p);
	roundtrip(&g);
	return 0;
}
