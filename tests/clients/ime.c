/* ime: a text field and an input method in one client; the input method
 * takes and releases the keyboard grab, then commits text into the field.
 * Both used to crash aro: wlroots sends those two signals with NULL data. */

#define _DEFAULT_SOURCE /* usleep */

#include "common.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEXT "xin chào"

static struct globals g;
static struct wl_seat *seat;
static struct zwp_text_input_manager_v3 *ti_mgr;
static struct zwp_input_method_manager_v2 *im_mgr;
static struct wl_surface *surf;
static struct zwp_text_input_v3 *ti;
static int drawn;
static int im_active;          /* activated and done */
static int im_pending_active;
static uint32_t im_serial;     /* done events so far */
static char got[64];           /* what reached the field */
static int got_done;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name))
		ti_mgr = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
	else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
		im_mgr = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	if (!drawn) {
		wl_surface_attach(surf, solid_buffer(&g, 400, 300, 0xff304020), 0, 0);
		wl_surface_commit(surf);
		drawn = 1;
	}
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

/* ── the text field ─────────────────────────────────────────────────────── */

static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
	(void)d; (void)s;
	zwp_text_input_v3_enable(t);
	zwp_text_input_v3_commit(t);
}

static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)d; (void)t; (void)s; }

static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *text, int32_t b, int32_t e)
{
	(void)d; (void)t; (void)text; (void)b; (void)e;
}

static void ti_commit_string(void *d, struct zwp_text_input_v3 *t, const char *text)
{
	(void)d; (void)t;
	snprintf(got, sizeof got, "%s", text ? text : "");
}

static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t b, uint32_t a) { (void)d; (void)t; (void)b; (void)a; }

static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial)
{
	(void)d; (void)t; (void)serial;
	if (got[0])
		got_done = 1;
}

/* named: newer wayland-protocols add version 2 events, never sent to this version 1 binding */
static const struct zwp_text_input_v3_listener ti_listener = {
	.enter = ti_enter,
	.leave = ti_leave,
	.preedit_string = ti_preedit,
	.commit_string = ti_commit_string,
	.delete_surrounding_text = ti_delete,
	.done = ti_done,
};

/* ── the input method ───────────────────────────────────────────────────── */

static void im_activate(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; im_pending_active = 1; }
static void im_deactivate(void *d, struct zwp_input_method_v2 *im) { (void)d; (void)im; im_pending_active = 0; }

static void im_surrounding(void *d, struct zwp_input_method_v2 *im, const char *t, uint32_t c, uint32_t a)
{
	(void)d; (void)im; (void)t; (void)c; (void)a;
}

static void im_cause(void *d, struct zwp_input_method_v2 *im, uint32_t c) { (void)d; (void)im; (void)c; }
static void im_content(void *d, struct zwp_input_method_v2 *im, uint32_t h, uint32_t p) { (void)d; (void)im; (void)h; (void)p; }

static void im_done(void *d, struct zwp_input_method_v2 *im)
{
	(void)d; (void)im;
	im_serial++;
	im_active = im_pending_active;
}

static void im_unavailable(void *d, struct zwp_input_method_v2 *im)
{
	(void)d; (void)im;
	fail("aro says another input method is running");
}

static const struct zwp_input_method_v2_listener im_listener = {
	im_activate, im_deactivate, im_surrounding, im_cause, im_content, im_done, im_unavailable,
};

int main(void)
{
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!seat || !ti_mgr || !im_mgr)
		fail("aro does not offer text-input-v3 and input-method-v2");

	struct zwp_input_method_v2 *im = zwp_input_method_manager_v2_get_input_method(im_mgr, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);

	surf = wl_compositor_create_surface(g.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g.wm, surf);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_set_app_id(top, "aro-test-ime");
	ti = zwp_text_input_manager_v3_get_text_input(ti_mgr, seat);
	zwp_text_input_v3_add_listener(ti, &ti_listener, NULL);
	wl_surface_commit(surf);

	/* the window has to map and take focus first, which is slow under ASan */
	for (int i = 0; i < 150 && !im_active; i++) {
		roundtrip(&g);
		if (!im_active)
			usleep(20000);
	}
	if (!im_active)
		fail("the input method was never activated");
	puts("activated");

	/* what fcitx5 does when a key is accepted: it lets go of the keyboard,
	 * and aro crashed on the grab's destroy */
	struct zwp_input_method_keyboard_grab_v2 *grab = zwp_input_method_v2_grab_keyboard(im);
	roundtrip(&g);
	zwp_input_method_keyboard_grab_v2_release(grab);
	roundtrip(&g);
	roundtrip(&g);
	puts("grab released");

	/* and when you type: aro crashed on this commit too */
	zwp_input_method_v2_commit_string(im, TEXT);
	zwp_input_method_v2_commit(im, im_serial);
	for (int i = 0; i < 50 && !got_done; i++) {
		roundtrip(&g);
		if (!got_done)
			usleep(20000);
	}
	if (strcmp(got, TEXT))
		fail("the field got \"%s\", wanted \"" TEXT "\"", got);
	printf("committed %s\n", got);

	zwp_input_method_v2_destroy(im);
	zwp_text_input_v3_destroy(ti);
	roundtrip(&g);
	return 0;
}
