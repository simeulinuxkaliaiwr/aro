/*
 * map.c: layout = canvas, mod+o. Instead of the overview, the plane as a
 * map: every window a live card where it lies, the cards joined by soft
 * lines (the shortest tree through them), the screen's own patch of the
 * plane drawn dashed. The windows fly out of their places into the map and
 * back. A card takes you to its window; empty map flies the camera there.
 */

#define _DEFAULT_SOURCE /* M_PI */
/* scene.h must come first */
#include "scene.h"

#include "map.h"
#include "aro.h"
#include "core.h"
#include "theme.h"

#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

#define MAP_MAX 64
#define MAP_MARGIN 0.12         /* of the screen, round the map */
#define MAP_MAX_K 0.5           /* the map is always a step back */
#define MAP_EDGE 2              /* a card's frame, px */
#define MAP_LABEL 22            /* room for a card's title under it */

static ly_box lerp_box(ly_box a, ly_box b, double t)
{
	return (ly_box){ (int)lround(a.x + (b.x - a.x) * t), (int)lround(a.y + (b.y - a.y) * t),
	                 (int)lround(a.w + (b.w - a.w) * t), (int)lround(a.h + (b.h - a.h) * t) };
}

static bool meet(ly_box a, ly_box b, ly_box *out)
{
	const int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
	const int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
	const int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
	*out = (ly_box){ x0, y0, x1 - x0, y1 - y0 };
	return x1 > x0 && y1 > y0;
}

