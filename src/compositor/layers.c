/* layers.c: layer-shell surfaces, such as bars, launchers and notifications */

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "config.h"
#include "aro.h"
#include "core.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

/* ── layer shell ───────────────────────────────────────────────────────── */

static struct wlr_scene_tree *layer_tree_for(struct aro_server *s,
                                             enum zwlr_layer_shell_v1_layer layer)
{
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return s->l_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return s->l_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return s->l_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return s->l_overlay;
	}
	return s->l_top;
}

static void layer_focus(struct aro_server *s, struct aro_layer *l)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (!kb)
		return;

	s->focused_layer = l;
	if (l) {
		wlr_seat_keyboard_notify_enter(s->seat, l->layer_surface->surface,
		                               kb->keycodes, kb->num_keycodes,
		                               &kb->modifiers);
	} else if (s->focused) {
		wlr_seat_keyboard_notify_enter(s->seat,
		                               view_surface(s->focused),
		                               kb->keycodes, kb->num_keycodes,
		                               &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
	}

	/* a launcher taking the keyboard releases a game's pointer lock */
	keyboard_focus_changed(s);
}

bool layer_listed(struct aro_layer *l, const char *globs)
{
	const char *ns = l->layer_surface->namespace;
	if (!globs || !ns)
		return false;
	for (const char *p = globs + strspn(globs, " ,"); *p;) {
		const size_t n = strcspn(p, " ,");
		char g[256];
		if (n < sizeof g) {
			memcpy(g, p, n);
			g[n] = '\0';
			if (glob_match(g, ns))
				return true;
		}
		p += n;
		p += strspn(p, " ,");
	}
	return false;
}

struct wlr_scene_tree *layer_home(struct aro_layer *l)
{
	return layer_tree_for(l->server, l->layer_surface->current.layer);
}

/* blur behind a layer surface whose namespace blur_layers names */
static void layer_blur_update(struct aro_layer *l)
{
#ifdef ARO_EFFECTS
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	const struct aro_config *c = &l->server->cfg;
	bool on = c->blur && ls->surface->mapped && layer_listed(l, c->blur_layers);
	if (on && !l->blur) {
		l->blur = wlr_scene_blur_create(l->scene->tree, 1, 1);
		if (l->blur)
			wlr_scene_node_lower_to_bottom(&l->blur->node);
	}
	if (!l->blur)
		return;
	wlr_scene_node_set_enabled(&l->blur->node, on);
	wlr_scene_blur_set_size(l->blur, ls->surface->current.width, ls->surface->current.height);
#else
	(void)l;
#endif
}

void layers_blur_update(struct aro_server *s)
{
	struct aro_layer *l;
	wl_list_for_each(l, &s->layers, link)
		layer_blur_update(l);
}

static void layer_map(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, map);
	(void)data;

	wlr_log(WLR_DEBUG, "layer surface mapped: %s",
	        l->layer_surface->namespace ? l->layer_surface->namespace : "?");

	arrange_layers(l->server);
	aro_arrange(l->server);
	overview_rebuild(l->server);
	layer_blur_update(l);

	/* focus interactive layer surfaces */
	if (l->layer_surface->current.keyboard_interactive)
		layer_focus(l->server, l);
}

static void layer_unmap(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, unmap);
	(void)data;

	if (l->server->focused_layer == l)
		layer_focus(l->server, NULL);

	arrange_layers(l->server);
	aro_arrange(l->server);
	overview_rebuild(l->server);
}

static void layer_commit(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, commit);
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	(void)data;

	/* handle layer change */
	if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		struct wlr_scene_tree *tree =
			layer_tree_for(l->server, ls->current.layer);
		wlr_scene_node_reparent(&l->scene->tree->node, tree);
	}

	if (ls->initial_commit || ls->current.committed) {
		arrange_layers(l->server);
		aro_arrange(l->server);
	}

	/* a mapped surface can ask for the keyboard later: a shell's bar that
	 * opens into a launcher. layer_map only sees the state it maps with.
	 * On-demand is a request to be focusable, not to be focused now */
	if (ls->surface->mapped &&
	    (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY)) {
		if (ls->current.keyboard_interactive ==
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
			layer_focus(l->server, l);
		else if (!ls->current.keyboard_interactive &&
		         l->server->focused_layer == l)
			layer_focus(l->server, NULL);
	}
	layer_blur_update(l);
	overview_layer_commit(l->server, ls->surface);
}

static void layer_destroy(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, destroy);
	(void)data;

	if (l->server->focused_layer == l)
		layer_focus(l->server, NULL);

	wl_list_remove(&l->map.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->commit.link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->new_popup.link);
	wl_list_remove(&l->link);

	struct aro_server *s = l->server;
	free(l);

	arrange_layers(s);
	aro_arrange(s);
}

