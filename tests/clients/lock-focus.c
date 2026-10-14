/*
 * lock-focus: the keyboard goes where the user expects around the lock
 * screen. A window mapped after the last-used keyboard went away still gets
 * the keys; a layer surface that asks for the keyboard while locked does not
 * take it from the lock; unlocking gives it back to the focused window.
 */
#define _GNU_SOURCE /* memfd_create */
#include "common.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static struct wl_seat *seat;
static struct wl_output *output;
static struct ext_session_lock_manager_v1 *lock_mgr;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;

static struct wl_keyboard *keyboard;
static struct wl_surface *focus;        /* what has the keyboard, as we see it */
static bool locked;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name))
		lock_mgr = wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		vk_mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
	(void)d; (void)k; (void)fmt; (void)size;
	close(fd);
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
	(void)d; (void)k; (void)serial; (void)keys;
	focus = s;
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)k; (void)serial;
	if (focus == s)
		focus = NULL;
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
	(void)d; (void)k; (void)serial; (void)t; (void)key; (void)state;
}

static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g)
{
	(void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g;
}

static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)d; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener kb_listener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = { seat_caps, NULL };

/* a virtual keyboard with a plain us keymap */
static struct zwp_virtual_keyboard_v1 *new_vk(void)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap *map = ctx ? xkb_keymap_new_from_names(ctx, &names, 0) : NULL;
	char *text = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	if (!text)
		fail("cannot build a keymap");
	size_t size = strlen(text) + 1;
	int fd = memfd_create("aro-test-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, text, size) != (ssize_t)size)
		fail("cannot share the keymap");
	free(text);
	xkb_keymap_unref(map);
	xkb_context_unref(ctx);

	struct zwp_virtual_keyboard_v1 *vk =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr, seat);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	return vk;
}

static void tap(struct globals *g, struct zwp_virtual_keyboard_v1 *vk)
{
	zwp_virtual_keyboard_v1_key(vk, 1, KEY_F13, WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(vk, 2, KEY_F13, WL_KEYBOARD_KEY_STATE_RELEASED);
	roundtrip(g);
}

/* ── windows ── */

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

static struct wl_surface *window(struct globals *g)
{
	struct wl_surface *s = wl_compositor_create_surface(g->compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g->wm, s);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	xdg_toplevel_set_app_id(xdg_surface_get_toplevel(xs), "lock-focus");
	wl_surface_commit(s);
	roundtrip(g);
	wl_surface_attach(s, solid_buffer(g, 200, 200, 0xff336699), 0, 0);
	wl_surface_commit(s);
	roundtrip(g);
	roundtrip(g);
	return s;
}

/* ── lock ── */

static void lk_locked(void *d, struct ext_session_lock_v1 *l) { (void)d; (void)l; locked = true; }
static void lk_finished(void *d, struct ext_session_lock_v1 *l) { (void)d; (void)l; fail("lock refused"); }

static const struct ext_session_lock_v1_listener lk_listener = { lk_locked, lk_finished };

static struct globals *G;

static void ls_configure(void *d, struct ext_session_lock_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	struct wl_surface *s = d;
	ext_session_lock_surface_v1_ack_configure(ls, serial);
	wl_surface_attach(s, solid_buffer(G, w, h, 0xff000000), 0, 0);
	wl_surface_commit(s);
}

static const struct ext_session_lock_surface_v1_listener ls_listener = { ls_configure };

/* ── layer ── */

static void lay_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	struct wl_surface *s = d;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	wl_surface_attach(s, solid_buffer(G, w ? w : 100, h ? h : 100, 0xffcc3333), 0, 0);
	wl_surface_commit(s);
}

static void lay_closed(void *d, struct zwlr_layer_surface_v1 *ls) { (void)d; (void)ls; }

static const struct zwlr_layer_surface_v1_listener lay_listener = { lay_configure, lay_closed };

static int failed;

static void expect(const char *what, struct wl_surface *want)
{
	if (focus == want) {
		printf("ok %s\n", what);
	} else {
		printf("FAIL %s\n", what);
		failed = 1;
	}
}

int main(void)
{
	struct globals g;
	G = &g;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!seat || !output || !lock_mgr || !vk_mgr)
		fail("aro lacks a seat, output, session lock or virtual keyboards");
	wl_seat_add_listener(seat, &seat_listener, NULL);

	struct zwp_virtual_keyboard_v1 *vk = new_vk();
	roundtrip(&g);
	roundtrip(&g);
	if (!keyboard)
		fail("the seat never got a keyboard");

	struct wl_surface *a = window(&g);
	expect("a new window gets the keyboard", a);

	/* wtype does this: a keyboard comes, types, and goes */
	struct zwp_virtual_keyboard_v1 *gone = new_vk();
	roundtrip(&g);
	tap(&g, gone);
	zwp_virtual_keyboard_v1_destroy(gone);
	roundtrip(&g);
	struct wl_surface *b = window(&g);
	expect("a window mapped after the typing keyboard left gets the keyboard", b);

	tap(&g, vk);
	struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(lock_mgr);
	ext_session_lock_v1_add_listener(lock, &lk_listener, NULL);
	roundtrip(&g);
	if (!locked)
		fail("never locked");
	struct wl_surface *ls = wl_compositor_create_surface(g.compositor);
	struct ext_session_lock_surface_v1 *lss =
		ext_session_lock_v1_get_lock_surface(lock, ls, output);
	ext_session_lock_surface_v1_add_listener(lss, &ls_listener, ls);
	roundtrip(&g);
	roundtrip(&g);
	expect("the lock surface gets the keyboard", ls);

	/* a shell's launcher or panel asking for the keyboard behind the lock */
	struct wl_surface *ps = wl_compositor_create_surface(g.compositor);
	struct zwlr_layer_surface_v1 *lay = zwlr_layer_shell_v1_get_layer_surface(
		g.layer_shell, ps, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lock-focus");
	zwlr_layer_surface_v1_add_listener(lay, &lay_listener, ps);
	zwlr_layer_surface_v1_set_size(lay, 100, 100);
	zwlr_layer_surface_v1_set_keyboard_interactivity(lay,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(ps);
	roundtrip(&g);
	roundtrip(&g);
	expect("a layer surface mapped while locked leaves the lock the keyboard", ls);
	zwlr_layer_surface_v1_destroy(lay);
	wl_surface_destroy(ps);
	roundtrip(&g);
	expect("that layer surface going away leaves the lock the keyboard", ls);

	ext_session_lock_v1_unlock_and_destroy(lock);
	ext_session_lock_surface_v1_destroy(lss);
	wl_surface_destroy(ls);
	roundtrip(&g);
	roundtrip(&g);
	expect("unlocking gives the keyboard back to the focused window", b);

	/* the same with nothing else happening behind the lock */
	locked = false;
	lock = ext_session_lock_manager_v1_lock(lock_mgr);
	ext_session_lock_v1_add_listener(lock, &lk_listener, NULL);
	roundtrip(&g);
	if (!locked)
		fail("never locked again");
	ls = wl_compositor_create_surface(g.compositor);
	lss = ext_session_lock_v1_get_lock_surface(lock, ls, output);
	ext_session_lock_surface_v1_add_listener(lss, &ls_listener, ls);
	roundtrip(&g);
	roundtrip(&g);
	expect("a second lock gets the keyboard", ls);
	ext_session_lock_v1_unlock_and_destroy(lock);
	ext_session_lock_surface_v1_destroy(lss);
	wl_surface_destroy(ls);
	roundtrip(&g);
	roundtrip(&g);
	expect("a plain unlock gives the keyboard back too", b);

	return failed;
}
