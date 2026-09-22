/*
 * ime.c: the input method relay.
 *
 * The flow follows sway's relay (MIT): one input method per seat, text
 * inputs entered and left as keyboard focus moves, and a text input that
 * gains focus before any input method exists is remembered as pending so
 * it can be entered when one connects.
 */
#include "scene.h"

#include "ime.h"
#include "aro.h"

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

struct aro_text_input {
	struct wl_list link;                    /* aro_ime.text_inputs */
	struct aro_ime *ime;
	struct wlr_text_input_v3 *input;

	/* focused before any input method existed */
	struct wlr_surface *pending;

	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
	struct wl_listener pending_destroy;
};

struct aro_ime_popup {
	struct wl_list link;                    /* aro_ime.popups */
	struct aro_ime *ime;
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_tree *tree;
	struct wlr_box last_rect;               /* last rectangle sent */

	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};

struct aro_ime {
	struct aro_server *server;
	struct wlr_text_input_manager_v3 *ti_mgr;
	struct wlr_input_method_manager_v2 *im_mgr;
	struct wlr_input_method_v2 *im;         /* at most one */

	struct wl_list text_inputs;
	struct wl_list popups;

	struct wl_listener new_text_input;
	struct wl_listener new_input_method;
	struct wl_listener im_commit;
	struct wl_listener im_grab_keyboard;
	struct wl_listener im_grab_destroy;
	struct wl_listener im_new_popup;
	struct wl_listener im_destroy;
};

static struct aro_text_input *focused_text_input(struct aro_ime *ime)
{
	struct aro_text_input *ti;
	wl_list_for_each(ti, &ime->text_inputs, link)
		if (ti->input->focused_surface)
			return ti;
	return NULL;
}

/* ── popups (candidate windows) ─────────────────────────────────────────── */

/* where a surface's (0,0) is in layout coordinates */
static bool surface_origin(struct aro_server *s, struct wlr_surface *surface,
                           int *lx, int *ly)
{
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped || view_surface(v) != surface || !v->surface_tree)
			continue;
		if (!wlr_scene_node_coords(&v->surface_tree->node, lx, ly))
			return false;
		/* the tree's origin is the window geometry, not the surface */
		struct wlr_box geo = { 0 };
		view_geometry(v, &geo);
		*lx -= geo.x;
		*ly -= geo.y;
		return true;
	}

	struct aro_layer *l;
	wl_list_for_each(l, &s->layers, link) {
		if (l->layer_surface->surface == surface && l->scene)
			return wlr_scene_node_coords(&l->scene->tree->node, lx, ly);
	}
	return false;
}

/* under the text cursor, flipped above it if there is no room below */
static void popup_place(struct aro_ime_popup *p)
{
	struct aro_server *s = p->ime->server;
	struct aro_text_input *ti = focused_text_input(p->ime);
	struct wlr_surface *ps = p->popup->surface;

	int lx, ly;
	if (!ti || !ps->mapped ||
	    !surface_origin(s, ti->input->focused_surface, &lx, &ly)) {
		wlr_scene_node_set_enabled(&p->tree->node, false);
		return;
	}

	struct wlr_box r = ti->input->current.cursor_rectangle;
	int pw = ps->current.width, ph = ps->current.height;
	int x = lx + r.x;
	int y = ly + r.y + r.height;

	struct aro_output *o = aro_focused_output(s);
	if (o) {
		ly_box b = o->box;
		if (x + pw > b.x + b.w)
			x = b.x + b.w - pw;
		if (x < b.x)
			x = b.x;
		if (y + ph > b.y + b.h && ly + r.y - ph >= b.y)
			y = ly + r.y - ph;
	}

	wlr_scene_node_set_position(&p->tree->node, x, y);
	wlr_scene_node_set_enabled(&p->tree->node, true);

	/* the cursor rectangle relative to the popup; only on change, since
	 * the input method may redraw in response */
	struct wlr_box rel = { lx + r.x - x, ly + r.y - y, r.width, r.height };
	if (!wlr_box_equal(&rel, &p->last_rect)) {
		p->last_rect = rel;
		wlr_input_popup_surface_v2_send_text_input_rectangle(p->popup, &rel);
	}
}

static void popups_place(struct aro_ime *ime)
{
	struct aro_ime_popup *p;
	wl_list_for_each(p, &ime->popups, link)
		popup_place(p);
}

static void popup_commit(struct wl_listener *l, void *data)
{
	struct aro_ime_popup *p = wl_container_of(l, p, commit);
	(void)data;
	popup_place(p);
}

