/*
 * _GNU_SOURCE before anything else: inotify_init1() is behind __USE_GNU, and
 * with -std=c11 glibc does not expose it by default. Must precede every
 * include, scene.h included.
 */
#define _GNU_SOURCE

/*
 * aro — aro.c
 *
 * The compositor. Backend, outputs, xdg-shell, input, and the loop that ties
 * the layout tree to real surfaces.
 *
 * The shape of the thing:
 *
 *   a window maps    -> ly_split() the focused leaf, leaf->user = view
 *   anything changes -> aro_arrange(): ly_arrange() writes boxes, each
 *                       view retargets its animation toward its box
 *   every frame      -> anim_box_tick() per view, geometry applied, commit;
 *                       schedule another frame while anything is moving
 *
 * BUILD STATUS: written against wlroots 0.19 without a compiler to hand.
 * The calls most likely to have drifted, in rough order of risk:
 *   - wlr_backend_autocreate(loop, session)      signature changed in 0.18
 *   - xdg_shell.events.new_toplevel              0.17 used new_surface
 *   - wlr_output_state_* / wlr_output_commit_state
 *   - wlr_scene_output_layout_add_output
 * Compare against the tinywl.c shipped with your wlroots before debugging
 * anything else.
 */
#define _POSIX_C_SOURCE 200809L

/* scene.h first: it decides which scene implementation the build uses,
 * and that only works if it is included before any wlroots header. */
#include "scene.h"

#include "bar.h"
#include "config.h"
#include "aro.h"
#include "text.h"
#include "theme.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

/* BTN_LEFT / BTN_RIGHT — evdev codes, which is what the pointer reports */
#include <linux/input-event-codes.h>

#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#ifdef ARO_EFFECTS
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_presentation_time.h>
/* two headers, near-identical names: wlr_primary_selection.h declares the
 * seat calls, wlr_primary_selection_v1.h declares the protocol manager */
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#ifdef ARO_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* Super by default. Nested inside another compositor it never reaches us —
 * the host grabs it first — so -Dmodkey=alt switches the whole keymap. */
/* The modifier is config, not a build option. `mod = alt` in the config
 * file, or -m alt on the command line for a one-off nested session. */

static const anim_ease SPRING = { TH_EASE_X1, TH_EASE_Y1, TH_EASE_X2, TH_EASE_Y2 };

/* No overshoot. Used for anything that animates to the edges of the screen,
 * where springing past the target means springing off it. */
static const anim_ease FLAT = { TH_EASE_FLAT_X1, TH_EASE_FLAT_Y1,
                                TH_EASE_FLAT_X2, TH_EASE_FLAT_Y2 };

static void arrange_layers(struct aro_server *s);
static void drag_icon_update(struct aro_server *s);
static void pointer_motion_common(struct aro_server *s, uint32_t time);
static void arrange_layers_output(struct aro_output *o);

uint32_t aro_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


/* ── layout → screen ───────────────────────────────────────────────────── */

/* The output the keyboard is on. Never NULL while any output exists: new
 * windows, workspace switches and spawned clients all land here. */
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

/* Which output contains a point, for deciding where the pointer is working.
 * Falls back to the focused one rather than NULL: a cursor between two
 * outputs in an odd layout should still be able to drop a window. */
static struct aro_output *output_at(struct aro_server *s, double x, double y)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (x >= o->box.x && x < o->box.x + o->box.w &&
		    y >= o->box.y && y < o->box.y + o->box.h)
			return o;
	}
	return aro_focused_output(s);
}

/* Cache an output's box in layout coordinates. Everything downstream reads
 * o->box rather than asking the layout, so a single source per output. */
static void output_refresh_box(struct aro_output *o)
{
	struct wlr_box box = { 0 };
	wlr_output_layout_get_box(o->server->output_layout, o->wlr_output, &box);
	o->box = (ly_box){ box.x, box.y, box.width, box.height };
}

/* The backdrop covers the whole layout — every output at once — since it is
 * what shows through the gaps everywhere. It has to track resizes: nested,
 * the host window can change at any moment, and a stale backdrop leaves a
 * strip of nothing along two edges. */
static void update_backdrop(struct aro_server *s)
{
	if (!s->root_bg)
		return;
	struct wlr_box box = { 0 };
	wlr_output_layout_get_box(s->output_layout, NULL, &box);
	if (box.width <= 0 || box.height <= 0)
		return;
	wlr_scene_node_set_position(&s->root_bg->node, box.x, box.y);
	wlr_scene_rect_set_size(s->root_bg, box.width, box.height);

	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		output_refresh_box(o);
		if (o->bar.tree) {
			bar_place(&o->bar, o->box.x,
			          o->box.y + o->box.h - s->cfg.theme.bar_h, o->box.w, o->scale);
			bar_update(&o->bar, o);
		}
	}
	arrange_layers(s);

	/* after the boxes are refreshed: the lock is sized against them */
	lock_arrange(s);
}

/* What the tiling tree on one output gets: that output, minus any exclusive
 * zones claimed by layer-shell clients on it, minus its bar. o->usable is
 * recomputed by arrange_layers() and is the output box when nothing has
 * claimed anything. */
static ly_box usable_area(struct aro_output *o)
{
	ly_box u = o->usable;
	if (u.w <= 0 || u.h <= 0)
		u = o->box;
	u.h -= o->server->cfg.theme.bar_h;
	return u;
}

/* Where a view is headed, whoever decides it: the tree for tiled windows,
 * fbox for floating ones, its own output for fullscreen. Everything that
 * used to read v->node->box should read this instead — floating views have
 * no node, and a fullscreen window means one screen, not all of them. */
static ly_box view_target(struct aro_view *v)
{
	if (v->fullscreen)
		return v->output ? v->output->box : (ly_box){ 0, 0, 1, 1 };
	if (v->floating)
		return v->fbox;
	if (v->node)
		return v->node->box;
	return (ly_box){ 0, 0, 1, 1 };
}

/*
 * The root of the workspace a view's leaf hangs from.
 *
 * Its output's, normally; the server's while parked. A client can unmap at
 * any moment, including while we are on another TTY with no outputs at all,
 * and &v->output->ws[...] is a null deref there.
 */
static ly_node **view_ws_root(struct aro_view *v)
{
	if (v->output)
		return &v->output->ws[v->workspace];
	return &v->server->orphan_ws[v->workspace];
}

/*
 * Hand every mapped layer surface the full output and the shrinking usable
 * box; wlr_scene_layer_surface_v1_configure() applies anchors, margins and
 * size for us and subtracts whatever exclusive zone the client asked for.
 * Whatever survives is what our windows may use.
 */
static void arrange_layers(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		arrange_layers_output(o);
}

static void arrange_layers_output(struct aro_output *o)
{
	struct aro_server *s = o->server;
	struct wlr_box full = { o->box.x, o->box.y, o->box.w, o->box.h };
	if (full.width <= 0 || full.height <= 0)
		return;

	struct wlr_box usable = full;

	struct aro_layer *l;
	wl_list_for_each(l, &s->layers, link) {
		/*
		 * Configure everything that has had its initial commit, NOT just
		 * what is mapped. A layer surface cannot map until it has been
		 * told its size, so gating this on `mapped` deadlocks the client:
		 * it waits for a configure that only arrives once it has mapped.
		 * That deadlock is silent — the client blocks without an error.
		 */
		if (!l->layer_surface->initialized || !l->scene)
			continue;
		/* A layer surface belongs to one output. Configuring it from
		 * every output's pass would subtract its exclusive zone from
		 * screens it is not even on. */
		if (l->layer_surface->output &&
		    l->layer_surface->output != o->wlr_output)
			continue;
		wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
	}

	o->usable = (ly_box){ usable.x, usable.y, usable.width, usable.height };
}

static bool box_eq(ly_box a, ly_box b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/*
 * Retarget only when the target actually changed.
 *
 * anim_box_to() restarts from wherever the box currently is, which is what
 * keeps interrupted moves continuous — but it also means calling it every
 * time arrange runs re-starts an in-flight animation from a partway point,
 * with a fresh 340ms and a fresh overshoot. Arrange runs on map, unmap,
 * focus, workspace switch, layer commit and output change, so a window can
 * easily be re-sprung several times toward a target it was already heading
 * for. That reads as a wobble. Comparing against the target it is already
 * chasing costs four int compares and removes the whole class.
 */
static void view_retarget(anim_box *g, ly_box t, uint32_t now, uint32_t dur,
                          const anim_ease *ease)
{
	if (box_eq(g->active ? g->to : g->cur, t))
		return;
	anim_box_to(g, t, now, dur, ease);
}

/* Is this view on screen right now: right output, right workspace on it. */
static bool view_visible(struct aro_view *v)
{
	return v->mapped && v->output && v->workspace == v->output->cur_ws;
}

void aro_arrange(struct aro_server *s)
{
	/* not static: the gaps come from the config now */
	const ly_metrics m = {
		.gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap, .min = s->cfg.theme.min,
	};

	uint32_t now = aro_now_ms();

	/* Arrange every output's current workspace. The tree may be empty —
	 * a workspace holding nothing but floating windows is legal — and
	 * everything after this still has to run either way. */
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		ly_node *root = o->ws[o->cur_ws];
		if (root)
			ly_arrange(root, usable_area(o), &m);
	}

	/*
	 * Retarget every visible view from its own source of truth. Walking
	 * s->views rather than each tree's leaves is what lets floating
	 * windows animate through the same path as tiled ones.
	 */
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v))
			continue;

		/* A window being dragged is following the cursor, not the tree.
		 * Retargeting it here would fight the pointer every frame. */
		if (s->grabbed == v && s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
			continue;

		ly_box t = view_target(v);

		/* Fullscreen animates flat. The spring overshoots ~8%, which on a
		 * box the size of the screen lands off every edge and snaps back —
		 * it reads as the window shaking rather than settling. */
		if (v->fullscreen)
			view_retarget(&v->geo, t, now, s->cfg.theme.anim_fs_ms, &FLAT);
		else
			view_retarget(&v->geo, t, now, s->cfg.theme.anim_ms, &SPRING);

		if (!v->fullscreen)
			ui_frame_title(v, t.w, v->output->scale);
	}

	wl_list_for_each(o, &s->outputs, link) {
		if (o->bar.tree)
			bar_update(&o->bar, o);
		wlr_output_schedule_frame(o->wlr_output);
	}
}

static void view_set_visible(struct aro_view *v, bool visible)
{
	wlr_scene_node_set_enabled(&v->frame_tree->node, visible);
}

/*
 * Switch the focused output to one of its own workspaces. The other outputs
 * are untouched — that is the whole point of the sway model, and it is why
 * "workspace 2" is only meaningful alongside an output.
 */
static void workspace_show(struct aro_server *s, int ws)
{
	struct aro_output *o = aro_focused_output(s);
	if (!o || ws < 0 || ws >= ARO_MAX_WS || ws == o->cur_ws)
		return;

	o->cur_ws = ws;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->output == o)
			view_set_visible(v, v->workspace == ws);
	}

	ly_node *root = o->ws[ws];

	/* Place the incoming windows before anything is drawn. Without this
	 * they animate in from wherever they sat on the old workspace, which
	 * reads as the layout breaking rather than as a switch. */
	if (root) {
		ly_arrange(root, usable_area(o), &(ly_metrics){
			.gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap, .min = s->cfg.theme.min });
		ly_node *leaves[256];
		int n = ly_collect(root, leaves, 256);
		for (int i = 0; i < n; i++) {
			struct aro_view *iv = leaves[i]->user;
			if (iv && iv->mapped)
				anim_box_set(&iv->geo, leaves[i]->box);
		}
	}

	/* same for the floating ones, which the tree above knows nothing about */
	struct aro_view *fv;
	wl_list_for_each(fv, &s->views, link) {
		if (fv->mapped && fv->output == o && fv->workspace == ws &&
		    (fv->floating || fv->fullscreen))
			anim_box_set(&fv->geo, view_target(fv));
	}

	struct aro_view *next = NULL;
	if (root) {
		ly_node *first = ly_first_leaf(root);
		next = first ? first->user : NULL;
	}
	if (!next) {
		/* a workspace with only floating windows would otherwise come up
		 * with nothing focused and the keyboard going nowhere */
		struct aro_view *cand;
		wl_list_for_each(cand, &s->views, link) {
			if (cand->mapped && cand->output == o && cand->workspace == ws) {
				next = cand;
				break;
			}
		}
	}
	aro_focus(s, next);
	aro_arrange(s);
}

