/* keep _GNU_SOURCE first; inotify_init1 needs it */
#define _GNU_SOURCE

/*
 * aro.c: main compositor code.
 *
 * startup, windows (xdg-shell), focus, floating, dragging and resizing,
 * the clipboard and config reload. The rest lives beside it: actions.c,
 * arrange.c, input.c, layers.c, output.c, toplevels.c, xwayland.c, sharing
 * core.h.
 */
#define _POSIX_C_SOURCE 200809L

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "config.h"
#include "aro.h"
#include "core.h"
#include "idle.h"
#include "ipc.h"
#include "logfile.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#ifdef ARO_EFFECTS
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_presentation_time.h>
/* primary selection: seat helpers vs v1 protocol manager */
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#ifdef ARO_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>



uint32_t aro_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


/* focused output; falls back to first output */
struct aro_output *aro_focused_output(struct aro_server *s)
{
	if (s->focused_output)
		return s->focused_output;
	if (wl_list_empty(&s->outputs))
		return NULL;
	struct aro_output *o;
	o = wl_container_of(s->outputs.next, o, link);
	return o;
}

/* ── focus ─────────────────────────────────────────────────────────────── */

static void focus_apply(struct aro_server *s, struct aro_view *v);

/* pointer constraints and text inputs both follow keyboard focus */
void keyboard_focus_changed(struct aro_server *s)
{
	constraint_sync(s);
	shortcuts_inhibit_sync(s);
	ime_set_focus(s, s->seat->keyboard_state.focused_surface);
}

void aro_focus(struct aro_server *s, struct aro_view *v)
{
	if (v && view_tab_hidden(v)) {
		group_activate(s, v);   /* its tab comes forward */
		aro_arrange(s);
	}
	focus_apply(s, v);
	if (v)
		monocle_sync(s, v->output);
	if (v && aro_view_clipped(v))
		aro_arrange(s);         /* the strip scrolls to the new focus */
	keyboard_focus_changed(s);
	ftl_sync_activated(s);
	mru_focus(s, s->focused);
	ipc_notify(s);
}

static void focus_apply(struct aro_server *s, struct aro_view *v)
{
	/* while locked, remember focus but don't apply it */
	if (aro_locked(s)) {
		s->focused = v;
		return;
	}

	if (s->focused == v)
		return;

	if (s->focused) {
		ui_frame_focus(s->focused, false);
		view_activate(s->focused, false);
	}

	s->focused = v;
	if (!v) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		return;
	}

	/* focus moves keyboard output */
	if (v->output)
		s->focused_output = v->output;

	ui_frame_focus(v, true);
	view_activate(v, true);
	wlr_scene_node_raise_to_top(&v->frame_tree->node);

	if (v->output && v->output->bar.tree)
		bar_update(&v->output->bar, v->output);

	/* layer surface may keep keyboard */
	if (s->focused_layer)
		return;

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, view_surface(v),
		                               kb->keycodes, kb->num_keycodes,
		                               &kb->modifiers);
}

/* keep view/node pointers in sync after swap */
void view_swap(struct aro_view *a, struct aro_view *b)
{
	ly_node *na = a->node, *nb = b->node;
	ly_swap(na, nb);
	a->node = nb;
	b->node = na;
	/* a group moves as one: every tab follows its leaf */
	for (int i = 0; a->group && i < a->group->n; i++)
		a->group->v[i]->node = nb;
	for (int i = 0; b->group && i < b->group->n; i++)
		b->group->v[i]->node = na;
}

/* ── shells ────────────────────────────────────────────────────────────── */

/* xdg-shell implementation */
static void xdg_configure(struct aro_view *v, int x, int y, int w, int h)
{
	(void)x; (void)y;       /* a Wayland client does not know where it is */
	if (!v->toplevel)
		return;
	/* tiled: fill the size given, mpv stops keeping its aspect */
	if (wl_resource_get_version(v->toplevel->resource) >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION)
		wlr_xdg_toplevel_set_tiled(v->toplevel, v->floating || v->fullscreen ? WLR_EDGE_NONE
		        : WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
	wlr_xdg_toplevel_set_size(v->toplevel, w, h);
}

static void xdg_close(struct aro_view *v)
{
	if (v->toplevel)
		wlr_xdg_toplevel_send_close(v->toplevel);
}

static void xdg_activate(struct aro_view *v, bool activated)
{
	if (v->toplevel)
		wlr_xdg_toplevel_set_activated(v->toplevel, activated);
}

static void xdg_set_fullscreen(struct aro_view *v, bool fullscreen)
{
	if (v->toplevel)
		wlr_xdg_toplevel_set_fullscreen(v->toplevel, fullscreen);
}

static const char *xdg_title(struct aro_view *v)
{
	return v->toplevel ? v->toplevel->title : NULL;
}

static const char *xdg_app_id(struct aro_view *v)
{
	return v->toplevel ? v->toplevel->app_id : NULL;
}

/* xdg-shell has no window types; a toplevel with a parent is a dialog */
/* a dialog has a parent, or says it is modal through xdg-dialog */
static bool xdg_is_dialog(struct wlr_xdg_toplevel *t)
{
	if (!t)
		return false;
	struct wlr_xdg_dialog_v1 *d = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(t);
	return t->parent || (d && d->modal);
}

static const char *xdg_type(struct aro_view *v)
{
	return xdg_is_dialog(v->toplevel) ? "dialog" : "normal";
}

/* float heuristics */
static bool xdg_wants_float(struct aro_view *v)
{
	struct wlr_xdg_toplevel *t = v->toplevel;
	if (!t)
		return false;
	if (xdg_is_dialog(t))
		return true;

	int minw = t->current.min_width, maxw = t->current.max_width;
	int minh = t->current.min_height, maxh = t->current.max_height;
	return minw > 0 && maxw > 0 && minh > 0 && maxh > 0 &&
	       minw == maxw && minh == maxh;
}

static void xdg_preferred_size(struct aro_view *v, int *w, int *h)
{
	*w = 0;
	*h = 0;
	if (!v->toplevel)
		return;

	/* effective geometry; fall back to buffer size */
	struct wlr_xdg_surface *base = v->toplevel->base;
	*w = base->geometry.width;
	*h = base->geometry.height;
	if ((*w <= 0 || *h <= 0) && base->surface) {
		*w = base->surface->current.width;
		*h = base->surface->current.height;
	}
}

static bool xdg_wants_fullscreen(struct aro_view *v)
{
	return v->toplevel && v->toplevel->requested.fullscreen;
}

static void xdg_geometry(struct aro_view *v, struct wlr_box *out)
{
	if (v->toplevel) {
		*out = v->toplevel->base->geometry;
		if (out->width > 0 && out->height > 0)
			return;
	}
	*out = (struct wlr_box){ 0, 0, 0, 0 };
}

static struct wlr_surface *xdg_surface(struct aro_view *v)
{
	return v->toplevel ? v->toplevel->base->surface : NULL;
}

static const struct view_impl xdg_impl = {
	.configure       = xdg_configure,
	.close           = xdg_close,
	.activate        = xdg_activate,
	.set_fullscreen  = xdg_set_fullscreen,
	.title           = xdg_title,
	.app_id          = xdg_app_id,
	.type            = xdg_type,
	.geometry        = xdg_geometry,
	.wants_float     = xdg_wants_float,
	.wants_fullscreen = xdg_wants_fullscreen,
	.preferred_size  = xdg_preferred_size,
	.surface         = xdg_surface,
};

/* shell wrappers */
void view_configure(struct aro_view *v, int x, int y, int w, int h)
{
	if (v && v->impl && v->impl->configure)
		v->impl->configure(v, x, y, w, h);
}

void view_close(struct aro_view *v)
{
	if (v && v->impl && v->impl->close)
		v->impl->close(v);
}

void view_activate(struct aro_view *v, bool activated)
{
	if (v && v->impl && v->impl->activate)
		v->impl->activate(v, activated);
}

/* zero size means no geometry yet */
void view_geometry(struct aro_view *v, struct wlr_box *out)
{
	*out = (struct wlr_box){ 0, 0, 0, 0 };
	if (v && v->impl && v->impl->geometry)
		v->impl->geometry(v, out);
}

const char *view_title(struct aro_view *v)
{
	return (v && v->impl && v->impl->title) ? v->impl->title(v) : NULL;
}

const char *view_app_id(struct aro_view *v)
{
	return (v && v->impl && v->impl->app_id) ? v->impl->app_id(v) : NULL;
}

static const char *view_type(struct aro_view *v)
{
	return v && v->impl && v->impl->type ? v->impl->type(v) : "normal";
}

struct wlr_surface *view_surface(struct aro_view *v)
{
	return (v && v->impl && v->impl->surface) ? v->impl->surface(v) : NULL;
}

/* surface visibility for idle inhibit */
bool aro_surface_visible(struct aro_server *s, struct wlr_surface *surface)
{
	if (!surface)
		return false;

	surface = wlr_surface_get_root_surface(surface);
	if (!surface->mapped)
		return false;               /* an unmapped popup of a visible window */

	for (int depth = 0; depth < 32; depth++) {
		struct wlr_xdg_popup *p = wlr_xdg_popup_try_from_wlr_surface(surface);
		if (!p || !p->parent)
			break;
		surface = wlr_surface_get_root_surface(p->parent);
	}
	if (!surface->mapped)
		return false;

	/* locked: only what the lock screen itself shows is on screen */
	if (aro_locked(s)) {
		if (s->lock->abandoned)
			return false;
		struct aro_lock_surface *ls;
		wl_list_for_each(ls, &s->lock->surfaces, link)
			if (ls->surface->surface == surface)
				return true;
		return false;
	}

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (view_surface(v) != surface)
			continue;
		if (!view_visible(v))
			return false;
		/* a fullscreen window on the same screen and workspace hides it */
		struct aro_view *f;
		wl_list_for_each(f, &s->views, link) {
			if (f != v && f->fullscreen && view_visible(f) &&
			    f->output == v->output && f->workspace == v->workspace)
				return false;
		}
		return true;
	}
	return true;
}

/* ── floating ──────────────────────────────────────────────────────────── */


/* initial floating box */
/* frame chrome size */
static void frame_chrome(struct aro_view *v, int *cw, int *ch)
{
	const ly_box big = { 0, 0, 10000, 10000 };
	ly_box c;
	ui_frame_content_box(v, big, &c);
	*cw = big.w - c.w;
	*ch = big.h - c.h;
}

/*
 * Centred on the screen. app_size asks the app first, with float_scale as
 * the fallback; without it, float_scale it is. An app on screen reports the
 * size it has, so a tiled window floated by key would keep its tile's (#2).
 */
