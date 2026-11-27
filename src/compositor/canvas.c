/*
 * canvas.c: layout = canvas. Every window floats on an endless plane, and
 * the screen is a camera over it. Windows keep screen coordinates like any
 * floating window, so nothing else needs a second coordinate system: moving
 * the camera shifts the windows instead.
 */

/* scene.h must come first */
#include "scene.h"

#include "canvas.h"
#include "config.h"
#include "aro.h"
#include "core.h"
#include "group.h"
#include "text.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

#define GLIDE_FRICTION 5.0      /* 1/s: how fast a fling slows */
#define GLIDE_STOP 15.0         /* px/s: below this it has stopped */
#define FLING_FRESH_MS 80       /* a release later than this after the last motion does not fling */
#define FLY_MS 320              /* a flight to a window, or home */
#define CASCADE 32              /* px: a new window off one already in its spot */
#define GRID_STEP 48            /* px between the plane's dots */

bool ws_canvas(struct aro_server *s, struct aro_output *o, int ws)
{
	return o && ws >= 0 && ws < ARO_MAX_WS && ws_layout(s, o, ws) == Q_LAYOUT_CANVAS;
}

bool view_on_plane(struct aro_view *v)
{
	return v->mapped && v->floating && !v->sticky && !v->scratch && !v->stashed &&
	       v->output && ws_canvas(v->server, v->output, v->workspace);
}

/* the dots move with the windows: show the grid from where the camera is, wrapped to a step */
static void grid_place(struct aro_output *o)
{
	struct wlr_scene_buffer *g = o->cam.grid;
	if (!g || !g->node.enabled)
		return;
	const int ws = o->cur_ws;
	const int sx = ((o->cam.ix[ws] % GRID_STEP) + GRID_STEP) % GRID_STEP;
	const int sy = ((o->cam.iy[ws] % GRID_STEP) + GRID_STEP) % GRID_STEP;
	const float k = o->cam.grid_scale;
	wlr_scene_node_set_position(&g->node, o->box.x, o->box.y);
	wlr_scene_buffer_set_source_box(g, &(struct wlr_fbox){ sx * k, sy * k, o->box.w * k, o->box.h * k });
	wlr_scene_buffer_set_dest_size(g, o->box.w, o->box.h);
}

void canvas_grid_sync(struct aro_server *s)
{
	const uint32_t rgba = s->cfg.theme.canvas_grid;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		const bool want = (rgba & 0xff) && ws_canvas(s, o, o->cur_ws) && o->box.w > 0;
		if (!want) {
			if (o->cam.grid)
				wlr_scene_node_set_enabled(&o->cam.grid->node, false);
			continue;
		}
		const float k = o->scale > 0 ? o->scale : 1.0f;
		if (!o->cam.grid || o->cam.grid_w != o->box.w || o->cam.grid_h != o->box.h ||
		    o->cam.grid_rgba != rgba || o->cam.grid_scale != k) {
			struct wlr_buffer *b = ui_dots_render(o->box.w + GRID_STEP, o->box.h + GRID_STEP,
			                                      GRID_STEP, 1.4, rgba, k);
			if (!b)
				continue;
			if (!o->cam.grid)
				o->cam.grid = wlr_scene_buffer_create(s->l_canvas, b);
			else
				wlr_scene_buffer_set_buffer(o->cam.grid, b);
			wlr_buffer_drop(b);
			if (!o->cam.grid)
				continue;
			o->cam.grid_w = o->box.w;
			o->cam.grid_h = o->box.h;
			o->cam.grid_rgba = rgba;
			o->cam.grid_scale = k;
		}
		wlr_scene_node_set_enabled(&o->cam.grid->node, true);
		grid_place(o);
	}
}

static ly_box shifted(ly_box b, int dx, int dy)
{
	return (ly_box){ b.x + dx, b.y + dy, b.w, b.h };
}

/* ── tiled in, floated out ─────────────────────────────────────────────── */