static void view_send_to(struct aro_server *s, struct aro_view *v, int ws)
{
	if (!v || ws < 0 || ws >= ARO_MAX_WS || ws == v->workspace)
		return;

	ly_node *next = NULL;

	if (v->floating) {
		/* nothing to re-parent: it was never in a tree */
		v->workspace = ws;
	} else {
		if (!v->node)
			return;
		next = ly_close(&v->output->ws[v->workspace], v->node);
		v->node = NULL;
		v->workspace = ws;

		ly_node **root = &v->output->ws[ws];
		if (!*root) {
			*root = ly_leaf(v);
			v->node = *root;
		} else {
			v->node = ly_split(root, ly_first_leaf(*root), LY_ROW, v);
		}
		if (!v->node)
			return;
	}

	view_set_visible(v, false);
	if (s->focused == v) {
		s->focused = NULL;
		aro_focus(s, next ? next->user : NULL);
	}
	aro_arrange(s);
}

/* ── focus ─────────────────────────────────────────────────────────────── */

void aro_focus(struct aro_server *s, struct aro_view *v)
{
	/* The lock owns the keyboard. Remember the intended focus so it can
	 * be restored on unlock, but do not act on it. */
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

	/*
	 * The keyboard's output follows the focused window. Without this,
	 * clicking a window on the second screen leaves focused_output on the
	 * first, so the next window you open — and the next mod+1..4 — lands
	 * on the screen you are not looking at.
	 */
	if (v->output)
		s->focused_output = v->output;

	ui_frame_focus(v, true);
	view_activate(v, true);
	wlr_scene_node_raise_to_top(&v->frame_tree->node);

	if (v->output && v->output->bar.tree)
		bar_update(&v->output->bar, v->output);

	/* a launcher or lock screen holds the keyboard until it goes away */
	if (s->focused_layer)
		return;

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, view_surface(v),
		                               kb->keycodes, kb->num_keycodes,
		                               &kb->modifiers);
}

/* ly_swap() exchanges the payloads of two leaves, so each view's back
 * pointer has to follow. Getting this backwards leaves node->user and
 * view->node disagreeing, which shows up much later as a wrong-frame move. */
static void view_swap(struct aro_view *a, struct aro_view *b)
{
	ly_node *na = a->node, *nb = b->node;
	ly_swap(na, nb);
	a->node = nb;
	b->node = na;
}

/* ── shells ────────────────────────────────────────────────────────────── */

/*
 * xdg-shell's answers to view_impl. Everything below this point in the file
 * goes through the wrappers instead of touching v->toplevel, so a second
 * shell only has to fill in another one of these.
 */
static void xdg_configure(struct aro_view *v, int x, int y, int w, int h)
{
	(void)x; (void)y;       /* a Wayland client does not know where it is */
	if (v->toplevel)
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

/*
 * What arrives already wanting to float. Two signals, both cheap:
 *
 *   parent != NULL     a dialog, a file picker, an about box. The client has
 *                      told us it belongs to another window.
 *   min == max size    the client cannot be resized, so tiling it means
 *                      either a stretched surface or a frame full of gap.
 *
 * Deliberately no app_id list: per-app quirks are `rule` lines in the config
 * now, and a rule that says float or tile overrides this guess entirely.
 *
 * API RISK: min_width/max_width are read from toplevel->current here. In some
 * wlroots versions these live on ->pending until the first commit, in which
 * case a non-resizable client tiles on its first map and floats on the next.
 */
static bool xdg_wants_float(struct aro_view *v)
{
	struct wlr_xdg_toplevel *t = v->toplevel;
	if (!t)
		return false;
	if (t->parent)
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

	/*
	 * base->geometry is the EFFECTIVE window geometry in wlroots 0.18+:
	 * what the client set with set_window_geometry, clamped to its
	 * surface, or the surface bounds if it never set one. (0.17 and older
	 * spelled this wlr_xdg_surface_get_geometry(); 0.20 has only the
	 * field. current.geometry is the raw client value — not this.)
	 *
	 * It may still be empty when float_box_for() runs at map — CONTEXT's
	 * standing theory for floats coming up at half the screen — so fall
	 * back to the size of the buffer the client committed, which is
	 * populated the moment one is attached, whatever order wlroots
	 * computes geometry in. For a GTK window the extents include its CSD
	 * shadow, but GTK always sets a geometry, so it never gets this far.
	 */
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
	.geometry        = xdg_geometry,
	.wants_float     = xdg_wants_float,
	.wants_fullscreen = xdg_wants_fullscreen,
	.preferred_size  = xdg_preferred_size,
	.surface         = xdg_surface,
};

/* Wrappers. Every one tolerates a view whose shell object is already gone,
 * which happens between destroy and teardown more often than you would like. */
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

/* Zero size means "no geometry yet"; callers lay out from the surface
 * origin in that case, which is right for a surface that has not committed. */
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

struct wlr_surface *view_surface(struct aro_view *v)
{
	return (v && v->impl && v->impl->surface) ? v->impl->surface(v) : NULL;
}

/* ── floating ──────────────────────────────────────────────────────────── */


/* A sensible first box for a window that never gets one from the tree: the
 * size the client asked for, clamped to the usable area, centred. */
/*
 * How much of a frame is not the client: borders, plus the header when there
 * is one. Asked of ui.c rather than recomputed, because whether there IS a
 * header depends on csd and header = auto, and a second copy of that rule
 * here got it wrong — CSD floats were configured one header too tall.
 */
static void frame_chrome(struct aro_view *v, int *cw, int *ch)
{
	const ly_box big = { 0, 0, 10000, 10000 };
	ly_box c;
	ui_frame_content_box(v, big, &c);
	*cw = big.w - c.w;
	*ch = big.h - c.h;
}

static ly_box float_box_for(struct aro_view *v)
{
	const struct q_theme *th = &v->server->cfg.theme;
	ly_box u = usable_area(v->output);

	/* float_scale is now the fallback for a client that has genuinely
	 * committed nothing, not the normal path — see xdg_preferred_size */
	int w = 0, h = 0;
	if (v->impl->preferred_size)
		v->impl->preferred_size(v, &w, &h);
	if (w <= 0 || h <= 0) {
		w = (int)(u.w * th->float_scale);
		h = (int)(u.h * th->float_scale);
	}

	/* the frame is chrome around the client, so ask for room for both */
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

/*
 * Hybrid float sizing, first half: tell the client its size exactly once.
 *
 * Called whenever a window starts floating with a box we chose — on map, on
 * mod+space, on leaving fullscreen — and never per frame. For an xdg client
 * this then hands the size over to it (float_follow). X11 keeps being
 * configured by ui_frame_geometry, because X11 is told where it is and
 * believes it: stop telling it and a moved window's menus open where the
 * window used to be.
 */
static void view_float_configure_once(struct aro_view *v)
{
	if (!v->floating || v->fullscreen)
		return;

	v->float_follow = v->toplevel != NULL;

	/*
	 * If we do not know the client's size, fbox came from the float_scale
	 * fallback — a guess, not a size — and configuring it would turn that
	 * guess into a demand the client obeys: the half-screen bug again,
	 * one step removed. An xdg client was already told "0x0, you decide"
	 * on its initial commit, so say nothing more; its first real commit
	 * sizes the frame through view_float_follow().
	 *
	 * X11 has no "you decide", and always reports a size anyway.
	 */
	int w = 0, h = 0;
	if (v->impl->preferred_size)
		v->impl->preferred_size(v, &w, &h);
	if (v->float_follow && (w <= 0 || h <= 0))
		return;

	ly_box c;
	ui_frame_content_box(v, v->fbox, &c);
	view_configure(v, c.x, c.y, c.w, c.h);
}

/*
 * Hybrid float sizing, second half: the client changed its own size, so the
 * frame follows. Called from the commit handler.
 *
 * The top-left stays put — a dialog that grows should grow away from where
 * you put it, not re-centre itself under you. And it snaps rather than
 * springs: this is not a layout change we made, and springing towards a size
 * we did not choose reads as a glitch.
 */
static void view_float_follow(struct aro_view *v)
{
	struct aro_server *s = v->server;
	const struct q_theme *th = &s->cfg.theme;

	if (!v->float_follow || !v->floating || v->fullscreen || !v->mapped ||
	    !v->output)
		return;
	/* mid-drag the pointer owns the box; a commit must not fight it */
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

	/* rule 6: the title re-renders when its width changes, and it just
	 * did — once, here, not on every frame */
	ui_frame_title(v, v->fbox.w, v->output->scale);
	wlr_output_schedule_frame(v->output->wlr_output);
}

/*
 * Move a view between the tree and the floating layer.
 *
 * Going out: ly_close() drops the leaf, and the position it held is gone for
 * good — coming back in re-splits against whatever is focused, the way i3
 * behaves. Keeping the slot would mean floating leaves the tree skips during
 * arrange, which buys exact restoration at the cost of layout.c learning a
 * concept it has no business knowing.
 */
static void view_set_floating(struct aro_server *s, struct aro_view *v,
                              bool floating)
{
	if (!v || v->floating == floating)
		return;

	if (floating) {
		if (v->node) {
			ly_close(&v->output->ws[v->workspace], v->node);
			v->node = NULL;
		}
		v->floating = true;
		v->fbox = float_box_for(v);
		if (!v->fullscreen)
			wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
		view_float_configure_once(v);
	} else {
		v->floating = false;
		v->float_follow = false;        /* the tree decides again */

		ly_node **root = &v->output->ws[v->workspace];
		if (!*root) {
			*root = ly_leaf(v);
			v->node = *root;
		} else {
			ly_node *target = s->focused && s->focused != v &&
			                  s->focused->node
			                ? s->focused->node
			                : ly_first_leaf(*root);
			v->node = ly_split(root, target, LY_ROW, v);
		}
		if (!v->node) {
			/* out of memory: stay floating rather than vanish */
			v->floating = true;
			return;
		}
		if (!v->fullscreen)
			wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
	}

	aro_arrange(s);
}

/*
 * Fullscreen sits on top of whatever the view already is. The tree is never
 * told: a tiled window keeps its leaf and its box, we simply stop using them
 * until it comes back. That is rule 2 — the tree states where things belong,
 * the compositor decides what to do about it.
 */
static void view_set_fullscreen(struct aro_server *s, struct aro_view *v,
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

	/* Coming back from fullscreen a following float is still at screen
	 * size, and its next commit would make that the new fbox. Tell it the
	 * box it had before, once, and let it follow from there. */
	if (!fullscreen)
		view_float_configure_once(v);
	aro_arrange(s);
}

static void view_request_fullscreen(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_fullscreen);
	(void)data;

	/*
	 * The protocol requires an ack even when we say no, and a client that
	 * asks before mapping must still be answered — wlr_xdg_surface_schedule_configure
	 * is what tinywl does for the unmapped case.
	 *
	 * API RISK: requested.fullscreen is the field in 0.18+; older trees put
	 * it on client_pending.
	 */
	if (!v->mapped) {
		wlr_xdg_surface_schedule_configure(v->toplevel->base);
		return;
	}
	view_set_fullscreen(v->server, v, v->toplevel->requested.fullscreen);
}

/* ── window rules ──────────────────────────────────────────────────────── */

/*
 * Ask the rules about a window that is already mapped, and apply only what
 * changed since the last answer (see rule_last in aro.h). Called on a
 * title change and on a config reload; view_map asks once itself, because
 * a window being placed for the first time has to be placed right, not
 * placed and then moved.
 *
 * A field that goes from set to unset — the rule was deleted, or the title
 * stopped matching — does nothing. The window stays as it is rather than
 * being put back to whatever the map-time heuristics guessed, which is an
 * answer you can no longer see and would not know to expect.
 */
static void view_rules_reapply(struct aro_view *v)
{
	struct aro_server *s = v->server;
	if (!v->mapped || !v->output)
		return;

	struct q_rule_result r, last = v->rule_last;
	config_rules_eval(&s->cfg, view_app_id(v), view_title(v), &r);
	v->rule_last = r;       /* before acting: nothing below re-enters, but
	                           a stale answer must never be compared twice */

	/* float first: view_send_to only re-inserts into the destination
	 * tree what is tiled at the moment it runs */
	if (r.floating != Q_RULE_UNSET && r.floating != last.floating)
		view_set_floating(s, v, r.floating == 1);
	if (r.workspace != Q_RULE_UNSET && r.workspace != last.workspace)
		view_send_to(s, v, r.workspace);
	if (r.fullscreen != Q_RULE_UNSET && r.fullscreen != last.fullscreen)
		view_set_fullscreen(s, v, true);
}

/* ── across outputs ────────────────────────────────────────────────────── */

/*
 * The output next to `from` in a direction, by box centres — the same
 * spatial rule ly_focus uses inside a tree, applied one level up between
 * them. Only outputs genuinely on that side are considered, so moving right
 * from the rightmost screen finds nothing rather than wrapping.
 */
static struct aro_output *output_toward(struct aro_server *s,
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

		/* punish drift across the axis, exactly as ly_focus does */
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

/* Something to focus on an output: its current workspace's first leaf, or
 * any floating window there if the tree is empty. */
static struct aro_view *output_pick_view(struct aro_server *s,
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
		if (v->mapped && v->output == o && v->workspace == o->cur_ws)
			return v;
	}
	return NULL;
}

/*
 * Hand a window to another output, onto whatever workspace that output is
 * currently showing. Floating windows keep their size but are nudged inside
 * the destination, since their box is in layout coordinates and would
 * otherwise stay on the old screen.
 */
static void view_move_to_output(struct aro_server *s, struct aro_view *v,
                                struct aro_output *dest)
{
	if (!v || !dest || v->output == dest)
		return;

	if (v->node) {
		ly_close(&v->output->ws[v->workspace], v->node);
		v->node = NULL;
	}

	struct aro_output *src = v->output;
	v->output = dest;
	v->workspace = dest->cur_ws;

	if (v->floating) {
		/* carry it over by the offset between the two outputs */
		v->fbox.x += dest->box.x - src->box.x;
		v->fbox.y += dest->box.y - src->box.y;
	} else {
		ly_node **root = &dest->ws[dest->cur_ws];
		if (!*root) {
			*root = ly_leaf(v);
			v->node = *root;
		} else {
			v->node = ly_split(root, ly_first_leaf(*root), LY_ROW, v);
		}
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

/*
 * Drag-and-drop tiling, the part you actually see.
 *
 * Pick up a tiled window and nothing happens until you have moved
 * s->cfg.theme.drag_tear pixels — click-to-focus with a twitchy hand should not
 * rearrange your screen. Past that the window tears out of the tree and
 * becomes floating under the cursor, keeping the size it had so the lift
 * reads as continuous rather than as a jump.
 *
 * While it is loose, whatever tiled window is under the cursor shows a
 * preview of the slot the drop would create: the half of that window nearest
 * the pointer. Let go over a preview and the window tiles there; let go over
 * the gaps and it simply stays floating.
 *
 * There is deliberately no centre "swap" zone. Four edges means the drop is
 * always one of four answers and the preview never flickers between two
 * meanings near the middle of a window.
 */

/* The slot a drop would create: half of the target, on the side you are
 * pointing at. */
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

/* The tiled window under a point, ignoring the one being dragged. Asks the
 * layout for boxes rather than the scene graph, because the dragged window is
 * sitting on top of the answer. */
static struct aro_view *tiled_view_at(struct aro_server *s,
                                         double x, double y)
{
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v))
			continue;
		if (v->floating || v->fullscreen || !v->node || v == s->grabbed)
			continue;
		/* only consider windows on the output the cursor is over */
		if (v->output != output_at(s, x, y))
			continue;
		if (box_contains(v->node->box, x, y))
			return v;
	}
	return NULL;
}

