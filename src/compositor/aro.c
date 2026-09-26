/* keep _GNU_SOURCE first; inotify_init1 needs it */
#define _GNU_SOURCE

/*
 * aro.c: main compositor code.
 *
 * backend, outputs, shell, input, and the bridge between the layout
 * tree and real surfaces.
 */
#define _POSIX_C_SOURCE 200809L

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "config.h"
#include "aro.h"
#include "idle.h"
#include "ipc.h"
#include "logfile.h"
#include "text.h"
#include "theme.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

/* BTN_LEFT/BTN_RIGHT are evdev codes */
#include <linux/input-event-codes.h>

#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#if WLR_HAS_LIBINPUT_BACKEND
#include <libinput.h>
#include <wlr/backend/libinput.h>
#endif
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
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#ifdef ARO_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include <wlr/util/edges.h>
#include <wlr/util/region.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>


static const anim_ease SPRING = { TH_EASE_X1, TH_EASE_Y1, TH_EASE_X2, TH_EASE_Y2 };

/* flat easing, no overshoot; used for screen-edge/fullscreen animations */
static const anim_ease FLAT = { TH_EASE_FLAT_X1, TH_EASE_FLAT_Y1,
                                TH_EASE_FLAT_X2, TH_EASE_FLAT_Y2 };

static void arrange_layers(struct aro_server *s);
static void drag_icon_update(struct aro_server *s);
static void pointer_motion_common(struct aro_server *s, uint32_t time);
static void constraint_sync(struct aro_server *s);
static void ftl_sync_view(struct aro_view *v);
static void ftl_sync_activated(struct aro_server *s);
static void ftl_create(struct aro_view *v);
static void ftl_destroy(struct aro_view *v);
static void ftl_update_ids(struct aro_view *v);
static struct aro_output *output_from_wlr(struct aro_server *s,
                                          struct wlr_output *wo);
static void arrange_layers_output(struct aro_output *o);
static void output_mgr_update(struct aro_server *s);
static struct aro_output *output_evacuate(struct aro_server *s,
                                          struct aro_output *o);

uint32_t aro_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


/* layout -> screen */

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

/* output containing a point */
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

/* cache output box in layout coordinates */
static void output_refresh_box(struct aro_output *o)
{
	struct wlr_box box = { 0 };
	wlr_output_layout_get_box(o->server->output_layout, o->wlr_output, &box);
	o->box = (ly_box){ box.x, box.y, box.width, box.height };
}

/* resize root backdrop to cover the layout */
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
	wl_list_for_each(o, &s->outputs, link)
		output_refresh_box(o);
	/* also places the bars */
	arrange_layers(s);

	/* lock uses refreshed boxes too */
	lock_arrange(s);
}

/* tiling area after exclusive zones and bar */
static ly_box usable_area(struct aro_output *o)
{
	ly_box u = o->usable;
	if (u.w <= 0 || u.h <= 0)
		u = o->box;
	u.h -= bar_height(&o->bar);
	return u;
}

ly_box aro_output_usable(struct aro_output *o)
{
	return usable_area(o);
}

/* final target geometry */
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

/* workspace root; parked views use orphan root */
static ly_node **view_ws_root(struct aro_view *v)
{
	if (v->output)
		return &v->output->ws[v->workspace];
	return &v->server->orphan_ws[v->workspace];
}

/* runtime override, else config */
static enum q_layout ws_layout(struct aro_server *s, struct aro_output *o,
                               int ws)
{
	if (o && ws >= 0 && ws < ARO_MAX_WS &&
	    o->ws_layout[ws] != Q_LAYOUT_INHERIT)
		return (enum q_layout)o->ws_layout[ws];
	return config_ws_layout(&s->cfg, ws);
}

/* insert into a workspace tree; forced dir beats dwindle */
static ly_node *tree_insert(struct aro_server *s, struct aro_output *o,
                            int ws, struct aro_view *v, ly_node *target,
                            ly_dir dir, bool forced)
{
	ly_node **root = &o->ws[ws];
	ly_node *leaf;

	if (!*root) {
		leaf = *root = ly_leaf(v);
	} else {
		bool dwindle = ws_layout(s, o, ws) == Q_LAYOUT_DWINDLE;
		/* dwindle continues the spiral */
		if (!target)
			target = dwindle ? ly_last_leaf(*root) : ly_first_leaf(*root);
		if (dwindle && !forced)
			dir = target->box.w > target->box.h ? LY_ROW : LY_COL;
		leaf = ly_split(root, target, dir, v);
	}
	/* hidden trees too, so dwindle reads real boxes */
	if (leaf)
		ly_arrange(*root, usable_area(o), &(ly_metrics){
			.gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap,
			.min = s->cfg.theme.min });
	return leaf;
}

/* bar = auto: hide while another client reserves space */
static void bar_return_cancel(struct aro_output *o)
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
		/* configure initialized layer surfaces, not just mapped ones */
		if (!l->layer_surface->initialized || !l->scene)
			continue;
		/* skip surfaces bound to another output */
		if (l->layer_surface->output &&
		    l->layer_surface->output != o->wlr_output)
			continue;
		wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
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

static bool box_eq(ly_box a, ly_box b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* retarget only when the target changed */
static void view_retarget(anim_box *g, ly_box t, uint32_t now, uint32_t dur,
                          const anim_ease *ease)
{
	if (box_eq(g->active ? g->to : g->cur, t))
		return;
	anim_box_to(g, t, now, dur, ease);
}

/* mapped, on an output, and on its current workspace */
static bool view_visible(struct aro_view *v)
{
	return v->mapped && v->output && v->workspace == v->output->cur_ws;
}

/* ── workspace slide ───────────────────────────────────────────────────── */
/*
 * Drawing only. view_visible() stays the truth for focus, arrange, rules,
 * idle and hit testing: a window on the outgoing workspace is drawn while
 * it leaves, and is otherwise already gone.
 */

/* on the workspace sliding out of this output */
static bool view_leaving(struct aro_view *v)
{
	struct aro_output *o = v->output;
	return v->mapped && o && o->slide.active &&
	       v->workspace == o->slide.out_ws && v->workspace != o->cur_ws;
}

/* drawn this frame: visible, or on its way out */
static bool view_on_screen(struct aro_view *v)
{
	return view_visible(v) || view_leaving(v);
}

/* the outgoing workspace's offset now; the incoming one is this + span */
static int slide_offset(const struct aro_output *o, uint32_t now)
{
	uint32_t el = now - o->slide.start_ms;
	double t = el >= o->slide.dur_ms ? 1.0
		: anim_ease_eval(&FLAT, (double)el / (double)o->slide.dur_ms);
	double off = o->slide.from + (o->slide.to - o->slide.from) * t;
	return (int)(off + (off >= 0 ? 0.5 : -0.5));
}

/* where to draw a window this frame: geo.cur, plus the slide */
static ly_box view_draw_box(struct aro_view *v, uint32_t now)
{
	ly_box b = v->geo.cur;
	struct aro_output *o = v->output;
	if (!o || !o->slide.active)
		return b;

	int off;
	if (view_visible(v))
		off = slide_offset(o, now) + o->slide.span;
	else if (view_leaving(v))
		off = slide_offset(o, now);
	else
		return b;

	if (o->slide.vertical)
		b.y += off;
	else
		b.x += off;
	return b;
}

/* end a slide now: the outgoing workspace is hidden, the incoming one
 * is drawn where it belongs on the next frame */
static void slide_finish(struct aro_output *o)
{
	if (!o->slide.active)
		return;
	o->slide.active = false;

	struct aro_view *v;
	wl_list_for_each(v, &o->server->views, link) {
		if (v->output == o && v->workspace != o->cur_ws)
			wlr_scene_node_set_enabled(&v->frame_tree->node, false);
	}
	wlr_output_schedule_frame(o->wlr_output);
}

/* finish every slide whose time is up; an output that is not drawing
 * (DPMS) would otherwise never finish its own */
static void slides_reap(struct aro_server *s, uint32_t now)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->slide.active && now - o->slide.start_ms >= o->slide.dur_ms)
			slide_finish(o);
	}
}

