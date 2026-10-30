/* dnd: open a window, press on it with a virtual pointer and start a drag from it, hold Shift, then Shift+Alt, through a virtual keyboard, let go of the keys and drop; "dnd MIME" offers MIME instead of text/uri-list */
#define _GNU_SOURCE /* memfd_create, usleep */
#include "common.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
static struct wl_data_device_manager *ddm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static uint32_t press_serial;
static int configured;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		vp_mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		vk_mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_data_device_manager_interface.name))
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
	else if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

/* ── the pointer: only the press serial matters ── */

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                    wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)serial; (void)s; (void)x; (void)y; }
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)p; (void)serial; (void)s; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; (void)x; (void)y; }
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state)
{
	(void)d; (void)p; (void)t; (void)b;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		press_serial = serial;
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = { seat_caps, NULL };

/* ── the dragged data: nobody reads it here ── */

static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *m, int32_t fd)
{
	(void)d; (void)s; (void)m;
	close(fd);
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_dropped(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }

static const struct wl_data_source_listener source_listener = {
	src_target, src_send, src_cancelled, src_dropped, src_finished, src_action,
};

/* ── the window ── */

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	configured = 1;
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

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

/* let aro report what changed before the next step */
static void settle(struct globals *g)
{
	roundtrip(g);
	usleep(150 * 1000);
	roundtrip(g);
}

int main(int argc, char **argv)
{
	const char *mime = argc > 1 ? argv[1] : "text/uri-list";
	/* the us keymap's modifier indices: Shift 0, Control 2, Mod1 (Alt) 3 */
	const uint32_t SHIFT = 1u << 0, ALT = 1u << 3;

	struct globals g;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!vp_mgr || !vk_mgr || !ddm || !seat)
		fail("aro does not offer virtual input or a data device");
	wl_seat_add_listener(seat, &seat_listener, NULL);
	roundtrip(&g);

	struct wl_surface *surf = wl_compositor_create_surface(g.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g.wm, surf);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_set_app_id(top, "aro-test-dnd");
	wl_surface_commit(surf);
	while (!configured)
		roundtrip(&g);
	wl_surface_attach(surf, solid_buffer(&g, 1280, 720, 0xff336699), 0, 0);
	wl_surface_commit(surf);
	settle(&g);

	struct wl_data_device *dev = wl_data_device_manager_get_data_device(ddm, seat);
	struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	struct zwp_virtual_keyboard_v1 *vk = new_vk();
	roundtrip(&g);

	/* from somewhere else, so the window surely gets an enter */
	zwlr_virtual_pointer_v1_motion_absolute(p, 1, 100, 100, 1280, 720);
	zwlr_virtual_pointer_v1_frame(p);
	settle(&g);
	zwlr_virtual_pointer_v1_motion_absolute(p, 1, 640, 360, 1280, 720);
	zwlr_virtual_pointer_v1_frame(p);
	zwlr_virtual_pointer_v1_button(p, 2, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(p);
	settle(&g);
	if (!press_serial)
		fail("the window never heard the press");

	struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(src, &source_listener, NULL);
	wl_data_source_offer(src, mime);
	wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	wl_data_device_start_drag(dev, src, surf, NULL, press_serial);
	settle(&g);

	zwp_virtual_keyboard_v1_modifiers(vk, SHIFT, 0, 0, 0);
	settle(&g);
	zwp_virtual_keyboard_v1_modifiers(vk, SHIFT | ALT, 0, 0, 0);
	settle(&g);
	zwlr_virtual_pointer_v1_motion_absolute(p, 3, 700, 400, 1280, 720);
	zwlr_virtual_pointer_v1_frame(p);
	zwp_virtual_keyboard_v1_modifiers(vk, 0, 0, 0, 0);
	settle(&g);

	zwlr_virtual_pointer_v1_button(p, 4, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(p);
	settle(&g);
	printf("dropped\n");
	fflush(stdout);
	return 0;
}