/* Which side of a box a point is nearest, normalised so that a wide window
 * and a tall one behave the same. */
static ly_edge nearest_edge(ly_box b, double x, double y)
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
	ui_preview_show(&s->preview, false);
}

/* Recompute the indicator from where the cursor is now. */
static void drop_update(struct aro_server *s)
{
	struct aro_view *target = tiled_view_at(s, s->cursor->x, s->cursor->y);
	if (!target) {
		drop_clear(s);
		return;
	}

	ly_edge e = nearest_edge(target->node->box, s->cursor->x, s->cursor->y);
	ly_box slot = drop_slot_box(target->node->box, e);

	bool fresh = !s->preview.active;
	s->drop_target = target;
	s->drop_edge = e;

	if (fresh) {
		/* appear where it belongs rather than flying in from the origin */
		anim_box_set(&s->preview.geo, slot);
		ui_preview_geometry(&s->preview, slot);
		ui_preview_show(&s->preview, true);
	} else {
		/* it chases the cursor between slots, so it moves flat and fast:
		 * an overshooting indicator points at the wrong window. */
		anim_box_to(&s->preview.geo, slot, aro_now_ms(), s->cfg.theme.drop_ms, &FLAT);
	}
}

/*
 * Put a floating view back into the tree beside `target`, on the given side.
 *
 * ly_split() does not promise which side the new leaf lands on, so rather
 * than assume, this arranges and then looks at where the two boxes actually
 * ended up, swapping payloads if the drop was meant for the other side. One
 * extra arrange, no guess about the tree's internals.
 */
static void view_tile_into(struct aro_server *s, struct aro_view *v,
                           struct aro_view *target, ly_edge e)
{
	if (!v || !target || !target->node || target == v)
		return;

	ly_dir dir = (e == LY_LEFT || e == LY_RIGHT) ? LY_ROW : LY_COL;
	/* the drop target's output wins: dragging across a screen boundary
	 * moves the window to that screen */
	v->output = target->output;
	v->workspace = target->workspace;
	ly_node **root = &v->output->ws[v->workspace];

	ly_node *leaf = ly_split(root, target->node, dir, v);
	if (!leaf)
		return;                 /* out of memory: stay floating */

	v->node = leaf;
	v->floating = false;
	v->float_follow = false;        /* back in the tree: the tree decides */
	wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);

	ly_arrange(*root, usable_area(v->output),
	           &(ly_metrics){ .gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap, .min = s->cfg.theme.min });

	bool want_first = (e == LY_LEFT || e == LY_UP);
	bool is_first = (dir == LY_ROW)
	              ? v->node->box.x < target->node->box.x
	              : v->node->box.y < target->node->box.y;
	if (want_first != is_first)
		view_swap(v, target);
}

/* ── resizing a shared boundary ────────────────────────────────────────── */

static void grab_end(struct aro_server *s);

/*
 * Mouse-resizing a tiled window does not resize that window. It moves a
 * boundary the window shares with whatever is on the other side, and both
 * sides have to follow the cursor at once — which is why this has its own
 * arrange path. Springing here would mean the boundary lags the pointer, and
 * a boundary that lags is worse than one that does not move.
 */

/* Which edges of a box the cursor is within `zone` pixels of, as a WLR_EDGE
 * mask so that corners come out naturally as two bits. */
static uint32_t edge_zone(ly_box b, double x, double y, int zone)
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

static uint32_t mask_from_ly_edge(ly_edge e)
{
	switch (e) {
	case LY_LEFT:  return WLR_EDGE_LEFT;
	case LY_RIGHT: return WLR_EDGE_RIGHT;
	case LY_UP:    return WLR_EDGE_TOP;
	case LY_DOWN:  return WLR_EDGE_BOTTOM;
	}
	return 0;
}

/*
 * The split node that owns the boundary on a given side of a leaf.
 *
 * Walking up until the axis matches is not enough: an ancestor splitting the
 * right way might put this leaf on the far side, in which case its boundary
 * is the one on the *opposite* edge. So the side matters too — for a right
 * edge the subtree has to be the first child, for a left edge the second.
 *
 * NULL means there is nothing to move: that edge is the edge of the screen.
 */
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

/*
 * The boundary to drag, given the edge the pointer asked for.
 *
 * Away from the borders there is no meaningful "nearest" edge — every
 * distance is about the same — so the asked-for edge is close to arbitrary,
 * and insisting on it means a centre grab on a leftmost window asks for the
 * screen edge and gets refused. Try what was asked for, then anything that
 * exists. Which edge we end up moving does not change the gesture: the
 * boundary follows the pointer either way, because the ratio belongs to the
 * split, not to the window that was grabbed.
 */
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

/*
 * Arrange and apply immediately, with no animation at all. The spring exists
 * so that windows appear to settle into place; during a live resize there is
 * nothing to settle into, because the target is wherever the pointer is this
 * millisecond.
 *
 * Titles are deliberately NOT refreshed here. ui_frame_title runs a pango
 * pass whenever max width changes, and max width changes on every motion
 * event — rule 6 exists precisely to keep that off this path. grab_end()
 * refreshes them once when the drag stops.
 */
static void arrange_live(struct aro_server *s)
{
	/* Only the output being resized on: a boundary drag cannot affect any
	 * other screen's tree. */
	struct aro_output *o = s->grabbed ? s->grabbed->output
	                                     : aro_focused_output(s);
	if (!o)
		return;
	ly_node *root = o->ws[o->cur_ws];
	if (!root)
		return;

	ly_arrange(root, usable_area(o),
	           &(ly_metrics){ .gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap, .min = s->cfg.theme.min });

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

	/*
	 * Offset from where the boundary already was, not from where the
	 * cursor is. Reading the ratio straight off the pointer would snap the
	 * boundary to the cursor the instant you press — grab a window
	 * anywhere but exactly on its edge and the layout jumps before you
	 * have moved. You grab the edge, wherever your pointer happens to be.
	 *
	 * Still computed from the total delta since mousedown rather than
	 * accumulated per event, so it does not drift and it retraces exactly
	 * when you drag back out of a clamp.
	 */
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

/* The tiled window nearest a point, by centre distance. Used when a drop
 * lands on the gaps, where there is nothing directly underneath. */
static struct aro_view *nearest_tiled_view(struct aro_server *s,
                                              double x, double y,
                                              struct aro_view *except)
{
	struct aro_view *best = NULL;
	double best_d = 0;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v) || v == except)
			continue;
		if (v->floating || v->fullscreen || !v->node)
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

/*
 * Put a torn-out window back into the tree when it is dropped.
 *
 * A window that was tiled before the drag always returns to tiling: the lift
 * is a way of moving it, not a way of turning it into a floating window.
 * Dropping on a preview uses that slot; dropping on the gaps falls back to
 * the nearest tiled window, so the gesture cannot silently do nothing.
 *
 * A window that was ALREADY floating — a dialog, or one toggled with
 * mod+space — is left alone. Dragging it is just moving it.
 */
static void view_retile(struct aro_server *s, struct aro_view *v)
{
	struct aro_view *target = NULL;
	ly_edge edge = LY_RIGHT;

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

	/* nothing else is tiled here: it becomes the whole workspace */
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

static void grab_end(struct aro_server *s)
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

	/* arrange refreshes every title against its settled target width —
	 * the pango pass that arrange_live() deliberately skipped */
	aro_arrange(s);
}

static void grab_begin(struct aro_server *s, struct aro_view *v,
                       enum aro_cursor_mode mode, uint32_t edges)
{
	if (!v || !v->mapped || v->fullscreen)
		return;

	/*
	 * Resizing a tiled window means moving a boundary it shares with a
	 * sibling. Find that boundary now: if there isn't one, the cursor is on
	 * an outer edge of the screen and there is nothing to drag.
	 */
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

	/* Dragging a float's edge pins its size: from now on we decide, and
	 * ui_frame_geometry configures it live as the edge moves. Never
	 * re-armed by a client commit, only by the window floating afresh. */
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

		/* view_set_floating centres a newly floating window, which is the
		 * right answer for mod+space and the wrong one here: a window
		 * being dragged should come loose exactly where it already was. */
		v->fbox = s->grab_box;
	}

	if (!v->floating)
		return;

	v->fbox = (ly_box){ s->grab_box.x + (int)dx, s->grab_box.y + (int)dy,
	                    s->grab_box.w, s->grab_box.h };

	/* No spring while dragging. The window is attached to the cursor and a
	 * window that lags the pointer feels broken rather than smooth. */
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

	/* Clamp by moving the edge back, not by letting the box invert: a
	 * negative width reaches wlr_scene_rect_set_size and asserts. */
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