static bool slides_active(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		if (o->slide.active)
			return true;
	return false;
}

/*
 * Begin (or redirect) a slide from workspace old to ws. Called before
 * cur_ws changes. A higher number comes in from the right, or from below.
 * If a slide is already running, the workspace that was coming in is
 * where it is now, and that is where the new slide starts from, so
 * reversing mid-way retraces instead of jumping.
 */
static void slide_start(struct aro_output *o, int old, int ws)
{
	struct aro_server *s = o->server;
	const bool vertical = s->cfg.ws_slide == Q_SLIDE_VERTICAL;
	const uint32_t now = aro_now_ms();

	if (s->cfg.ws_slide == Q_SLIDE_OFF || s->cfg.theme.ws_slide_ms <= 0 ||
	    !o->wlr_output->enabled || overview_shown(s)) {
		o->slide.active = false;
		return;
	}

	int base = 0;           /* where old is drawn right now */
	if (o->slide.active && o->slide.vertical == vertical &&
	    now - o->slide.start_ms < o->slide.dur_ms)
		base = slide_offset(o, now) + o->slide.span;

	const int size = vertical ? o->box.h : o->box.w;
	const int sgn = ws > old ? 1 : -1;

	o->slide.active = true;
	o->slide.out_ws = old;
	o->slide.vertical = vertical;
	o->slide.span = sgn * size;
	o->slide.from = base;
	o->slide.to = -sgn * size;
	o->slide.start_ms = now;
	o->slide.dur_ms = (uint32_t)s->cfg.theme.ws_slide_ms;
}

void aro_arrange(struct aro_server *s)
{
	const ly_metrics m = {
		.gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap, .min = s->cfg.theme.min,
	};

	uint32_t now = aro_now_ms();
	slides_reap(s, now);

	/* arrange each output's current workspace */
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		ly_node *root = o->ws[o->cur_ws];
		if (root)
			ly_arrange(root, usable_area(o), &m);
	}

	/* retarget visible views */
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v))
			continue;

		/* don't fight an active drag */
		if (s->grabbed == v && s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
			continue;

		ly_box t = view_target(v);

		/* fullscreen uses flat easing */
		if (v->fullscreen)
			view_retarget(&v->geo, t, now, s->cfg.theme.anim_fs_ms, &FLAT);
		else
			view_retarget(&v->geo, t, now, s->cfg.theme.anim_ms, &SPRING);

		if (!v->fullscreen)
			ui_frame_title(v, t.w, v->output->scale);
	}

	/* bar came or went: place, do not spring */
	wl_list_for_each(o, &s->outputs, link) {
		if (!o->bar_snap)
			continue;
		o->bar_snap = false;
		wl_list_for_each(v, &s->views, link) {
			if (v->output != o || !view_visible(v))
				continue;
			if (s->grabbed == v && s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
				continue;
			anim_box_set(&v->geo, view_target(v));
		}
	}

	wl_list_for_each(o, &s->outputs, link) {
		if (o->bar.tree)
			bar_update(&o->bar, o);
		wlr_output_schedule_frame(o->wlr_output);
	}

	/* recheck idle inhibitors */
	idle_update(s);

	/* taskbars see outputs and fullscreen changes; arrange follows all of them */
	struct aro_view *fv;
	wl_list_for_each(fv, &s->views, link)
		ftl_sync_view(fv);
}

static void view_set_visible(struct aro_view *v, bool visible)
{
	wlr_scene_node_set_enabled(&v->frame_tree->node, visible);
}

/* show a workspace on the focused output */
static void workspace_show(struct aro_server *s, int ws)
{
	struct aro_output *o = aro_focused_output(s);
	if (!o || ws < 0 || ws >= ARO_MAX_WS || ws == o->cur_ws)
		return;

	ghost_drop(s, o);       /* it would not slide with its workspace */

	/* the old workspace stays drawn while it slides out; anything left
	 * over from an earlier slide that is neither of these two is hidden */
	slide_start(o, o->cur_ws, ws);
	o->cur_ws = ws;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->output == o)
			view_set_visible(v, v->workspace == ws ||
			                    (o->slide.active &&
			                     v->workspace == o->slide.out_ws));
	}

	ly_node *root = o->ws[ws];

	/* place incoming windows before drawing */
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

	/* snap floating windows too */
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
		/* fallback focus for floating-only workspace */
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
		/* floating views are not in the tree */
		v->workspace = ws;
	} else {
		if (!v->node)
			return;
		next = ly_close(&v->output->ws[v->workspace], v->node);
		v->node = NULL;
		v->workspace = ws;

		v->node = tree_insert(s, v->output, ws, v, NULL, LY_ROW, false);
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

static void focus_apply(struct aro_server *s, struct aro_view *v);

/* pointer constraints and text inputs both follow keyboard focus */
static void keyboard_focus_changed(struct aro_server *s)
{
	constraint_sync(s);
	ime_set_focus(s, s->seat->keyboard_state.focused_surface);
}

void aro_focus(struct aro_server *s, struct aro_view *v)
{
	focus_apply(s, v);
	keyboard_focus_changed(s);
	ftl_sync_activated(s);
	mru_focus(s, s->focused);
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
static void view_swap(struct aro_view *a, struct aro_view *b)
{
	ly_node *na = a->node, *nb = b->node;
	ly_swap(na, nb);
	a->node = nb;
	b->node = na;
}

/* ── shells ────────────────────────────────────────────────────────────── */

/* xdg-shell implementation */
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

/* xdg-shell has no window types; a toplevel with a parent is a dialog */
static const char *xdg_type(struct aro_view *v)
{
	return v->toplevel && v->toplevel->parent ? "dialog" : "normal";
}

/* float heuristics */
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

static ly_box float_box_for(struct aro_view *v)
{
	const struct q_theme *th = &v->server->cfg.theme;
	ly_box u = usable_area(v->output);

	/* preferred size first, float_scale fallback */
	int w = 0, h = 0;
	if (v->impl->preferred_size)
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

	/* restore float size after fullscreen */
	if (!fullscreen)
		view_float_configure_once(v);
	aro_arrange(s);
}

static void view_request_fullscreen(struct wl_listener *l, void *data)
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

	/* answer fullscreen requests even when unmapped */
	if (!v->mapped) {
		wlr_xdg_surface_schedule_configure(v->toplevel->base);
		return;
	}
	view_set_fullscreen(v->server, v, v->toplevel->requested.fullscreen);
}

/* ── window rules ──────────────────────────────────────────────────────── */

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

	/* apply float before workspace move */
	if (r.floating != Q_RULE_UNSET && r.floating != last.floating)
		view_set_floating(s, v, r.floating == 1);
	if (r.workspace != Q_RULE_UNSET && r.workspace != last.workspace)
		view_send_to(s, v, r.workspace);
	if (r.fullscreen != Q_RULE_UNSET && r.fullscreen != last.fullscreen)
		view_set_fullscreen(s, v, true);
}