/* a tiled window on a canvas floats where its tile was */
static void adopt(struct aro_server *s, struct aro_view *v)
{
	const bool was_hidden = view_tab_hidden(v);
	const ly_box b = v->fullscreen ? v->node->box : view_target(v);
	view_detach(v);
	v->floating = true;
	v->canvas = true;
	v->float_follow = false;
	if (v->fullscreen) {
		v->pre_fs = b;
	} else {
		v->fbox = canvas_spot(v, b);    /* monocle had them all in one place */
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_float);
	}
	if (was_hidden)
		view_set_visible(v, view_visible(v));   /* a tab behind another, out on its own */
}

/* floated by a canvas, tiled again off one */
static void release(struct aro_server *s, struct aro_view *v)
{
	v->canvas = false;
	if (!v->floating || v->sticky || v->scratch || v->node)
		return;
	v->floating = false;
	v->float_follow = false;
	v->node = tree_insert(s, v->output, v->workspace, v, NULL, LY_ROW, false);
	if (!v->node) {
		v->floating = true;     /* out of memory: floating beats being nowhere */
		return;
	}
	if (!v->fullscreen)
		wlr_scene_node_reparent(&v->frame_tree->node, s->l_tiled);
}

static int by_x(const void *a, const void *b)
{
	const struct aro_view *va = *(struct aro_view *const *)a, *vb = *(struct aro_view *const *)b;
	const ly_box ba = va->fullscreen ? va->pre_fs : va->fbox;
	const ly_box bb = vb->fullscreen ? vb->pre_fs : vb->fbox;
	return ba.x != bb.x ? (ba.x > bb.x) - (ba.x < bb.x) : (ba.y > bb.y) - (ba.y < bb.y);
}

void canvas_reconcile(struct aro_server *s)
{
	struct aro_view *out[256];
	int n = 0;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped || !v->output)
			continue;
		const bool on = ws_canvas(s, v->output, v->workspace);
		if (on && v->node && !(s->grabbed == v && s->cursor_mode != ARO_CURSOR_PASSTHROUGH))
			adopt(s, v);
		else if (!on && v->canvas && n < 256)
			out[n++] = v;
	}
	/* left to right, so the tiling reads the way the plane did */
	qsort(out, n, sizeof *out, by_x);
	for (int i = 0; i < n; i++)
		release(s, out[i]);
}

ly_box canvas_spot(struct aro_view *self, ly_box b)
{
	struct aro_server *s = self->server;
	for (int tries = 0; tries < 24; tries++) {
		bool taken = false;
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link)
			if (v != self && view_on_plane(v) && v->output == self->output &&
			    v->workspace == self->workspace && abs(v->fbox.x - b.x) < CASCADE / 2 &&
			    abs(v->fbox.y - b.y) < CASCADE / 2) {
				taken = true;
				break;
			}
		if (!taken)
			break;
		b.x += CASCADE;
		b.y += CASCADE;
	}
	return b;
}

/* ── the camera ────────────────────────────────────────────────────────── */

void canvas_pan(struct aro_server *s, struct aro_output *o, double dx, double dy)
{
	if (!o || !ws_canvas(s, o, o->cur_ws) || (dx == 0 && dy == 0))
		return;
	const int ws = o->cur_ws;
	o->cam.x[ws] -= dx;
	o->cam.y[ws] -= dy;
	const int ix = (int)lround(o->cam.x[ws]), iy = (int)lround(o->cam.y[ws]);
	const int mx = o->cam.ix[ws] - ix, my = o->cam.iy[ws] - iy;
	o->cam.ix[ws] = ix;
	o->cam.iy[ws] = iy;
	if (mx == 0 && my == 0)
		return;

	/* a window being dragged stays under the pointer: the plane moves under it */
	struct aro_view *held = s->cursor_mode == ARO_CURSOR_MOVE ? s->grabbed : NULL;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v == held || !view_on_plane(v) || v->output != o || v->workspace != ws)
			continue;
		if (v->fullscreen) {
			v->pre_fs = shifted(v->pre_fs, mx, my);
			continue;
		}
		v->fbox = shifted(v->fbox, mx, my);
		v->geo.from = shifted(v->geo.from, mx, my);
		v->geo.to = shifted(v->geo.to, mx, my);
		v->geo.cur = shifted(v->geo.cur, mx, my);
	}
	grid_place(o);
	/* one being resized rides along with the plane */
	if (s->grabbed && s->cursor_mode == ARO_CURSOR_RESIZE && view_on_plane(s->grabbed) &&
	    s->grabbed->output == o && s->grabbed->workspace == ws)
		s->grab_box = shifted(s->grab_box, mx, my);
	wlr_output_schedule_frame(o->wlr_output);
}