static void grab_motion(struct aro_server *s)
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

/* A window can vanish mid-drag — the client exits, the dialog closes. */
static void grab_forget(struct aro_server *s, struct aro_view *v)
{
	if (s->grabbed != v)
		return;
	s->grabbed = NULL;
	s->grab_split = NULL;
	s->cursor_mode = ARO_CURSOR_PASSTHROUGH;
	drop_clear(s);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
}

/*
 * Clients ask for these when you drag their own title bar — which for a GTK
 * app drawing its own decorations is the only way we hear about it at all.
 */
static void view_request_move(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_move);
	(void)data;
	grab_begin(v->server, v, ARO_CURSOR_MOVE, 0);
}

static void view_request_resize(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_resize);
	struct wlr_xdg_toplevel_resize_event *ev = data;
	grab_begin(v->server, v, ARO_CURSOR_RESIZE, ev->edges);
}

/* ── view lifecycle ────────────────────────────────────────────────────── */

static void view_map(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, map);
	struct aro_server *s = v->server;
	(void)data;

	/* New windows land on the output the keyboard is on. The cursor's
	 * output would be the other defensible answer; focus is the one that
	 * matches a keyboard-first WM. */
	struct aro_output *o = aro_focused_output(s);
	if (!o) {
		wlr_log(WLR_ERROR, "a window mapped with no output to put it on");
		return;
	}
	v->output = o;
	v->mapped = true;
	v->float_follow = false;        /* a remap starts from scratch */

	/*
	 * Window rules, asked once, before anything is placed: a window a
	 * rule sends elsewhere must never appear here first and then jump.
	 *
	 * The log line is for writing rules — there is no other way to find
	 * out what app_id a program uses until foreign-toplevel exists.
	 */
	const char *app_id = view_app_id(v), *title = view_title(v);
	wlr_log(WLR_INFO, "map: app_id=\"%s\" title=\"%s\"",
	        app_id ? app_id : "", title ? title : "");

	struct q_rule_result r;
	config_rules_eval(&s->cfg, app_id, title, &r);
	v->rule_last = r;

	const int ws = r.workspace != Q_RULE_UNSET ? r.workspace : o->cur_ws;
	const bool here = ws == o->cur_ws;
	v->workspace = ws;

	/* a rule that says float or tile replaces the heuristic outright */
	bool floating = r.floating != Q_RULE_UNSET
	              ? r.floating == 1
	              : v->impl->wants_float && v->impl->wants_float(v);

	if (floating) {
		/*
		 * Never enters the tree at all. Note this leaves pending_split
		 * alone: a dialog opening between mod+v and the window you meant
		 * it for should not eat the one-shot.
		 */
		v->floating = true;
		v->fbox = float_box_for(v);
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
		view_float_configure_once(v);
	} else {
		ly_node **root = &o->ws[ws];
		if (!*root) {
			*root = ly_leaf(v);
			v->node = *root;
		} else {
			ly_node *target = s->focused &&
			                  s->focused->output == o &&
			                  s->focused->workspace == ws &&
			                  s->focused->node
			                ? s->focused->node
			                : ly_first_leaf(*root);
			/*
			 * Dwindle splits along the longer axis of the frame
			 * being split, so the layout spirals on its own. A
			 * one-shot mod+v / mod+s still wins: asking explicitly
			 * should never be overruled by the automatic choice.
			 */
			ly_dir dir = s->pending_split;
			if (s->cfg.layout == Q_LAYOUT_DWINDLE && !s->split_forced)
				dir = target->box.w > target->box.h ? LY_ROW : LY_COL;

			v->node = ly_split(root, target, dir, v);
		}
		if (!v->node) {
			wlr_log(WLR_ERROR, "out of memory inserting a window");
			return;
		}
		/* The one-shot mod+v / mod+s was meant for the window opening
		 * where you are looking. One a rule sends elsewhere must not
		 * use it up — the same courtesy a dialog gets. */
		if (here) {
			s->pending_split = LY_ROW;      /* one-shot, like i3 */
			s->split_forced = false;
		}

		/* grow into place from slightly small, the way panes appear in splits */
		ly_arrange(*root, usable_area(o),
		           &(ly_metrics){ .gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap, .min = s->cfg.theme.min });
	}

	/*
	 * A client may ask for fullscreen before it ever maps — video players
	 * started with --fs do exactly this — and the request arrives while
	 * v->mapped is still false, so it is answered here instead.
	 */
	if (r.fullscreen == 1 ||
	    (v->impl->wants_fullscreen && v->impl->wants_fullscreen(v)))
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

	/* Sent to another workspace: there, hidden, and not focused — taking
	 * focus into a window you cannot see is how keystrokes go missing.
	 * Its pill shows it arrived. */
	view_set_visible(v, here);
	if (here)
		aro_focus(s, v);
	aro_arrange(s);
}

static void view_unmap(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, unmap);
	struct aro_server *s = v->server;
	(void)data;

	if (!v->mapped)
		return;
	v->mapped = false;
	view_set_visible(v, false);
	grab_forget(s, v);

	/* Any tiled window closing can free the split node a live resize is
	 * holding — not just the one being dragged. Drop the grab rather than
	 * dereference a freed node on the next motion event. */
	if (s->cursor_mode == ARO_CURSOR_RESIZE_TILE)
		grab_forget(s, s->grabbed);

	/* A floating view has no leaf to close. Passing NULL into ly_close()
	 * would be a null deref inside the tree, which is the last place you
	 * want to be debugging from. */
	ly_node *next = NULL;
	if (v->node) {
		next = ly_close(view_ws_root(v), v->node);
		v->node = NULL;
	}
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
		/* 0 x 0 lets the client pick, then we override on map */
		wlr_xdg_toplevel_set_size(v->toplevel, 0, 0);
	}

	/* a float that sizes itself: the frame follows what it committed */
	view_float_follow(v);

	/* new buffers arrive square; round them as they come */
	ui_frame_clip_content(v);
}

static void view_set_title(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, set_title);
	(void)data;
	if (!v->mapped || !v->output)
		return;

	/* Rules can match on title, and titles change after map — Firefox
	 * only renames a window "Picture-in-Picture" once it is up. Before
	 * the fullscreen check below, so a rule can still act on one. */
	view_rules_reapply(v);

	if (v->fullscreen || !v->output)
		return;
	ui_frame_title(v, view_target(v).w, v->output->scale);
	if (v->output->bar.tree)
		bar_update(&v->output->bar, v->output);
}

static void view_destroy(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, destroy);
	(void)data;

	if (v->server->focused == v)
		v->server->focused = NULL;
	grab_forget(v->server, v);

	qtext_finish(&v->title);

	/* the frame tree is ours, so we destroy it — the client only owns the
	 * surface node inside v->content */
	if (v->frame_tree)
		wlr_scene_node_destroy(&v->frame_tree->node);

	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->link);
	free(v);
}

/*
 * A popup: a menu, a dropdown, a tooltip. The client creates it and expects
 * the compositor to put it on screen — nothing appears unless we build a
 * scene node for it here, which is why right-click menus were silently doing
 * nothing at all.
 *
 * The parent's xdg_surface carries the tree its children hang from in ->data:
 * a toplevel's is the view's popup tree, a popup's is its own, so submenus
 * nest correctly without this needing to know how deep it is.
 */
struct aro_popup {
	struct wlr_xdg_popup *popup;
	struct aro_server *server;
	struct wlr_scene_tree *parent_tree;

	struct wl_listener commit;
	struct wl_listener destroy;
};

/*
 * Unconstraining has to wait for the first commit.
 *
 * wlr_xdg_popup_unconstrain_from_box() schedules a configure, and an xdg
 * surface is not `initialized` until it has committed once — scheduling one
 * before that asserts and takes the compositor with it. This is the same
 * rule as the layer-shell one in the bug log, seen from the other side:
 * there the mistake was configuring too late, here too early.
 */
static void popup_commit(struct wl_listener *l, void *data)
{
	struct aro_popup *p = wl_container_of(l, p, commit);
	(void)data;

	if (!p->popup->base->initial_commit)
		return;

	/*
	 * Keep it on screen. A menu opened near the bottom of an output would
	 * otherwise run off it — the client picks a position and relies on us
	 * to flip or slide it. The box wants to be in the parent surface's
	 * coordinate space, hence subtracting the tree's layout position.
	 */
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

static void new_xdg_popup(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_xdg_popup);
	struct wlr_xdg_popup *popup = data;

	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent || !parent->data)
		return;

	struct wlr_scene_tree *parent_tree = parent->data;
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
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &v->request_fullscreen);
	v->request_move.notify = view_request_move;
	wl_signal_add(&toplevel->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &v->request_resize);
	v->destroy.notify = view_destroy;
	wl_signal_add(&toplevel->events.destroy, &v->destroy);

	wl_list_insert(&s->views, &v->link);
}

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
}

static void layer_map(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, map);
	(void)data;

	wlr_log(WLR_DEBUG, "layer surface mapped: %s",
	        l->layer_surface->namespace ? l->layer_surface->namespace : "?");

	arrange_layers(l->server);
	aro_arrange(l->server);

	/* fuzzel and friends are useless without this */
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
}

static void layer_commit(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, commit);
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	(void)data;

	/* the client may move itself between layers at any time */
	if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		struct wlr_scene_tree *tree =
			layer_tree_for(l->server, ls->current.layer);
		wlr_scene_node_reparent(&l->scene->tree->node, tree);
	}

	if (ls->initial_commit || ls->current.committed) {
		arrange_layers(l->server);
		aro_arrange(l->server);
	}
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
	wl_list_remove(&l->link);

	struct aro_server *s = l->server;
	free(l);

	arrange_layers(s);
	aro_arrange(s);
}

static void new_layer_surface(struct wl_listener *listener, void *data)
{
	struct aro_server *s = wl_container_of(listener, s, new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;

	/* a client may leave the output up to us */
	if (!ls->output) {
		struct aro_output *o;
		if (wl_list_empty(&s->outputs)) {
			wlr_layer_surface_v1_destroy(ls);
			return;
		}
		o = wl_container_of(s->outputs.next, o, link);
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

	wl_list_insert(&s->layers, &l->link);

	wlr_log(WLR_DEBUG, "layer surface: namespace=%s layer=%d",
	        ls->namespace ? ls->namespace : "?", ls->pending.layer);
}

/* ── decorations ───────────────────────────────────────────────────────── */
/*
 * Without xdg-decoration a client assumes it owns its own chrome, which is
 * why terminals arrive already wearing a title bar. We draw the frame, so we
 * ask for server-side mode on every toplevel.
 *
 * Note this is a request, not a command: GTK applications ignore it and draw
 * client-side decorations regardless. Nothing to be done about that short of
 * per-application quirks.
 */
struct aro_decoration {
	struct aro_server *server;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

/* The view a decoration belongs to, or NULL if it has not mapped yet. */
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
	/* setting a mode before the surface's initial commit is a protocol
	 * error, so wait until it is initialised */
	if (!d->deco->toplevel->base->initialized)
		return;

	wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

	/*
	 * It asked, and it accepted server-side: we draw the title bar, so
	 * the header comes back. A client that never creates a decoration
	 * object never reaches this and keeps csd = true.
	 */
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

/* ── output ────────────────────────────────────────────────────────────── */

static void output_frame(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, frame);
	struct aro_server *s = o->server;
	(void)data;

	uint32_t now = aro_now_ms();
	bool moving = false;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v))
			continue;
		if (anim_box_tick(&v->geo, now))
			moving = true;
		ui_frame_geometry(v, v->geo.cur);
	}

	if (notify_tick(s, now))
		moving = true;
	if (prompt_tick(s, now))
		moving = true;

	/* the drop indicator animates between slots like everything else */
	if (s->preview.active) {
		if (anim_box_tick(&s->preview.geo, now))
			moving = true;
		ui_preview_geometry(&s->preview, s->preview.geo.cur);
	}

	struct wlr_scene_output *so =
		wlr_scene_get_scene_output(s->scene, o->wlr_output);
	if (so) {
		wlr_scene_output_commit(so, NULL);
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		wlr_scene_output_send_frame_done(so, &ts);
	}

	if (moving)
		wlr_output_schedule_frame(o->wlr_output);
}