/* ── across outputs ────────────────────────────────────────────────────── */

/* nearest output in a direction */
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

/* move view to another output */
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

/* update drop indicator */
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
	ly_node *next = NULL;
	if (v->node) {
		next = ly_close(&src->ws[v->workspace], v->node);
		v->node = NULL;
	}
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
			v->fbox = float_box_for(v);
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

static void grab_end(struct aro_server *s);

/* tiled resize moves a shared boundary */

/* edge mask near cursor */
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

/* retile dropped window */
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

	/* become root if workspace empty */
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

	/* update titles after drag */
	aro_arrange(s);
}

static void grab_begin(struct aro_server *s, struct aro_view *v,
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

/* drop grab if view disappears */
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

/* client move/resize requests */
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

	const int ws = r.workspace != Q_RULE_UNSET ? r.workspace : o->cur_ws;
	const bool here = ws == o->cur_ws;
	v->workspace = ws;

	/* rule overrides float heuristic */
	bool floating = r.floating != Q_RULE_UNSET
	              ? r.floating == 1
	              : v->impl->wants_float && v->impl->wants_float(v);

	if (floating) {
		/* floating windows skip the tree */
		v->floating = true;
		v->fbox = float_box_for(v);
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
		view_float_configure_once(v);
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
	ftl_create(v);
	mru_add(s, v);
	overview_rebuild(s);

	/* don't focus hidden windows */
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

	/* before anything moves: it starts where the frame is drawn */
	if (view_visible(v) && !v->output->slide.active && !aro_locked(s) &&
	    !overview_shown(s))
		ghost_spawn(s, v, view_draw_box(v, aro_now_ms()));

	v->mapped = false;
	view_set_visible(v, false);
	grab_forget(s, v);
	ftl_destroy(v);
	mru_remove(s, v);
	overview_rebuild(s);

	/* drop tiled resize grab if windows close */
	if (s->cursor_mode == ARO_CURSOR_RESIZE_TILE)
		grab_forget(s, s->grabbed);

	/* only close tree leaf if present */
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
		/* let client choose initial size */
		wlr_xdg_toplevel_set_size(v->toplevel, 0, 0);
	}

	/* follow client float commits */
	view_float_follow(v);

	/* clip rounded corners */
	ui_frame_clip_content(v);
	overview_view_commit(v->server, v);
}

static void view_set_title(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, set_title);
	(void)data;
	if (!v->mapped || !v->output)
		return;

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
static void view_set_app_id(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, set_app_id);
	(void)data;
	if (!v->mapped || !v->output)
		return;
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

static void layer_map(struct wl_listener *listener, void *data)
{
	struct aro_layer *l = wl_container_of(listener, l, map);
	(void)data;

	wlr_log(WLR_DEBUG, "layer surface mapped: %s",
	        l->layer_surface->namespace ? l->layer_surface->namespace : "?");

	arrange_layers(l->server);
	aro_arrange(l->server);
	overview_rebuild(l->server);

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

	/* pick default output for layer */
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

/* ── output ────────────────────────────────────────────────────────────── */

static void output_frame(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, frame);
	struct aro_server *s = o->server;
	(void)data;

	uint32_t now = aro_now_ms();
	slides_reap(s, now);
	/* any output's slide: its windows can cross onto this one */
	bool moving = slides_active(s);

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_on_screen(v))
			continue;
		if (anim_box_tick(&v->geo, now))
			moving = true;
		ui_frame_geometry(v, view_draw_box(v, now));
	}

	if (notify_tick(s, now))
		moving = true;
	if (prompt_tick(s, now))
		moving = true;
	if (overview_tick(s, now))
		moving = true;
	if (ghost_tick(s, o, now))
		moving = true;

	/* animate drop preview */
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
		overview_frame_done(s, o, &ts);
	}

	if (moving)
		wlr_output_schedule_frame(o->wlr_output);
}

static void output_request_state(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr_output, ev->state);
	o->scale = o->wlr_output->scale > 0 ? o->wlr_output->scale : 1.0f;
	output_refresh_box(o);
	update_backdrop(o->server);
	aro_arrange(o->server);
	output_mgr_update(o->server);
}

/* evacuate output on destroy */
static void output_destroy(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, destroy);
	struct aro_server *s = o->server;
	(void)data;

	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);

	struct aro_output *dest = NULL;
	bar_return_cancel(o);   /* before free */
	if (o->enabled) {
		dest = output_evacuate(s, o);
		bar_finish(&o->bar);
	}

	/* toplevel handles drop a destroyed output themselves; forget it too,
	 * so a new output at the same address is not taken for this one */
	struct aro_view *fv;
	wl_list_for_each(fv, &s->views, link)
		if (fv->ftl_output == o->wlr_output)
			fv->ftl_output = NULL;
	free(o);

	if (dest) {
		update_backdrop(s);
		aro_arrange(s);
	}
	output_mgr_update(s);
}

