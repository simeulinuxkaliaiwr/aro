/* input.c: keyboards, pointers and the cursor */

/* scene.h must come first */
#include "scene.h"

#include "config.h"
#include "aro.h"
#include "core.h"
#include "idle.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#if WLR_HAS_LIBINPUT_BACKEND
#include <libinput.h>
#include <wlr/backend/libinput.h>
#endif
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/region.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* ── input ─────────────────────────────────────────────────────────────── */

/* VT switching */
static bool handle_vt(struct aro_server *s, xkb_keysym_t sym)
{
	if (!s->session)
		return false;
	if (sym < XKB_KEY_XF86Switch_VT_1 || sym > XKB_KEY_XF86Switch_VT_12)
		return false;
	wlr_session_change_vt(s->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
	return true;
}

/* key bindings */
/* warp cursor on keyboard focus */
void cursor_warp_to_view(struct aro_server *s, struct aro_view *v)
{
	if (!v)
		return;

	ly_box b = view_target(v);
	if (b.w <= 0 || b.h <= 0)
		return;

	wlr_cursor_warp_closest(s->cursor, NULL,
	                        b.x + b.w / 2.0, b.y + b.h / 2.0);
	/* our own move, not the user's: what is drawn there may still be sliding away */
	s->warping = true;
	pointer_motion_common(s, aro_now_ms());
	s->warping = false;
}

static void keyboard_key(struct wl_listener *l, void *data)
{
	struct aro_keyboard *kb = wl_container_of(l, kb, key);
	struct aro_server *s = kb->server;
	struct wlr_keyboard_key_event *ev = data;

	idle_activity(s);

	uint32_t keycode = ev->keycode + 8;
	struct xkb_state *state = kb->wlr_keyboard->xkb_state;
	struct xkb_keymap *keymap = xkb_state_get_keymap(state);

	/* use level-0 syms for bindings */
	const xkb_keysym_t *trans, *raw;
	int ntrans = xkb_state_key_get_syms(state, keycode, &trans);
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
	int nraw = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &raw);

	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	bool handled = false;
	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (int i = 0; i < ntrans; i++)
			handled |= handle_vt(s, trans[i]);

	/* prompt consumes keys */
	if (!handled && !aro_locked(s) && prompt_active(s)) {
		if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
			for (int i = 0; i < nraw && prompt_active(s); i++)
				prompt_key(s, raw[i]);
		prompt_after(s, true);
		return;
	}

	/* overview owns key presses; releases still reach the client */
	if (!handled && !aro_locked(s) && overview_active(s) &&
	    ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nraw && overview_active(s); i++)
			overview_key(s, mods, raw[i]);
		return;
	}

	/*
	 * The switcher owns key presses while it is open, so mod+q on the way
	 * to a window cannot close another. Releases still reach the client:
	 * it saw mod go down before the switcher opened.
	 */
	if (!handled && !aro_locked(s) && switcher_active(s) &&
	    ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		bool stepped = false;
		for (int i = 0; i < nraw && !stepped; i++)
			stepped = handle_switch_bind(s, mods, raw[i]);
		for (int i = 0; i < nraw && !stepped && switcher_active(s); i++)
			switcher_key(s, raw[i]);
		return;
	}

	if (!handled && ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (int i = 0; i < nraw; i++)
			handled |= handle_bind(s, mods, raw[i]);

	/* typing into a window makes it the most recent one at once */
	if (!handled && ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		mru_touch(s);

	/* bindings first, then an input method's grab, then the client */
	if (!handled && ime_key(s, kb, ev))
		return;

	if (!handled) {
		wlr_seat_set_keyboard(s->seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(s->seat, ev->time_msec,
		                             ev->keycode, ev->state);
	}
}

static void keyboard_modifiers(struct wl_listener *l, void *data)
{
	struct aro_keyboard *kb = wl_container_of(l, kb, modifiers);
	(void)data;

	/* letting go of mod picks the selected window */
	switcher_modifiers(kb->server, wlr_keyboard_get_modifiers(kb->wlr_keyboard));

	if (ime_modifiers(kb->server, kb))
		return;

	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
	                                   &kb->wlr_keyboard->modifiers);
}

static void keyboard_destroy(struct wl_listener *l, void *data)
{
	struct aro_keyboard *kb = wl_container_of(l, kb, destroy);
	(void)data;
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

/* unset xkb fields use system defaults */
void apply_keymap(struct aro_server *s, struct wlr_keyboard *wlr_kb)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ctx)
		return;

	struct xkb_rule_names names = {
		.rules   = s->cfg.xkb_rules,
		.model   = s->cfg.xkb_model,
		.layout  = s->cfg.xkb_layout,
		.variant = s->cfg.xkb_variant,
		.options = s->cfg.xkb_options,
	};
	struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, &names,
	                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (map) {
		wlr_keyboard_set_keymap(wlr_kb, map);
		xkb_keymap_unref(map);
	} else {
		wlr_log(WLR_ERROR, "could not compile the keymap; keeping the old one");
	}
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);
}