static void output_request_state(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr_output, ev->state);
	output_refresh_box(o);
	update_backdrop(o->server);
	aro_arrange(o->server);
}

/*
 * An output going away takes its workspaces with it, so every window on it
 * has to be rehomed before the trees are freed. They land on the surviving
 * output's current workspace — the windows are still running, and leaving
 * them pointing at freed nodes is how you get a crash minutes later.
 */
static void output_destroy(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, destroy);
	struct aro_server *s = o->server;
	(void)data;

	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);

	/* a prompt centred on this output has nowhere left to be drawn */
	prompt_output_gone(s, o);

	/* anything grabbed on this output stops being grabbed */
	if (s->grabbed && s->grabbed->output == o)
		grab_forget(s, s->grabbed);

	if (s->focused_output == o)
		s->focused_output = NULL;
	struct aro_output *dest = aro_focused_output(s);   /* now another */
	s->focused_output = dest;

  	struct aro_view *v, *tmp;
	wl_list_for_each_safe(v, tmp, &s->views, link) {
		if (v->output != o)
			continue;

		if (!dest) {
			/*
			 * Nowhere to send it: park it. The leaf stays valid
			 * because the trees below are handed over rather than
			 * freed. Boxes go output-relative so they can be
			 * replayed onto whatever output comes back.
			 */
			v->output = NULL;
			v->fbox.x -= o->box.x;
			v->fbox.y -= o->box.y;
			v->pre_fs.x -= o->box.x;
			v->pre_fs.y -= o->box.y;
			view_set_visible(v, false);
			continue;
		}

		/* drop the leaf first: the tree it points into is about to go */
		v->node = NULL;
		v->output = dest;
		v->workspace = dest->cur_ws;
		if (v->floating) {
			v->fbox.x += dest->box.x - o->box.x;
			v->fbox.y += dest->box.y - o->box.y;
		} else {
			ly_node **root = &dest->ws[dest->cur_ws];
			if (!*root) {
				*root = ly_leaf(v);
				v->node = *root;
			} else {
				v->node = ly_split(root, ly_first_leaf(*root), LY_ROW, v);
			}
		}
		view_set_visible(v, true);
	}

	if (dest) {
		for (int i = 0; i < ARO_MAX_WS; i++) {
			ly_free(o->ws[i]);
			o->ws[i] = NULL;
		}
	} else {
		for (int i = 0; i < ARO_MAX_WS; i++) {
			s->orphan_ws[i] = o->ws[i];
			o->ws[i] = NULL;
		}
		s->orphan_cur_ws = o->cur_ws;
		s->parked = true;
		wlr_log(WLR_INFO, "last output gone — parking its workspaces");
	}
	bar_finish(&o->bar);

	if (s->focused && s->focused->output == NULL)
		aro_focus(s, dest ? output_pick_view(s, dest) : NULL);

	free(o);

	if (dest) {
		update_backdrop(s);
		aro_arrange(s);
	}
}

/*
 * Take back whatever was parked when the last output went away — a VT switch,
 * or the only monitor being unplugged. Called once the output's box and the
 * layer-shell exclusive zones are settled, so the boxes it computes are final
 * and nothing springs afterwards.
 */
static void output_adopt_parked(struct aro_server *s, struct aro_output *o)
{
	if (!s->parked)
		return;
	s->parked = false;

	/* o was calloc'd moments ago: nothing is being dropped here */
	for (int i = 0; i < ARO_MAX_WS; i++) {
		o->ws[i] = s->orphan_ws[i];
		s->orphan_ws[i] = NULL;
	}
	o->cur_ws = s->orphan_cur_ws;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		/*
		 * Parked means mapped with no output. A view that has never
		 * mapped also has no output and must NOT be adopted: view_map
		 * has not picked a workspace or a box for it yet.
		 */
		if (v->output || !v->mapped)
			continue;
		v->output = o;
		if (v->workspace < 0 || v->workspace >= ARO_MAX_WS)
			v->workspace = o->cur_ws;
		v->fbox.x += o->box.x;
		v->fbox.y += o->box.y;
		v->pre_fs.x += o->box.x;
		v->pre_fs.y += o->box.y;
		view_set_visible(v, v->workspace == o->cur_ws);
	}

	/* Place them before anything is drawn, as a workspace switch does:
	 * animating in from where they sat on a screen that no longer exists
	 * reads as the layout breaking, not as coming back. */
	ly_node *root = o->ws[o->cur_ws];
	if (root)
		ly_arrange(root, usable_area(o), &(ly_metrics){
			.gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap,
			.min = s->cfg.theme.min });
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && v->output == o)
			anim_box_set(&v->geo, view_target(v));

	aro_focus(s, output_pick_view(s, o));
	wlr_log(WLR_INFO, "output %s adopted the parked workspaces",
	        o->wlr_output->name);
}

static void new_output(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, s->allocator, s->renderer);

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&st, mode);
	wlr_output_commit_state(wlr_output, &st);
	wlr_output_state_finish(&st);

	struct aro_output *o = calloc(1, sizeof *o);
	if (!o)
		return;
	o->server = s;
	o->wlr_output = wlr_output;
	o->scale = wlr_output->scale > 0 ? wlr_output->scale : 1.0f;
	o->cur_ws = 0;

	o->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &o->destroy);
	wl_list_insert(&s->outputs, &o->link);

	struct wlr_output_layout_output *lo =
		wlr_output_layout_add_auto(s->output_layout, wlr_output);
	struct wlr_scene_output *so = wlr_scene_output_create(s->scene, wlr_output);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, so);

	/* the box has to exist before the bar can be placed against it */
	output_refresh_box(o);

	/* One bar per output. It lives in the shared l_bar layer — the scene
	 * clips each output's render to its own box, so a bar positioned over
	 * this output only appears on it. */
	if (!bar_create(&o->bar, o))
		wlr_log(WLR_ERROR, "could not build a bar for this output");

	if (!s->focused_output)
		s->focused_output = o;

	update_backdrop(s);
	output_adopt_parked(s, o);      /* after the boxes and layers are final */
	aro_arrange(s);
}

/* ── input ─────────────────────────────────────────────────────────────── */

/* Ctrl+Alt+F1..F12. The keymap only produces these keysyms with both
 * modifiers down, so this is checked against the TRANSLATED syms, unlike
 * every other binding below. Without it a TTY session has no way out except
 * killing the compositor from another machine. */
static bool handle_vt(struct aro_server *s, xkb_keysym_t sym)
{
	if (!s->session)
		return false;
	if (sym < XKB_KEY_XF86Switch_VT_1 || sym > XKB_KEY_XF86Switch_VT_12)
		return false;
	wlr_session_change_vt(s->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
	return true;
}

/*
 * Bindings. `sym` here is the UNSHIFTED keysym (see keyboard_key), so
 * shift is read from `mods` and never changes which case matches.
 *
 *   mod+Return      terminal          mod+d           launcher
 *   mod+v / mod+s   next window opens right / below
 *   mod+q           close             mod+shift+e     quit aro
 *   mod+hjkl        move focus        mod+shift+hjkl  move the window
 *   mod+ctrl+hjkl   resize            mod+1..4        workspace
 *   mod+shift+1..4  send window to workspace
 *   ctrl+alt+F1..12 switch TTY
 */
/*
 * Put the cursor on a window the keyboard just moved to.
 *
 * Only for keyboard-driven focus changes. Clicking already puts the cursor
 * where it belongs, and warping on every focus change would fight the mouse
 * — a window opening or closing must not move the pointer under your hand.
 *
 * The pointer focus has to be re-sent afterwards: the cursor has moved
 * without a motion event, so nothing else would tell the client underneath
 * that it is now being hovered.
 */
static void cursor_warp_to_view(struct aro_server *s, struct aro_view *v)
{
	if (!v)
		return;

	ly_box b = view_target(v);
	if (b.w <= 0 || b.h <= 0)
		return;

	wlr_cursor_warp_closest(s->cursor, NULL,
	                        b.x + b.w / 2.0, b.y + b.h / 2.0);
	pointer_motion_common(s, aro_now_ms());
}

/*
 * Run one bound action. Everything a binding can do lives here, so the
 * config parser only has to name it — and the built-in bindings and a
 * user's config go down the same path.
 */
/* ── quitting ──────────────────────────────────────────────────────────── */

static void quit_now(struct aro_server *s)
{
	wl_display_terminate(s->display);
}

/*
 * The quit bind, when confirm_quit is on. The detail line states the real
 * stakes — how many windows go with it — because "Are you sure?" on its own
 * trains you to press y without reading.
 */
static void quit_ask(struct aro_server *s)
{
	if (prompt_active(s))
		return;

	struct aro_output *o = aro_focused_output(s);
	if (!o) {
		quit_now(s);            /* nowhere to ask: nothing to lose either */
		return;
	}

	/* a drag in flight would keep the pointer grabbed under the card */
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_end(s);

	int n = 0;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped)
			n++;

	char detail[64];
	if (n == 0)
		snprintf(detail, sizeof detail, "Nothing is open.");
	else
		snprintf(detail, sizeof detail, "%d open window%s will close.",
		         n, n == 1 ? "" : "s");

	/*
	 * If the card cannot be built, do NOT quit. The prompt is the safety
	 * net; failing to put the net up is not permission to jump.
	 */
	if (!prompt_open(s, o, "Exit aro?", detail, "Exit", "Cancel",
	                 quit_now)) {
		notify(s, NOTIFY_INFO, "could not show the exit prompt; "
		       "set confirm_quit = false to quit without it");
		return;
	}

	/* the window under the pointer gets a leave now, not whatever the
	 * pointer does next while the card holds it */
	wlr_seat_pointer_clear_focus(s->seat);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
}

/*
 * After routing input to the prompt: if that input closed it, give the
 * pointer back to whatever is under it, so the window you return to gets
 * an enter rather than waiting for the mouse to move.
 */
static void prompt_after(struct aro_server *s, bool was_active)
{
	if (was_active && !prompt_active(s))
		pointer_motion_common(s, aro_now_ms());
}

static void run_action(struct aro_server *s, const struct q_bind *b)
{
	struct aro_view *f = s->focused;

	switch (b->action) {
	case Q_SPAWN:
		config_spawn(b->arg);
		return;
	case Q_CLOSE:
		if (f)
			view_close(f);
		return;
	case Q_QUIT:
		if (s->cfg.confirm_quit)
			quit_ask(s);
		else
			quit_now(s);
		return;
	case Q_FLOAT:
		if (f)
			view_set_floating(s, f, !f->floating);
		return;
	case Q_FULLSCREEN:
		if (f)
			view_set_fullscreen(s, f, !f->fullscreen);
		return;
	case Q_SPLIT:
		/* one-shot, i3 style — and it beats dwindle for this window */
		s->pending_split = (ly_dir)b->num;
		s->split_forced = true;
		return;
	case Q_WORKSPACE:
		workspace_show(s, b->num);
		return;
	case Q_SENDTO:
		view_send_to(s, f, b->num);
		return;
	case Q_FOCUS:
	case Q_MOVE:
	case Q_RESIZE:
		break;                                  /* below */
	case Q_NONE:
		return;
	}

	/* the directional three, which all need a focused tiled window */
	if (!f || !f->node || !f->output)
		return;

	ly_edge e = (ly_edge)b->num;
	struct aro_output *o = f->output;

	if (b->action == Q_RESIZE) {
		if (ly_resize(f->node, e, s->cfg.theme.resize_step))
			aro_arrange(s);
		return;
	}

	if (b->action == Q_MOVE) {
		ly_node *target = ly_focus(o->ws[o->cur_ws], f->node, e);
		if (target && target->user) {
			view_swap(f, target->user);
			aro_arrange(s);
		} else {
			/* nothing that way on this screen: hand the window over */
			struct aro_output *dest = output_toward(s, o, e);
			if (dest)
				view_move_to_output(s, f, dest);
		}
		return;
	}

	ly_node *next = ly_focus(o->ws[o->cur_ws], f->node, e);
	if (next) {
		aro_focus(s, next->user);
		cursor_warp_to_view(s, next->user);
		return;
	}

	/*
	 * Focus falls off the edge of one tree and onto the next screen.
	 * ly_focus is spatial within a tree; this is the same idea one level
	 * up, between them. An empty output is still somewhere to go, so the
	 * output focus moves even with nothing to focus on it.
	 */
	struct aro_output *dest = output_toward(s, o, e);
	if (dest) {
		s->focused_output = dest;
		struct aro_view *cand = output_pick_view(s, dest);
		aro_focus(s, cand);
		cursor_warp_to_view(s, cand);
	}
}