/* adopt parked workspaces */
static void output_adopt_parked(struct aro_server *s, struct aro_output *o)
{
	if (!s->parked)
		return;
	s->parked = false;

	/* output starts empty */
	for (int i = 0; i < ARO_MAX_WS; i++) {
		o->ws[i] = s->orphan_ws[i];
		s->orphan_ws[i] = NULL;
		o->ws_layout[i] = s->orphan_ws_layout[i];
		s->orphan_ws_layout[i] = Q_LAYOUT_INHERIT;
	}
	o->cur_ws = s->orphan_cur_ws;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		/* only adopt mapped views */
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

	/* place adopted views before drawing */
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

/* ── output configuration ──────────────────────────────────────────────── */
/* output config paths */

/* output request defaults */
struct out_req {
	int enabled;                    /* -1 leave, 0, 1 */
	struct wlr_output_mode *mode;   /* an exact mode (protocol) */
	bool custom;                    /* mode_w x mode_h @ mode_mhz, as given */
	int mode_w, mode_h, mode_mhz;   /* 0 leave; -1 preferred; else look up */
	bool has_pos;
	bool pos_auto;                  /* place it to the right of the rest */
	int x, y;
	float scale;                    /* 0 leave; -1 work it out from the DPI */
	int transform;                  /* -1 leave */
	int adaptive_sync;              /* -1 leave */
};

#define OUT_REQ_NONE { .enabled = -1, .transform = -1, .adaptive_sync = -1 }

/* notify output management clients */
static void output_mgr_update(struct aro_server *s)
{
	if (!s->output_mgr)
		return;
	struct wlr_output_configuration_v1 *cfg =
		wlr_output_configuration_v1_create();
	if (!cfg)
		return;

	struct wl_list *lists[2] = { &s->outputs, &s->outputs_off };
	for (int i = 0; i < 2; i++) {
		struct aro_output *o;
		wl_list_for_each(o, lists[i], link) {
			struct wlr_output_configuration_head_v1 *h =
				wlr_output_configuration_head_v1_create(cfg, o->wlr_output);
			if (!h)
				continue;
			/* report layout-enabled state */
			h->state.enabled = o->enabled;
			if (o->enabled) {
				h->state.x = o->box.x;
				h->state.y = o->box.y;
			}
		}
	}
	/* takes ownership of cfg */
	wlr_output_manager_v1_set_configuration(s->output_mgr, cfg);
}

static struct aro_output *output_from_wlr(struct aro_server *s,
                                          struct wlr_output *wo)
{
	struct wl_list *lists[2] = { &s->outputs, &s->outputs_off };
	for (int i = 0; i < 2; i++) {
		struct aro_output *o;
		wl_list_for_each(o, lists[i], link)
			if (o->wlr_output == wo)
				return o;
	}
	return NULL;
}

/* find output mode */
static struct wlr_output_mode *output_find_mode(struct wlr_output *wo,
                                                int w, int h, int mhz)
{
	struct wlr_output_mode *best = NULL, *m;
	wl_list_for_each(m, &wo->modes, link) {
		if (m->width != w || m->height != h)
			continue;
		if (!best) {
			best = m;
		} else if (mhz) {
			if (abs(m->refresh - mhz) < abs(best->refresh - mhz))
				best = m;
		} else if (m->refresh > best->refresh ||
		           (m->refresh == best->refresh && m->preferred)) {
			best = m;
		}
	}
	if (best && mhz && abs(best->refresh - mhz) > 1000)
		return NULL;
	return best;
}

/* automatic scale */
static float output_auto_scale(struct wlr_output *wo)
{
	int w = wo->width, h = wo->height;
	if (w <= 0 || h <= 0) {
		struct wlr_output_mode *m = wlr_output_preferred_mode(wo);
		if (!m)
			return 1.0f;
		w = m->width;
		h = m->height;
	}
	if (wo->phys_width <= 0 || wo->phys_height <= 0)
		return 1.0f;

	/* the diagonal, so a rotated or oddly shaped panel still reads right */
	double px = sqrt((double)w * w + (double)h * h);
	double mm = sqrt((double)wo->phys_width * wo->phys_width +
	                 (double)wo->phys_height * wo->phys_height);
	double dpi = px / (mm / 25.4);
	return dpi >= 192 ? 2.0f : 1.0f;
}

/* evacuate output */
static struct aro_output *output_evacuate(struct aro_server *s,
                                          struct aro_output *o)
{
	/* the views below are rehomed or parked and made visible or hidden
	 * there; only the flag is left. No slide_finish(): this runs from
	 * output_destroy, and scheduling a frame on a dying output is not
	 * something to rely on */
	o->slide.active = false;

	/* dismiss prompt on output loss */
	prompt_output_gone(s, o);
	switcher_output_gone(s, o);
	overview_output_gone(s, o);
	ghost_drop(s, o);

	/* clear grabs on output loss */
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
			/* park views if no destination */
			v->output = NULL;
			v->fbox.x -= o->box.x;
			v->fbox.y -= o->box.y;
			v->pre_fs.x -= o->box.x;
			v->pre_fs.y -= o->box.y;
			view_set_visible(v, false);
			continue;
		}

		/* remove leaf before tree is freed */
		v->node = NULL;
		v->output = dest;
		v->workspace = dest->cur_ws;
		if (v->floating) {
			v->fbox.x += dest->box.x - o->box.x;
			v->fbox.y += dest->box.y - o->box.y;
		} else {
			v->node = tree_insert(s, dest, dest->cur_ws, v, NULL,
			                      LY_ROW, false);
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
			s->orphan_ws_layout[i] = o->ws_layout[i];
		}
		s->orphan_cur_ws = o->cur_ws;
		s->parked = true;
		wlr_log(WLR_INFO, "last output gone — parking its workspaces");
	}
	o->cur_ws = 0;
	/* reset layout overrides */
	for (int i = 0; i < ARO_MAX_WS; i++)
		o->ws_layout[i] = Q_LAYOUT_INHERIT;

	if (s->focused && s->focused->output == NULL)
		aro_focus(s, dest ? output_pick_view(s, dest) : NULL);

	return dest;
}

/* disable output */
static void output_disable(struct aro_output *o)
{
	struct aro_server *s = o->server;
	struct wlr_output *wo = o->wlr_output;

	wl_list_remove(&o->link);
	wl_list_insert(&s->outputs_off, &o->link);
	o->enabled = false;

	struct aro_output *dest = output_evacuate(s, o);

	/* destroy layer surfaces on disabled output */
	struct aro_layer *l, *ltmp;
	wl_list_for_each_safe(l, ltmp, &s->layers, link) {
		if (l->layer_surface->output == wo)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	bar_return_cancel(o);
	bar_finish(&o->bar);
	memset(&o->bar, 0, sizeof o->bar);
	o->bar_snap = false;

	/* remove scene/layout entries */
	struct wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, wo);
	if (so)
		wlr_scene_output_destroy(so);
	wlr_output_layout_remove(s->output_layout, wo);

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, false);
	wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);

	if (dest) {
		update_backdrop(s);
		aro_arrange(s);
	}
	wlr_log(WLR_INFO, "output %s disabled", wo->name);
}