static void popup_map(struct wl_listener *l, void *data)
{
	struct aro_ime_popup *p = wl_container_of(l, p, map);
	(void)data;
	popup_place(p);
}

static void popup_unmap(struct wl_listener *l, void *data)
{
	struct aro_ime_popup *p = wl_container_of(l, p, unmap);
	(void)data;
	wlr_scene_node_set_enabled(&p->tree->node, false);
}

static void popup_destroy(struct wl_listener *l, void *data)
{
	struct aro_ime_popup *p = wl_container_of(l, p, destroy);
	(void)data;
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->map.link);
	wl_list_remove(&p->unmap.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	wlr_scene_node_destroy(&p->tree->node);
	free(p);
}

static void im_new_popup(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, im_new_popup);
	struct wlr_input_popup_surface_v2 *popup = data;

	struct aro_ime_popup *p = calloc(1, sizeof *p);
	if (!p)
		return;
	p->ime = ime;
	p->popup = popup;

	/* above fullscreen windows, below the lock screen */
	p->tree = wlr_scene_tree_create(ime->server->l_notify);
	if (!p->tree || !wlr_scene_subsurface_tree_create(p->tree, popup->surface)) {
		if (p->tree)
			wlr_scene_node_destroy(&p->tree->node);
		free(p);
		return;
	}
	wlr_scene_node_set_enabled(&p->tree->node, false);

	p->commit.notify = popup_commit;
	wl_signal_add(&popup->surface->events.commit, &p->commit);
	p->map.notify = popup_map;
	wl_signal_add(&popup->surface->events.map, &p->map);
	p->unmap.notify = popup_unmap;
	wl_signal_add(&popup->surface->events.unmap, &p->unmap);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);

	wl_list_insert(&ime->popups, &p->link);
}

/* ── text input → input method ──────────────────────────────────────────── */

static void send_im_state(struct aro_ime *ime, struct wlr_text_input_v3 *in)
{
	struct wlr_input_method_v2 *im = ime->im;
	if (!im)
		return;
	if (in->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
		wlr_input_method_v2_send_surrounding_text(im,
			in->current.surrounding.text, in->current.surrounding.cursor,
			in->current.surrounding.anchor);
	wlr_input_method_v2_send_text_change_cause(im, in->current.text_change_cause);
	if (in->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
		wlr_input_method_v2_send_content_type(im,
			in->current.content_type.hint, in->current.content_type.purpose);
	wlr_input_method_v2_send_done(im);
	popups_place(ime);
}

static void text_input_deactivate(struct aro_ime *ime,
                                  struct aro_text_input *ti)
{
	if (!ime->im)
		return;
	wlr_input_method_v2_send_deactivate(ime->im);
	send_im_state(ime, ti->input);
}

static void set_pending(struct aro_text_input *ti, struct wlr_surface *surface)
{
	wl_list_remove(&ti->pending_destroy.link);
	wl_list_init(&ti->pending_destroy.link);
	ti->pending = surface;
	if (surface)
		wl_signal_add(&surface->events.destroy, &ti->pending_destroy);
}

static void ti_pending_destroy(struct wl_listener *l, void *data)
{
	struct aro_text_input *ti = wl_container_of(l, ti, pending_destroy);
	(void)data;
	set_pending(ti, NULL);
}

static void ti_enable(struct wl_listener *l, void *data)
{
	struct aro_text_input *ti = wl_container_of(l, ti, enable);
	(void)data;
	if (!ti->ime->im)
		return;
	wlr_input_method_v2_send_activate(ti->ime->im);
	send_im_state(ti->ime, ti->input);
}

static void ti_commit(struct wl_listener *l, void *data)
{
	struct aro_text_input *ti = wl_container_of(l, ti, commit);
	(void)data;
	if (ti->input->current_enabled && ti->ime->im)
		send_im_state(ti->ime, ti->input);
}

static void ti_disable(struct wl_listener *l, void *data)
{
	struct aro_text_input *ti = wl_container_of(l, ti, disable);
	(void)data;
	if (ti->input->focused_surface)
		text_input_deactivate(ti->ime, ti);
}

static void ti_destroy(struct wl_listener *l, void *data)
{
	struct aro_text_input *ti = wl_container_of(l, ti, destroy);
	(void)data;
	if (ti->input->current_enabled)
		text_input_deactivate(ti->ime, ti);
	set_pending(ti, NULL);
	wl_list_remove(&ti->pending_destroy.link);
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->link);
	free(ti);
}

static void new_text_input(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, new_text_input);
	struct wlr_text_input_v3 *in = data;
	if (in->seat != ime->server->seat)
		return;

	struct aro_text_input *ti = calloc(1, sizeof *ti);
	if (!ti)
		return;
	ti->ime = ime;
	ti->input = in;

	ti->enable.notify = ti_enable;
	wl_signal_add(&in->events.enable, &ti->enable);
	ti->commit.notify = ti_commit;
	wl_signal_add(&in->events.commit, &ti->commit);
	ti->disable.notify = ti_disable;
	wl_signal_add(&in->events.disable, &ti->disable);
	ti->destroy.notify = ti_destroy;
	wl_signal_add(&in->events.destroy, &ti->destroy);
	ti->pending_destroy.notify = ti_pending_destroy;
	wl_list_init(&ti->pending_destroy.link);

	wl_list_insert(&ime->text_inputs, &ti->link);

	/* the focused client may create its text input after gaining focus */
	ime_set_focus(ime->server, ime->server->seat->keyboard_state.focused_surface);
}