/*
 * Bindings come from the config, or from the built-in set when there is no
 * config. Matching is on the LEVEL-0 keysym with the modifier mask compared
 * exactly: translated symbols turn `e` into `E` and `1` into `!`, which
 * silently killed every shifted binding once.
 */
static bool handle_bind(struct aro_server *s, uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * No bindings while locked — but FALSE, not true.
	 *
	 * Returning true means "handled", and the caller then does not forward
	 * the key to the seat. That swallows everything the user types into
	 * the lock screen, so the password never arrives and the only way out
	 * is a TTY switch. False means no binding matched and the key goes to
	 * the lock surface, which is exactly what should happen.
	 *
	 * VT switching is handled before this and still works: the other VT
	 * has its own login, so it does not expose the session.
	 */
	if (aro_locked(s))
		return false;

	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	                      WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	mods &= care;

	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->sym == sym && b->mods == mods) {
			run_action(s, b);
			return true;
		}
	}
	return false;
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

	/* Translated syms have modifiers applied: shift+e is E, shift+1 is !.
	 * Matching bindings against those means every shifted bind silently
	 * fails, so bindings use level-0 syms and read shift from `mods`.
	 * VT switching is the exception — those keysyms only exist translated. */
	const xkb_keysym_t *trans, *raw;
	int ntrans = xkb_state_key_get_syms(state, keycode, &trans);
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
	int nraw = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &raw);

	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	bool handled = false;
	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (int i = 0; i < ntrans; i++)
			handled |= handle_vt(s, trans[i]);

	/*
	 * A prompt is modal: it takes every key, press AND release, so a
	 * client never sees half a keystroke. VT switching still works above
	 * it, and the lock still wins over it — keys go to the lock surface,
	 * which is drawn over the card anyway.
	 */
	if (!handled && !aro_locked(s) && prompt_active(s)) {
		if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
			for (int i = 0; i < nraw && prompt_active(s); i++)
				prompt_key(s, raw[i]);
		prompt_after(s, true);
		return;
	}

	if (!handled && ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (int i = 0; i < nraw; i++)
			handled |= handle_bind(s, mods, raw[i]);

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

/*
 * NULL xkb fields fall back to the XKB_DEFAULT_* environment variables and
 * then to the system default, so an unset key in the config behaves exactly
 * as it did before there was a config.
 */
static void apply_keymap(struct aro_server *s, struct wlr_keyboard *wlr_kb)
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

static void new_keyboard(struct aro_server *s, struct wlr_input_device *dev)
{
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(dev);

	struct aro_keyboard *kb = calloc(1, sizeof *kb);
	if (!kb)
		return;
	kb->server = s;
	kb->wlr_keyboard = wlr_kb;

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

static void new_input(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;

	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
		new_keyboard(s, dev);
	} else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
		wlr_cursor_attach_input_device(s->cursor, dev);

		/*
		 * Some pointers belong to one output rather than to the whole
		 * layout. The nested wayland backend makes one per host window,
		 * each reporting absolute positions relative to its own surface;
		 * touchscreens and tablets are the same idea on real hardware.
		 *
		 * Without this mapping those positions are stretched across the
		 * entire layout, so a click in the second window lands in the
		 * first and the second output can never be reached. A pointer
		 * with no output_name — an ordinary mouse — is left alone and
		 * keeps the run of every screen.
		 */
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

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

/*
 * Pointer handling. Motion and buttons are forwarded to the client under the
 * cursor unless we have taken the pointer for a drag.
 *
 * `surface` comes back NULL when the cursor is over our own chrome — the
 * border or the header strip — rather than over the client. That distinction
 * is what makes dragging a header work without a modifier: the scene node
 * under the cursor is one of our rects, not a buffer, so there is no client
 * to send the event to and we can take it ourselves.
 */
static struct aro_view *view_at(struct aro_server *s, double lx, double ly,
                                   struct wlr_surface **surface,
                                   double *sx, double *sy)
{
	*surface = NULL;

	/*
	 * While locked the scene still contains every window, and they are
	 * still hit-testable — the lock only covers them visually. Refusing
	 * to report anything is what stops a click landing on a window behind
	 * it. The lock's own surfaces are found by the normal path below.
	 */
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

	/* walk up to whichever frame_tree carries a view, chrome or not */
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data)
		tree = tree->node.parent;
	return tree ? tree->node.data : NULL;
}

static void pointer_motion_common(struct aro_server *s, uint32_t time)
{
	drag_icon_update(s);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct aro_view *v = view_at(s, s->cursor->x, s->cursor->y,
	                                &surface, &sx, &sy);

	/*
	 * Hovering focuses, when the config asks for it. The output follows
	 * too, so an empty screen can be reached without clicking.
	 *
	 * Only when the pointer is over a window: crossing the gaps between
	 * frames would otherwise drop focus on the way past, and a focus that
	 * flickers off mid-gesture is worse than one that lags. A layer
	 * surface holding the keyboard (a launcher) keeps it.
	 */
	if (s->cfg.focus_follows_mouse && !s->focused_layer) {
		struct aro_output *po = output_at(s, s->cursor->x, s->cursor->y);
		if (po)
			s->focused_output = po;
		if (v && v != s->focused)
			aro_focus(s, v);
	} else {
		(void)v;
	}

	/*
	 * While a button is held the pointer focus must not move.
	 *
	 * The surface that received the press keeps receiving motion until the
	 * button is released, even once the cursor has wandered off it — that
	 * is the implicit grab, and it is what menus are built on. Re-running
	 * view_at on every motion and entering whatever is underneath sends the
	 * original surface a leave instead, which breaks the grab: an xterm
	 * menu opens on press and closes the instant you move.
	 *
	 * Coordinates stay in the grabbed surface's space, computed from the
	 * origin recorded at enter. They legitimately go negative or past the
	 * surface's size while the cursor is outside it; clients expect that.
	 */
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

static void cursor_motion(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_motion);
	idle_activity(s);
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_motion(s);
	else
		pointer_motion_common(s, ev->time_msec);
}

static void cursor_motion_abs(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_motion_abs);
	idle_activity(s);
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_motion(s, s->cursor->x, s->cursor->y);
		return;
	}
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_motion(s);
	else
		pointer_motion_common(s, ev->time_msec);
}

static void cursor_button(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_button);
	idle_activity(s);
	struct wlr_pointer_button_event *ev = data;

	/* modal: the card answers every button, and nothing reaches a client */
	if (!aro_locked(s) && prompt_active(s)) {
		prompt_pointer_button(s, s->cursor->x, s->cursor->y,
		                      ev->state == WL_POINTER_BUTTON_STATE_PRESSED);
		prompt_after(s, true);
		return;
	}

	/* A drag ends on any button release, whichever button it was. Missing
	 * this leaves the pointer grabbed with no way to let go. */
	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED &&
	    s->cursor_mode != ARO_CURSOR_PASSTHROUGH) {
		grab_end(s);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct aro_view *v = view_at(s, s->cursor->x, s->cursor->y,
	                                &surface, &sx, &sy);

	/*
	 * Clicking anywhere on an output focuses that output — including its
	 * empty background, which is the only way a screen with nothing on it
	 * can ever become the one new windows open on.
	 *
	 * On a click rather than on motion, deliberately. Tying this to the
	 * pointer moving means a cursor left resting on one screen silently
	 * redirects every new window there while you work by keyboard on the
	 * other, and nested it is worse still: the host compositor decides
	 * which of our windows gets the real keyboard, and nothing keeps that
	 * in step with where our pointer happens to be. A click moves both at
	 * once, so they cannot drift.
	 */
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

		/*
		 * mod+right resizes. So does a press on the frame's border, with
		 * no modifier — that is what a border is for, and the zone is
		 * wider than the 2px line so it is actually hittable. A press
		 * anywhere else on our chrome (the header) moves.
		 */
		bool want_resize = (ev->button == BTN_RIGHT && (mods & s->cfg.modkey)) ||
		                   (on_chrome && zone != 0);

		if ((mods & s->cfg.modkey) || on_chrome) {
			uint32_t edges = zone;
			if (want_resize && edges == 0) {
				/* grabbed in the middle: pick the nearest edge so the
				 * gesture still means something */
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

	/*
	 * Make sure the pointer focus is actually current before forwarding.
	 *
	 * Motion is what normally establishes it, and a click is normally
	 * preceded by one — but not always. A surface can appear or vanish
	 * under a stationary cursor, which is exactly what an X11 menu does:
	 * it takes pointer focus while it is up, then unmaps. Without a motion
	 * event in between, the next press is delivered to a surface that is
	 * no longer there and the client sees nothing at all.
	 *
	 * notify_enter is a no-op when the surface is already focused, so this
	 * costs nothing in the common case.
	 */
	if (surface) {
		s->ptr_lx = s->cursor->x - sx;
		s->ptr_ly = s->cursor->y - sy;
		wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
	}

	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_axis);
	idle_activity(s);
	struct wlr_pointer_axis_event *ev = data;
	if (!aro_locked(s) && prompt_active(s))
		return;                 /* no scrolling the window under the card */
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation,
	                             ev->delta, ev->delta_discrete, ev->source,
	                             ev->relative_direction);
}

static void cursor_frame(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, cursor_frame);
	(void)data;
	wlr_seat_pointer_notify_frame(s->seat);
}

static void request_cursor(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (s->seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_surface(s->cursor, ev->surface,
		                       ev->hotspot_x, ev->hotspot_y);
}

static int clock_tick(void *data)
{
	struct aro_server *s = data;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->bar.tree)
			bar_update(&o->bar, o);
		wlr_output_schedule_frame(o->wlr_output);
	}

	wl_event_source_timer_update(s->clock_timer, 1000);
	return 0;
}

#ifdef ARO_XWAYLAND
/* ── XWayland ──────────────────────────────────────────────────────────── */

/*
 * X11 windows, through wlr_xwayland.
 *
 * Three things differ from xdg-shell and everything here follows from them:
 *
 *  - An X11 window is TOLD its position and believes it. There is no size
 *    negotiation: we configure it with an absolute box and it complies, so
 *    xwl_configure takes the whole rectangle where xdg ignores x and y.
 *
 *  - The surface arrives later than the window. An xwayland_surface exists
 *    as soon as the X client creates the window, but has no wl_surface until
 *    it is "associated" — so map, unmap and commit can only be listened for
 *    then, and have to be detached again on dissociate.
 *
 *  - Override-redirect windows — menus, tooltips, drag icons — must not be
 *    framed or tiled at all. X11 has already decided exactly where they go
 *    and the window manager is explicitly not consulted. They are a third
 *    category beside tiled and floating: not managed.
 */

static void xwl_configure(struct aro_view *v, int x, int y, int w, int h)
{
	if (v->xsurface)
		wlr_xwayland_surface_configure(v->xsurface, x, y, w, h);
}

static void xwl_close(struct aro_view *v)
{
	if (v->xsurface)
		wlr_xwayland_surface_close(v->xsurface);
}

static void xwl_activate(struct aro_view *v, bool activated)
{
	if (v->xsurface)
		wlr_xwayland_surface_activate(v->xsurface, activated);
}

static void xwl_set_fullscreen(struct aro_view *v, bool fullscreen)
{
	if (v->xsurface)
		wlr_xwayland_surface_set_fullscreen(v->xsurface, fullscreen);
}

static const char *xwl_title(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->title : NULL;
}