/* apply output config */
static bool output_configure(struct aro_output *o, const struct out_req *r,
                             bool test, char *why, size_t why_len)
{
	struct aro_server *s = o->server;
	struct wlr_output *wo = o->wlr_output;
	const bool enable = r->enabled < 0 ? o->enabled : r->enabled == 1;
	why[0] = '\0';

	if (!enable) {
		if (test)
			return true;
		if (o->enabled) {
			output_disable(o);
		} else {
			/* ensure disabled output is off */
			struct wlr_output_state st;
			wlr_output_state_init(&st);
			wlr_output_state_set_enabled(&st, false);
			wlr_output_commit_state(wo, &st);
			wlr_output_state_finish(&st);
		}
		output_mgr_update(s);
		return true;
	}

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);

	if (r->mode) {
		wlr_output_state_set_mode(&st, r->mode);
	} else if (r->custom) {
		wlr_output_state_set_custom_mode(&st, r->mode_w, r->mode_h, r->mode_mhz);
	} else if (r->mode_w > 0) {
		struct wlr_output_mode *m =
			output_find_mode(wo, r->mode_w, r->mode_h, r->mode_mhz);
		if (m) {
			wlr_output_state_set_mode(&st, m);
		} else if (wl_list_empty(&wo->modes)) {
			/* headless outputs accept custom mode */
			wlr_output_state_set_custom_mode(&st, r->mode_w, r->mode_h,
			                                 r->mode_mhz);
		} else {
			if (r->mode_mhz)
				snprintf(why, why_len, "no %dx%d mode at %.3g Hz",
				         r->mode_w, r->mode_h, r->mode_mhz / 1000.0);
			else
				snprintf(why, why_len, "no %dx%d mode",
				         r->mode_w, r->mode_h);
			wlr_output_state_finish(&st);
			return false;
		}
	} else if (r->mode_w < 0 || !o->enabled) {
		/* use preferred mode */
		struct wlr_output_mode *m = wlr_output_preferred_mode(wo);
		if (m)
			wlr_output_state_set_mode(&st, m);
	}
	if (r->scale > 0)
		wlr_output_state_set_scale(&st, r->scale);
	else if (r->scale < 0)
		wlr_output_state_set_scale(&st, output_auto_scale(wo));
	if (r->transform >= 0)
		wlr_output_state_set_transform(&st, (enum wl_output_transform)r->transform);
	if (r->adaptive_sync >= 0)
		wlr_output_state_set_adaptive_sync_enabled(&st, r->adaptive_sync == 1);

	bool ok = wlr_output_test_state(wo, &st);
	if (ok && !test)
		ok = wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);
	if (!ok) {
		snprintf(why, why_len, "the output refused that combination%s",
		         r->adaptive_sync == 1 ? " (adaptive_sync?)" : "");
		return false;
	}
	if (test)
		return true;

	/* ── placement ── */
	const bool was_on = o->enabled;
	const ly_box before = o->box;

	struct wlr_output_layout_output *lo =
		wlr_output_layout_get(s->output_layout, wo);
	if (r->has_pos)
		lo = wlr_output_layout_add(s->output_layout, wo, r->x, r->y);
	else if (r->pos_auto || !lo)
		lo = wlr_output_layout_add_auto(s->output_layout, wo);
	if (!was_on && lo) {
		struct wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, wo);
		if (!so)
			so = wlr_scene_output_create(s->scene, wo);
		if (so)
			wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
	}

	o->scale = wo->scale > 0 ? wo->scale : 1.0f;

	if (!was_on) {
		wl_list_remove(&o->link);
		wl_list_insert(&s->outputs, &o->link);
		o->enabled = true;

		/* refresh box before bar */
		output_refresh_box(o);

		/* create bar */
		if (!bar_create(&o->bar, o))
			wlr_log(WLR_ERROR, "could not build a bar for %s", wo->name);

		if (!s->focused_output)
			s->focused_output = o;
	}

	update_backdrop(s);

	if (!was_on) {
		output_adopt_parked(s, o);      /* after the boxes and layers are final */
	} else if (before.x != o->box.x || before.y != o->box.y) {
		/* shift floating views when output moves */
		int dx = o->box.x - before.x, dy = o->box.y - before.y;
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link) {
			if (v->output != o)
				continue;
			v->fbox.x += dx;
			v->fbox.y += dy;
			v->pre_fs.x += dx;
			v->pre_fs.y += dy;
		}
	}

	aro_arrange(s);

	/* snap views after output geometry change */
	if (was_on && !box_eq(before, o->box)) {
		slide_finish(o);        /* its span was the old width */
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link)
			if (v->output == o && view_visible(v))
				anim_box_set(&v->geo, view_target(v));
	}

	output_mgr_update(s);
	wlr_log(WLR_INFO, "output %s: %dx%d at %d,%d, scale %.2f",
	        wo->name, o->box.w, o->box.h, o->box.x, o->box.y, o->scale);
	return true;
}

/* diff monitor block against last applied */
static bool monitor_req(const struct q_monitor_set *m,
                        const struct q_monitor_set *last, struct out_req *r)
{
	*r = (struct out_req)OUT_REQ_NONE;
	bool any = false;

	if (m->enabled != Q_RULE_UNSET &&
	    (!last || last->enabled != m->enabled)) {
		r->enabled = m->enabled;
		any = true;
	}
	if (m->mode_w && (!last || last->mode_w != m->mode_w ||
	                  last->mode_h != m->mode_h ||
	                  last->mode_mhz != m->mode_mhz)) {
		r->mode_w = m->mode_w;
		r->mode_h = m->mode_h;
		r->mode_mhz = m->mode_mhz;
		any = true;
	}
	if (m->has_pos && (!last || !last->has_pos ||
	                   last->x != m->x || last->y != m->y)) {
		r->has_pos = true;
		r->x = m->x;
		r->y = m->y;
		any = true;
	}
	if (m->pos_auto && (!last || !last->pos_auto)) {
		r->pos_auto = true;
		any = true;
	}
	if (m->scale != 0 && (!last || last->scale != m->scale)) {
		/* auto scale */
		r->scale = (float)m->scale;
		any = true;
	}
	if (m->transform != Q_RULE_UNSET &&
	    (!last || last->transform != m->transform)) {
		r->transform = m->transform;
		any = true;
	}
	if (m->adaptive_sync != Q_RULE_UNSET &&
	    (!last || last->adaptive_sync != m->adaptive_sync)) {
		r->adaptive_sync = m->adaptive_sync;
		any = true;
	}
	return any;
}

static void monitor_eval(struct aro_output *o, struct q_monitor_set *out)
{
	struct wlr_output *wo = o->wlr_output;
	char desc[256];
	snprintf(desc, sizeof desc, "%s %s %s",
	         wo->make ? wo->make : "", wo->model ? wo->model : "",
	         wo->serial ? wo->serial : "");
	config_monitor_eval(&o->server->cfg, wo->name, desc, out);
}

/* apply monitor block */
static void monitor_apply(struct aro_output *o, bool initial)
{
	struct aro_server *s = o->server;
	const char *name = o->wlr_output->name;

	struct q_monitor_set m;
	monitor_eval(o, &m);

	struct out_req r;
	const bool fresh = initial || !o->mon_applied;
	bool any = monitor_req(&m, fresh ? NULL : &o->mon_last, &r);
	if (initial && r.enabled < 0)
		r.enabled = 1;

	/* never disable last active output */
	bool refused = false;
	if (r.enabled == 0) {
		int lit = wl_list_length(&s->outputs) - (o->enabled ? 1 : 0);
		if (lit == 0) {
			notify(s, NOTIFY_ERROR,
			       "monitor %s: not turning off the only screen that is on",
			       name);
			r.enabled = initial ? 1 : -1;
			refused = true;
		}
	}

	if (!initial && !any)
		return;

	char why[128];
	bool ok = output_configure(o, &r, false, why, sizeof why);
	if (!ok) {
		if (initial) {
			notify(s, NOTIFY_ERROR, "monitor %s: %s — using its "
			       "preferred mode instead", name, why);
			struct out_req plain = OUT_REQ_NONE;
			plain.enabled = 1;
			if (!output_configure(o, &plain, false, why, sizeof why))
				wlr_log(WLR_ERROR, "output %s would not come on: %s",
				        name, why);
		} else {
			notify(s, NOTIFY_ERROR, "monitor %s: %s", name, why);
		}
	}

	if (ok && !refused) {
		if (o->enabled) {
			o->mon_last = m;
		} else {
			/* remember only enabled while off */
			if (fresh)
				config_monitor_unset(&o->mon_last);
			o->mon_last.enabled = m.enabled;
		}
		o->mon_applied = true;
	}
}