static void seat_update_caps(struct aro_server *s)
{
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

/* virtual keyboards (wtype, remote input) send their own keymap */
static void new_keyboard(struct aro_server *s, struct wlr_input_device *dev,
                         bool is_virtual)
{
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(dev);

	struct aro_keyboard *kb = calloc(1, sizeof *kb);
	if (!kb)
		return;
	kb->server = s;
	kb->wlr_keyboard = wlr_kb;
	kb->is_virtual = is_virtual;

	if (!is_virtual)
		apply_keymap(s, wlr_kb);

	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_key;
	wl_signal_add(&wlr_kb->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&dev->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(s->seat, wlr_kb);
	wl_list_insert(&s->keyboards, &kb->link);
}

/* touchpad settings; tap finger count tells a touchpad from a mouse */
void pointer_configure(struct aro_server *s,
                              struct wlr_input_device *dev)
{
#if WLR_HAS_LIBINPUT_BACKEND
	if (!wlr_input_device_is_libinput(dev))
		return;
	struct libinput_device *li = wlr_libinput_get_device_handle(dev);
	if (!li || libinput_device_config_tap_get_finger_count(li) <= 0)
		return;

	const struct aro_config *c = &s->cfg;
	libinput_device_config_tap_set_enabled(li, c->tp_tap
		? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	if (libinput_device_config_scroll_has_natural_scroll(li))
		libinput_device_config_scroll_set_natural_scroll_enabled(li,
			c->tp_natural_scroll);
	if (libinput_device_config_dwt_is_available(li))
		libinput_device_config_dwt_set_enabled(li, c->tp_dwt
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	if (libinput_device_config_accel_is_available(li))
		libinput_device_config_accel_set_speed(li, c->tp_speed);
#else
	(void)s;
	(void)dev;
#endif
}

static void pointer_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct aro_pointer *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	free(p);
}

static void new_pointer(struct aro_server *s, struct wlr_input_device *dev)
{
	pointer_configure(s, dev);

	struct aro_pointer *p = calloc(1, sizeof *p);
	if (!p)
		return;         /* works, just not re-read on reload */
	p->server = s;
	p->dev = dev;
	p->destroy.notify = pointer_destroy;
	wl_signal_add(&dev->events.destroy, &p->destroy);
	wl_list_insert(&s->pointers, &p->link);
}

void new_input(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;

	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
		new_keyboard(s, dev, false);
	} else if (dev->type == WLR_INPUT_DEVICE_TABLET) {
		tablet_add(s, dev);
	} else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
		wlr_cursor_attach_input_device(s->cursor, dev);
		new_pointer(s, dev);

		/* map absolute pointers to their output */
		struct wlr_pointer *p = wlr_pointer_from_input_device(dev);
		if (p->output_name) {
			struct aro_output *o;
			wl_list_for_each(o, &s->outputs, link) {
				if (o->wlr_output->name &&
				    strcmp(o->wlr_output->name, p->output_name) == 0) {
					wlr_cursor_map_input_to_output(s->cursor, dev,
					                               o->wlr_output);
					break;
				}
			}
		}
	}

	seat_update_caps(s);
}

void new_virtual_keyboard(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	new_keyboard(s, &vk->keyboard.base, true);
	seat_update_caps(s);
}

void new_virtual_pointer(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *ev = data;
	struct wlr_input_device *dev = &ev->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(s->cursor, dev);
	if (ev->suggested_output)
		wlr_cursor_map_input_to_output(s->cursor, dev, ev->suggested_output);
}

/* pointer hit testing */
struct aro_view *view_at(struct aro_server *s, double lx, double ly,
                                   struct wlr_surface **surface,
                                   double *sx, double *sy)
{
	*surface = NULL;

	/* only lock surfaces are clickable while locked */
	if (aro_locked(s) && !s->lock->abandoned) {
		struct wlr_scene_node *node =
			wlr_scene_node_at(&s->lock->tree->node, lx, ly, sx, sy);
		if (node && node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_surface *ss =
				wlr_scene_surface_try_from_buffer(
					wlr_scene_buffer_from_node(node));
			if (ss)
				*surface = ss->surface;
		}
		return NULL;
	}
	if (aro_locked(s))
		return NULL;            /* abandoned: nothing at all is clickable */

	struct wlr_scene_node *node =
		wlr_scene_node_at(&s->scene->tree.node, lx, ly, sx, sy);
	if (!node)
		return NULL;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(buf);
		if (scene_surface)
			*surface = scene_surface->surface;
	}

	/* find owning view */
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data)
		tree = tree->node.parent;
	struct aro_view *v = tree ? tree->node.data : NULL;

	/* a workspace sliding out is drawn, not there: clicking it would
	 * focus a window on a workspace nobody is looking at */
	if (v && !view_visible(v)) {
		*surface = NULL;
		return NULL;
	}
	return v;
}