/* The class half of WM_CLASS ("Gimp"), not the instance ("gimp") — the one
 * that names the application rather than this run of it. Rules match it
 * case-insensitively, so the difference is mostly cosmetic.
 * API RISK: xsurface->class, not compiled. */
static const char *xwl_app_id(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->class : NULL;
}

/*
 * The X11 equivalents of the xdg float heuristics: a transient window (one
 * that names a parent) is a dialog, and equal min/max hints mean the client
 * cannot be resized. modal is the third, and is unambiguous.
 *
 * API RISK: size_hints is a pointer and may be NULL; the flags field naming
 * differs between xcb versions, so this checks the values rather than flags.
 */
static bool xwl_wants_float(struct aro_view *v)
{
	struct wlr_xwayland_surface *x = v->xsurface;
	if (!x)
		return false;
	if (x->modal || x->parent)
		return true;

	if (x->size_hints) {
		int minw = x->size_hints->min_width, maxw = x->size_hints->max_width;
		int minh = x->size_hints->min_height, maxh = x->size_hints->max_height;
		if (minw > 0 && maxw > 0 && minh > 0 && maxh > 0 &&
		    minw == maxw && minh == maxh)
			return true;
	}
	return false;
}

static bool xwl_wants_fullscreen(struct aro_view *v)
{
	return v->xsurface && v->xsurface->fullscreen;
}

static void xwl_preferred_size(struct aro_view *v, int *w, int *h)
{
	*w = v->xsurface ? v->xsurface->width : 0;
	*h = v->xsurface ? v->xsurface->height : 0;
}

static void xwl_geometry(struct aro_view *v, struct wlr_box *out)
{
	/* X11 windows have no invisible margins: the surface is the window */
	*out = (struct wlr_box){ 0, 0,
		v->xsurface ? v->xsurface->width : 0,
		v->xsurface ? v->xsurface->height : 0 };
}

static struct wlr_surface *xwl_surface(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->surface : NULL;
}

static const struct view_impl xwl_impl = {
	.configure        = xwl_configure,
	.close            = xwl_close,
	.activate         = xwl_activate,
	.set_fullscreen   = xwl_set_fullscreen,
	.title            = xwl_title,
	.app_id           = xwl_app_id,
	.geometry         = xwl_geometry,
	.wants_float      = xwl_wants_float,
	.wants_fullscreen = xwl_wants_fullscreen,
	.preferred_size   = xwl_preferred_size,
	.surface          = xwl_surface,
};

/* ── override-redirect ─────────────────────────────────────────────────── */

/*
 * Not managed, not framed, not tiled. X11 menus and tooltips place
 * themselves; we put the surface exactly where it asks and otherwise stay
 * out of the way. Getting this wrong makes every X11 menu appear as a
 * titled, tiled window, which is unmistakable.
 */
struct aro_unmanaged {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *tree;

	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener request_configure;
	struct wl_listener destroy;
};

static void unmanaged_map(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, map);
	(void)data;

	u->tree = wlr_scene_subsurface_tree_create(u->server->l_unmanaged,
	                                           u->xsurface->surface);
	if (u->tree)
		wlr_scene_node_set_position(&u->tree->node,
		                            u->xsurface->x, u->xsurface->y);
}

static void unmanaged_unmap(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, unmap);
	(void)data;
	if (u->tree)
		wlr_scene_node_destroy(&u->tree->node);
	u->tree = NULL;
}

static void unmanaged_request_configure(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	/* it knows where it wants to be; say yes */
	wlr_xwayland_surface_configure(u->xsurface, ev->x, ev->y,
	                               ev->width, ev->height);
	if (u->tree)
		wlr_scene_node_set_position(&u->tree->node, ev->x, ev->y);
}

static void unmanaged_associate(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, associate);
	(void)data;
	u->map.notify = unmanaged_map;
	wl_signal_add(&u->xsurface->surface->events.map, &u->map);
	u->unmap.notify = unmanaged_unmap;
	wl_signal_add(&u->xsurface->surface->events.unmap, &u->unmap);
}

static void unmanaged_dissociate(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, dissociate);
	(void)data;
	wl_list_remove(&u->map.link);
	wl_list_remove(&u->unmap.link);
	wl_list_init(&u->map.link);     /* destroy removes them again */
	wl_list_init(&u->unmap.link);
}

static void unmanaged_destroy(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, destroy);
	(void)data;
	wl_list_remove(&u->associate.link);
	wl_list_remove(&u->dissociate.link);
	wl_list_remove(&u->request_configure.link);
	wl_list_remove(&u->destroy.link);
	wl_list_remove(&u->link);
	free(u);
}

static void new_unmanaged(struct aro_server *s,
                          struct wlr_xwayland_surface *xsurface)
{
	struct aro_unmanaged *u = calloc(1, sizeof *u);
	if (!u)
		return;
	u->server = s;
	u->xsurface = xsurface;
	wl_list_init(&u->map.link);
	wl_list_init(&u->unmap.link);

	u->associate.notify = unmanaged_associate;
	wl_signal_add(&xsurface->events.associate, &u->associate);
	u->dissociate.notify = unmanaged_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &u->dissociate);
	u->request_configure.notify = unmanaged_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &u->request_configure);
	u->destroy.notify = unmanaged_destroy;
	wl_signal_add(&xsurface->events.destroy, &u->destroy);

	wl_list_insert(&s->unmanaged, &u->link);
}

/* ── managed X11 windows ───────────────────────────────────────────────── */

static void xwl_commit(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, commit);
	(void)data;
	/* new buffers arrive square; round them as they come */
	ui_frame_clip_content(v);
}

/*
 * Before it is mapped an X11 window may ask to be moved or resized. We are
 * going to tile it and override this anyway, but it has to be answered or
 * the client waits — so acknowledge exactly what it asked for, and let the
 * first arrange put it where it really goes.
 */
static void xwl_request_configure(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	const struct q_theme *th = &v->server->cfg.theme;

	if (!v->mapped) {
		wlr_xwayland_surface_configure(v->xsurface, ev->x, ev->y,
		                               ev->width, ev->height);
		return;
	}

	/* mapped and floating: honour the size, keep our position */
	if (v->floating && !v->fullscreen) {
		v->fbox.w = ev->width + th->border * 2;
		v->fbox.h = ev->height + th->border * 2 + th->header_h;
		aro_arrange(v->server);
	} else {
		/* tiled: re-assert where it actually is */
		ly_box b = view_target(v);
		wlr_xwayland_surface_configure(v->xsurface,
		                               b.x + th->border,
		                               b.y + th->border + th->header_h,
		                               b.w - th->border * 2,
		                               b.h - th->border * 2 - th->header_h);
	}
}

static void xwl_associate(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, associate);
	(void)data;

	v->surface_tree = wlr_scene_subsurface_tree_create(v->content,
	                                                   v->xsurface->surface);

	v->map.notify = view_map;
	wl_signal_add(&v->xsurface->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&v->xsurface->surface->events.unmap, &v->unmap);
	v->commit.notify = xwl_commit;
	wl_signal_add(&v->xsurface->surface->events.commit, &v->commit);
}

static void xwl_dissociate(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, dissociate);
	(void)data;
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_init(&v->map.link);
	wl_list_init(&v->unmap.link);
	wl_list_init(&v->commit.link);
}

static void xwl_destroy(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, destroy);
	(void)data;

	if (v->server->focused == v)
		v->server->focused = NULL;
	grab_forget(v->server, v);

	qtext_finish(&v->title);
	if (v->frame_tree)
		wlr_scene_node_destroy(&v->frame_tree->node);

	/* map/unmap/commit were detached on dissociate, but wl_list_init left
	 * them safe to remove again */
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->associate.link);
	wl_list_remove(&v->dissociate.link);
	wl_list_remove(&v->request_configure.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->link);
	free(v);
}

static void new_xwayland_surface(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	if (xsurface->override_redirect) {
		new_unmanaged(s, xsurface);
		return;
	}

	struct aro_view *v = calloc(1, sizeof *v);
	if (!v)
		return;
	v->server = s;
	v->impl = &xwl_impl;
	v->csd = false;     /* X11 clients expect us to decorate them */
	v->xsurface = xsurface;

	if (!ui_frame_create(v, s->l_tiled)) {
		wlr_log(WLR_ERROR, "could not build a frame for an X11 window");
		free(v);
		return;
	}
	v->frame_tree->node.data = v;   /* view_at() walks up looking for this */
	wlr_scene_node_set_enabled(&v->frame_tree->node, false);

	/* no surface yet: associate is what tells us it exists */
	wl_list_init(&v->map.link);
	wl_list_init(&v->unmap.link);
	wl_list_init(&v->commit.link);

	v->associate.notify = xwl_associate;
	wl_signal_add(&xsurface->events.associate, &v->associate);
	v->dissociate.notify = xwl_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &v->dissociate);
	v->set_title.notify = view_set_title;
	wl_signal_add(&xsurface->events.set_title, &v->set_title);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen, &v->request_fullscreen);
	v->request_move.notify = view_request_move;
	wl_signal_add(&xsurface->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_resize;
	wl_signal_add(&xsurface->events.request_resize, &v->request_resize);
	v->request_configure.notify = xwl_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &v->request_configure);
	v->destroy.notify = xwl_destroy;
	wl_signal_add(&xsurface->events.destroy, &v->destroy);

	wl_list_insert(&s->views, &v->link);
}

static void xwayland_ready(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, xwayland_ready);
	(void)data;
	wlr_xwayland_set_seat(s->xwayland, s->seat);
	wlr_log(WLR_INFO, "xwayland ready on %s", s->xwayland->display_name);
}
#endif /* ARO_XWAYLAND */

/* ── selections and drag-and-drop ──────────────────────────────────────── */

/*
 * Creating the data-device manager is not enough: when a client copies, the
 * seat asks US whether that client may own the selection, and if nobody
 * answers the request is dropped and the clipboard silently never works.
 *
 * Accepting unconditionally is what every compositor does — the check exists
 * for policy nobody has, and refusing would mean no client could ever copy.
 *
 * This is also what XWayland bridges through: wlr_xwayland_set_seat() hooks
 * the X11 selection to this same seat, so answering here makes copy and
 * paste work between X11 and Wayland clients in both directions.
 */
static void request_set_selection(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

/* the middle-click "primary" selection, which is a separate clipboard */
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

	/* Only honour a drag the client can actually justify: the serial has
	 * to match an input event the client received. Skipping this check is
	 * how a client steals the pointer without the user having clicked. */
	if (wlr_seat_validate_pointer_grab_serial(s->seat, ev->origin, ev->serial))
		wlr_seat_start_pointer_drag(s->seat, ev->drag, ev->serial);
	else
		wlr_data_source_destroy(ev->drag->source);
}

/* Keep the drag icon under the cursor. Called from both pointer paths, since
 * a drag can be in flight while we are running a grab of our own. */
static void drag_icon_update(struct aro_server *s)
{
	if (s->drag_icon)
		wlr_scene_node_set_position(&s->drag_icon->node,
		                            (int)s->cursor->x, (int)s->cursor->y);
}

static void drag_icon_destroy(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, drag_icon_destroy);
	(void)data;
	/* the scene node belongs to the icon and goes with it */
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

	/* Above everything the user can drop onto — including a fullscreen
	 * window, since you can drag onto one. */
	s->drag_icon = wlr_scene_drag_icon_create(s->l_overlay, drag->icon);
	drag_icon_update(s);

	s->drag_icon_destroy.notify = drag_icon_destroy;
	wl_signal_add(&drag->icon->events.destroy, &s->drag_icon_destroy);
}

/* ── live config reload ────────────────────────────────────────────────── */

/*
 * Re-read the config and apply it without restarting anything.
 *
 * The rule is that a reload must leave the session exactly as a fresh start
 * with that file would — so everything derived from config is recomputed,
 * not patched. Cheap enough to do wholesale: parsing is a few hundred lines
 * of text and the rest is setting colours.
 *
 * Bindings and the modifier come along for free, since handle_bind reads the
 * table on every keypress rather than caching anything.
 */
/*
 * Put every parse problem on screen. Capped, because a file that is badly
 * wrong — the wrong file entirely, say — would otherwise bury the desktop
 * in toasts, and the log has all of them regardless.
 */