static ly_box float_box_for(struct aro_view *v, bool app_size)
{
	const struct q_theme *th = &v->server->cfg.theme;
	ly_box u = usable_area(v->output);

	int w = 0, h = 0;
	if (app_size && v->impl->preferred_size)
		v->impl->preferred_size(v, &w, &h);
	if (w <= 0 || h <= 0) {
		w = (int)(u.w * th->float_scale);
		h = (int)(u.h * th->float_scale);
	}

	/* add frame chrome */
	int cw, ch;
	frame_chrome(v, &cw, &ch);
	w += cw;
	h += ch;

	int og = th->outer_gap;
	if (w > u.w - og * 2)
		w = u.w - og * 2;
	if (h > u.h - og * 2)
		h = u.h - og * 2;
	if (w < th->float_min_w)
		w = th->float_min_w;
	if (h < th->float_min_h)
		h = th->float_min_h;

	return (ly_box){ u.x + (u.w - w) / 2, u.y + (u.h - h) / 2, w, h };
}

/* configure a new floating window once */
static void view_float_configure_once(struct aro_view *v)
{
	if (!v->floating || v->fullscreen)
		return;

	v->float_follow = v->toplevel != NULL;

	/* don't force a guessed size */
	int w = 0, h = 0;
	if (v->impl->preferred_size)
		v->impl->preferred_size(v, &w, &h);
	if (v->float_follow && (w <= 0 || h <= 0))
		return;

	ly_box c;
	ui_frame_content_box(v, v->fbox, &c);
	view_configure(v, c.x, c.y, c.w, c.h);
}

/* follow client-requested float size */
static void view_float_follow(struct aro_view *v)
{
	struct aro_server *s = v->server;
	const struct q_theme *th = &s->cfg.theme;

	if (!v->float_follow || !v->floating || v->fullscreen || !v->mapped ||
	    !v->output)
		return;
	/* don't fight mid-drag commits */
	if (s->grabbed == v)
		return;

	int w = 0, h = 0;
	if (v->impl->preferred_size)
		v->impl->preferred_size(v, &w, &h);
	if (w <= 0 || h <= 0)
		return;         /* nothing committed yet: keep what we have */

	int cw, ch;
	frame_chrome(v, &cw, &ch);
	w += cw;
	h += ch;

	ly_box u = usable_area(v->output);
	int og = th->outer_gap;
	if (w > u.w - og * 2)
		w = u.w - og * 2;
	if (h > u.h - og * 2)
		h = u.h - og * 2;
	if (w < th->float_min_w)
		w = th->float_min_w;
	if (h < th->float_min_h)
		h = th->float_min_h;

	if (w == v->fbox.w && h == v->fbox.h)
		return;

	v->fbox.w = w;
	v->fbox.h = h;
	anim_box_set(&v->geo, view_target(v));

	/* update title when width changes */
	ui_frame_title(v, v->fbox.w, v->output->scale);
	wlr_output_schedule_frame(v->output->wlr_output);
}

/* toggle floating */
void view_set_floating(struct aro_server *s, struct aro_view *v,
                              bool floating)
{
	if (!v || v->floating == floating)
		return;
	if (!floating)
		v->sticky = v->scratch = false; /* both are floating only */

	if (floating) {
		/* a window dragged out of the layout keeps its size; the float key gives float_scale */
		const bool dragged = s->grabbed == v;
		view_detach(v);
		v->floating = true;
		v->fbox = float_box_for(v, dragged);
		if (!v->fullscreen)
			wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
		view_float_configure_once(v);
		if (!dragged)
			v->float_follow = false;        /* or its next commit, still tile-sized, undoes it */
	} else {
		v->floating = false;
		v->float_follow = false;        /* the tree decides again */

		ly_node *target = s->focused && s->focused != v &&
		                  s->focused->node &&
		                  s->focused->output == v->output &&
		                  s->focused->workspace == v->workspace
		                ? s->focused->node : NULL;
		v->node = tree_insert(s, v->output, v->workspace, v, target,
		                      LY_ROW, false);
		if (!v->node) {
			/* OOM: stay floating */
			v->floating = true;
			return;
		}
		if (!v->fullscreen)
			wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
	}

	aro_arrange(s);
}

/* fullscreen state */
void view_set_fullscreen(struct aro_server *s, struct aro_view *v,
                                bool fullscreen)
{
	if (!v || v->fullscreen == fullscreen)
		return;

	v->fullscreen = fullscreen;

	if (fullscreen) {
		v->pre_fs = v->fbox;
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_fullscreen);
	} else {
		v->fbox = v->pre_fs;
		wlr_scene_node_reparent(&v->frame_tree->node,
		                        v->floating ? s->l_float : s->l_tiled);
	}

	ui_frame_fullscreen(v, fullscreen);
	if (v->impl->set_fullscreen)
		v->impl->set_fullscreen(v, fullscreen);

	/* restore float size after fullscreen */
	if (!fullscreen)
		view_float_configure_once(v);
	aro_arrange(s);
}

void view_request_fullscreen(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_fullscreen);
	(void)data;

#ifdef ARO_XWAYLAND
	/* shared by both shells; an X11 view has no xdg toplevel. Before
	 * map the state is read by wants_fullscreen instead. */
	if (v->xsurface) {
		if (v->mapped)
			view_set_fullscreen(v->server, v, v->xsurface->fullscreen);
		return;
	}
#endif

	/* answer fullscreen requests even when unmapped; before the first commit, map reads them */
	if (!v->mapped) {
		if (v->toplevel->base->initialized)
			wlr_xdg_surface_schedule_configure(v->toplevel->base);
		return;
	}
	view_set_fullscreen(v->server, v, v->toplevel->requested.fullscreen);
}

/* ── window rules ──────────────────────────────────────────────────────── */

/* a rule length in pixels, along a span of the screen */
static int rule_px(struct q_len l, int span)
{
	return (int)(l.pct ? l.v * span : l.v);
}

static bool rule_place_same(const struct q_rule_result *a, const struct q_rule_result *b)
{
	for (int i = 0; i < 2; i++)
		if (a->size[i].set != b->size[i].set || a->size[i].v != b->size[i].v ||
		    a->size[i].pct != b->size[i].pct || a->pos[i].set != b->pos[i].set ||
		    a->pos[i].v != b->pos[i].v || a->pos[i].pct != b->pos[i].pct)
			return false;
	return a->center == b->center;
}

/* a floating window's size and place from its rules */
static void rule_place(struct aro_view *v, const struct q_rule_result *r)
{
	if (!v->floating || v->fullscreen || !v->output ||
	    (!r->size[0].set && !r->pos[0].set && r->center != 1))
		return;
	const struct q_theme *th = &v->server->cfg.theme;
	ly_box u = usable_area(v->output);
	if (r->size[0].set) {
		v->fbox.w = rule_px(r->size[0], u.w);
		v->fbox.h = rule_px(r->size[1], u.h);
		if (v->fbox.w < th->float_min_w)
			v->fbox.w = th->float_min_w;
		if (v->fbox.h < th->float_min_h)
			v->fbox.h = th->float_min_h;
		v->float_follow = false;        /* the rule's size, not the app's */
	}
	if (r->pos[0].set) {
		v->fbox.x = u.x + rule_px(r->pos[0], u.w);
		v->fbox.y = u.y + rule_px(r->pos[1], u.h);
	} else {
		v->fbox.x = u.x + (u.w - v->fbox.w) / 2;
		v->fbox.y = u.y + (u.h - v->fbox.h) / 2;
	}
	ly_box c;
	ui_frame_content_box(v, v->fbox, &c);
	view_configure(v, c.x, c.y, c.w, c.h);
}

/* border, header, corners and opacity as the rules say; true if any changed */
static bool rule_chrome(struct aro_view *v, const struct q_rule_result *r)
{
	const bool nb = r->no_border == 1, nh = r->no_header == 1, nr = r->no_radius == 1;
	if (v->no_border == nb && v->no_header == nh && v->no_radius == nr &&
	    v->rule_opacity[0] == r->opacity[0] && v->rule_opacity[1] == r->opacity[1])
		return false;
	v->no_border = nb;
	v->no_header = nh;
	v->no_radius = nr;
	v->rule_opacity[0] = r->opacity[0];
	v->rule_opacity[1] = r->opacity[1];
	ui_frame_retheme(v);
	return true;
}

/* how strong blur is, for windows, layers and the overview alike */
static void blur_strength_apply(struct aro_server *s)
{
#ifdef ARO_EFFECTS
	struct blur_data d = blur_data_get_default();
	wlr_scene_set_blur_data(s->scene, s->cfg.blur_passes, s->cfg.blur_radius,
	                        d.noise, d.brightness, d.contrast, d.saturation);
#else
	(void)s;
#endif
}

/* the first enabled output whose name or "make model serial" matches */
static struct aro_output *rule_output(struct aro_server *s, const char *glob)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct wlr_output *wo = o->wlr_output;
		char desc[256];
		snprintf(desc, sizeof desc, "%s %s %s", wo->make ? wo->make : "",
		         wo->model ? wo->model : "", wo->serial ? wo->serial : "");
		if (glob_match(glob, wo->name) || glob_match(glob, desc))
			return o;
	}
	return NULL;
}

/* reapply rules only on change */
static void view_rules_reapply(struct aro_view *v)
{
	struct aro_server *s = v->server;
	if (!v->mapped || !v->output)
		return;

	struct q_rule_result r, last = v->rule_last;
	config_rules_eval(&s->cfg, view_app_id(v), view_title(v), view_type(v), &r);
	v->rule_last = r;       /* before acting: nothing below re-enters, but
	                           a stale answer must never be compared twice */

	bool moved = rule_chrome(v, &r);
	if (r.monitor_hash && r.monitor_hash != last.monitor_hash) {
		struct aro_output *to = rule_output(s, r.monitor);
		if (to && to != v->output)
			view_move_to_output(s, v, to);
	}

	/* apply float before workspace move */
	if (r.floating != Q_RULE_UNSET && r.floating != last.floating)
		view_set_floating(s, v, r.floating == 1);
	if (r.sticky == 1 && last.sticky != 1 && !v->sticky && !v->fullscreen) {
		view_set_floating(s, v, true);
		v->sticky = true;
		v->scratch = v->stashed = false;
	}
	if (!rule_place_same(&r, &last)) {
		rule_place(v, &r);
		moved = true;
	}
	if (moved)
		aro_arrange(s);
	if (r.workspace != Q_RULE_UNSET && r.workspace != last.workspace)
		view_send_to(s, v, r.workspace);
	if (r.fullscreen != Q_RULE_UNSET && r.fullscreen != last.fullscreen)
		view_set_fullscreen(s, v, true);
	if (r.scratch == 1 && last.scratch != 1 && !v->scratch)
		scratch_toggle(s, v);
}

