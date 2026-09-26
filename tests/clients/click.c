/* click: press and release a mouse button (left, right, middle, back, forward) through a virtual pointer */
#include "common.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <string.h>

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
	if (!button)
		fail("usage: click left|right|middle|back|forward");

	struct globals g;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!mgr || !seat)
		fail("aro does not offer virtual pointers");

	struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	roundtrip(&g);
	zwlr_virtual_pointer_v1_button(p, 1, button, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(p);
	roundtrip(&g);
	zwlr_virtual_pointer_v1_button(p, 2, button, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(p);
	roundtrip(&g);
	zwlr_virtual_pointer_v1_destroy(p);
	roundtrip(&g);
	return 0;
}