void pointer_motion_common(struct aro_server *s, uint32_t time)
{
	drag_icon_update(s);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct aro_view *v = view_at(s, s->cursor->x, s->cursor->y,
	                                &surface, &sx, &sy);

	/* focus follows mouse */
	if (s->cfg.focus_follows_mouse && !s->focused_layer && !s->warping) {
		struct aro_output *po = output_at(s, s->cursor->x, s->cursor->y);
		if (po)
			s->focused_output = po;
		if (v && v != s->focused)
			aro_focus(s, v);
	} else {
		(void)v;
	}

	/* keep pointer grab during button hold */
	if (s->seat->pointer_state.button_count > 0 &&
	    s->seat->pointer_state.focused_surface) {
		wlr_seat_pointer_notify_motion(s->seat, time,
		                               s->cursor->x - s->ptr_lx,
		                               s->cursor->y - s->ptr_ly);
		return;
	}

	if (surface) {
		s->ptr_lx = s->cursor->x - sx;
		s->ptr_ly = s->cursor->y - sy;
		wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
	} else {
		wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
		wlr_seat_pointer_clear_focus(s->seat);
	}
}

/* ── pointer constraints ───────────────────────────────────────────────── */
/*
 * Games lock or confine the pointer to their window. A constraint is only
 * active for the surface with keyboard focus, and only binds while the
 * pointer is over that surface. ptr_lx/ptr_ly is the focused surface's
 * origin, valid exactly when it has pointer focus.
 */

/* a released lock puts the cursor where the game last showed it */
static void constraint_warp_to_hint(struct aro_server *s,
                                    struct wlr_pointer_constraint_v1 *c,
                                    bool tell_client)
{
	if (c->type != WLR_POINTER_CONSTRAINT_V1_LOCKED)
		return;
	if (!(c->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT))
		return;
	if (s->seat->pointer_state.focused_surface != c->surface)
		return;

	double sx = c->current.cursor_hint.x, sy = c->current.cursor_hint.y;
	wlr_cursor_warp(s->cursor, NULL, s->ptr_lx + sx, s->ptr_ly + sy);
	if (tell_client)
		wlr_seat_pointer_warp(s->seat, sx, sy);
}