static void schedule(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

static double ease(double t)
{
	return 1.0 - pow(1.0 - t, 3.0);
}

static void rgba(uint32_t c, double *r, double *g, double *b, double *a)
{
	*r = ((c >> 24) & 0xff) / 255.0;
	*g = ((c >> 16) & 0xff) / 255.0;
	*b = ((c >> 8) & 0xff) / 255.0;
	*a = (c & 0xff) / 255.0;
}

/* the plane's point as it lies on the map, and back */
static ly_box to_map(const struct aro_map *m, ly_box b)
{
	return (ly_box){ (int)lround(m->ox + (b.x - m->x0) * m->k), (int)lround(m->oy + (b.y - m->y0) * m->k),
	                 (int)lround(b.w * m->k), (int)lround(b.h * m->k) };
}

/* ── the art: dots, lines, the screen ──────────────────────────────────── */

/* where the straight line from a's middle toward b leaves the box a */
static void exit_point(ly_box a, double tx, double ty, double *x, double *y)
{
	const double cx = a.x + a.w / 2.0, cy = a.y + a.h / 2.0;
	const double dx = tx - cx, dy = ty - cy;
	double t = 1.0;
	if (dx != 0)
		t = fmin(t, (a.w / 2.0 + 3) / fabs(dx));
	if (dy != 0)
		t = fmin(t, (a.h / 2.0 + 3) / fabs(dy));
	*x = cx + dx * t;
	*y = cy + dy * t;
}

static void draw_art(struct aro_server *s)
{
	struct aro_map *m = &s->map;
	struct aro_output *o = m->o;
	const struct q_theme *th = &s->cfg.theme;
	const float k = o->scale > 0 ? o->scale : 1.0f;
	const int w = o->box.w, h = o->box.h;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)(w * k + 0.5), (int)(h * k + 0.5));
	cairo_t *cr = cairo_create(surf);
	cairo_scale(cr, k, k);
	cairo_translate(cr, -o->box.x, -o->box.y);
	double r, g, b, a;

	/* the plane's grid, at the map's scale, faint */
	rgba(th->ink, &r, &g, &b, &a);
	const double step = 48.0 * m->k * 2;
	if (step >= 8) {
		cairo_set_source_rgba(cr, r, g, b, 0.07);
		const double sx = fmod(m->ox - o->box.x, step), sy = fmod(m->oy - o->box.y, step);
		for (double y = o->box.y + sy; y < o->box.y + h; y += step)
			for (double x = o->box.x + sx; x < o->box.x + w; x += step) {
				cairo_new_sub_path(cr);
				cairo_arc(cr, x, y, 1.2, 0, 2 * M_PI);
			}
		cairo_fill(cr);
	}

	/* the soft lines: the shortest tree through the cards' middles (Prim) */
	const int n = m->n;
	bool in[MAP_MAX] = { 0 };
	double best[MAP_MAX];
	int from[MAP_MAX];
	for (int i = 0; i < n; i++) {
		best[i] = INFINITY;
		from[i] = -1;
	}
	rgba(th->accent, &r, &g, &b, &a);
	if (n > 0)
		best[0] = 0;
	for (int step_i = 0; step_i < n; step_i++) {
		int u = -1;
		for (int i = 0; i < n; i++)
			if (!in[i] && (u < 0 || best[i] < best[u]))
				u = i;
		in[u] = true;
		if (from[u] >= 0) {
			const ly_box A = m->items[from[u]].at, B = m->items[u].at;
			double ax, ay, bx, by;
			exit_point(A, B.x + B.w / 2.0, B.y + B.h / 2.0, &ax, &ay);
			exit_point(B, A.x + A.w / 2.0, A.y + A.h / 2.0, &bx, &by);
			/* a gentle arc: the control point off the middle, square to the line */
			const double mx = (ax + bx) / 2, my = (ay + by) / 2;
			const double len = hypot(bx - ax, by - ay);
			const double nx = len > 0 ? -(by - ay) / len : 0, ny = len > 0 ? (bx - ax) / len : 0;
			const double bend = len * 0.12 * ((u & 1) ? 1 : -1);
			const double cx = mx + nx * bend, cy = my + ny * bend;
			for (int pass = 0; pass < 2; pass++) {
				cairo_move_to(cr, ax, ay);
				cairo_curve_to(cr, (ax + 2 * cx) / 3, (ay + 2 * cy) / 3, (bx + 2 * cx) / 3, (by + 2 * cy) / 3, bx, by);
				cairo_set_line_width(cr, pass ? 1.6 : 9.0);
				cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
				cairo_set_source_rgba(cr, r, g, b, pass ? 0.75 : 0.14);
				cairo_stroke(cr);
			}
			/* a small knot where the line meets each card */
			cairo_set_source_rgba(cr, r, g, b, 0.85);
			cairo_new_sub_path(cr);
			cairo_arc(cr, ax, ay, 2.6, 0, 2 * M_PI);
			cairo_new_sub_path(cr);
			cairo_arc(cr, bx, by, 2.6, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		for (int i = 0; i < n; i++) {
			if (in[i])
				continue;
			const ly_box A = m->items[u].at, B = m->items[i].at;
			const double d = hypot((A.x + A.w / 2.0) - (B.x + B.w / 2.0), (A.y + A.h / 2.0) - (B.y + B.h / 2.0));
			if (d < best[i]) {
				best[i] = d;
				from[i] = u;
			}
		}
	}

	/* the patch the screen shows now, dashed */
	const ly_box here = to_map(m, usable_area(o));
	rgba(th->ink, &r, &g, &b, &a);
	cairo_set_source_rgba(cr, r, g, b, 0.45);
	cairo_set_line_width(cr, 1.0);
	const double dash[] = { 6, 5 };
	cairo_set_dash(cr, dash, 2, 0);
	cairo_rectangle(cr, here.x + 0.5, here.y + 0.5, here.w - 1, here.h - 1);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0);
	/* its corners, solid */
	const int c = 10;
	cairo_set_source_rgba(cr, r, g, b, 0.8);
	cairo_set_line_width(cr, 1.6);
	const double X0 = here.x + 0.5, Y0 = here.y + 0.5, X1 = here.x + here.w - 0.5, Y1 = here.y + here.h - 0.5;
	cairo_move_to(cr, X0, Y0 + c); cairo_line_to(cr, X0, Y0); cairo_line_to(cr, X0 + c, Y0);
	cairo_move_to(cr, X1 - c, Y0); cairo_line_to(cr, X1, Y0); cairo_line_to(cr, X1, Y0 + c);
	cairo_move_to(cr, X1, Y1 - c); cairo_line_to(cr, X1, Y1); cairo_line_to(cr, X1 - c, Y1);
	cairo_move_to(cr, X0 + c, Y1); cairo_line_to(cr, X0, Y1); cairo_line_to(cr, X0, Y1 - c);
	cairo_stroke(cr);

	cairo_destroy(cr);
	struct wlr_buffer *buf = ui_surface_buffer(surf);
	if (!buf)
		return;
	if (!m->art)
		m->art = wlr_scene_buffer_create(m->tree, buf);
	else
		wlr_scene_buffer_set_buffer(m->art, buf);
	wlr_buffer_drop(buf);
	if (m->art) {
		wlr_scene_node_set_position(&m->art->node, o->box.x, o->box.y);
		wlr_scene_buffer_set_dest_size(m->art, w, h);
		/* the cards go over the lines */
		wlr_scene_node_lower_to_bottom(&m->art->node);
		if (m->scrim)
			wlr_scene_node_lower_to_bottom(&m->scrim->node);
	}
}