void layer_new_popup(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, new_popup);
	popup_track(l->server, data, l->scene->tree);
}

void new_layer_surface(struct wl_listener *listener, void *data)
{
	struct aro_server *s = wl_container_of(listener, s, new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;

	/* pick default output for layer */
	if (!ls->output) {
		struct aro_output *o;
		if (wl_list_empty(&s->outputs)) {
			wlr_layer_surface_v1_destroy(ls);
			return;
		}
		/* the focused output, like the windows */
		o = aro_focused_output(s);
		ls->output = o->wlr_output;
	}

	struct aro_layer *l = calloc(1, sizeof *l);
	if (!l)
		return;
	l->server = s;
	l->layer_surface = ls;
	l->scene = wlr_scene_layer_surface_v1_create(
		layer_tree_for(s, ls->pending.layer), ls);
	if (!l->scene) {
		free(l);
		return;
	}

	l->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &l->map);
	l->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &l->unmap);
	l->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &l->commit);
	l->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &l->destroy);
	l->new_popup.notify = layer_new_popup;
	wl_signal_add(&ls->events.new_popup, &l->new_popup);

	wl_list_insert(&s->layers, &l->link);

	wlr_log(WLR_DEBUG, "layer surface: namespace=%s layer=%d",
	        ls->namespace ? ls->namespace : "?", ls->pending.layer);
}

/* bar = auto: hide while another client reserves space */
void bar_return_cancel(struct aro_output *o)
{
	if (o->bar_return) {
		wl_event_source_remove(o->bar_return);
		o->bar_return = NULL;
	}
}

static void bar_set_yielded(struct aro_output *o, bool yielded)
{
	if (o->bar.yielded == yielded)
		return;
	o->bar.yielded = yielded;
	o->bar_snap = true;
	wlr_log(WLR_INFO, "bar: %s on %s", yielded
	        ? "hidden, another bar reserved space" : "shown",
	        o->wlr_output->name);
}

static int bar_return_fire(void *data)
{
	struct aro_output *o = data;
	struct aro_server *s = o->server;

	bar_return_cancel(o);
	bar_set_yielded(o, false);
	arrange_layers(s);
	aro_arrange(s);
	overview_rebuild(s);
	return 0;
}

static void bar_yield_update(struct aro_output *o, bool other)
{
	struct aro_server *s = o->server;

	/* not auto, or no bar */
	if (s->cfg.bar != Q_BAR_AUTO || !o->bar.tree) {
		bar_return_cancel(o);
		bar_set_yielded(o, false);
		return;
	}
	if (other) {
		/* hide now, cancel a pending return */
		bar_return_cancel(o);
		bar_set_yielded(o, true);
		return;
	}
	if (!o->bar.yielded || o->bar_return)
		return;
	o->bar_return = wl_event_loop_add_timer(s->loop, bar_return_fire, o);
	if (!o->bar_return ||
	    wl_event_source_timer_update(o->bar_return, TH_BAR_RETURN_MS) < 0) {
		/* no timer: show now */
		bar_return_cancel(o);
		bar_set_yielded(o, false);
	}
}

/* configure layer surfaces and update usable area */
void arrange_layers(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		arrange_layers_output(o);
}

void arrange_layers_output(struct aro_output *o)
{
	struct aro_server *s = o->server;
	struct wlr_box full = { o->box.x, o->box.y, o->box.w, o->box.h };
	if (full.width <= 0 || full.height <= 0)
		return;

	struct wlr_box usable = full;

	/* exclusive zones first, so the rest keep clear of every bar however new they are */
	struct aro_layer *l;
	for (int pass = 0; pass < 2; pass++) {
		wl_list_for_each(l, &s->layers, link) {
			/* configure initialized layer surfaces, not just mapped ones */
			if (!l->layer_surface->initialized || !l->scene)
				continue;
			/* skip surfaces bound to another output */
			if (l->layer_surface->output &&
			    l->layer_surface->output != o->wlr_output)
				continue;
			if ((l->layer_surface->current.exclusive_zone > 0) != (pass == 0))
				continue;
			wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
		}
	}

	o->usable = (ly_box){ usable.x, usable.y, usable.width, usable.height };

	/* an exclusive zone means another bar */
	bar_yield_update(o, usable.x != full.x || usable.y != full.y ||
	                    usable.width != full.width ||
	                    usable.height != full.height);

	/* bottom of the usable area */
	if (o->bar.tree) {
		bar_place(&o->bar, usable.x,
		          usable.y + usable.height - bar_height(&o->bar),
		          usable.width, o->scale);
		bar_update(&o->bar, o);
	}
}