/* ── across outputs ────────────────────────────────────────────────────── */

/* nearest output in a direction */
struct aro_output *output_toward(struct aro_server *s,
                                           struct aro_output *from,
                                           ly_edge e)
{
	double fx = from->box.x + from->box.w / 2.0;
	double fy = from->box.y + from->box.h / 2.0;

	struct aro_output *best = NULL;
	double best_d = 0;

	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o == from)
			continue;
		double ox = o->box.x + o->box.w / 2.0;
		double oy = o->box.y + o->box.h / 2.0;
		double dx = ox - fx, dy = oy - fy;

		bool right_way =
			(e == LY_LEFT  && dx < 0) || (e == LY_RIGHT && dx > 0) ||
			(e == LY_UP    && dy < 0) || (e == LY_DOWN  && dy > 0);
		if (!right_way)
			continue;

		/* penalize cross-axis drift */
		double along = (e == LY_LEFT || e == LY_RIGHT) ? dx : dy;
		double cross = (e == LY_LEFT || e == LY_RIGHT) ? dy : dx;
		if (along < 0)
			along = -along;
		if (cross < 0)
			cross = -cross;
		double d = along + cross * 2.0;

		if (!best || d < best_d) {
			best = o;
			best_d = d;
		}
	}
	return best;
}

/* pick a view on an output */
struct aro_view *output_pick_view(struct aro_server *s,
                                            struct aro_output *o)
{
	ly_node *root = o->ws[o->cur_ws];
	if (root) {
		ly_node *first = ly_first_leaf(root);
		if (first && first->user)
			return first->user;
	}
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->mapped && !v->stashed && v->output == o && v->workspace == o->cur_ws)
			return v;
	}
	return NULL;
}

/* move view to another output */
void view_move_to_output(struct aro_server *s, struct aro_view *v,
                                struct aro_output *dest)
{
	if (!v || !dest || v->output == dest)
		return;

	view_detach(v);

	struct aro_output *src = v->output;
	v->output = dest;
	v->workspace = dest->cur_ws;

	if (v->floating) {
		/* shift floating box to destination */
		v->fbox.x += dest->box.x - src->box.x;
		v->fbox.y += dest->box.y - src->box.y;
	} else {
		v->node = tree_insert(s, dest, dest->cur_ws, v, NULL, LY_ROW, false);
		if (!v->node) {
			wlr_log(WLR_ERROR, "out of memory moving a window between outputs");
			return;
		}
	}

	s->focused_output = dest;
	view_set_visible(v, true);
	aro_focus(s, v);
	aro_arrange(s);
}

/* ── dragging ──────────────────────────────────────────────────────────── */

/* drag-and-drop tiling */

/* drop slot preview */
static ly_box drop_slot_box(ly_box t, ly_edge e)
{
	switch (e) {
	case LY_LEFT:  return (ly_box){ t.x, t.y, t.w / 2, t.h };
	case LY_RIGHT: return (ly_box){ t.x + t.w - t.w / 2, t.y, t.w / 2, t.h };
	case LY_UP:    return (ly_box){ t.x, t.y, t.w, t.h / 2 };
	case LY_DOWN:  return (ly_box){ t.x, t.y + t.h - t.h / 2, t.w, t.h / 2 };
	}
	return t;
}

static bool box_contains(ly_box b, double x, double y)
{
	return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

/* tiled view under cursor */
static struct aro_view *tiled_view_at(struct aro_server *s,
                                         double x, double y)
{
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v))
			continue;
		if (v->floating || v->fullscreen || !v->node || v == s->grabbed)
			continue;
		/* only views on cursor output */
		if (v->output != output_at(s, x, y))
			continue;
		if (box_contains(v->node->box, x, y))
			return v;
	}
	return NULL;
}

/* nearest edge */
ly_edge nearest_edge(ly_box b, double x, double y)
{
	double fx = b.w > 0 ? (x - b.x) / (double)b.w : 0.5;
	double fy = b.h > 0 ? (y - b.y) / (double)b.h : 0.5;

	double d[4] = { fx, 1.0 - fx, fy, 1.0 - fy };   /* L R U D */
	ly_edge e[4] = { LY_LEFT, LY_RIGHT, LY_UP, LY_DOWN };

	int best = 0;
	for (int i = 1; i < 4; i++)
		if (d[i] < d[best])
			best = i;
	return e[best];
}

static void drop_clear(struct aro_server *s)
{
	s->drop_target = NULL;
	s->drop_join = false;
	ui_preview_show(&s->preview, false);
}

/* update drop indicator */
static void drop_update(struct aro_server *s)
{
	struct aro_view *target = tiled_view_at(s, s->cursor->x, s->cursor->y);
	if (!target) {
		drop_clear(s);
		return;
	}

	ly_edge e = nearest_edge(target->node->box, s->cursor->x, s->cursor->y);
	/* over its title: the whole tile lights up, the drop makes a tab */
	const bool join = ui_frame_in_header(target, s->cursor->x, s->cursor->y) &&
	                  !(target->group && target->group->n == ARO_GROUP_MAX);
	ly_box slot = join ? view_target(target) : drop_slot_box(drop_target_box(s, target, e), e);

	bool fresh = !s->preview.active;
	s->drop_target = target;
	s->drop_edge = e;
	s->drop_join = join;

	if (fresh) {
		/* place preview instantly first time */
		anim_box_set(&s->preview.geo, slot);
		ui_preview_geometry(&s->preview, slot);
		ui_preview_show(&s->preview, true);
	} else {
		/* preview moves flat/fast */
		anim_box_to(&s->preview.geo, slot, aro_now_ms(), s->cfg.theme.drop_ms, &FLAT);
	}
}

/* drop floating view into tree */
static void view_tile_into(struct aro_server *s, struct aro_view *v,
                           struct aro_view *target, ly_edge e)
{
	if (!v || !target || !target->node || target == v)
		return;

	ly_dir dir = (e == LY_LEFT || e == LY_RIGHT) ? LY_ROW : LY_COL;
	/* drop moves view to target output */
	v->output = target->output;
	v->workspace = target->workspace;
	ly_node **root = &v->output->ws[v->workspace];

	/* on a strip, beside a window is beside its whole column */
	ly_node *at = dir == LY_ROW ? scroll_beside(s, v->output, v->workspace, target->node)
	                            : target->node;
	ly_node *leaf = ly_split(root, at, dir, v);
	if (!leaf)
		return;                 /* out of memory: stay floating */
	if (at != target->node) {
		/* the new column goes first when dropped on the left */
		if (e == LY_LEFT) {
			ly_node *sp = leaf->parent;
			sp->a = leaf;
			sp->b = at;
		}
		v->node = leaf;
		v->floating = false;
		v->float_follow = false;
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
		ws_arrange(s, v->output, v->workspace);
		return;
	}

	v->node = leaf;
	v->floating = false;
	v->float_follow = false;        /* back in the tree: the tree decides */
	wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);

	ws_arrange(s, v->output, v->workspace);

	bool want_first = (e == LY_LEFT || e == LY_UP);
	bool is_first = (dir == LY_ROW)
	              ? v->node->box.x < target->node->box.x
	              : v->node->box.y < target->node->box.y;
	if (want_first != is_first)
		view_swap(v, target);
}

/* overview drop: beside target on edge e, else where mod+shift+N puts it */
void aro_view_drop(struct aro_server *s, struct aro_view *v,
                   struct aro_output *o, int ws, struct aro_view *target,
                   ly_edge e, const ly_box *fbox)
{
	if (!v || !v->mapped || !v->output || !o || ws < 0 || ws >= ARO_MAX_WS)
		return;
	if (target && (target == v || !target->mapped || !target->node ||
	               target->floating || target->fullscreen ||
	               target->output != o || target->workspace != ws))
		target = NULL;
	if (!target && o == v->output && ws == v->workspace &&
	    !(v->floating && fbox))
		return;                 /* dropped where it already is */

	struct aro_output *src = v->output;
	ly_node *next = view_detach(v);
	v->output = o;
	v->workspace = ws;

	/* floating boxes are in layout coordinates */
	if (o != src) {
		int dx = o->box.x - src->box.x, dy = o->box.y - src->box.y;
		v->fbox.x += dx;
		v->fbox.y += dy;
		v->pre_fs.x += dx;
		v->pre_fs.y += dy;
	}
	if (v->floating && fbox && !v->fullscreen)
		v->fbox = *fbox;

	if (!v->floating) {
		if (target && !v->fullscreen)
			view_tile_into(s, v, target, e);
		if (!v->node)
			v->node = tree_insert(s, o, ws, v, NULL, LY_ROW, false);
		if (!v->node) {
			/* out of memory: floating beats being nowhere */
			wlr_log(WLR_ERROR, "out of memory dropping a window");
			v->floating = true;
			v->fbox = float_box_for(v, true);
			if (!v->fullscreen)
				wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
		}
	}

	view_set_visible(v, ws == o->cur_ws);
	if (s->focused == v && !view_visible(v)) {
		s->focused = NULL;
		aro_focus(s, next ? next->user : NULL);
	}
	aro_arrange(s);
	anim_box_set(&v->geo, view_target(v));  /* placed, not sprung */
}

ly_box aro_drop_slot(ly_box t, ly_edge e)
{
	return drop_slot_box(t, e);
}

ly_edge aro_nearest_edge(ly_box b, double x, double y)
{
	return nearest_edge(b, x, y);
}

/* ── resizing a shared boundary ────────────────────────────────────────── */


/* tiled resize moves a shared boundary */

/* edge mask near cursor */
uint32_t edge_zone(ly_box b, double x, double y, int zone)
{
	uint32_t m = 0;
	if (x - b.x < zone)
		m |= WLR_EDGE_LEFT;
	else if ((b.x + b.w) - x < zone)
		m |= WLR_EDGE_RIGHT;
	if (y - b.y < zone)
		m |= WLR_EDGE_TOP;
	else if ((b.y + b.h) - y < zone)
		m |= WLR_EDGE_BOTTOM;
	return m;
}

static ly_edge ly_edge_from_mask(uint32_t m)
{
	if (m & WLR_EDGE_LEFT)
		return LY_LEFT;
	if (m & WLR_EDGE_RIGHT)
		return LY_RIGHT;
	if (m & WLR_EDGE_TOP)
		return LY_UP;
	return LY_DOWN;
}

uint32_t mask_from_ly_edge(ly_edge e)
{
	switch (e) {
	case LY_LEFT:  return WLR_EDGE_LEFT;
	case LY_RIGHT: return WLR_EDGE_RIGHT;
	case LY_UP:    return WLR_EDGE_TOP;
	case LY_DOWN:  return WLR_EDGE_BOTTOM;
	}
	return 0;
}