static void constraint_set_active(struct aro_server *s,
                                  struct wlr_pointer_constraint_v1 *c)
{
	struct wlr_pointer_constraint_v1 *old = s->active_constraint;
	if (old == c)
		return;
	s->active_constraint = c;
	if (old) {
		constraint_warp_to_hint(s, old, true);
		wlr_pointer_constraint_v1_send_deactivated(old);
	}
	if (c)
		wlr_pointer_constraint_v1_send_activated(c);
}

/*
 * Called on focus changes, new constraints and every motion event. The
 * lock screen and the exit prompt release any constraint: both need the
 * pointer free.
 */
void constraint_sync(struct aro_server *s)
{
	if (!s->pointer_constraints)
		return;

	struct wlr_pointer_constraint_v1 *c = NULL;
	struct wlr_surface *kf = s->seat->keyboard_state.focused_surface;
	if (kf && !aro_locked(s) && !prompt_active(s))
		c = wlr_pointer_constraints_v1_constraint_for_surface(
			s->pointer_constraints, kf, s->seat);
	constraint_set_active(s, c);
}

static void constraint_destroy(struct wl_listener *l, void *data)
{
	struct aro_constraint *ac = wl_container_of(l, ac, destroy);
	struct aro_server *s = ac->server;
	(void)data;

	/* destroying a lock is how a client releases it. The surface may be
	 * going away too, so move the cursor but send the client nothing. */
	if (s->active_constraint == ac->constraint) {
		constraint_warp_to_hint(s, ac->constraint, false);
		s->active_constraint = NULL;
	}

	wl_list_remove(&ac->destroy.link);
	free(ac);
}

void new_constraint(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_constraint);
	struct wlr_pointer_constraint_v1 *c = data;

	struct aro_constraint *ac = calloc(1, sizeof *ac);
	if (!ac)
		return;
	ac->server = s;
	ac->constraint = c;
	ac->destroy.notify = constraint_destroy;
	wl_signal_add(&c->events.destroy, &ac->destroy);

	constraint_sync(s);
}

/*
 * Clip a motion delta to the active constraint. Returns false when the
 * pointer is locked and must not move at all. Grabs (our own move and
 * resize) are never constrained.
 */