/* reapply monitor blocks */
static void monitors_reapply(struct aro_server *s)
{
	int n = wl_list_length(&s->outputs) + wl_list_length(&s->outputs_off);
	if (n == 0)
		return;
	struct aro_output **all = calloc(n, sizeof *all);
	if (!all)
		return;

	/* snapshot outputs before reapplying */
	int i = 0;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		all[i++] = o;
	wl_list_for_each(o, &s->outputs_off, link)
		all[i++] = o;

	for (int pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			struct q_monitor_set m;
			monitor_eval(all[i], &m);
			if ((m.enabled == 0) != (pass == 1))
				continue;
			monitor_apply(all[i], false);
		}
	}
	free(all);
}

/* apply output management config */
static void output_mgr_apply_or_test(struct aro_server *s,
                                     struct wlr_output_configuration_v1 *cfg,
                                     bool test)
{
	bool ok = true;
	struct wlr_output_configuration_head_v1 *h;

	int lit = 0;
	wl_list_for_each(h, &cfg->heads, link)
		if (h->state.enabled)
			lit++;
	if (lit == 0) {
		wlr_log(WLR_ERROR, "output management: refusing to turn every "
		        "output off");
		ok = false;
	}

	/* enable before disable */
	for (int pass = 0; ok && pass < 2; pass++) {
		wl_list_for_each(h, &cfg->heads, link) {
			if (h->state.enabled != (pass == 0))
				continue;
			struct aro_output *o = output_from_wlr(s, h->state.output);
			if (!o) {
				ok = false;
				break;
			}

			struct out_req r = OUT_REQ_NONE;
			r.enabled = h->state.enabled;
			if (h->state.enabled) {
				if (h->state.mode) {
					r.mode = h->state.mode;
				} else if (h->state.custom_mode.width > 0) {
					r.custom = true;
					r.mode_w = h->state.custom_mode.width;
					r.mode_h = h->state.custom_mode.height;
					r.mode_mhz = h->state.custom_mode.refresh;
				}
				r.has_pos = true;
				r.x = h->state.x;
				r.y = h->state.y;
				r.scale = h->state.scale;
				r.transform = h->state.transform;
				r.adaptive_sync = h->state.adaptive_sync_enabled;
			}

			char why[128];
			if (!output_configure(o, &r, test, why, sizeof why)) {
				wlr_log(WLR_ERROR, "output management: %s: %s",
				        o->wlr_output->name, why);
				ok = false;
				break;
			}
		}
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(cfg);
	else
		wlr_output_configuration_v1_send_failed(cfg);
	wlr_output_configuration_v1_destroy(cfg);

	/* update client state even on failure */
	output_mgr_update(s);
}

static void output_mgr_apply(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, output_mgr_apply);
	output_mgr_apply_or_test(s, data, false);
}

static void output_mgr_test(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, output_mgr_test);
	output_mgr_apply_or_test(s, data, true);
}

/* new outputs start off until monitor blocks apply */
static void new_output(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, s->allocator, s->renderer);

	struct aro_output *o = calloc(1, sizeof *o);
	if (!o)
		return;
	o->server = s;
	o->wlr_output = wlr_output;
	o->scale = 1.0f;
	o->cur_ws = 0;
	for (int i = 0; i < ARO_MAX_WS; i++)
		o->ws_layout[i] = Q_LAYOUT_INHERIT;

	o->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &o->destroy);
	wl_list_insert(&s->outputs_off, &o->link);

	/* log output identifiers */
	wlr_log(WLR_INFO, "output: name=\"%s\" desc=\"%s %s %s\"",
	        wlr_output->name,
	        wlr_output->make ? wlr_output->make : "",
	        wlr_output->model ? wlr_output->model : "",
	        wlr_output->serial ? wlr_output->serial : "");

	monitor_apply(o, true);
	output_mgr_update(s);
}

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

/* run binding action */
/* ── quitting ──────────────────────────────────────────────────────────── */

static void quit_now(struct aro_server *s)
{
	wl_display_terminate(s->display);
}

/* ask before quitting */
static void quit_ask(struct aro_server *s)
{
	if (prompt_active(s))
		return;

	struct aro_output *o = aro_focused_output(s);
	if (!o) {
		quit_now(s);            /* nowhere to ask: nothing to lose either */
		return;
	}

	/* end drag before prompt */
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

	/* don't quit if prompt fails */
	if (!prompt_open(s, o, "Exit aro?", detail, "Exit", "Cancel",
	                 quit_now)) {
		notify(s, NOTIFY_INFO, "could not show the exit prompt; "
		       "set confirm_quit = false to quit without it");
		return;
	}

	/* clear pointer focus under prompt */
	wlr_seat_pointer_clear_focus(s->seat);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
}

/* restore pointer focus after prompt */
static void prompt_after(struct aro_server *s, bool was_active)
{
	if (was_active && !prompt_active(s))
		pointer_motion_common(s, aro_now_ms());
}

/*
 * mod+hjkl and friends on a floating window. Floating windows live
 * outside the tree, so they get their own versions:
 *   focus   the nearest floating window that way on this workspace, by the
 *           tree's own spatial rule (ly_pick); none that way does nothing,
 *           since jumping screens from a dialog would surprise
 *   move    nudge by resize_step of the usable area, kept on screen
 *   resize  grow (right, down) or shrink (left, up) the far edges
 * Reaching the tiled windows from here is mod+tab's job.
 */
static void float_directional(struct aro_server *s, struct aro_view *f,
                              enum q_action action, ly_edge e)
{
	struct aro_output *o = f->output;
	const struct q_theme *th = &s->cfg.theme;

	if (action == Q_FOCUS) {
		struct aro_view *cand[64];
		ly_box boxes[64];
		int n = 0;
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link) {
			if (n == 64)
				break;
			if (v == f || !v->floating || v->fullscreen || !view_visible(v) ||
			    v->output != o)
				continue;
			cand[n] = v;
			boxes[n++] = view_target(v);
		}
		int at = ly_pick(boxes, n, view_target(f), e);
		if (at >= 0) {
			aro_focus(s, cand[at]);
			cursor_warp_to_view(s, cand[at]);
		}
		return;
	}

	ly_box u = usable_area(o);
	int sx = (int)(u.w * th->resize_step + 0.5), sy = (int)(u.h * th->resize_step + 0.5);
	if (sx < 1)
		sx = 1;
	if (sy < 1)
		sy = 1;
	int dx = e == LY_LEFT ? -sx : e == LY_RIGHT ? sx : 0;
	int dy = e == LY_UP ? -sy : e == LY_DOWN ? sy : 0;
	ly_box b = f->fbox;

	if (action == Q_MOVE) {
		b.x += dx;
		b.y += dy;
		/* stay on screen: flush against an edge, not past it */
		if (b.x + b.w > u.x + u.w)
			b.x = u.x + u.w - b.w;
		if (b.y + b.h > u.y + u.h)
			b.y = u.y + u.h - b.h;
		if (b.x < u.x)
			b.x = u.x;
		if (b.y < u.y)
			b.y = u.y;
	} else {
		b.w += dx;
		b.h += dy;
		if (b.w < th->float_min_w)
			b.w = th->float_min_w;
		if (b.h < th->float_min_h)
			b.h = th->float_min_h;
		if (b.x + b.w > u.x + u.w)
			b.w = u.x + u.w - b.x > th->float_min_w ? u.x + u.w - b.x : b.w;
		if (b.y + b.h > u.y + u.h)
			b.h = u.y + u.h - b.y > th->float_min_h ? u.y + u.h - b.y : b.h;
		/* we chose a size, so the client stops choosing it: the same
		 * hand-over as dragging an edge */
		f->float_follow = false;
	}

	if (b.x == f->fbox.x && b.y == f->fbox.y &&
	    b.w == f->fbox.w && b.h == f->fbox.h)
		return;
	f->fbox = b;
	aro_arrange(s);
}