/* find split owning this boundary */
static ly_node *boundary_for(ly_node *leaf, ly_edge e)
{
	ly_dir want = (e == LY_LEFT || e == LY_RIGHT) ? LY_ROW : LY_COL;
	bool want_first = (e == LY_RIGHT || e == LY_DOWN);

	for (ly_node *n = leaf; n && n->parent; n = n->parent) {
		ly_node *p = n->parent;
		if (p->kind == LY_SPLIT && p->dir == want &&
		    ((p->a == n) == want_first))
			return p;
	}
	return NULL;
}

/* pick a boundary to resize */
static ly_node *boundary_pick(ly_node *leaf, ly_edge want)
{
	ly_node *split = boundary_for(leaf, want);
	if (split)
		return split;

	static const ly_edge fallback[4] = { LY_RIGHT, LY_LEFT, LY_DOWN, LY_UP };
	for (int i = 0; i < 4; i++) {
		if (fallback[i] == want)
			continue;
		split = boundary_for(leaf, fallback[i]);
		if (split)
			return split;
	}
	return NULL;           /* the only window on the workspace */
}

/* live resize: no animation */
static void arrange_live(struct aro_server *s)
{
	/* only resize current output */
	struct aro_output *o = s->grabbed ? s->grabbed->output
	                                     : aro_focused_output(s);
	if (!o)
		return;
	ly_node *root = o->ws[o->cur_ws];
	if (!root)
		return;

	ws_arrange(s, o, o->cur_ws);

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v) || v->output != o)
			continue;
		if (v->floating || v->fullscreen || !v->node)
			continue;
		anim_box_set(&v->geo, v->node->box);
	}
}

static void grab_motion_resize_tile(struct aro_server *s)
{
	ly_node *p = s->grab_split;
	if (!p || p->kind != LY_SPLIT) {
		grab_end(s);
		return;
	}

	bool horiz = (p->dir == LY_ROW);
	double span = horiz ? p->box.w : p->box.h;
	if (span <= 0)
		return;

	/* resize relative to grab start */
	double delta = horiz ? s->cursor->x - s->grab_x : s->cursor->y - s->grab_y;
	double r = s->grab_ratio + delta / span;

	/* leave room for s->cfg.theme.min plus the gap on both sides */
	double floor_r = (s->cfg.theme.min + s->cfg.theme.gap) / span;
	if (floor_r > 0.45)
		floor_r = 0.45;
	if (r < floor_r)
		r = floor_r;
	if (r > 1.0 - floor_r)
		r = 1.0 - floor_r;

	p->ratio = r;
	arrange_live(s);
}

/* nearest tiled view */
static struct aro_view *nearest_tiled_view(struct aro_server *s,
                                              double x, double y,
                                              struct aro_view *except)
{
	struct aro_view *best = NULL;
	double best_d = 0;
	struct aro_output *at = output_at(s, x, y);

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v) || v == except)
			continue;
		if (v->floating || v->fullscreen || !v->node)
			continue;
		/* only views on cursor output */
		if (v->output != at)
			continue;

		double cx = v->node->box.x + v->node->box.w / 2.0;
		double cy = v->node->box.y + v->node->box.h / 2.0;
		double dx = cx - x, dy = cy - y;
		double d = dx * dx + dy * dy;

		if (!best || d < best_d) {
			best = v;
			best_d = d;
		}
	}
	return best;
}

/* retile dropped window */
static void view_retile(struct aro_server *s, struct aro_view *v)
{
	struct aro_view *target = NULL;
	ly_edge edge = LY_RIGHT;

	if (s->preview.active && s->drop_target && s->drop_join) {
		target = s->drop_target;
		v->output = target->output;
		v->workspace = target->workspace;
		if (group_join(s, v, target)) {
			v->floating = false;
			v->float_follow = false;
			wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
			return;
		}
	}
	if (s->preview.active && s->drop_target) {
		target = s->drop_target;
		edge = s->drop_edge;
	} else {
		target = nearest_tiled_view(s, s->cursor->x, s->cursor->y, v);
		if (target)
			edge = nearest_edge(target->node->box,
			                    s->cursor->x, s->cursor->y);
	}

	if (target) {
		view_tile_into(s, v, target, edge);
		return;
	}

	/* become root if workspace empty, on the output under the cursor */
	struct aro_output *o = output_at(s, s->cursor->x, s->cursor->y);
	v->output = o;
	v->workspace = o->cur_ws;
	s->focused_output = o;
	ly_node **root = &v->output->ws[v->workspace];
	if (*root)
		return;                 /* a tree with no visible leaves; leave it */
	*root = ly_leaf(v);
	if (!*root)
		return;                 /* out of memory: stay floating */
	v->node = *root;
	v->floating = false;
	v->float_follow = false;        /* back in the tree: the tree decides */
	wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
}

void grab_end(struct aro_server *s)
{
	struct aro_view *v = s->grabbed;

	if (v && s->cursor_mode == ARO_CURSOR_MOVE &&
	    s->grab_from_tree && v->floating)
		view_retile(s, v);

	drop_clear(s);
	s->cursor_mode = ARO_CURSOR_PASSTHROUGH;
	s->grabbed = NULL;
	s->grab_split = NULL;
	s->grab_from_tree = false;
	s->grab_tore_out = false;

	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");

	/* update titles after drag */
	aro_arrange(s);
}

void grab_begin(struct aro_server *s, struct aro_view *v,
                       enum aro_cursor_mode mode, uint32_t edges)
{
	if (!v || !v->mapped || v->fullscreen)
		return;

	/* grabs read geo.cur; a window still sliding in is drawn elsewhere */
	if (v->output)
		slide_finish(v->output);

	/* begin tiled resize */
	if (mode == ARO_CURSOR_RESIZE && !v->floating) {
		if (!v->node)
			return;
		ly_node *split = boundary_pick(v->node, ly_edge_from_mask(edges));
		if (!split)
			return;
		s->grab_split = split;
		s->grab_ratio = split->ratio;
		mode = ARO_CURSOR_RESIZE_TILE;
	}

	aro_focus(s, v);

	/* edge drag overrides float_follow */
	if (mode == ARO_CURSOR_RESIZE && v->floating)
		v->float_follow = false;

	s->cursor_mode = mode;
	s->grabbed = v;
	s->grab_x = s->cursor->x;
	s->grab_y = s->cursor->y;
	s->grab_box = view_target(v);
	s->grab_edges = edges;
	s->grab_from_tree = !v->floating;
	s->grab_tore_out = false;

	const char *shape = "grabbing";
	if (mode == ARO_CURSOR_RESIZE_TILE)
		shape = (s->grab_split->dir == LY_ROW) ? "ew-resize" : "ns-resize";
	else if (mode == ARO_CURSOR_RESIZE)
		shape = "se-resize";
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, shape);
}

static void grab_motion_move(struct aro_server *s)
{
	struct aro_view *v = s->grabbed;
	double dx = s->cursor->x - s->grab_x;
	double dy = s->cursor->y - s->grab_y;

	if (s->grab_from_tree && !s->grab_tore_out) {
		double ax = dx < 0 ? -dx : dx;
		double ay = dy < 0 ? -dy : dy;
		if (ax < s->cfg.theme.drag_tear && ay < s->cfg.theme.drag_tear)
			return;

		s->grab_tore_out = true;
		view_set_floating(s, v, true);

		/* keep drag position when tearing out */
		v->fbox = s->grab_box;
	}

	if (!v->floating)
		return;

	v->fbox = (ly_box){ s->grab_box.x + (int)dx, s->grab_box.y + (int)dy,
	                    s->grab_box.w, s->grab_box.h };

	/* no animation while dragging */
	anim_box_set(&v->geo, v->fbox);

	if (s->grab_from_tree)
		drop_update(s);
}

static void grab_motion_resize(struct aro_server *s)
{
	struct aro_view *v = s->grabbed;
	int dx = (int)(s->cursor->x - s->grab_x);
	int dy = (int)(s->cursor->y - s->grab_y);
	ly_box b = s->grab_box;

	if (s->grab_edges & WLR_EDGE_LEFT) {
		b.x += dx;
		b.w -= dx;
	} else if (s->grab_edges & WLR_EDGE_RIGHT) {
		b.w += dx;
	}
	if (s->grab_edges & WLR_EDGE_TOP) {
		b.y += dy;
		b.h -= dy;
	} else if (s->grab_edges & WLR_EDGE_BOTTOM) {
		b.h += dy;
	}

	/* clamp to minimum float size */
	if (b.w < s->cfg.theme.float_min_w) {
		if (s->grab_edges & WLR_EDGE_LEFT)
			b.x = s->grab_box.x + s->grab_box.w - s->cfg.theme.float_min_w;
		b.w = s->cfg.theme.float_min_w;
	}
	if (b.h < s->cfg.theme.float_min_h) {
		if (s->grab_edges & WLR_EDGE_TOP)
			b.y = s->grab_box.y + s->grab_box.h - s->cfg.theme.float_min_h;
		b.h = s->cfg.theme.float_min_h;
	}

	v->fbox = b;
	anim_box_set(&v->geo, b);
}

void grab_motion(struct aro_server *s)
{
	if (!s->grabbed || !s->grabbed->mapped) {
		grab_end(s);
		return;
	}

	drag_icon_update(s);

	if (s->cursor_mode == ARO_CURSOR_MOVE)
		grab_motion_move(s);
	else if (s->cursor_mode == ARO_CURSOR_RESIZE_TILE)
		grab_motion_resize_tile(s);
	else
		grab_motion_resize(s);

	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

/* drop grab if view disappears */
void grab_forget(struct aro_server *s, struct aro_view *v)
{
	if (s->grabbed != v)
		return;
	s->grabbed = NULL;
	s->grab_split = NULL;
	s->cursor_mode = ARO_CURSOR_PASSTHROUGH;
	drop_clear(s);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
}

/* client move/resize requests */
void view_request_move(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_move);
	(void)data;
	grab_begin(v->server, v, ARO_CURSOR_MOVE, 0);
}

void view_request_resize(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_resize);
	struct wlr_xdg_toplevel_resize_event *ev = data;
	grab_begin(v->server, v, ARO_CURSOR_RESIZE, ev->edges);
}

/* ── view lifecycle ────────────────────────────────────────────────────── */