static void fling(struct aro_output *o, double vx, double vy)
{
	o->cam.flying = false;
	if (hypot(vx, vy) < GLIDE_STOP * 4) {
		o->cam.vx = o->cam.vy = 0;
		return;
	}
	o->cam.vx = vx;
	o->cam.vy = vy;
	o->cam.ms = aro_now_ms();
	wlr_output_schedule_frame(o->wlr_output);
}

/* the camera to (x, y) on the plane, easing out */
static void fly(struct aro_output *o, double x, double y)
{
	const int ws = o->cur_ws;
	o->cam.vx = o->cam.vy = 0;
	o->cam.flying = true;
	o->cam.fly_ws = ws;
	o->cam.fx0 = o->cam.x[ws];
	o->cam.fy0 = o->cam.y[ws];
	o->cam.fx1 = x;
	o->cam.fy1 = y;
	o->cam.fstart = aro_now_ms();
	wlr_output_schedule_frame(o->wlr_output);
}

/* where the camera is going: a flight's end, else where it is */
static void cam_dest(struct aro_output *o, double *x, double *y)
{
	const int ws = o->cur_ws;
	const bool f = o->cam.flying && o->cam.fly_ws == ws;
	*x = f ? o->cam.fx1 : o->cam.x[ws];
	*y = f ? o->cam.fy1 : o->cam.y[ws];
}

void canvas_glide(struct aro_server *s, struct aro_output *o, ly_edge e, bool home)
{
	if (!o || !ws_canvas(s, o, o->cur_ws))
		return;
	if (home) {
		fly(o, 0, 0);
		return;
	}
	const ly_box u = usable_area(o);
	double x, y;
	cam_dest(o, &x, &y);
	x += e == LY_LEFT ? -u.w / 3.0 : e == LY_RIGHT ? u.w / 3.0 : 0;
	y += e == LY_UP ? -u.h / 3.0 : e == LY_DOWN ? u.h / 3.0 : 0;
	fly(o, x, y);
}

void canvas_show(struct aro_server *s, struct aro_view *v, int *dx, int *dy)
{
	(void)s;
	if (dx)
		*dx = 0;
	if (dy)
		*dy = 0;
	if (!v || !view_on_plane(v) || v->fullscreen || v->workspace != v->output->cur_ws)
		return;
	struct aro_output *o = v->output;
	const ly_box u = usable_area(o);
	double cx, cy;
	cam_dest(o, &cx, &cy);
	/* where it will be once a flight under way ends */
	const ly_box b = shifted(v->fbox, (int)lround(o->cam.x[o->cur_ws] - cx),
	                         (int)lround(o->cam.y[o->cur_ws] - cy));
	if (b.x >= u.x && b.y >= u.y && b.x + b.w <= u.x + u.w && b.y + b.h <= u.y + u.h)
		return;         /* already in view */
	/* centred; one bigger than the screen shows its top left */
	const int mx = b.w <= u.w ? u.x + (u.w - b.w) / 2 - b.x : u.x - b.x;
	const int my = b.h <= u.h ? u.y + (u.h - b.h) / 2 - b.y : u.y - b.y;
	fly(o, cx - mx, cy - my);
	if (dx)
		*dx = (int)lround(o->cam.x[o->cur_ws] - (cx - mx));
	if (dy)
		*dy = (int)lround(o->cam.y[o->cur_ws] - (cy - my));
}

