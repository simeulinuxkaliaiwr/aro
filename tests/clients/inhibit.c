/* inhibit: a window that asks for aro's shortcuts, printing each time it gets or loses them */
#define _DEFAULT_SOURCE /* usleep */

#include "common.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct globals g;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *mgr;
static struct wl_seat *seat;
static struct wl_surface *surf;
static int drawn;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	if (!drawn) {
		wl_surface_attach(surf, solid_buffer(&g, 400, 300, 0xff203040), 0, 0);
		wl_surface_commit(surf);
		drawn = 1;
	}
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

static void on_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i)
{
	(void)d; (void)i;
	puts("active");
	fflush(stdout);
}

static void on_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i)
{
	(void)d; (void)i;
	puts("inactive");
	fflush(stdout);
}

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inh_listener = {
	on_active, on_inactive,
};

int main(int argc, char **argv)
{
	int hold = argc > 1 ? atoi(argv[1]) : 5;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!mgr || !seat)
		fail("aro does not offer keyboard-shortcuts-inhibit");

	surf = wl_compositor_create_surface(g.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g.wm, surf);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_set_app_id(top, "aro-test-inhibit");
	wl_surface_commit(surf);
	for (int i = 0; i < 20 && !drawn; i++)
		roundtrip(&g);

	struct zwp_keyboard_shortcuts_inhibitor_v1 *inh =
		zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(mgr, surf, seat);
	zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inh, &inh_listener, NULL);

	for (int i = 0; i < hold * 10; i++) {
		roundtrip(&g);
		usleep(100000);
	}
	return 0;
}