void view_map(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, map);
	struct aro_server *s = v->server;
	(void)data;

	/* new windows open on focused output */
	struct aro_output *o = aro_focused_output(s);
	if (!o) {
		wlr_log(WLR_ERROR, "a window mapped with no output to put it on");
		return;
	}
	v->output = o;
	v->mapped = true;
	v->float_follow = false;        /* a remap starts from scratch */

	/* evaluate rules before placement */
	const char *app_id = view_app_id(v), *title = view_title(v);
	wlr_log(WLR_INFO, "map: app_id=\"%s\" title=\"%s\" type=%s",
	        app_id ? app_id : "", title ? title : "", view_type(v));

	struct q_rule_result r;
	config_rules_eval(&s->cfg, app_id, title, view_type(v), &r);
	v->rule_last = r;

	struct aro_output *ro = r.monitor ? rule_output(s, r.monitor) : NULL;
	if (ro)
		v->output = o = ro;
	rule_chrome(v, &r);             /* before any size is worked out */

	const int ws = r.workspace != Q_RULE_UNSET ? r.workspace : o->cur_ws;
	v->scratch = v->stashed = r.scratch == 1;       /* starts hidden */
	const bool here = ws == o->cur_ws && !v->stashed;
	v->workspace = ws;

	/* rule overrides float heuristic */
	bool floating = v->scratch || r.sticky == 1 || (r.floating != Q_RULE_UNSET
	              ? r.floating == 1
	              : v->impl->wants_float && v->impl->wants_float(v));

	if (floating) {
		/* floating windows skip the tree */
		v->floating = true;
		v->fbox = float_box_for(v, true);
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
		view_float_configure_once(v);
		rule_place(v, &r);
		v->sticky = r.sticky == 1 && !v->scratch;
	} else {
		ly_node *target = s->focused &&
		                  s->focused->output == o &&
		                  s->focused->workspace == ws &&
		                  s->focused->node
		                ? s->focused->node : NULL;
		/* pending split only applies here */
		v->node = tree_insert(s, o, ws, v, target,
		                      here ? s->pending_split : LY_ROW,
		                      here && s->split_forced);
		if (!v->node) {
			wlr_log(WLR_ERROR, "out of memory inserting a window");
			return;
		}
		/* consume one-shot split only if placed here */
		if (here) {
			s->pending_split = LY_ROW;      /* one-shot, like i3 */
			s->split_forced = false;
		}
	}

	/* handle pre-map fullscreen request */
	if (!v->stashed && (r.fullscreen == 1 ||
	    (v->impl->wants_fullscreen && v->impl->wants_fullscreen(v))))
		view_set_fullscreen(s, v, true);

	ly_box t = view_target(v);
	ly_box small = {
		.x = t.x + (int)(t.w * (1 - s->cfg.theme.open_scale) / 2),
		.y = t.y + (int)(t.h * (1 - s->cfg.theme.open_scale) / 2),
		.w = (int)(t.w * s->cfg.theme.open_scale),
		.h = (int)(t.h * s->cfg.theme.open_scale),
	};
	anim_box_set(&v->geo, small);

	ui_frame_clip_content(v);
	ftl_create(v);
	mru_add(s, v);
	overview_rebuild(s);

	/* don't focus hidden windows */
	view_set_visible(v, here);
	if (here)
		aro_focus(s, v);
	aro_arrange(s);
}

void view_unmap(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, unmap);
	struct aro_server *s = v->server;
	(void)data;

	if (!v->mapped)
		return;

	/* before anything moves: it starts where the frame is drawn */
	if (view_visible(v) && !v->output->slide.active && !aro_locked(s) &&
	    !overview_shown(s))
		ghost_spawn(s, v, view_draw_box(v, aro_now_ms()));

	v->mapped = false;
	view_set_visible(v, false);
	v->scratch = v->stashed = false;        /* rules decide again on the next map */
	grab_forget(s, v);
	ftl_destroy(v);
	mru_remove(s, v);
	overview_rebuild(s);

	/* drop tiled resize grab if windows close */
	if (s->cursor_mode == ARO_CURSOR_RESIZE_TILE)
		grab_forget(s, s->grabbed);

	/* only close tree leaf if present */
	ly_node *next = view_detach(v);
	v->floating = false;
	v->float_follow = false;
	if (v->fullscreen) {
		v->fullscreen = false;
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
		ui_frame_fullscreen(v, false);
	}

	if (s->focused == v) {
		s->focused = NULL;
		if (!next) {
			struct aro_output *fo = aro_focused_output(s);
			ly_node *root = fo ? fo->ws[fo->cur_ws] : NULL;
			next = root ? ly_first_leaf(root) : NULL;
		}
		aro_focus(s, next ? next->user : NULL);
	}
	aro_arrange(s);
}

static void view_commit(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, commit);
	(void)data;

	if (v->toplevel->base->initial_commit) {
		/* let client choose initial size */
		wlr_xdg_toplevel_set_size(v->toplevel, 0, 0);
	}

	/* follow client float commits */
	view_float_follow(v);

	/* clip rounded corners */
	ui_frame_clip_content(v);
	overview_view_commit(v->server, v);
}

void view_set_title(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, set_title);
	(void)data;
	if (!v->mapped || !v->output)
		return;
	ipc_notify(v->server);

	/* reapply rules on title change */
	view_rules_reapply(v);
	ftl_update_ids(v);

	if (v->fullscreen || !v->output)
		return;
	ui_frame_title(v, view_target(v).w, v->output->scale);
	if (v->output->bar.tree)
		bar_update(&v->output->bar, v->output);
}

/* app_id (WM_CLASS on X11) changed after map: rules and taskbars follow */
void view_set_app_id(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, set_app_id);
	(void)data;
	if (!v->mapped || !v->output)
		return;
	ipc_notify(v->server);
	view_rules_reapply(v);
	ftl_update_ids(v);
}

static void view_destroy(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, destroy);
	(void)data;

	ftl_destroy(v);
	mru_remove(v->server, v);

	if (v->server->focused == v)
		v->server->focused = NULL;
	grab_forget(v->server, v);

	qtext_finish(&v->title);

	/* destroy our frame tree */
	if (v->frame_tree)
		wlr_scene_node_destroy(&v->frame_tree->node);

	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->link);
	free(v);
}

/* popups */
struct aro_popup {
	struct wlr_xdg_popup *popup;
	struct aro_server *server;
	struct wlr_scene_tree *parent_tree;

	struct wl_listener commit;
	struct wl_listener destroy;
};

/* unconstrain popup after first commit */
static void popup_commit(struct wl_listener *l, void *data)
{
	struct aro_popup *p = wl_container_of(l, p, commit);
	(void)data;

	if (!p->popup->base->initial_commit)
		return;

	/* keep popup on screen */
	int lx = 0, ly = 0;
	wlr_scene_node_coords(&p->parent_tree->node, &lx, &ly);

	struct aro_output *o = output_at(p->server, lx, ly);
	if (!o)
		return;

	struct wlr_box box = {
		.x = o->box.x - lx,
		.y = o->box.y - ly,
		.width = o->box.w,
		.height = o->box.h,
	};
	wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
}

static void popup_destroy(struct wl_listener *l, void *data)
{
	struct aro_popup *p = wl_container_of(l, p, destroy);
	(void)data;
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

/* give a popup its scene node under parent_tree, and keep it on screen */
void popup_track(struct aro_server *s, struct wlr_xdg_popup *popup,
                        struct wlr_scene_tree *parent_tree)
{
	struct wlr_scene_tree *tree =
		wlr_scene_xdg_surface_create(parent_tree, popup->base);
	if (!tree)
		return;
	popup->base->data = tree;

	struct aro_popup *p = calloc(1, sizeof *p);
	if (!p)
		return;
	p->popup = popup;
	p->server = s;
	p->parent_tree = parent_tree;

	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->base->events.destroy, &p->destroy);
}

static void new_xdg_popup(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_xdg_popup);
	struct wlr_xdg_popup *popup = data;

	/* no parent yet: a layer surface adopts it, see layer_new_popup */
	if (!popup->parent)
		return;

	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent || !parent->data)
		return;
	popup_track(s, popup, parent->data);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct aro_view *v = calloc(1, sizeof *v);
	if (!v)
		return;

	v->server = s;
	v->impl = &xdg_impl;
	v->csd = true;      /* until a decoration says otherwise */
	v->toplevel = toplevel;
	wl_list_init(&v->mru_link);

	if (!ui_frame_create(v, s->l_tiled)) {
		free(v);
		return;
	}
	v->surface_tree = wlr_scene_xdg_surface_create(v->content, toplevel->base);
	view_set_visible(v, false);

	toplevel->base->data = v->popups;   /* where this window's popups hang */
	v->frame_tree->node.data = v;      /* view_at() walks up looking for this */

	v->map.notify = view_map;
	wl_signal_add(&toplevel->base->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &v->commit);
	v->set_title.notify = view_set_title;
	wl_signal_add(&toplevel->events.set_title, &v->set_title);
	v->set_app_id.notify = view_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &v->set_app_id);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &v->request_fullscreen);
	v->request_move.notify = view_request_move;
	wl_signal_add(&toplevel->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &v->request_resize);
	v->destroy.notify = view_destroy;
	wl_signal_add(&toplevel->events.destroy, &v->destroy);

	v->id = ++s->next_view_id;
	wl_list_insert(&s->views, &v->link);
}

/* ── decorations ───────────────────────────────────────────────────────── */
/* request server-side decorations */
struct aro_decoration {
	struct aro_server *server;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

/* find view for toplevel */
static struct aro_view *view_for_toplevel(struct aro_server *s,
                                             struct wlr_xdg_toplevel *t)
{
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->toplevel == t)
			return v;
	return NULL;
}

static void decoration_set_mode(struct aro_decoration *d)
{
	/* wait for initialized surface */
	if (!d->deco->toplevel->base->initialized)
		return;

	wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

	/* enable our header when server-side */
	struct aro_view *v = view_for_toplevel(d->server, d->deco->toplevel);
	if (v && v->csd) {
		v->csd = false;
		aro_arrange(d->server);
	}
}

static void decoration_request_mode(struct wl_listener *l, void *data)
{
	struct aro_decoration *d = wl_container_of(l, d, request_mode);
	(void)data;
	decoration_set_mode(d);
}