/* set current workspace layout; matching config clears override */
static void layout_set(struct aro_server *s, int want)
{
	struct aro_output *o = aro_focused_output(s);
	if (!o)
		return;
	const int ws = o->cur_ws;
	enum q_layout cur = ws_layout(s, o, ws);
	enum q_layout next = want == Q_LAYOUT_TOGGLE
	                   ? (cur == Q_LAYOUT_DWINDLE ? Q_LAYOUT_MANUAL
	                                              : Q_LAYOUT_DWINDLE)
	                   : (enum q_layout)want;

	o->ws_layout[ws] = next == config_ws_layout(&s->cfg, ws)
	                 ? Q_LAYOUT_INHERIT : (int)next;
	wlr_log(WLR_INFO, "layout: workspace %d on %s is %s", ws + 1,
	        o->wlr_output->name, config_layout_name(next));
	if (next != cur)
		notify(s, NOTIFY_INFO, "Workspace %d: %s", ws + 1,
		       config_layout_name(next));
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
	case Q_SWITCH:
		switcher_step(s, b->num);
		return;
	case Q_SPLIT:
		/* one-shot split */
		s->pending_split = (ly_dir)b->num;
		s->split_forced = true;
		return;
	case Q_LAYOUT:
		layout_set(s, b->num);
		return;
	case Q_OVERVIEW:
		if (s->cursor_mode == ARO_CURSOR_PASSTHROUGH)
			overview_toggle(s);
		return;
	case Q_WORKSPACE:
		workspace_show(s, b->num);
		return;
	case Q_SENDTO:
		view_send_to(s, f, b->num);
		view_raise_and_focus(s, f);     /* follow it there */
		return;
	case Q_FOCUS:
	case Q_MOVE:
	case Q_RESIZE:
		break;                                  /* below */
	case Q_NONE:
		return;
	}

	/* the directional three; a floating window has its own */
	if (f && f->floating && !f->fullscreen && f->output) {
		float_directional(s, f, b->action, (ly_edge)b->num);
		return;
	}
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
			/* move across outputs if possible */
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

	/* focus across outputs */
	struct aro_output *dest = output_toward(s, o, e);
	if (dest) {
		s->focused_output = dest;
		struct aro_view *cand = output_pick_view(s, dest);
		aro_focus(s, cand);
		cursor_warp_to_view(s, cand);
	}
}

/* match bindings */
static bool handle_bind(struct aro_server *s, uint32_t mods, xkb_keysym_t sym)
{
	/* let lock screen receive keys */
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

/* while the switcher is open only its own bind acts */
static bool handle_switch_bind(struct aro_server *s, uint32_t mods,
                               xkb_keysym_t sym)
{
	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	                      WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	mods &= care;
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->action == Q_SWITCH && b->sym == sym && b->mods == mods) {
			switcher_step(s, b->num);
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
static void pointer_configure(struct aro_server *s,
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

static void new_input(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;

	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
		new_keyboard(s, dev, false);
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

static void new_virtual_keyboard(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	new_keyboard(s, &vk->keyboard.base, true);
	seat_update_caps(s);
}

static void new_virtual_pointer(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *ev = data;
	struct wlr_input_device *dev = &ev->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(s->cursor, dev);
	if (ev->suggested_output)
		wlr_cursor_map_input_to_output(s->cursor, dev, ev->suggested_output);
}

/* pointer hit testing */
static struct aro_view *view_at(struct aro_server *s, double lx, double ly,
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

static void pointer_motion_common(struct aro_server *s, uint32_t time)
{
	drag_icon_update(s);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct aro_view *v = view_at(s, s->cursor->x, s->cursor->y,
	                                &surface, &sx, &sy);

	/* focus follows mouse */
	if (s->cfg.focus_follows_mouse && !s->focused_layer) {
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
static void constraint_sync(struct aro_server *s)
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

static void new_constraint(struct wl_listener *l, void *data)
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

static void cursor_motion(struct wl_listener *l, void *data)
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

static void cursor_motion_abs(struct wl_listener *l, void *data)
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

static void cursor_button(struct wl_listener *l, void *data)
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

static void cursor_axis(struct wl_listener *l, void *data)
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

/* cursor-shape-v1: a named cursor instead of a client-drawn surface */
static void request_set_shape(struct wl_listener *l, void *data)
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
static void request_activate(struct wl_listener *l, void *data)
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

/* ── foreign toplevel ──────────────────────────────────────────────────── */
/*
 * Two protocols: wlr-foreign-toplevel-management (waybar's taskbar; can
 * activate, close, fullscreen) and ext-foreign-toplevel-list (read-only).
 * Handles exist while a window is mapped. Minimize and maximize requests
 * are ignored: a tiling layout has neither.
 */

static void ftl_on_activate(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, ftl_activate);
	(void)data;
	if (aro_locked(v->server) || prompt_active(v->server))
		return;
	view_raise_and_focus(v->server, v);
}

static void ftl_on_close(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, ftl_close);
	(void)data;
	view_close(v);
}

static void ftl_on_fullscreen(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, ftl_fullscreen_req);
	const struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
	view_set_fullscreen(v->server, v, ev->fullscreen);
}

/* wlroots copies these; NULL is not accepted everywhere, "" is */
static void ftl_update_ids(struct aro_view *v)
{
	const char *title = view_title(v), *app_id = view_app_id(v);
	title = title ? title : "";
	app_id = app_id ? app_id : "";

	if (v->ftl) {
		wlr_foreign_toplevel_handle_v1_set_title(v->ftl, title);
		wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, app_id);
	}
	if (v->ext_ftl) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title, .app_id = app_id,
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(v->ext_ftl, &st);
	}
}

static void ftl_create(struct aro_view *v)
{
	struct aro_server *s = v->server;
	const char *title = view_title(v), *app_id = view_app_id(v);

	if (s->ext_ftl_list && !v->ext_ftl) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title ? title : "", .app_id = app_id ? app_id : "",
		};
		v->ext_ftl = wlr_ext_foreign_toplevel_handle_v1_create(s->ext_ftl_list, &st);
	}

	if (s->ftl_mgr && !v->ftl) {
		v->ftl = wlr_foreign_toplevel_handle_v1_create(s->ftl_mgr);
		if (v->ftl) {
			v->ftl_output = NULL;
			v->ftl_fullscreen = false;
			v->ftl_activate.notify = ftl_on_activate;
			wl_signal_add(&v->ftl->events.request_activate, &v->ftl_activate);
			v->ftl_close.notify = ftl_on_close;
			wl_signal_add(&v->ftl->events.request_close, &v->ftl_close);
			v->ftl_fullscreen_req.notify = ftl_on_fullscreen;
			wl_signal_add(&v->ftl->events.request_fullscreen,
			              &v->ftl_fullscreen_req);
		}
	}

	ftl_update_ids(v);
	ftl_sync_view(v);
}