/* ── building and taking down ──────────────────────────────────────────── */

static void item_finish(struct map_item *it)
{
	qtext_finish(&it->label);
	if (it->tree)
		wlr_scene_node_destroy(&it->tree->node);      /* takes the snap with it */
	it->tree = NULL;
	it->snap = NULL;
}

static void teardown(struct aro_server *s)
{
	struct aro_map *m = &s->map;
	for (int i = 0; i < m->n; i++)
		item_finish(&m->items[i]);
	free(m->items);
	m->items = NULL;
	m->n = 0;
	if (m->tree)
		wlr_scene_node_destroy(&m->tree->node);
	m->tree = NULL;
	m->scrim = NULL;
	m->art = NULL;
	m->shown = m->open = false;
	m->o = NULL;
}

/* plane to map: everything on the plane and the screen's patch, fitted with a margin */
static void fit(struct aro_server *s)
{
	struct aro_map *m = &s->map;
	const ly_box u = usable_area(m->o);
	int x0 = u.x, y0 = u.y, x1 = u.x + u.w, y1 = u.y + u.h;
	for (int i = 0; i < m->n; i++) {
		const ly_box b = view_target(m->items[i].view);
		x0 = b.x < x0 ? b.x : x0;
		y0 = b.y < y0 ? b.y : y0;
		x1 = b.x + b.w > x1 ? b.x + b.w : x1;
		y1 = b.y + b.h + MAP_LABEL > y1 ? b.y + b.h + MAP_LABEL : y1;
	}
	const ly_box area = { u.x + (int)(u.w * MAP_MARGIN), u.y + (int)(u.h * MAP_MARGIN),
	                      (int)(u.w * (1 - 2 * MAP_MARGIN)), (int)(u.h * (1 - 2 * MAP_MARGIN)) };
	double k = fmin((double)area.w / (x1 - x0), (double)area.h / (y1 - y0));
	k = fmin(k, MAP_MAX_K);
	m->k = k;
	m->x0 = x0;
	m->y0 = y0;
	m->ox = area.x + (area.w - (x1 - x0) * k) / 2.0;
	m->oy = area.y + (area.h - (y1 - y0) * k) / 2.0;
	for (int i = 0; i < m->n; i++)
		m->items[i].at = to_map(m, view_target(m->items[i].view));
}

static bool build(struct aro_server *s, struct aro_output *o)
{
	struct aro_map *m = &s->map;
	m->o = o;
	m->ws = o->cur_ws;
	m->tree = wlr_scene_tree_create(s->l_overview);
	if (!m->tree)
		return false;
	float col[4];
	ui_color(s->cfg.theme.bg, col);
	col[3] = 0;
	m->scrim = wlr_scene_rect_create(m->tree, o->box.w, o->box.h, col);
	if (m->scrim)
		wlr_scene_node_set_position(&m->scrim->node, o->box.x, o->box.y);

	m->items = calloc(MAP_MAX, sizeof *m->items);
	if (!m->items)
		return false;
	struct aro_view *v;
	/* oldest first, so the newest card lies on top */
	wl_list_for_each_reverse(v, &s->views, link) {
		if (m->n == MAP_MAX)
			break;
		if (!view_on_plane(v) || v->fullscreen || v->output != o || v->workspace != o->cur_ws)
			continue;
		struct map_item *it = &m->items[m->n];
		it->view = v;
		it->tree = wlr_scene_tree_create(m->tree);
		if (!it->tree)
			continue;
		ui_color(s->cfg.theme.line, col);
		it->edge = wlr_scene_rect_create(it->tree, 1, 1, col);
		it->snap = ui_snap_create(it->tree, v, 0);
		if (qtext_init(&it->label, it->tree, s->cfg.theme.font_small)) {
			const char *t = view_title(v);
			const char *app = view_app_id(v);
			qtext_set(&it->label, t && *t ? t : app ? app : "", s->cfg.theme.dim, o->scale, 260);
			qtext_show(&it->label, false);
		}
		m->n++;
	}
	fit(s);
	draw_art(s);
	return true;
}

/* ── open, close ───────────────────────────────────────────────────────── */