static void decoration_destroy(struct wl_listener *l, void *data)
{
	struct aro_decoration *d = wl_container_of(l, d, destroy);
	(void)data;
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

static void new_decoration(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;

	struct aro_decoration *d = calloc(1, sizeof *d);
	if (!d)
		return;
	d->deco = deco;
	d->server = s;

	d->request_mode.notify = decoration_request_mode;
	wl_signal_add(&deco->events.request_mode, &d->request_mode);
	d->destroy.notify = decoration_destroy;
	wl_signal_add(&deco->events.destroy, &d->destroy);

	decoration_set_mode(d);
}

/* ── selections and drag-and-drop ──────────────────────────────────────── */

/* allow selection */
static void request_set_selection(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

/* primary selection */
static void request_set_primary_selection(struct wl_listener *l, void *data)
{
	struct aro_server *s =
		wl_container_of(l, s, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

static void request_start_drag(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_start_drag);
	struct wlr_seat_request_start_drag_event *ev = data;

	/* validate drag serial */
	if (wlr_seat_validate_pointer_grab_serial(s->seat, ev->origin, ev->serial))
		wlr_seat_start_pointer_drag(s->seat, ev->drag, ev->serial);
	else
		wlr_data_source_destroy(ev->drag->source);
}

/* drag icon follows cursor */
void drag_icon_update(struct aro_server *s)
{
	if (s->drag_icon)
		wlr_scene_node_set_position(&s->drag_icon->node,
		                            (int)s->cursor->x, (int)s->cursor->y);
}

static void drag_icon_destroy(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, drag_icon_destroy);
	(void)data;
	/* clear drag icon */
	s->drag_icon = NULL;
	wl_list_remove(&s->drag_icon_destroy.link);
	wl_list_init(&s->drag_icon_destroy.link);
}

static void start_drag(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, start_drag);
	struct wlr_drag *drag = data;

	if (!drag->icon)
		return;

	/* drag icon on overlay */
	s->drag_icon = wlr_scene_drag_icon_create(s->l_overlay, drag->icon);
	drag_icon_update(s);

	s->drag_icon_destroy.notify = drag_icon_destroy;
	wl_signal_add(&drag->icon->events.destroy, &s->drag_icon_destroy);
}

/* ── live config reload ────────────────────────────────────────────────── */

/* live config reload */
/* show config errors */
static void notify_config_errors(struct aro_server *s)
{
	const int cap = 4;
	int n = s->cfg.nerrors;

	/* clear old error toasts */
	notify_clear_errors(s);

	for (int i = 0; i < n && i < cap; i++)
		notify(s, NOTIFY_ERROR, "%s", s->cfg.errors[i]);
	if (n > cap)
		notify(s, NOTIFY_ERROR, "...and %d more config problems", n - cap);
}

/* wallpaper.c's problems, as toasts */
static void wallpaper_report(void *data, const char *msg)
{
	notify(data, NOTIFY_ERROR, "%s", msg);
}

/* load the config's cursor theme, or the session's; export it to children */
static void cursor_theme_apply(struct aro_server *s)
{
	const char *theme = s->cfg.cursor_theme ? s->cfg.cursor_theme
	                                        : s->env_cursor_theme;
	int size = s->cfg.cursor_size ? s->cfg.cursor_size : s->env_cursor_size;

	bool same = theme && s->xcursor_theme ?
	            !strcmp(theme, s->xcursor_theme) : theme == s->xcursor_theme;
	if (s->xcursor_mgr && same && size == s->xcursor_size)
		return;

	struct wlr_xcursor_manager *mgr = wlr_xcursor_manager_create(theme, size);
	if (!mgr) {
		wlr_log(WLR_ERROR, "could not load cursor theme %s",
		        theme ? theme : "default");
		return;
	}
	wlr_log(WLR_INFO, "cursor: theme=%s size=%d",
	        theme ? theme : "default", size);

	/* a missing theme silently becomes wlroots' built-in one, named "default" */
	if (theme && wlr_xcursor_manager_load(mgr, 1)) {
		struct wlr_xcursor_manager_theme *mt;
		wl_list_for_each(mt, &mgr->scaled_themes, link) {
			if (mt->theme && strcmp(mt->theme->name, theme) != 0) {
				wlr_log(WLR_ERROR, "cursor theme '%s' not found, "
				        "using the built-in cursors", theme);
				if (s->xcursor_mgr)
					notify(s, NOTIFY_ERROR, "cursor theme '%s' not found",
					       theme);
			}
			break;
		}
	}

	char buf[16];
	snprintf(buf, sizeof buf, "%d", size);
	setenv("XCURSOR_SIZE", buf, true);
	if (theme)
		setenv("XCURSOR_THEME", theme, true);
	else
		unsetenv("XCURSOR_THEME");

	struct wlr_xcursor_manager *old = s->xcursor_mgr;
	s->xcursor_mgr = mgr;
	free(s->xcursor_theme);
	s->xcursor_theme = theme ? strdup(theme) : NULL;
	s->xcursor_size = size;
	if (!old)
		return;         /* startup: nothing drawn with it yet */

	/* swap the image off the old theme, then let the client resend its own */
	wlr_cursor_set_xcursor(s->cursor, mgr, "default");
	wlr_xcursor_manager_destroy(old);
	wlr_seat_pointer_clear_focus(s->seat);
	pointer_motion_common(s, aro_now_ms());
}

/* aroctl's wallpaper if it set one, else the config's */
static void wallpaper_sync(struct aro_server *s)
{
	if (s->wallpaper_set)
		wallpaper_apply(&s->wallpaper, s->wallpaper_mode, s->wallpaper_file);
	else
		wallpaper_apply(&s->wallpaper, s->cfg.wallpaper, s->cfg.wallpaper_file);
}

bool aro_wallpaper_set(struct aro_server *s, enum q_wallpaper mode, const char *file)
{
	char *copy = NULL;
	if (mode == Q_WALLPAPER_FILE && (!file || !(copy = strdup(file))))
		return false;
	free(s->wallpaper_file);
	s->wallpaper_file = copy;
	s->wallpaper_mode = mode;
	s->wallpaper_set = true;
	wallpaper_save(&s->wallpaper, mode, copy, s->cfg.wallpaper, s->cfg.wallpaper_file);
	wallpaper_sync(s);
	return true;
}

static void config_reload(struct aro_server *s)
{
	struct aro_config nc;
	config_defaults(&nc);
	config_load(&nc, s->cfg_path);

	/* a changed wallpaper line wins over aroctl's */
	const bool wp_changed = nc.wallpaper != s->cfg.wallpaper ||
		(nc.wallpaper == Q_WALLPAPER_FILE &&
		 strcmp(nc.wallpaper_file ? nc.wallpaper_file : "",
		        s->cfg.wallpaper_file ? s->cfg.wallpaper_file : ""));
	if (wp_changed) {
		s->wallpaper_set = false;
		wallpaper_forget(&s->wallpaper);
	}

	/* old layouts, for changed-only reset */
	enum q_layout was[ARO_MAX_WS];
	for (int i = 0; i < ARO_MAX_WS; i++)
		was[i] = config_ws_layout(&s->cfg, i);

	config_finish(&s->cfg);
	s->cfg = nc;

	/* changed config layout wins over runtime override */
	for (int i = 0; i < ARO_MAX_WS; i++) {
		if (config_ws_layout(&s->cfg, i) == was[i])
			continue;
		struct aro_output *lo;
		wl_list_for_each(lo, &s->outputs, link)
			lo->ws_layout[i] = Q_LAYOUT_INHERIT;
		wl_list_for_each(lo, &s->outputs_off, link)
			lo->ws_layout[i] = Q_LAYOUT_INHERIT;
		s->orphan_ws_layout[i] = Q_LAYOUT_INHERIT;
	}

	wlr_log(WLR_INFO, "config reloaded");

	/* reload keymaps */
	struct aro_keyboard *kb;
	wl_list_for_each(kb, &s->keyboards, link)
		if (!kb->is_virtual)
			apply_keymap(s, kb->wlr_keyboard);

	cursor_theme_apply(s);

	/* reapply touchpad settings */
	struct aro_pointer *ptr;
	wl_list_for_each(ptr, &s->pointers, link)
		pointer_configure(s, ptr->dev);

	/* update background color */
	if (s->root_bg) {
		float bg[4];
		ui_color(s->cfg.theme.bg, bg);
		wlr_scene_rect_set_color(s->root_bg, bg);
	}

	/* retheme views */
	blur_strength_apply(s);
	layers_blur_update(s);
	bgeffect_sync(s);
	struct aro_view *v, *vtmp;
	wl_list_for_each(v, &s->views, link)
		ui_frame_retheme(v);

	/* reapply rules */
	wl_list_for_each_safe(v, vtmp, &s->views, link)
		view_rules_reapply(v);

	ui_preview_retheme(&s->preview);

	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		bar_retheme(&o->bar);

	/* update backdrop before arrange */
	update_backdrop(s);
	aro_arrange(s);

	notify_retheme(s);
	prompt_retheme(s);
	switcher_retheme(s);
	picker_retheme(s);
	ghost_drop(s, NULL);    /* drawn with the old theme */
	overview_rebuild(s);
	notify_config_errors(s);

	/* reapply monitor blocks after errors */
	monitors_reapply(s);

	/* after the error toasts are redrawn, or they would clear its own */
	wallpaper_sync(s);

	for (int i = 0; i < s->cfg.nexec_always; i++)
		config_spawn(s->cfg.exec_always[i]);
}

/* ── exported for ipc.c ────────────────────────────────────────────────── */

void aro_run_action(struct aro_server *s, const struct q_bind *b)
{
	run_action(s, b);
}

void aro_config_reload(struct aro_server *s)
{
	config_reload(s);
}

ly_box aro_view_box(struct aro_view *v)
{
	return view_target(v);
}

enum q_layout aro_ws_layout(struct aro_output *o, int ws)
{
	return ws_layout(o->server, o, ws);
}

const char *aro_view_type(struct aro_view *v)
{
	return view_type(v);
}

/* watch config directory */
static int config_fd_event(int fd, uint32_t mask, void *data)
{
	struct aro_server *s = data;
	(void)mask;

	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const char *want = strrchr(s->cfg_path, '/');
	want = want ? want + 1 : s->cfg_path;

	bool hit = false;
	ssize_t n;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		for (char *p = buf; p < buf + n; ) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && strcmp(ev->name, want) == 0)
				hit = true;
			p += sizeof *ev + ev->len;
		}
	}

	if (hit)
		config_reload(s);
	return 0;
}