static void ftl_destroy(struct aro_view *v)
{
	struct aro_server *s = v->server;
	if (s->ftl_activated == v)
		s->ftl_activated = NULL;

	if (v->ftl) {
		wl_list_remove(&v->ftl_activate.link);
		wl_list_remove(&v->ftl_close.link);
		wl_list_remove(&v->ftl_fullscreen_req.link);
		wlr_foreign_toplevel_handle_v1_destroy(v->ftl);
		v->ftl = NULL;
	}
	v->ftl_output = NULL;
	if (v->ext_ftl) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(v->ext_ftl);
		v->ext_ftl = NULL;
	}
}

/* the output a window is on, and whether it is fullscreen */
static void ftl_sync_view(struct aro_view *v)
{
	if (!v->ftl)
		return;

	struct wlr_output *wo = v->output && v->output->enabled
	                      ? v->output->wlr_output : NULL;
	if (wo != v->ftl_output) {
		if (v->ftl_output)
			wlr_foreign_toplevel_handle_v1_output_leave(v->ftl, v->ftl_output);
		if (wo)
			wlr_foreign_toplevel_handle_v1_output_enter(v->ftl, wo);
		v->ftl_output = wo;
	}
	if (v->fullscreen != v->ftl_fullscreen) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(v->ftl, v->fullscreen);
		v->ftl_fullscreen = v->fullscreen;
	}
}

static void ftl_sync_activated(struct aro_server *s)
{
	struct aro_view *want = s->focused && s->focused->ftl ? s->focused : NULL;
	if (want == s->ftl_activated)
		return;
	if (s->ftl_activated && s->ftl_activated->ftl)
		wlr_foreign_toplevel_handle_v1_set_activated(s->ftl_activated->ftl, false);
	if (want)
		wlr_foreign_toplevel_handle_v1_set_activated(want->ftl, true);
	s->ftl_activated = want;
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

/* XWayland views */

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

/* WM_CLASS class */
static const char *xwl_app_id(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->class : NULL;
}

/* _NET_WM_WINDOW_TYPE, most specific first; the names `type:` rules match */
static const struct {
	enum wlr_xwayland_net_wm_window_type type;
	const char *name;
} xwl_types[] = {
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,        "splash" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,        "dialog" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,       "utility" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,       "toolbar" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,          "menu" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, "dropdown-menu" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,    "popup-menu" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,       "tooltip" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,  "notification" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,         "combo" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,           "dnd" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DOCK,          "dock" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DESKTOP,       "desktop" },
};

static const char *xwl_type(struct aro_view *v)
{
	struct wlr_xwayland_surface *x = v->xsurface;
	if (!x)
		return "normal";
	for (size_t i = 0; i < sizeof xwl_types / sizeof xwl_types[0]; i++)
		if (wlr_xwayland_surface_has_window_type(x, xwl_types[i].type))
			return xwl_types[i].name;
	/* untyped but transient or modal: as good as a dialog */
	return x->modal || x->parent ? "dialog" : "normal";
}

/* X11 float heuristics */
static bool xwl_wants_float(struct aro_view *v)
{
	struct wlr_xwayland_surface *x = v->xsurface;
	if (!x)
		return false;
	if (x->modal || x->parent)
		return true;

	/* the types that are never a main window */
	if (wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
	    wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
	    wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
	    wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH))
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
	/* X11 geometry is the surface */
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
	.type             = xwl_type,
	.geometry         = xwl_geometry,
	.wants_float      = xwl_wants_float,
	.wants_fullscreen = xwl_wants_fullscreen,
	.preferred_size   = xwl_preferred_size,
	.surface          = xwl_surface,
};

/* ── override-redirect ─────────────────────────────────────────────────── */

/* override-redirect X11 surfaces */
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

	/* honor override-redirect configure */
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
	overview_view_commit(v->server, v);
}

/* answer unmapped X11 configure */
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

	/* floating X11 resize */
	if (v->floating && !v->fullscreen) {
		v->fbox.w = ev->width + th->border * 2;
		v->fbox.h = ev->height + th->border * 2 + th->header_h;
		aro_arrange(v->server);
	} else {
		/* tiled X11 configure */
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

	ftl_destroy(v);
	mru_remove(v->server, v);
	qtext_finish(&v->title);
	if (v->frame_tree)
		wlr_scene_node_destroy(&v->frame_tree->node);

	/* listeners are safe to remove */
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->associate.link);
	wl_list_remove(&v->dissociate.link);
	wl_list_remove(&v->request_configure.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
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
	wl_list_init(&v->mru_link);

	if (!ui_frame_create(v, s->l_tiled)) {
		wlr_log(WLR_ERROR, "could not build a frame for an X11 window");
		free(v);
		return;
	}
	v->frame_tree->node.data = v;   /* view_at() walks up looking for this */
	wlr_scene_node_set_enabled(&v->frame_tree->node, false);

	/* surface arrives on associate */
	wl_list_init(&v->map.link);
	wl_list_init(&v->unmap.link);
	wl_list_init(&v->commit.link);

	v->associate.notify = xwl_associate;
	wl_signal_add(&xsurface->events.associate, &v->associate);
	v->dissociate.notify = xwl_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &v->dissociate);
	v->set_title.notify = view_set_title;
	wl_signal_add(&xsurface->events.set_title, &v->set_title);
	v->set_app_id.notify = view_set_app_id;
	wl_signal_add(&xsurface->events.set_class, &v->set_app_id);
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

	v->id = ++s->next_view_id;
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

static void config_reload(struct aro_server *s)
{
	struct aro_config nc;
	config_defaults(&nc);
	config_load(&nc, s->cfg_path);

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
	ghost_drop(s, NULL);    /* titles borrowed the old font */
	overview_rebuild(s);
	notify_config_errors(s);

	/* reapply monitor blocks after errors */
	monitors_reapply(s);

	/* after the error toasts are redrawn, or they would clear its own */
	wallpaper_apply(&s->wallpaper, &s->cfg);

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
		wl_display_destroy(s.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	/* set XDG_CURRENT_DESKTOP */
	setenv("XDG_CURRENT_DESKTOP", "aro", false);
	wlr_log(WLR_INFO, "aro running on %s", socket);

	/* aroctl; before autostart, so every child inherits ARO_SOCKET */
	ipc_init(&s, socket);

	/* spawn after environment is ready */
	config_watch_start(&s);
	notify_config_errors(&s);

	/* retry failed monitor blocks */
	monitors_reapply(&s);

	/* before exec lines: the wallpaper is the first thing to come up */
	wallpaper_init(&s.wallpaper, s.loop, wallpaper_report, &s);
	wallpaper_apply(&s.wallpaper, &s.cfg);

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

	/* teardown order matters */
	wl_display_destroy_clients(s.display);
	ime_finish(&s);

	/* finish UI state */
	ui_preview_finish(&s.preview);
	notify_finish(&s);
	prompt_finish(&s);
	overview_finish(&s);
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
	s.pointer_constraints = NULL;
	s.active_constraint = NULL;

	wlr_scene_node_destroy(&s.scene->tree.node);
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
	return 0;
}