/* ── input method → text input ──────────────────────────────────────────── */

static void im_commit(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, im_commit);
	struct wlr_input_method_v2 *im = data;
	struct aro_text_input *ti = focused_text_input(ime);
	if (!ti)
		return;

	if (im->current.preedit.text)
		wlr_text_input_v3_send_preedit_string(ti->input,
			im->current.preedit.text, im->current.preedit.cursor_begin,
			im->current.preedit.cursor_end);
	if (im->current.commit_text)
		wlr_text_input_v3_send_commit_string(ti->input, im->current.commit_text);
	if (im->current.delete.before_length || im->current.delete.after_length)
		wlr_text_input_v3_send_delete_surrounding_text(ti->input,
			im->current.delete.before_length, im->current.delete.after_length);
	wlr_text_input_v3_send_done(ti->input);
}

static void im_grab_destroy(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, im_grab_destroy);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	wl_list_remove(&ime->im_grab_destroy.link);
	wl_list_init(&ime->im_grab_destroy.link);

	/* the application missed every modifier change during the grab */
	if (grab->keyboard) {
		wlr_seat_set_keyboard(ime->server->seat, grab->keyboard);
		wlr_seat_keyboard_notify_modifiers(ime->server->seat,
		                                   &grab->keyboard->modifiers);
	}
}

static void im_grab_keyboard(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, im_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;

	wlr_input_method_keyboard_grab_v2_set_keyboard(grab,
		wlr_seat_get_keyboard(ime->server->seat));

	wl_list_remove(&ime->im_grab_destroy.link);
	ime->im_grab_destroy.notify = im_grab_destroy;
	wl_signal_add(&grab->events.destroy, &ime->im_grab_destroy);
}

static void im_destroy(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, im_destroy);
	(void)data;
	wl_list_remove(&ime->im_commit.link);
	wl_list_remove(&ime->im_grab_keyboard.link);
	wl_list_remove(&ime->im_new_popup.link);
	wl_list_remove(&ime->im_destroy.link);
	wl_list_init(&ime->im_commit.link);
	wl_list_init(&ime->im_grab_keyboard.link);
	wl_list_init(&ime->im_new_popup.link);
	wl_list_init(&ime->im_destroy.link);
	ime->im = NULL;

	/* keep the focused field at hand in case the input method restarts */
	struct aro_text_input *ti = focused_text_input(ime);
	if (ti) {
		set_pending(ti, ti->input->focused_surface);
		wlr_text_input_v3_send_leave(ti->input);
	}
}

static void new_input_method(struct wl_listener *l, void *data)
{
	struct aro_ime *ime = wl_container_of(l, ime, new_input_method);
	struct wlr_input_method_v2 *im = data;
	if (im->seat != ime->server->seat)
		return;
	if (ime->im) {
		wlr_log(WLR_INFO, "a second input method connected; refusing it");
		wlr_input_method_v2_send_unavailable(im);
		return;
	}

	ime->im = im;
	ime->im_commit.notify = im_commit;
	wl_signal_add(&im->events.commit, &ime->im_commit);
	ime->im_grab_keyboard.notify = im_grab_keyboard;
	wl_signal_add(&im->events.grab_keyboard, &ime->im_grab_keyboard);
	ime->im_new_popup.notify = im_new_popup;
	wl_signal_add(&im->events.new_popup_surface, &ime->im_new_popup);
	ime->im_destroy.notify = im_destroy;
	wl_signal_add(&im->events.destroy, &ime->im_destroy);

	struct aro_text_input *ti;
	wl_list_for_each(ti, &ime->text_inputs, link) {
		if (ti->pending) {
			wlr_text_input_v3_send_enter(ti->input, ti->pending);
			set_pending(ti, NULL);
			break;
		}
	}
}