static void config_watch_start(struct aro_server *s)
{
	s->cfg_fd = -1;
	s->cfg_wd = -1;
	s->cfg_path = config_path();
	if (!s->cfg_path)
		return;

	s->cfg_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (s->cfg_fd < 0) {
		wlr_log(WLR_ERROR, "inotify unavailable; config will not reload");
		return;
	}

	/* watch config directory */
	char dir[512];
	snprintf(dir, sizeof dir, "%s", s->cfg_path);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';

	s->cfg_wd = inotify_add_watch(s->cfg_fd, dir,
	                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (s->cfg_wd < 0) {
		/* missing config dir is fine */
		wlr_log(WLR_INFO, "not watching %s for config changes", dir);
		close(s->cfg_fd);
		s->cfg_fd = -1;
		return;
	}

	s->cfg_source = wl_event_loop_add_fd(s->loop, s->cfg_fd, WL_EVENT_READABLE,
	                                     config_fd_event, s);
	wlr_log(WLR_INFO, "watching %s for config changes", s->cfg_path);
}

static void config_watch_stop(struct aro_server *s)
{
	if (s->cfg_source)
		wl_event_source_remove(s->cfg_source);
	if (s->cfg_fd >= 0)
		close(s->cfg_fd);
	free(s->cfg_path);
	s->cfg_source = NULL;
	s->cfg_fd = -1;
	s->cfg_path = NULL;
}

/* ── main ──────────────────────────────────────────────────────────────── */

/* aro -c: 0 ok, 1 errors, 2 unreadable */
static int config_check(const char *arg)
{
	char *owned = arg ? NULL : config_path();
	const char *path = arg ? arg : owned;
	if (!path) {
		fprintf(stderr, "aro: no config path (HOME and XDG_CONFIG_HOME "
		        "are both unset)\n");
		return 2;
	}

	/* missing file is an error here */
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "aro: %s: %s\n", path, strerror(errno));
		free(owned);
		return 2;
	}
	fclose(f);

	struct aro_config c;
	config_defaults(&c);
	config_load(&c, path);

	/* file:line: message */
	for (int i = 0; i < c.nerrors; i++) {
		const char *e = c.errors[i];
		if (!strncmp(e, "config:", 7))
			fprintf(stderr, "%s:%s\n", path, e + 7);
		else
			fprintf(stderr, "%s: %s\n", path, e);
	}
	int n = c.nerrors;
	if (!n)
		printf("%s: ok\n", path);
	config_finish(&c);
	free(owned);
	return n ? 1 : 0;
}

/* hand our display to systemd and dbus, so portals started by them find it */
static void session_env_export(void)
{
	static const char *const vars[] = {
		"WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP",
		"XCURSOR_THEME", "XCURSOR_SIZE",
	};
	char cmd[256] = "dbus-update-activation-environment --systemd";
	size_t len = strlen(cmd);
	for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
		if (!getenv(vars[i]))
			continue;       /* DISPLAY, without XWayland */
		len += snprintf(cmd + len, sizeof cmd - len, " %s", vars[i]);
	}
	config_spawn(cmd);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [-s startup-command] [-m logo|alt] [-l log-file|none]\n"
	        "       %s -c [config-file]    check a config and exit\n",
	        argv0, argv0);
}

int main(int argc, char *argv[])
{
	const char *startup = NULL;
	const char *modkey_override = NULL;      /* -m beats the config file */
	const char *log_arg = NULL;              /* -l; NULL = the default file */
	bool check = false;
	const char *check_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			startup = argv[++i];
		else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
			modkey_override = argv[++i];
		else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
			log_arg = argv[++i];
		else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--check")) {
			check = true;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				check_path = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (check) {
		wlr_log_init(WLR_SILENT, NULL);
		return config_check(check_path);
	}

	/* before we set WAYLAND_DISPLAY ourselves */
	const bool nested = getenv("WAYLAND_DISPLAY") ||
	                    getenv("WAYLAND_SOCKET") || getenv("DISPLAY");
	logfile_init(log_arg, nested, WLR_DEBUG);

	struct aro_server s = { 0 };
	s.cfg_fd = -1;          /* not stdin, if we give up before the watch starts */
	int ret = 0;
	config_defaults(&s.cfg);
	config_load(&s.cfg, NULL);

	/* -m overrides config modkey */
	if (modkey_override) {
		uint32_t m = 0;
		if (!strcmp(modkey_override, "alt"))
			m = WLR_MODIFIER_ALT;
		else if (!strcmp(modkey_override, "logo") ||
		         !strcmp(modkey_override, "super"))
			m = WLR_MODIFIER_LOGO;
		else
			wlr_log(WLR_ERROR, "-m: expected logo or alt");

		if (m && m != s.cfg.modkey)
			config_set_modkey(&s.cfg, m);
	}

	/* the session's cursor, before cursor_theme_apply() exports ours */
	const char *env_theme = getenv("XCURSOR_THEME");
	const char *env_size = getenv("XCURSOR_SIZE");
	s.env_cursor_theme = env_theme && *env_theme ? strdup(env_theme) : NULL;
	s.env_cursor_size = env_size ? atoi(env_size) : 0;
	if (s.env_cursor_size <= 0)
		s.env_cursor_size = 24;

	s.pending_split = LY_ROW;
	for (int i = 0; i < ARO_MAX_WS; i++)
		s.orphan_ws_layout[i] = Q_LAYOUT_INHERIT;
	wl_list_init(&s.outputs);
	wl_list_init(&s.outputs_off);
	wl_list_init(&s.views);
	wl_list_init(&s.keyboards);
	wl_list_init(&s.pointers);
	wl_list_init(&s.layers);
	wl_list_init(&s.notifications);

	s.display = wl_display_create();
	s.loop = wl_display_get_event_loop(s.display);

	s.backend = wlr_backend_autocreate(s.loop, &s.session);
	if (!s.backend) {
		wlr_log(WLR_ERROR, "no backend");
		return 1;
	}

	/* create renderer */
#ifdef ARO_EFFECTS
	s.renderer = fx_renderer_create(s.backend);
	if (!s.renderer) {
		wlr_log(WLR_ERROR, "SceneFX renderer failed to start. "
		        "Rebuild with -Deffects=false to use the wlroots renderer.");
		return 1;
	}
#else
	s.renderer = wlr_renderer_autocreate(s.backend);
	if (!s.renderer) {
		wlr_log(WLR_ERROR, "no renderer available");
		return 1;
	}
#endif

	if (!wlr_renderer_init_wl_display(s.renderer, s.display)) {
		wlr_log(WLR_ERROR, "could not bind the renderer to the display");
		return 1;
	}
	protocols_init(&s);

	s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
	if (!s.allocator) {
		wlr_log(WLR_ERROR, "no allocator available");
		return 1;
	}

#ifdef ARO_EFFECTS
	wlr_log(WLR_INFO, "renderer: scenefx (GLES2), effects on");
#else
	wlr_log(WLR_INFO, "renderer: wlroots autocreate, effects off");
#endif

	s.compositor = wlr_compositor_create(s.display, 6, s.renderer);
	wlr_subcompositor_create(s.display);
	wlr_data_device_manager_create(s.display);

	/* required client globals */
	s.output_layout = wlr_output_layout_create(s.display);
	if (!s.output_layout) {
		wlr_log(WLR_ERROR, "could not create the output layout");
		return 1;
	}
	wlr_xdg_output_manager_v1_create(s.display, s.output_layout);

	/* output management */
	s.output_mgr = wlr_output_manager_v1_create(s.display);
	if (s.output_mgr) {
		s.output_mgr_apply.notify = output_mgr_apply;
		wl_signal_add(&s.output_mgr->events.apply, &s.output_mgr_apply);
		s.output_mgr_test.notify = output_mgr_test;
		wl_signal_add(&s.output_mgr->events.test, &s.output_mgr_test);
	} else {
		wlr_log(WLR_ERROR, "output management unavailable; "
		        "wlr-randr and kanshi will not work");
	}
	wlr_viewporter_create(s.display);
	wlr_fractional_scale_manager_v1_create(s.display, 1);
	wlr_single_pixel_buffer_manager_v1_create(s.display);
	wlr_primary_selection_v1_device_manager_create(s.display);

	/* xdg-foreign for out-of-process dialogs */
	struct wlr_xdg_foreign_registry *foreign =
		wlr_xdg_foreign_registry_create(s.display);
	if (foreign) {
		wlr_xdg_foreign_v1_create(s.display, foreign);
		wlr_xdg_foreign_v2_create(s.display, foreign);
	} else {
		wlr_log(WLR_ERROR, "xdg-foreign unavailable; out-of-process "
		                   "dialogs will not open");
	}
	wlr_screencopy_manager_v1_create(s.display);
	s.scene = wlr_scene_create();
	if (!s.scene) {
		wlr_log(WLR_ERROR, "could not create the scene graph");
		return 1;
	}
	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
	colour_init(&s);
	blur_strength_apply(&s);

	/* stacking layers */
	float bg[4];
	ui_color(s.cfg.theme.bg, bg);
	s.root_bg = wlr_scene_rect_create(&s.scene->tree, 1920, 1080, bg);
	s.l_background = wlr_scene_tree_create(&s.scene->tree);
	s.l_bottom     = wlr_scene_tree_create(&s.scene->tree);
	s.l_tiled      = wlr_scene_tree_create(&s.scene->tree);
	s.l_preview    = wlr_scene_tree_create(&s.scene->tree);
	s.l_float      = wlr_scene_tree_create(&s.scene->tree);
	s.l_overview   = wlr_scene_tree_create(&s.scene->tree);
	s.l_unmanaged  = wlr_scene_tree_create(&s.scene->tree);
	s.l_bar        = wlr_scene_tree_create(&s.scene->tree);
	s.l_top        = wlr_scene_tree_create(&s.scene->tree);
	s.l_fullscreen = wlr_scene_tree_create(&s.scene->tree);
	s.l_notify     = wlr_scene_tree_create(&s.scene->tree);
	s.l_overlay    = wlr_scene_tree_create(&s.scene->tree);
	if (!s.root_bg || !s.l_background || !s.l_bottom || !s.l_tiled ||
	    !s.l_preview || !s.l_float || !s.l_overview || !s.l_unmanaged || !s.l_bar || !s.l_top || !s.l_fullscreen || !s.l_notify ||
	    !s.l_overlay) {
		wlr_log(WLR_ERROR, "could not build the scene layers");
		return 1;
	}

	/* bars are per-output */

	switcher_init(&s);
	picker_init(&s);
	ghost_init(&s);

	if (!ui_preview_create(&s.preview, &s)) {
		wlr_log(WLR_ERROR, "could not build the drop indicator");
		return 1;
	}

	s.clock_timer = wl_event_loop_add_timer(s.loop, clock_tick, &s);
	wl_event_source_timer_update(s.clock_timer, 1000);

	s.new_output.notify = new_output;
	wl_signal_add(&s.backend->events.new_output, &s.new_output);

	s.xdg_shell = wlr_xdg_shell_create(s.display, 3);
	s.new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_xdg_toplevel);
	toplevel_icons_init(&s);
	s.new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);