static void animate(struct aro_server *s, double to)
{
	struct aro_map *m = &s->map;
	m->p_from = m->p;
	m->p_to = to;
	m->p_start = aro_now_ms();
	schedule(s);
}

bool map_wants(struct aro_server *s)
{
	struct aro_output *o = aro_focused_output(s);
	return o && ws_canvas(s, o, o->cur_ws);
}

void map_init(struct aro_server *s)
{
	s->map = (struct aro_map){ .sel = -1 };
}

void map_finish(struct aro_server *s)
{
	teardown(s);
}

bool map_active(struct aro_server *s)
{
	return s->map.open;
}

void map_close(struct aro_server *s)
{
	struct aro_map *m = &s->map;
	if (!m->open)
		return;
	m->open = false;
	m->pressed = false;
	for (int i = 0; i < m->n; i++)
		qtext_show(&m->items[i].label, false);
	animate(s, 0);
	pointer_motion_common(s, aro_now_ms());
}

void map_toggle(struct aro_server *s)
{
	struct aro_map *m = &s->map;
	if (m->open) {
		map_close(s);
		return;
	}
	struct aro_output *o = aro_focused_output(s);
	if (!o || !ws_canvas(s, o, o->cur_ws) || aro_locked(s))
		return;
	if (m->shown)
		teardown(s);            /* still closing: start again from here */
	if (!build(s, o)) {
		teardown(s);
		return;
	}
	m->shown = m->open = true;
	m->p = 0;
	/* the focused window starts chosen */
	m->sel = -1;
	for (int i = 0; i < m->n; i++)
		if (m->items[i].view == s->focused)
			m->sel = i;
	wlr_seat_pointer_notify_clear_focus(s->seat);
	animate(s, 1);
}

/* ── every frame ───────────────────────────────────────────────────────── */

bool map_tick(struct aro_server *s, uint32_t now)
{
	struct aro_map *m = &s->map;
	if (!m->shown)
		return false;
	const int dur = s->cfg.theme.overview_ms > 0 ? s->cfg.theme.overview_ms : 1;
	double t = (double)(now - m->p_start) / dur;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	m->p = m->p_from + (m->p_to - m->p_from) * ease(t);
	const double e = m->p;

	float col[4];
	ui_color(s->cfg.theme.bg, col);
	col[3] *= (float)e;          /* the map is its own surface: nothing shows through */
	if (m->scrim)
		wlr_scene_rect_set_color(m->scrim, col);
	if (m->art)
		wlr_scene_buffer_set_opacity(m->art, (float)(e * e));

	const ly_box screen = m->o->box;
	for (int i = 0; i < m->n; i++) {
		struct map_item *it = &m->items[i];
		/* from where the window is now (the camera may be flying) to its card */
		const ly_box b = lerp_box(view_target(it->view), it->at, e);
		const bool chosen = i == m->sel && m->open;
		ly_box vis;
		const bool on = meet(b, screen, &vis);
		wlr_scene_node_set_enabled(&it->tree->node, on);
		if (!on)
			continue;
		if (it->edge) {
			ui_color(chosen ? s->cfg.theme.accent : s->cfg.theme.line, col);
			wlr_scene_rect_set_color(it->edge, col);
			wlr_scene_node_set_position(&it->edge->node, vis.x, vis.y);
			wlr_scene_rect_set_size(it->edge, vis.w, vis.h);
		}
		const int bw = chosen ? MAP_EDGE + 1 : MAP_EDGE;
		const ly_box in = { b.x + bw, b.y + bw, b.w - 2 * bw, b.h - 2 * bw };
		if (it->snap && in.w > 0 && in.h > 0) {
			ly_box cut;
			if (meet(in, screen, &cut))
				ui_snap_place(it->snap, in, cut);
		}
		qtext_move(&it->label, b.x, b.y + b.h + 4);
	}
	if (t >= 1 && m->p_to >= 1) {
		for (int i = 0; i < m->n; i++)
			qtext_show(&m->items[i].label, true);
	}
	if (t >= 1 && m->p_to <= 0) {
		teardown(s);
		return false;
	}
	return t < 1;
}

/* ── input ─────────────────────────────────────────────────────────────── */

static int item_at(struct aro_map *m, double x, double y)
{
	for (int i = m->n - 1; i >= 0; i--) {
		const ly_box b = m->items[i].at;
		if (x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h)
			return i;
	}
	return -1;
}