void canvas_look_at(struct aro_server *s, struct aro_output *o, double x, double y)
{
	if (!o || !ws_canvas(s, o, o->cur_ws))
		return;
	const ly_box u = usable_area(o);
	double cx, cy;
	cam_dest(o, &cx, &cy);
	/* where (x, y) will be once a flight under way ends */
	const double fx = x + (o->cam.x[o->cur_ws] - cx), fy = y + (o->cam.y[o->cur_ws] - cy);
	fly(o, cx + (fx - (u.x + u.w / 2.0)), cy + (fy - (u.y + u.h / 2.0)));
}

bool canvas_tick(struct aro_server *s, struct aro_output *o, uint32_t now)
{
	if (!ws_canvas(s, o, o->cur_ws)) {
		o->cam.vx = o->cam.vy = 0;
		o->cam.flying = false;
		return false;
	}
	const int ws = o->cur_ws;
	if (o->cam.flying) {
		if (o->cam.fly_ws != ws) {
			o->cam.flying = false;
			return false;
		}
		double t = (double)(now - o->cam.fstart) / FLY_MS;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		const double e = 1 - (1 - t) * (1 - t) * (1 - t);
		const double x = o->cam.fx0 + (o->cam.fx1 - o->cam.fx0) * e;
		const double y = o->cam.fy0 + (o->cam.fy1 - o->cam.fy0) * e;
		canvas_pan(s, o, o->cam.x[ws] - x, o->cam.y[ws] - y);
		if (t >= 1)
			o->cam.flying = false;
		return true;
	}
	if (o->cam.vx == 0 && o->cam.vy == 0)
		return false;
	double dt = (double)(now - o->cam.ms) / 1000.0;
	if (dt <= 0 || dt > 0.1)
		dt = 1.0 / 60.0;
	o->cam.ms = now;
	const double vx = o->cam.vx, vy = o->cam.vy;
	const double k = exp(-dt * GLIDE_FRICTION);
	o->cam.vx *= k;
	o->cam.vy *= k;
	if (hypot(o->cam.vx, o->cam.vy) < GLIDE_STOP)
		o->cam.vx = o->cam.vy = 0;
	canvas_pan(s, o, vx * dt, vy * dt);
	return true;
}

/* ── grabbing the desktop ──────────────────────────────────────────────── */

struct aro_output *canvas_under_cursor(struct aro_server *s, struct wlr_surface *surface)
{
	struct aro_output *o = output_at(s, s->cursor->x, s->cursor->y);
	if (!o || !ws_canvas(s, o, o->cur_ws))
		return NULL;
	/* the bar and panels' zones are not the plane */
	const ly_box u = usable_area(o);
	if (s->cursor->x < u.x || s->cursor->y < u.y ||
	    s->cursor->x >= u.x + u.w || s->cursor->y >= u.y + u.h)
		return NULL;
	if (!surface)
		return o;
	/* the wallpaper counts as desktop; panels, docks and desktop widgets do not */
	struct wlr_layer_surface_v1 *ls =
		wlr_layer_surface_v1_try_from_wlr_surface(wlr_surface_get_root_surface(surface));
	if (ls && ls->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
		return o;
	return NULL;
}

void canvas_pan_begin(struct aro_server *s, struct aro_output *o)
{
	o->cam.vx = o->cam.vy = 0;
	o->cam.flying = false;
	s->cursor_mode = ARO_CURSOR_PAN;
	s->pan.o = o;
	s->pan.lx = s->cursor->x;
	s->pan.ly = s->cursor->y;
	s->pan.vx = s->pan.vy = 0;
	s->pan.ms = aro_now_ms();
	wlr_seat_pointer_notify_clear_focus(s->seat);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "grabbing");
}

void canvas_pan_motion(struct aro_server *s)
{
	struct aro_output *o = s->pan.o;
	if (!o) {
		canvas_pan_end(s);
		return;
	}
	const double dx = s->cursor->x - s->pan.lx, dy = s->cursor->y - s->pan.ly;
	s->pan.lx = s->cursor->x;
	s->pan.ly = s->cursor->y;
	canvas_pan(s, o, dx, dy);

	const uint32_t now = aro_now_ms();
	const double dt = (double)(now - s->pan.ms) / 1000.0;
	if (dt > 0) {
		s->pan.vx = s->pan.vx * 0.5 + dx / dt * 0.5;
		s->pan.vy = s->pan.vy * 0.5 + dy / dt * 0.5;
		s->pan.ms = now;
	}
}