static bool constrain_motion(struct aro_server *s, double *dx, double *dy)
{
	struct wlr_pointer_constraint_v1 *c = s->active_constraint;
	if (!c || s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		return true;
	if (s->seat->pointer_state.focused_surface != c->surface)
		return true;
	if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
		return false;

	double sx = s->cursor->x - s->ptr_lx, sy = s->cursor->y - s->ptr_ly;
	double cx, cy;
	if (wlr_region_confine(&c->region, sx, sy, sx + *dx, sy + *dy, &cx, &cy)) {
		*dx = cx - sx;
		*dy = cy - sy;
	}
	return true;
}

/* raw deltas for games, sent even while the pointer is locked */
static void send_relative_motion(struct aro_server *s, uint32_t time_msec,
                                 double dx, double dy,
                                 double udx, double udy)
{
	if (s->relative_pointer_mgr)
		wlr_relative_pointer_manager_v1_send_relative_motion(
			s->relative_pointer_mgr, s->seat,
			(uint64_t)time_msec * 1000, dx, dy, udx, udy);
}

void cursor_motion(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_motion);
	idle_activity(s);
	struct wlr_pointer_motion_event *ev = data;

	constraint_sync(s);
	send_relative_motion(s, ev->time_msec, ev->delta_x, ev->delta_y,
	                     ev->unaccel_dx, ev->unaccel_dy);

	double dx = ev->delta_x, dy = ev->delta_y;
	if (!constrain_motion(s, &dx, &dy))
		return;
	wlr_cursor_move(s->cursor, &ev->pointer->base, dx, dy);
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (!aro_locked(s) && overview_active(s)) {
		overview_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_motion(s);
	else
		pointer_motion_common(s, ev->time_msec);
}

void cursor_motion_abs(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_motion_abs);
	idle_activity(s);
	struct wlr_pointer_motion_absolute_event *ev = data;

	/* as a delta, so constraints and relative motion work the same way
	 * for tablets and nested sessions */
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(s->cursor, &ev->pointer->base,
	                                     ev->x, ev->y, &lx, &ly);
	double dx = lx - s->cursor->x, dy = ly - s->cursor->y;

	constraint_sync(s);
	send_relative_motion(s, ev->time_msec, dx, dy, dx, dy);
	if (!constrain_motion(s, &dx, &dy))
		return;
	wlr_cursor_move(s->cursor, &ev->pointer->base, dx, dy);
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (!aro_locked(s) && overview_active(s)) {
		overview_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_motion(s);
	else
		pointer_motion_common(s, ev->time_msec);
}

/* a bound mouse button runs its action; neither press nor release reaches the app */
static bool mouse_bind(struct aro_server *s, struct wlr_pointer_button_event *ev)
{
	uint32_t bit = ev->button >= BTN_MOUSE && ev->button < BTN_MOUSE + 32
	             ? 1u << (ev->button - BTN_MOUSE) : 0;
	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		bool held = s->mouse_binds_held & bit;
		s->mouse_binds_held &= ~bit;
		return held;
	}
	if (!bit || aro_locked(s) || shortcuts_inhibited(s))
		return false;

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
	mods &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->button == ev->button && b->mods == mods) {
			s->mouse_binds_held |= bit;
			run_action(s, b);
			return true;
		}
	}
	return false;
}

void cursor_button(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_button);
	idle_activity(s);
	struct wlr_pointer_button_event *ev = data;

	/* prompt consumes pointer buttons */
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_button(s, s->cursor->x, s->cursor->y,
		                      ev->state == WL_POINTER_BUTTON_STATE_PRESSED);
		prompt_after(s, true);
		return;
	}

	if (!aro_locked(s) && overview_active(s)) {
		bool down = ev->state == WL_POINTER_BUTTON_STATE_PRESSED;
		/* a client that saw the press gets the release */
		if (!down && s->seat->pointer_state.button_count > 0)
			wlr_seat_pointer_notify_button(s->seat, ev->time_msec,
			                               ev->button, ev->state);
		overview_pointer_button(s, s->cursor->x, s->cursor->y, down);
		return;
	}

	/* end grab on release */
	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED &&
	    s->cursor_mode != ARO_CURSOR_PASSTHROUGH) {
		grab_end(s);
		return;
	}

	if (mouse_bind(s, ev))
		return;

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct aro_view *v = view_at(s, s->cursor->x, s->cursor->y,
	                                &surface, &sx, &sy);

	/* click focuses output */
	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct aro_output *po = output_at(s, s->cursor->x, s->cursor->y);
		if (po)
			s->focused_output = po;
	}

	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && v) {
		aro_focus(s, v);

		uint32_t mods = 0;
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
		if (kb)
			mods = wlr_keyboard_get_modifiers(kb);

		bool on_chrome = (surface == NULL);
		ly_box box = view_target(v);
		uint32_t zone = edge_zone(box, s->cursor->x, s->cursor->y,
		                          s->cfg.theme.resize_zone);

		/* start move/resize */
		bool want_resize = (ev->button == BTN_RIGHT && (mods & s->cfg.modkey)) ||
		                   (on_chrome && zone != 0);

		if ((mods & s->cfg.modkey) || on_chrome) {
			uint32_t edges = zone;
			if (want_resize && edges == 0) {
				/* choose nearest edge */
				edges = v->floating
				      ? (WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT)
				      : mask_from_ly_edge(nearest_edge(box, s->cursor->x,
				                                       s->cursor->y));
			}

			grab_begin(s, v,
			           want_resize ? ARO_CURSOR_RESIZE : ARO_CURSOR_MOVE,
			           edges);
			if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
				return;         /* consumed: the client never sees it */
		}
	}

	/* refresh pointer focus before button */
	if (surface) {
		s->ptr_lx = s->cursor->x - sx;
		s->ptr_ly = s->cursor->y - sy;
		wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
	}

	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