/* go to v: the map folds back while the camera flies to it */
static void pick(struct aro_server *s, int i)
{
	struct aro_map *m = &s->map;
	if (i < 0 || i >= m->n)
		return;
	struct aro_view *v = m->items[i].view;
	map_close(s);
	aro_focus(s, v);
	int dx, dy;
	canvas_show(s, v, &dx, &dy);
	if (s->cfg.focus_follows_mouse)
		cursor_warp_to_box(s, view_target(v), dx, dy);
}

void map_pointer_motion(struct aro_server *s, double x, double y)
{
	struct aro_map *m = &s->map;
	const int i = item_at(m, x, y);
	if (i >= 0 && i != m->sel) {
		m->sel = i;
		schedule(s);
	}
}

void map_pointer_button(struct aro_server *s, uint32_t button, bool pressed)
{
	struct aro_map *m = &s->map;
	if (button != BTN_LEFT) {
		if (pressed)
			map_close(s);
		return;
	}
	if (pressed) {
		m->pressed = true;
		m->press_x = s->cursor->x;
		m->press_y = s->cursor->y;
		return;
	}
	if (!m->pressed)
		return;
	m->pressed = false;
	const double x = s->cursor->x, y = s->cursor->y;
	const int i = item_at(m, x, y);
	if (i >= 0) {
		pick(s, i);
		return;
	}
	/* empty map: the camera goes there */
	struct aro_output *o = m->o;
	const double px = m->x0 + (x - m->ox) / m->k, py = m->y0 + (y - m->oy) / m->k;
	map_close(s);
	canvas_look_at(s, o, px, py);
}

void map_key(struct aro_server *s, uint32_t mods, xkb_keysym_t sym)
{
	struct aro_map *m = &s->map;
	/* the overview's own bind closes the map it stands in for */
	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->action == Q_OVERVIEW && !b->button && b->sym == sym && b->mods == (mods & care)) {
			map_close(s);
			return;
		}
	}
	ly_edge e;
	switch (sym) {
	case XKB_KEY_Escape:
		map_close(s);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		pick(s, m->sel);
		return;
	case XKB_KEY_Left: case XKB_KEY_h: case XKB_KEY_a: e = LY_LEFT; break;
	case XKB_KEY_Right: case XKB_KEY_l: case XKB_KEY_d: e = LY_RIGHT; break;
	case XKB_KEY_Up: case XKB_KEY_k: case XKB_KEY_w: e = LY_UP; break;
	case XKB_KEY_Down: case XKB_KEY_j: case XKB_KEY_s: e = LY_DOWN; break;
	case XKB_KEY_Tab:
		if (m->n > 0)
			m->sel = (m->sel + 1) % m->n;
		schedule(s);
		return;
	default:
		return;
	}
	if (m->n == 0)
		return;
	if (m->sel < 0) {
		m->sel = 0;
	} else {
		ly_box boxes[MAP_MAX];
		int idx[MAP_MAX], k = 0;
		for (int i = 0; i < m->n; i++)
			if (i != m->sel) {
				boxes[k] = m->items[i].at;
				idx[k++] = i;
			}
		const int at = ly_pick(boxes, k, m->items[m->sel].at, e);
		if (at >= 0)
			m->sel = idx[at];
	}
	schedule(s);
}

/* ── the world changing under it ───────────────────────────────────────── */

void map_view_commit(struct aro_server *s, struct aro_view *v)
{
	struct aro_map *m = &s->map;
	if (!m->shown)
		return;
	for (int i = 0; i < m->n; i++)
		if (m->items[i].view == v && m->items[i].snap)
			ui_snap_sync(m->items[i].snap, v);
}

void map_view_gone(struct aro_server *s, struct aro_view *v)
{
	struct aro_map *m = &s->map;
	if (!m->shown)
		return;
	for (int i = 0; i < m->n; i++) {
		if (m->items[i].view != v)
			continue;
		item_finish(&m->items[i]);
		memmove(&m->items[i], &m->items[i + 1], (size_t)(m->n - i - 1) * sizeof *m->items);
		m->n--;
		if (m->sel == i)
			m->sel = -1;
		else if (m->sel > i)
			m->sel--;
		draw_art(s);
		schedule(s);
		return;
	}
}

void map_output_gone(struct aro_server *s, struct aro_output *o)
{
	if (s->map.shown && s->map.o == o)
		teardown(s);
}