void canvas_pan_end(struct aro_server *s)
{
	struct aro_output *o = s->pan.o;
	if (o && aro_now_ms() - s->pan.ms < FLING_FRESH_MS)
		fling(o, s->pan.vx, s->pan.vy);
	s->pan.o = NULL;
	s->cursor_mode = ARO_CURSOR_PASSTHROUGH;
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
	pointer_motion_common(s, aro_now_ms());
	aro_arrange(s);
}

void canvas_scroll(struct aro_server *s, struct aro_output *o, bool horizontal,
                   double delta, bool finger)
{
	/* scrolling down shows what is below: the plane moves up */
	const double d = -delta * (finger ? 1.5 : 4.0);
	o->cam.vx = o->cam.vy = 0;
	o->cam.flying = false;
	canvas_pan(s, o, horizontal ? d : 0, horizontal ? 0 : d);
}

/* ── touchpad ──────────────────────────────────────────────────────────── */

bool canvas_swipe_begin(struct aro_server *s, uint32_t time)
{
	struct aro_gestures *g = &s->gestures;
	struct aro_output *o = output_at(s, s->cursor->x, s->cursor->y);
	g->panning = NULL;
	if (!o || !ws_canvas(s, o, o->cur_ws) || aro_locked(s))
		return false;
	g->panning = o;
	o->cam.vx = o->cam.vy = 0;
	o->cam.flying = false;
	g->pan_vx = g->pan_vy = 0;
	g->pan_ms = time;
	return true;
}

void canvas_swipe_update(struct aro_server *s, double dx, double dy, uint32_t time)
{
	struct aro_gestures *g = &s->gestures;
	const double k = 1.8;   /* touchpad units to screen pixels */
	canvas_pan(s, g->panning, dx * k, dy * k);
	const double dt = (double)(time - g->pan_ms) / 1000.0;
	if (dt > 0) {
		g->pan_vx = g->pan_vx * 0.5 + dx * k / dt * 0.5;
		g->pan_vy = g->pan_vy * 0.5 + dy * k / dt * 0.5;
		g->pan_ms = time;
	}
}

void canvas_swipe_end(struct aro_server *s, bool cancelled)
{
	struct aro_gestures *g = &s->gestures;
	struct aro_output *o = g->panning;
	g->panning = NULL;
	if (o && !cancelled)
		fling(o, g->pan_vx, g->pan_vy);
	aro_arrange(s);
}

void canvas_drag_end(struct aro_server *s, struct aro_view *v)
{
	if (!v || !v->mapped || !v->floating || v->fullscreen || v->sticky || !v->output)
		return;
	struct aro_output *to = output_at(s, v->fbox.x + v->fbox.w / 2.0, v->fbox.y + v->fbox.h / 2.0);
	if (!to || to == v->output ||
	    !(ws_canvas(s, v->output, v->workspace) || ws_canvas(s, to, to->cur_ws)))
		return;
	v->output = to;
	v->workspace = to->cur_ws;
	s->focused_output = to;
	/* put down on purpose: it stays a floating window there, not a tile */
	if (!ws_canvas(s, to, to->cur_ws))
		v->canvas = false;
}

void canvas_forget_output(struct aro_server *s, struct aro_output *o)
{
	if (s->pan.o == o) {
		s->pan.o = NULL;
		if (s->cursor_mode == ARO_CURSOR_PAN)
			s->cursor_mode = ARO_CURSOR_PASSTHROUGH;
	}
	if (s->gestures.panning == o)
		s->gestures.panning = NULL;
	o->cam.vx = o->cam.vy = 0;
	o->cam.flying = false;
	if (o->cam.grid)
		wlr_scene_node_destroy(&o->cam.grid->node);
	o->cam.grid = NULL;
}