#ifdef ARO_XWAYLAND
	/* start XWayland lazily */
	wl_list_init(&s.unmanaged);
	s.xwayland = wlr_xwayland_create(s.display, s.compositor, true);
	if (!s.xwayland) {
		wlr_log(WLR_ERROR, "xwayland failed to start; X11 clients will not run");
	} else {
		s.new_xwayland_surface.notify = new_xwayland_surface;
		wl_signal_add(&s.xwayland->events.new_surface, &s.new_xwayland_surface);
		s.xwayland_ready.notify = xwayland_ready;
		wl_signal_add(&s.xwayland->events.ready, &s.xwayland_ready);

		/* set DISPLAY early */
		setenv("DISPLAY", s.xwayland->display_name, true);
	}
#endif

	s.layer_shell = wlr_layer_shell_v1_create(s.display, 4);
	if (!s.layer_shell) {
		wlr_log(WLR_ERROR, "could not create the layer shell");
		return 1;
	}
	lock_init(&s);
	idle_init(&s);
	s.new_layer_surface.notify = new_layer_surface;
	wl_signal_add(&s.layer_shell->events.new_surface, &s.new_layer_surface);

	s.xdg_decoration = wlr_xdg_decoration_manager_v1_create(s.display);
	s.new_decoration.notify = new_decoration;
	wl_signal_add(&s.xdg_decoration->events.new_toplevel_decoration,
	              &s.new_decoration);

	s.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(s.cursor, s.output_layout);
	cursor_theme_apply(&s);

	s.cursor_motion.notify = cursor_motion;
	wl_signal_add(&s.cursor->events.motion, &s.cursor_motion);
	s.cursor_motion_abs.notify = cursor_motion_abs;
	wl_signal_add(&s.cursor->events.motion_absolute, &s.cursor_motion_abs);
	s.cursor_button.notify = cursor_button;
	wl_signal_add(&s.cursor->events.button, &s.cursor_button);
	s.cursor_axis.notify = cursor_axis;
	wl_signal_add(&s.cursor->events.axis, &s.cursor_axis);
	s.cursor_frame.notify = cursor_frame;
	wl_signal_add(&s.cursor->events.frame, &s.cursor_frame);

	s.seat = wlr_seat_create(s.display, "seat0");
	s.new_input.notify = new_input;
	wl_signal_add(&s.backend->events.new_input, &s.new_input);
	s.request_cursor.notify = request_cursor;
	wl_signal_add(&s.seat->events.request_set_cursor, &s.request_cursor);
	s.request_set_selection.notify = request_set_selection;
	wl_signal_add(&s.seat->events.request_set_selection, &s.request_set_selection);
	s.request_set_primary_selection.notify = request_set_primary_selection;
	wl_signal_add(&s.seat->events.request_set_primary_selection,
	              &s.request_set_primary_selection);
	s.request_start_drag.notify = request_start_drag;
	wl_signal_add(&s.seat->events.request_start_drag, &s.request_start_drag);
	s.start_drag.notify = start_drag;
	wl_signal_add(&s.seat->events.start_drag, &s.start_drag);
	wl_list_init(&s.drag_icon_destroy.link);

	/* presentation needs backend */
	wlr_presentation_create(s.display, s.backend, 2);

	/* clipboard managers: wl-paste --watch, cliphist, clipman */
	wlr_data_control_manager_v1_create(s.display);

	/* games: raw motion and pointer lock/confine */
	s.relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(s.display);
	gestures_init(&s);
	tablet_init(&s);
	touch_init(&s);
	s.pointer_constraints = wlr_pointer_constraints_v1_create(s.display);
	s.new_constraint.notify = new_constraint;
	wl_signal_add(&s.pointer_constraints->events.new_constraint,
	              &s.new_constraint);

	s.cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(s.display, 1);
	s.request_set_shape.notify = request_set_shape;
	wl_signal_add(&s.cursor_shape_mgr->events.request_set_shape,
	              &s.request_set_shape);

	s.xdg_activation = wlr_xdg_activation_v1_create(s.display);
	s.request_activate.notify = request_activate;
	wl_signal_add(&s.xdg_activation->events.request_activate,
	              &s.request_activate);

	/*
	 * Night light: wlsunset, gammastep. The scene owns gamma: it folds the
	 * table into every frame's color transform, so a table applied by hand
	 * lasts exactly one frame before the next commit replaces it. Where the
	 * hardware has no gamma LUT (nested sessions), the scene applies it
	 * while rendering instead.
	 */
	s.gamma_mgr = wlr_gamma_control_manager_v1_create(s.display);
	if (s.gamma_mgr)
		wlr_scene_set_gamma_control_manager_v1(s.scene, s.gamma_mgr);

	/* input methods: fcitx5, ibus */
	ime_init(&s);

	/* window lists: waybar's taskbar, and ext-foreign-toplevel readers */
	s.ftl_mgr = wlr_foreign_toplevel_manager_v1_create(s.display);
	s.ext_ftl_list = wlr_ext_foreign_toplevel_list_v1_create(s.display, 1);
	extws_init(&s);

	/* synthetic input: wtype, ydotool-style tools, remote desktop */
	s.virtual_kbd_mgr = wlr_virtual_keyboard_manager_v1_create(s.display);
	s.new_virtual_keyboard.notify = new_virtual_keyboard;
	wl_signal_add(&s.virtual_kbd_mgr->events.new_virtual_keyboard,
	              &s.new_virtual_keyboard);
	s.virtual_ptr_mgr = wlr_virtual_pointer_manager_v1_create(s.display);
	s.new_virtual_pointer.notify = new_virtual_pointer;
	wl_signal_add(&s.virtual_ptr_mgr->events.new_virtual_pointer,
	              &s.new_virtual_pointer);

	const char *socket = wl_display_add_socket_auto(s.display);
	if (!socket || !wlr_backend_start(s.backend)) {
		wlr_log(WLR_ERROR, "could not start");
		ret = 1;
		goto teardown;  /* every listener must go before the display does */
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	/* set XDG_CURRENT_DESKTOP */
	setenv("XDG_CURRENT_DESKTOP", "aro", false);
	wlr_log(WLR_INFO, "aro running on %s", socket);

	/* aroctl; before autostart, so every child inherits ARO_SOCKET */
	ipc_init(&s, socket);

	/* a nested aro would steal the host session's portals */
	if (!nested)
		session_env_export();

	/* spawn after environment is ready */
	config_watch_start(&s);
	notify_config_errors(&s);

	/* retry failed monitor blocks */
	monitors_reapply(&s);

	/* before exec lines: the wallpaper is the first thing to come up */
	wallpaper_init(&s.wallpaper, s.loop, nested, wallpaper_report, &s);
	s.wallpaper_set = wallpaper_saved(&s.wallpaper, s.cfg.wallpaper, s.cfg.wallpaper_file,
	                                  &s.wallpaper_mode, &s.wallpaper_file);
	wallpaper_sync(&s);

	for (int i = 0; i < s.cfg.nexec; i++)
		config_spawn(s.cfg.exec[i]);
	for (int i = 0; i < s.cfg.nexec_always; i++)
		config_spawn(s.cfg.exec_always[i]);
	if (startup)
		config_spawn(startup);

	wl_display_run(s.display);

	/* first: its connections are event sources on the loop */
	ipc_finish(&s);
	/* likewise its pidfds; and aropaper goes before the clients do */
	wallpaper_finish(&s.wallpaper);
	free(s.wallpaper_file);

teardown:
	/* teardown order matters */
	wl_display_destroy_clients(s.display);
	ime_finish(&s);

	/* finish UI state */
	ui_preview_finish(&s.preview);
	notify_finish(&s);
	prompt_finish(&s);
	overview_finish(&s);
	picker_finish(&s);
	switcher_finish(&s);
	ghost_drop(&s, NULL);
	config_watch_stop(&s);
	config_finish(&s.cfg);
	/* outputs free their trees */
	if (s.clock_timer)
		wl_event_source_remove(s.clock_timer);

	wl_list_remove(&s.new_output.link);
	wl_list_remove(&s.new_xdg_toplevel.link);
	wl_list_remove(&s.new_xdg_popup.link);
#ifdef ARO_XWAYLAND
	if (s.xwayland) {
		wl_list_remove(&s.new_xwayland_surface.link);
		wl_list_remove(&s.xwayland_ready.link);
		/* destroy XWayland before display */
		wlr_xwayland_destroy(s.xwayland);
		s.xwayland = NULL;
	}
#endif
	wl_list_remove(&s.new_decoration.link);
	lock_finish(&s);
	idle_finish(&s);
	if (s.output_mgr) {
		wl_list_remove(&s.output_mgr_apply.link);
		wl_list_remove(&s.output_mgr_test.link);
		s.output_mgr = NULL;    /* output_destroy runs after this */
	}
	wl_list_remove(&s.new_layer_surface.link);
	wl_list_remove(&s.new_input.link);
	wl_list_remove(&s.request_cursor.link);
	wl_list_remove(&s.request_set_selection.link);
	wl_list_remove(&s.request_set_primary_selection.link);
	wl_list_remove(&s.request_start_drag.link);
	wl_list_remove(&s.start_drag.link);
	wl_list_remove(&s.drag_icon_destroy.link);
	wl_list_remove(&s.cursor_motion.link);
	wl_list_remove(&s.cursor_motion_abs.link);
	wl_list_remove(&s.cursor_button.link);
	wl_list_remove(&s.cursor_axis.link);
	wl_list_remove(&s.cursor_frame.link);
	wl_list_remove(&s.new_constraint.link);
	wl_list_remove(&s.request_set_shape.link);
	wl_list_remove(&s.request_activate.link);
	wl_list_remove(&s.new_virtual_keyboard.link);
	wl_list_remove(&s.new_virtual_pointer.link);
	extws_finish(&s);
	protocols_finish(&s);
	gestures_finish(&s);
	touch_finish(&s);
	toplevel_icons_finish(&s);
	tablet_finish(&s);
	s.pointer_constraints = NULL;
	s.active_constraint = NULL;

	/* output_destroy runs later, from wlr_backend_destroy: leave it no scene work */
	struct aro_output *o;
	wl_list_for_each(o, &s.outputs, link) {
		bar_finish(&o->bar);
		o->enabled = false;
	}
	wlr_scene_node_destroy(&s.scene->tree.node);
	if (s.gloss_buf)
		wlr_buffer_drop(s.gloss_buf);
	wlr_xcursor_manager_destroy(s.xcursor_mgr);
	free(s.xcursor_theme);
	free(s.env_cursor_theme);
	wlr_cursor_destroy(s.cursor);
	wlr_allocator_destroy(s.allocator);
	wlr_renderer_destroy(s.renderer);
	wlr_backend_destroy(s.backend);
	for (int i = 0; i < ARO_MAX_WS; i++) {
		ly_free(s.orphan_ws[i]);
		s.orphan_ws[i] = NULL;
	}
	wl_display_destroy(s.display);
	logfile_finish();
	return ret;
}