/* for tablet.c: a pen used as a mouse goes through the same paths as one */
void aro_pointer_moved(struct aro_server *s, uint32_t time)
{
	idle_activity(s);
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (!aro_locked(s) && overview_active(s)) {
		overview_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_motion(s);
	else
		pointer_motion_common(s, time);
}

void aro_pointer_button(struct aro_server *s, uint32_t time, uint32_t button,
                        bool pressed)
{
	struct wlr_pointer_button_event ev = {
		.time_msec = time,
		.button = button,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
		                 : WL_POINTER_BUTTON_STATE_RELEASED,
	};
	cursor_button(&s->cursor_button, &ev);
}

struct wlr_surface *aro_surface_at(struct aro_server *s, double *sx, double *sy)
{
	struct wlr_surface *surface = NULL;
	view_at(s, s->cursor->x, s->cursor->y, &surface, sx, sy);
	return surface;
}

void aro_focus_at_cursor(struct aro_server *s)
{
	struct wlr_surface *surface;
	double sx, sy;
	struct aro_view *v = view_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
	if (v && v != s->focused)
		aro_focus(s, v);
}

void cursor_axis(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_axis);
	idle_activity(s);
	struct wlr_pointer_axis_event *ev = data;
	if (!aro_locked(s) && prompt_active(s))
		return;                 /* no scrolling the window under the card */
	if (!aro_locked(s) && overview_active(s)) {
		overview_pointer_axis(s, ev->delta, ev->delta_discrete);
		return;
	}
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation,
	                             ev->delta, ev->delta_discrete, ev->source,
	                             ev->relative_direction);
}

void cursor_frame(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_frame);
	(void)data;
	wlr_seat_pointer_notify_frame(s->seat);
}

void request_cursor(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (s->seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_surface(s->cursor, ev->surface,
		                       ev->hotspot_x, ev->hotspot_y);
}

/* cursor-shape-v1: a named cursor instead of a client-drawn surface */
void request_set_shape(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_set_shape);
	const struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;

	if (ev->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER)
		return;
	if (s->seat->pointer_state.focused_client != ev->seat_client)
		return;
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr,
	                       wlr_cursor_shape_v1_name(ev->shape));
}

/*
 * Show a window and focus it, switching its output's workspace if needed.
 * With focus-follows-mouse the cursor goes too, or the next twitch of the
 * mouse would hand focus back to whatever it was resting on.
 */
void view_raise_and_focus(struct aro_server *s, struct aro_view *v)
{
	if (!v || !v->mapped || !v->output)
		return;             /* parked: no screen to show it on */

	s->focused_output = v->output;
	if (v->workspace != v->output->cur_ws)
		workspace_show(s, v->workspace);
	aro_focus(s, v);
	if (s->cfg.focus_follows_mouse)
		cursor_warp_to_view(s, v);
}

/*
 * xdg-activation: a window asks for focus, e.g. Firefox after a link is
 * clicked in a terminal. Only tokens born from real input are honoured;
 * one with no seat came from nothing the user did.
 */
void request_activate(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_activate);
	const struct wlr_xdg_activation_v1_request_activate_event *ev = data;

	if (aro_locked(s) || prompt_active(s))
		return;

	struct aro_view *v = NULL, *it;
	wl_list_for_each(it, &s->views, link) {
		if (it->mapped && view_surface(it) == ev->surface) {
			v = it;
			break;
		}
	}
	if (!v)
		return;

	if (!ev->token->seat) {
		wlr_log(WLR_DEBUG, "activation for \"%s\" ignored: no input behind it",
		        view_app_id(v) ? view_app_id(v) : "");
		return;
	}
	view_raise_and_focus(s, v);
}