/* ── public ─────────────────────────────────────────────────────────────── */

void ime_set_focus(struct aro_server *s, struct wlr_surface *surface)
{
	struct aro_ime *ime = s->ime;
	if (!ime)
		return;

	struct aro_text_input *ti;
	wl_list_for_each(ti, &ime->text_inputs, link) {
		if (ti->pending) {
			if (surface != ti->pending)
				set_pending(ti, NULL);
		} else if (ti->input->focused_surface) {
			if (surface == ti->input->focused_surface)
				continue;
			text_input_deactivate(ime, ti);
			wlr_text_input_v3_send_leave(ti->input);
		}

		if (surface && wl_resource_get_client(ti->input->resource) ==
		               wl_resource_get_client(surface->resource)) {
			if (ime->im)
				wlr_text_input_v3_send_enter(ti->input, surface);
			else
				set_pending(ti, surface);
		}
	}
	popups_place(ime);
}

/*
 * The grab gets keys unless the screen is locked (a password must reach
 * the lock screen, not a candidate list) or the key came from the input
 * method's own virtual keyboard, which would loop straight back to it.
 */
static struct wlr_input_method_keyboard_grab_v2 *
grab_for(struct aro_server *s, struct aro_keyboard *kb)
{
	struct aro_ime *ime = s->ime;
	if (!ime || !ime->im || !ime->im->keyboard_grab || aro_locked(s))
		return NULL;

	struct wlr_input_method_keyboard_grab_v2 *grab = ime->im->keyboard_grab;
	struct wlr_virtual_keyboard_v1 *vk =
		wlr_input_device_get_virtual_keyboard(&kb->wlr_keyboard->base);
	if (vk && wl_resource_get_client(vk->resource) ==
	          wl_resource_get_client(grab->resource))
		return NULL;
	return grab;
}

bool ime_key(struct aro_server *s, struct aro_keyboard *kb,
             const struct wlr_keyboard_key_event *ev)
{
	struct wlr_input_method_keyboard_grab_v2 *grab = grab_for(s, kb);
	if (!grab)
		return false;
	wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb->wlr_keyboard);
	wlr_input_method_keyboard_grab_v2_send_key(grab, ev->time_msec,
	                                           ev->keycode, ev->state);
	return true;
}

bool ime_modifiers(struct aro_server *s, struct aro_keyboard *kb)
{
	struct wlr_input_method_keyboard_grab_v2 *grab = grab_for(s, kb);
	if (!grab)
		return false;
	wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb->wlr_keyboard);
	wlr_input_method_keyboard_grab_v2_send_modifiers(grab,
		&kb->wlr_keyboard->modifiers);
	return true;
}

void ime_init(struct aro_server *s)
{
	struct aro_ime *ime = calloc(1, sizeof *ime);
	if (!ime)
		return;
	ime->server = s;
	wl_list_init(&ime->text_inputs);
	wl_list_init(&ime->popups);
	wl_list_init(&ime->im_commit.link);
	wl_list_init(&ime->im_grab_keyboard.link);
	wl_list_init(&ime->im_grab_destroy.link);
	wl_list_init(&ime->im_new_popup.link);
	wl_list_init(&ime->im_destroy.link);

	ime->ti_mgr = wlr_text_input_manager_v3_create(s->display);
	ime->im_mgr = wlr_input_method_manager_v2_create(s->display);
	if (!ime->ti_mgr || !ime->im_mgr) {
		wlr_log(WLR_ERROR, "input methods unavailable");
		free(ime);
		return;
	}
	ime->new_text_input.notify = new_text_input;
	wl_signal_add(&ime->ti_mgr->events.new_text_input, &ime->new_text_input);
	ime->new_input_method.notify = new_input_method;
	wl_signal_add(&ime->im_mgr->events.new_input_method, &ime->new_input_method);

	s->ime = ime;
}

/* after clients are gone: every text input and popup is already freed */
void ime_finish(struct aro_server *s)
{
	struct aro_ime *ime = s->ime;
	if (!ime)
		return;
	wl_list_remove(&ime->new_text_input.link);
	wl_list_remove(&ime->new_input_method.link);
	wl_list_remove(&ime->im_commit.link);
	wl_list_remove(&ime->im_grab_keyboard.link);
	wl_list_remove(&ime->im_grab_destroy.link);
	wl_list_remove(&ime->im_new_popup.link);
	wl_list_remove(&ime->im_destroy.link);
	free(ime);
	s->ime = NULL;
}