static void notify_config_errors(struct aro_server *s)
{
	const int cap = 4;
	int n = s->cfg.nerrors;

	/* Whatever was wrong before has been re-judged by this parse: a fixed
	 * line should take its toast away with it. */
	notify_clear_errors(s);

	for (int i = 0; i < n && i < cap; i++)
		notify(s, NOTIFY_ERROR, "%s", s->cfg.errors[i]);
	if (n > cap)
		notify(s, NOTIFY_ERROR, "...and %d more config problems", n - cap);
}

static void config_reload(struct aro_server *s)
{
	struct aro_config nc;
	config_defaults(&nc);
	config_load(&nc, s->cfg_path);

	config_finish(&s->cfg);
	s->cfg = nc;

	wlr_log(WLR_INFO, "config reloaded");

	/* keymap: the layout may have changed under us */
	struct aro_keyboard *kb;
	wl_list_for_each(kb, &s->keyboards, link)
		apply_keymap(s, kb->wlr_keyboard);

	/* the backdrop seen through the gaps */
	if (s->root_bg) {
		float bg[4];
		ui_color(s->cfg.theme.bg, bg);
		wlr_scene_rect_set_color(s->root_bg, bg);
	}

	/* colours, radii and fonts; sizes follow from the arrange below */
	struct aro_view *v, *vtmp;
	wl_list_for_each(v, &s->views, link)
		ui_frame_retheme(v);

	/*
	 * Rules apply to windows that are already open, not only new ones —
	 * but only where the answer changed. Saving the file to tweak a
	 * colour must not yank a window you moved by hand back to where a
	 * rule once put it. Safe iteration: nothing here destroys a view
	 * today, but a rule action reshuffling the list would be the day
	 * that stops being true.
	 */
	wl_list_for_each_safe(v, vtmp, &s->views, link)
		view_rules_reapply(v);

	ui_preview_retheme(&s->preview);

	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		bar_retheme(&o->bar);

	/* bar height feeds the usable area, so this has to come before arrange */
	update_backdrop(s);
	aro_arrange(s);

	notify_retheme(s);
	prompt_retheme(s);
	notify_config_errors(s);

	for (int i = 0; i < s->cfg.nexec_always; i++)
		config_spawn(s->cfg.exec_always[i]);
}

/*
 * The watch is on the config's DIRECTORY, not the file.
 *
 * Every editor worth using writes to a temporary file and renames it over
 * the original, which replaces the inode — a watch on the file itself sees
 * the first save and nothing after. Watching the directory and filtering by
 * name catches rename, create and plain writes alike.
 */
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

	/* watch the directory the file lives in */
	char dir[512];
	snprintf(dir, sizeof dir, "%s", s->cfg_path);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';

	s->cfg_wd = inotify_add_watch(s->cfg_fd, dir,
	                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (s->cfg_wd < 0) {
		/* no config directory yet is not an error — there is just
		 * nothing to watch until someone creates one */
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

int main(int argc, char *argv[])
{
	wlr_log_init(WLR_DEBUG, NULL);

	const char *startup = NULL;
	const char *modkey_override = NULL;      /* -m beats the config file */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			startup = argv[++i];
		else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
			modkey_override = argv[++i];
		else {
			fprintf(stderr,
			        "usage: %s [-s startup-command] [-m logo|alt]\n", argv[0]);
			return 1;
		}
	}

	struct aro_server s = { 0 };
	config_defaults(&s.cfg);
	config_load(&s.cfg, NULL);

	/*
	 * -m last, so it beats the config: it exists for testing nested inside
	 * another compositor, where Super never reaches us, and having to edit
	 * the config to do that would defeat the point.
	 *
	 * The bindings were already built against the old modifier, so they
	 * have to be rebuilt — the same rebuild the `mod` key does mid-file.
	 */
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

	s.pending_split = LY_ROW;
	wl_list_init(&s.outputs);
	wl_list_init(&s.views);
	wl_list_init(&s.keyboards);
	wl_list_init(&s.layers);
	wl_list_init(&s.notifications);

	s.display = wl_display_create();
	s.loop = wl_display_get_event_loop(s.display);

	s.backend = wlr_backend_autocreate(s.loop, &s.session);
	if (!s.backend) {
		wlr_log(WLR_ERROR, "no backend");
		return 1;
	}

	/* SceneFX renders through its own GLES renderer; the scene graph will
	 * not apply any effect without it.
	 *
	 * Both of these return NULL on failure and every line after them
	 * dereferences the result, so an unchecked failure here is a segfault
	 * during startup with nothing logged. fx_renderer_create() is the more
	 * likely of the two to fail: it is GLES2-only and needs extensions the
	 * generic renderer can do without. */
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

	s.compositor = wlr_compositor_create(s.display, 5, s.renderer);
	wlr_subcompositor_create(s.display);
	wlr_data_device_manager_create(s.display);

	/*
	 * Globals clients treat as mandatory. tinywl creates almost none of
	 * these, so anything beyond a bare terminal refuses to start:
	 *
	 *   xdg-output        output names and logical geometry. Layer-shell
	 *                     clients (fuzzel, waybar) need it to pick a screen.
	 *   viewporter        surface scaling and cropping
	 *   fractional-scale  non-integer scale factors
	 *   single-pixel      cheap solid-colour surfaces
	 *   primary-selection middle-click paste
	 *   presentation-time accurate frame timing; video players want it
	 *   screencopy        grim and friends — how screenshots happen
	 */
	s.output_layout = wlr_output_layout_create(s.display);
	if (!s.output_layout) {
		wlr_log(WLR_ERROR, "could not create the output layout");
		return 1;
	}
	wlr_xdg_output_manager_v1_create(s.display, s.output_layout);
	wlr_viewporter_create(s.display);
	wlr_fractional_scale_manager_v1_create(s.display, 1);
	wlr_single_pixel_buffer_manager_v1_create(s.display);
	wlr_primary_selection_v1_device_manager_create(s.display);

	/*
	 * xdg-foreign: one client exports a surface handle, another imports it
	 * and says "my window belongs to that one".
	 *
	 * Needed because out-of-process dialogs are normal now — Firefox runs
	 * its file picker in a separate process, and without this GTK refuses
	 * to open the dialog at all rather than opening a parentless one. The
	 * symptom is a warning about missing xdg_foreign support and no
	 * window, which looks nothing like a missing protocol.
	 *
	 * v1 and v2 share one registry; both exist because clients have not
	 * all moved to v2.
	 */
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

	/* creation order IS stacking order */
	float bg[4];
	ui_color(s.cfg.theme.bg, bg);
	s.root_bg = wlr_scene_rect_create(&s.scene->tree, 1920, 1080, bg);
	s.l_background = wlr_scene_tree_create(&s.scene->tree);
	s.l_bottom     = wlr_scene_tree_create(&s.scene->tree);
	s.l_tiled      = wlr_scene_tree_create(&s.scene->tree);
	s.l_preview    = wlr_scene_tree_create(&s.scene->tree);
	s.l_float      = wlr_scene_tree_create(&s.scene->tree);
	s.l_unmanaged  = wlr_scene_tree_create(&s.scene->tree);
	s.l_bar        = wlr_scene_tree_create(&s.scene->tree);
	s.l_top        = wlr_scene_tree_create(&s.scene->tree);
	s.l_fullscreen = wlr_scene_tree_create(&s.scene->tree);
	s.l_notify     = wlr_scene_tree_create(&s.scene->tree);
	s.l_overlay    = wlr_scene_tree_create(&s.scene->tree);
	if (!s.root_bg || !s.l_background || !s.l_bottom || !s.l_tiled ||
	    !s.l_preview || !s.l_float || !s.l_unmanaged || !s.l_bar || !s.l_top || !s.l_fullscreen || !s.l_notify ||
	    !s.l_overlay) {
		wlr_log(WLR_ERROR, "could not build the scene layers");
		return 1;
	}

	/* Bars are created per output, in new_output(). */

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
	s.new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);

#ifdef ARO_XWAYLAND
	/*
	 * lazy = true: the Xwayland server is not started until an X client
	 * actually connects, so a session with no X11 apps never pays for it.
	 */
	wl_list_init(&s.unmanaged);
	s.xwayland = wlr_xwayland_create(s.display, s.compositor, true);
	if (!s.xwayland) {
		wlr_log(WLR_ERROR, "xwayland failed to start; X11 clients will not run");
	} else {
		s.new_xwayland_surface.notify = new_xwayland_surface;
		wl_signal_add(&s.xwayland->events.new_surface, &s.new_xwayland_surface);
		s.xwayland_ready.notify = xwayland_ready;
		wl_signal_add(&s.xwayland->events.ready, &s.xwayland_ready);

		/*
		 * Set DISPLAY now, not on ready. The display name is decided
		 * when the object is created; with lazy start the server does
		 * not launch until an X client connects, so waiting for ready
		 * means anything spawned before that — your first terminal —
		 * inherits no DISPLAY at all.
		 */
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
	s.xcursor_mgr = wlr_xcursor_manager_create(NULL, 24);

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

	/* needs the backend, so it comes after everything above */
	wlr_presentation_create(s.display, s.backend, 2);

	const char *socket = wl_display_add_socket_auto(s.display);
	if (!socket || !wlr_backend_start(s.backend)) {
		wlr_log(WLR_ERROR, "could not start");
		wl_display_destroy(s.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	/*
	 * The portal picks its backend by desktop name. Without this it finds
	 * nothing, and GTK's file chooser — which runs out of process — hangs
	 * waiting for a backend that will never answer. The symptom is a
	 * dialog that simply never appears, which looks like a compositor bug
	 * and is not one.
	 *
	 * Setting it only if unset: a session manager that already decided
	 * knows better than we do.
	 */
	setenv("XDG_CURRENT_DESKTOP", "aro", false);
	wlr_log(WLR_INFO, "aro running on %s", socket);

	/*
	 * Autostart last, so children inherit a usable environment: the
	 * Wayland socket exists by now, and DISPLAY was set when XWayland
	 * came up. Starting them any earlier means a bar that cannot connect.
	 */
	config_watch_start(&s);
	notify_config_errors(&s);

	for (int i = 0; i < s.cfg.nexec; i++)
		config_spawn(s.cfg.exec[i]);
	for (int i = 0; i < s.cfg.nexec_always; i++)
		config_spawn(s.cfg.exec_always[i]);
	if (startup)
		config_spawn(startup);

	wl_display_run(s.display);

	/*
	 * Teardown order is not cosmetic. The backend destroys outputs and
	 * input devices as it goes, and anything still listening to those —
	 * our listeners, the cursor — gets dragged through freed objects.
	 * Detach first, destroy from the top of the scene down, and leave the
	 * backend and display for last.
	 */
	wl_display_destroy_clients(s.display);

	/* our own state, while the scene it lives in is still valid */
	ui_preview_finish(&s.preview);
	notify_finish(&s);
	prompt_finish(&s);
	config_watch_stop(&s);
	config_finish(&s.cfg);
	/* outputs own the trees now; each frees its own in output_destroy() */
	if (s.clock_timer)
		wl_event_source_remove(s.clock_timer);

	wl_list_remove(&s.new_output.link);
	wl_list_remove(&s.new_xdg_toplevel.link);
	wl_list_remove(&s.new_xdg_popup.link);
#ifdef ARO_XWAYLAND
	if (s.xwayland) {
		wl_list_remove(&s.new_xwayland_surface.link);
		wl_list_remove(&s.xwayland_ready.link);
		/* before the seat and the display it is attached to */
		wlr_xwayland_destroy(s.xwayland);
		s.xwayland = NULL;
	}
#endif
	wl_list_remove(&s.new_decoration.link);
	lock_finish(&s);
	idle_finish(&s);
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

	wlr_scene_node_destroy(&s.scene->tree.node);
	wlr_xcursor_manager_destroy(s.xcursor_mgr);
	wlr_cursor_destroy(s.cursor);
	wlr_allocator_destroy(s.allocator);
	wlr_renderer_destroy(s.renderer);
	wlr_backend_destroy(s.backend);
	for (int i = 0; i < ARO_MAX_WS; i++) {
		ly_free(s.orphan_ws[i]);
		s.orphan_ws[i] = NULL;
	}
	wl_display_destroy(s.display);
	return 0;
}
