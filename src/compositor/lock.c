/* lock.c: session lock */
/* scene.h must come first */
#include "scene.h"

#include "lock.h"
#include "aro.h"
#include "theme.h"

#include <stdlib.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

bool aro_locked(struct aro_server *s)
{
	return s->lock != NULL;
}

/* fit black blank to whole layout */
static void lock_blank_fit(struct aro_server *s, struct aro_lock *l)
{
	struct wlr_box box = { 0 };
	wlr_output_layout_get_box(s->output_layout, NULL, &box);
	if (box.width <= 0 || box.height <= 0)
		return;

	wlr_scene_node_set_position(&l->blank->node, box.x, box.y);
	wlr_scene_rect_set_size(l->blank, box.width, box.height);
}

void lock_arrange(struct aro_server *s)
{
	struct aro_lock *l = s->lock;
	if (!l)
		return;

	lock_blank_fit(s, l);

	struct aro_lock_surface *ls;
	wl_list_for_each(ls, &l->surfaces, link) {
		struct aro_output *o;
		wl_list_for_each(o, &s->outputs, link) {
			if (o->wlr_output != ls->surface->output)
				continue;
			wlr_session_lock_surface_v1_configure(ls->surface,
			                                      o->box.w, o->box.h);
			if (ls->tree)
				wlr_scene_node_set_position(&ls->tree->node,
				                            o->box.x, o->box.y);
			break;
		}
	}
}

/* lock gets keyboard focus */
static void lock_focus(struct aro_server *s, struct wlr_surface *surface)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes,
		                               kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
}

static void lock_surface_map(struct wl_listener *listener, void *data)
{
	struct aro_lock_surface *ls = wl_container_of(listener, ls, map);
	(void)data;
	lock_focus(ls->lock->server, ls->surface->surface);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data)
{
	struct aro_lock_surface *ls = wl_container_of(listener, ls, destroy);
	(void)data;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	free(ls);
}

static void lock_new_surface(struct wl_listener *listener, void *data)
{
	struct aro_lock *l = wl_container_of(listener, l, new_surface);
	struct wlr_session_lock_surface_v1 *surface = data;
	struct aro_server *s = l->server;

	struct aro_lock_surface *ls = calloc(1, sizeof *ls);
	if (!ls)
		return;
	ls->lock = l;
	ls->surface = surface;

	ls->tree = wlr_scene_subsurface_tree_create(l->tree, surface->surface);
	if (!ls->tree) {
		free(ls);
		return;
	}

	ls->map.notify = lock_surface_map;
	wl_signal_add(&surface->surface->events.map, &ls->map);
	ls->destroy.notify = lock_surface_destroy;
	wl_signal_add(&surface->events.destroy, &ls->destroy);

	wl_list_insert(&l->surfaces, &ls->link);

	/* it needs a size before it can draw anything */
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->wlr_output == surface->output) {
			wlr_session_lock_surface_v1_configure(surface, o->box.w, o->box.h);
			wlr_scene_node_set_position(&ls->tree->node, o->box.x, o->box.y);
			break;
		}
	}
}

/* unlock */
static void lock_release(struct aro_server *s, struct aro_lock *l)
{
	if (l->tree)
		wlr_scene_node_destroy(&l->tree->node);

	wl_list_remove(&l->new_surface.link);
	wl_list_remove(&l->unlock.link);
	wl_list_remove(&l->destroy.link);

	s->lock = NULL;
	free(l);

	/* whatever was focused before is still in s->focused; re-assert it so
	 * the keyboard comes back from the lock surface */
	aro_focus(s, s->focused);

	/* the windows count for idle inhibition again */
	idle_update(s);
	wlr_log(WLR_INFO, "session unlocked");
}

static void lock_handle_unlock(struct wl_listener *listener, void *data)
{
	struct aro_lock *l = wl_container_of(listener, l, unlock);
	(void)data;
	lock_release(l->server, l);
}

/* lock client destroyed; stay locked if abandoned */
static void lock_handle_destroy(struct wl_listener *listener, void *data)
{
	struct aro_lock *l = wl_container_of(listener, l, destroy);
	struct aro_server *s = l->server;
	(void)data;

	wl_list_remove(&l->new_surface.link);
	wl_list_remove(&l->unlock.link);
	wl_list_remove(&l->destroy.link);
	wl_list_init(&l->new_surface.link);
	wl_list_init(&l->unlock.link);
	wl_list_init(&l->destroy.link);

	l->abandoned = true;
	l->lock = NULL;

	/* the surfaces die with the client; the blank does not */
	struct aro_lock_surface *ls, *tmp;
	wl_list_for_each_safe(ls, tmp, &l->surfaces, link) {
		wl_list_remove(&ls->map.link);
		wl_list_remove(&ls->destroy.link);
		wl_list_remove(&ls->link);
		free(ls);
	}

	lock_blank_fit(s, l);
	wlr_seat_keyboard_notify_clear_focus(s->seat);
	wlr_seat_pointer_clear_focus(s->seat);
	idle_update(s);                 /* nothing inhibits a black screen */

	wlr_log(WLR_ERROR, "the lock client died; the session stays locked");
	notify(s, NOTIFY_ERROR, "lock client crashed — session is still locked");
}

static void handle_new_lock(struct wl_listener *listener, void *data)
{
	struct aro_server *s = wl_container_of(listener, s, new_lock);
	struct wlr_session_lock_v1 *lock = data;

	/* allow takeover only after abandoned lock */
	if (s->lock) {
		if (!s->lock->abandoned) {
			wlr_session_lock_v1_destroy(lock);
			return;
		}
		if (s->lock->tree)
			wlr_scene_node_destroy(&s->lock->tree->node);
		free(s->lock);
		s->lock = NULL;
	}

	struct aro_lock *l = calloc(1, sizeof *l);
	if (!l) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	l->server = s;
	l->lock = lock;
	wl_list_init(&l->surfaces);

	/* lock layer above everything */
	l->tree = wlr_scene_tree_create(s->l_overlay);
	if (!l->tree) {
		free(l);
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	/* black background under lock surfaces */
	float black[4] = { 0, 0, 0, 1 };
	l->blank = wlr_scene_rect_create(l->tree, 1, 1, black);
	if (!l->blank) {
		wlr_scene_node_destroy(&l->tree->node);
		free(l);
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	s->lock = l;
	lock_blank_fit(s, l);

	l->new_surface.notify = lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &l->new_surface);
	l->unlock.notify = lock_handle_unlock;
	wl_signal_add(&lock->events.unlock, &l->unlock);
	l->destroy.notify = lock_handle_destroy;
	wl_signal_add(&lock->events.destroy, &l->destroy);

	/* clear input focus before locking */
	wlr_seat_keyboard_notify_clear_focus(s->seat);
	wlr_seat_pointer_clear_focus(s->seat);

	wlr_session_lock_v1_send_locked(lock);

	/* windows behind the lock stop holding the screen awake */
	idle_update(s);
	wlr_log(WLR_INFO, "session locked");
}

void lock_init(struct aro_server *s)
{
	s->lock_mgr = wlr_session_lock_manager_v1_create(s->display);
	if (!s->lock_mgr) {
		wlr_log(WLR_ERROR, "session lock unavailable");
		return;
	}
	s->new_lock.notify = handle_new_lock;
	wl_signal_add(&s->lock_mgr->events.new_lock, &s->new_lock);
}

void lock_finish(struct aro_server *s)
{
	if (s->lock_mgr)
		wl_list_remove(&s->new_lock.link);
	s->lock_mgr = NULL;
}
