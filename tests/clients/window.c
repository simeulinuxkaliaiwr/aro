/* window: opens N windows, keeps them for HOLD seconds, closes them; "modal" makes the first a modal dialog, "tear" a fullscreen game, "icon" gives it a red icon, "iconname NAME" a theme icon, "sub" draws it grey with a red subsurface */
#define _DEFAULT_SOURCE /* usleep */

#include "common.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static struct xdg_wm_dialog_v1 *dialogs;
static struct wp_tearing_control_manager_v1 *tearing;
static struct xdg_toplevel_icon_manager_v1 *icons;
static struct wl_subcompositor *subs;
static int sub;
static struct wl_surface *sub_surf;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, xdg_wm_dialog_v1_interface.name))
		dialogs = wl_registry_bind(r, name, &xdg_wm_dialog_v1_interface, 1);
	else if (!strcmp(iface, wp_tearing_control_manager_v1_interface.name))
		tearing = wl_registry_bind(r, name, &wp_tearing_control_manager_v1_interface, 1);
	else if (!strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name))
		icons = wl_registry_bind(r, name, &xdg_toplevel_icon_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_subcompositor_interface.name))
		subs = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	struct win *w = data;
	xdg_surface_ack_configure(xs, serial);
	int bw = w->w > 0 ? w->w : 400, bh = w->h > 0 ? w->h : 300;
	/* like Firefox: a plain main surface, the content in a subsurface */
	if (sub && w == &wins[0]) {
		if (!sub_surf) {
			sub_surf = wl_compositor_create_surface(g.compositor);
			struct wl_subsurface *ss = wl_subcompositor_get_subsurface(subs, sub_surf, w->surf);
			wl_subsurface_set_position(ss, 20, 20);
		}
		wl_surface_attach(sub_surf, solid_buffer(&g, bw - 40, bh - 40, 0xffff0000), 0, 0);
		wl_surface_commit(sub_surf);
	}
	wl_surface_attach(w->surf, solid_buffer(&g, bw, bh, sub && w == &wins[0] ? 0xff808080 : 0xff203040), 0, 0);
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
	int modal = argc > 3 && !strcmp(argv[3], "modal");
	int tear = argc > 3 && !strcmp(argv[3], "tear");
	int icon = argc > 3 && !strcmp(argv[3], "icon");
	const char *icon_name = argc > 4 && !strcmp(argv[3], "iconname") ? argv[4] : NULL;
	icon |= icon_name != NULL;
	sub = argc > 3 && !strcmp(argv[3], "sub");
	if (n < 1 || n > MAX_WINDOWS)
		fail("usage: window N HOLD [modal], N from 1 to %d", MAX_WINDOWS);
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (modal && !dialogs)
		fail("aro does not offer xdg-dialog");
	if (tear && !tearing)
		fail("aro does not offer tearing-control");
	if (icon && !icons)
		fail("aro does not offer xdg-toplevel-icon");
	if (sub && !subs)
		fail("aro does not offer wl_subcompositor");

	for (int i = 0; i < n; i++) {
		struct win *w = &wins[i];
		w->surf = wl_compositor_create_surface(g.compositor);
		w->xs = xdg_wm_base_get_xdg_surface(g.wm, w->surf);
		xdg_surface_add_listener(w->xs, &xs_listener, w);
		w->top = xdg_surface_get_toplevel(w->xs);
		xdg_toplevel_add_listener(w->top, &top_listener, w);
		xdg_toplevel_set_app_id(w->top, "aro-test");
		if (modal && i == 0)
			xdg_dialog_v1_set_modal(xdg_wm_dialog_v1_get_xdg_dialog(dialogs, w->top));
		if (icon && i == 0) {
			struct xdg_toplevel_icon_v1 *ic = xdg_toplevel_icon_manager_v1_create_icon(icons);
			if (icon_name)
				xdg_toplevel_icon_v1_set_name(ic, icon_name);
			else
				xdg_toplevel_icon_v1_add_buffer(ic, solid_buffer(&g, 32, 32, 0xffff0000), 1);
			xdg_toplevel_icon_manager_v1_set_icon(icons, w->top, ic);
			xdg_toplevel_icon_v1_destroy(ic);
		}
		if (tear && i == 0) {
			xdg_toplevel_set_fullscreen(w->top, NULL);
			wp_tearing_control_v1_set_presentation_hint(
				wp_tearing_control_manager_v1_get_tearing_control(tearing, w->surf),
				WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
		}
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
		/* a game draws all the time: new frames keep the tearing path busy */
		if (tear && wins[0].drawn) {
			wl_surface_attach(wins[0].surf, solid_buffer(&g, wins[0].w > 0 ? wins[0].w : 400,
				wins[0].h > 0 ? wins[0].h : 300, i & 1 ? 0xff400000 : 0xff004000), 0, 0);
			wl_surface_damage_buffer(wins[0].surf, 0, 0, INT32_MAX, INT32_MAX);
			wl_surface_commit(wins[0].surf);
		}
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
