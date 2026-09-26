/* overview.c: zoomed-out view of every workspace */
/* scene.h must come first */
#include "scene.h"

#include "overview.h"
#include "aro.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* from niri's documentation of its Overview; none of its code */

static const anim_ease OV_EASE = {
	TH_EASE_FLAT_X1, TH_EASE_FLAT_Y1, TH_EASE_FLAT_X2, TH_EASE_FLAT_Y2,
};

/* ── geometry ──────────────────────────────────────────────────────────── */

struct ov_geom {
	ly_box ob;              /* output box */
	ly_box ub;              /* usable area, minus the bar */
	double z;
	int gap;
	bool vertical;
};

static double lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

static int round_i(double v)
{
	return (int)(v >= 0 ? v + 0.5 : v - 0.5);
}

static struct ov_geom geom_for(struct aro_server *s, struct aro_output *o)
{
	const struct q_theme *th = &s->cfg.theme;
	struct ov_geom g = {
		.ob = o->box,
		.ub = aro_output_usable(o),
		.gap = th->gap * 3,
		.vertical = s->cfg.ws_slide == Q_SLIDE_VERTICAL,
	};
	double z = th->overview_zoom;
	double fw = (double)(g.ub.w - 2 * g.gap) / (g.ob.w > 0 ? g.ob.w : 1);
	double fh = (double)(g.ub.h - 2 * g.gap) / (g.ob.h > 0 ? g.ob.h : 1);
	if (z > fw)
		z = fw;
	if (z > fh)
		z = fh;
	g.z = z < 0.05 ? 0.05 : (z > 0.9 ? 0.9 : z);
	return g;
}

/* a box on workspace `slot` cards from the strip's centre, at progress p */
static ly_box ov_map(const struct ov_geom *g, ly_box b, double slot, double p)
{
	const double sc = 1.0 + (g->z - 1.0) * p;
	const double cx = lerp(g->ob.w / 2.0, g->ub.x - g->ob.x + g->ub.w / 2.0, p);
	const double cy = lerp(g->ob.h / 2.0, g->ub.y - g->ob.y + g->ub.h / 2.0, p);
	const double step = (g->vertical ? g->ob.h : g->ob.w) * sc + g->gap * p;
	const double ox = g->vertical ? 0 : slot * step;
	const double oy = g->vertical ? slot * step : 0;

	double lx0 = b.x - g->ob.x, ly0 = b.y - g->ob.y;
	double lx1 = lx0 + b.w, ly1 = ly0 + b.h;
	int x0 = round_i(g->ob.x + cx + (lx0 - g->ob.w / 2.0) * sc + ox);
	int x1 = round_i(g->ob.x + cx + (lx1 - g->ob.w / 2.0) * sc + ox);
	int y0 = round_i(g->ob.y + cy + (ly0 - g->ob.h / 2.0) * sc + oy);
	int y1 = round_i(g->ob.y + cy + (ly1 - g->ob.h / 2.0) * sc + oy);
	return (ly_box){ x0, y0, x1 - x0, y1 - y0 };
}

/* ov_map backwards, for a point */
static void ov_unmap(const struct ov_geom *g, double x, double y, double slot,
                     double p, double *lx, double *ly)
{
	const double sc = 1.0 + (g->z - 1.0) * p;
	const double cx = lerp(g->ob.w / 2.0, g->ub.x - g->ob.x + g->ub.w / 2.0, p);
	const double cy = lerp(g->ob.h / 2.0, g->ub.y - g->ob.y + g->ub.h / 2.0, p);
	const double step = (g->vertical ? g->ob.h : g->ob.w) * sc + g->gap * p;
	const double ox = g->vertical ? 0 : slot * step;
	const double oy = g->vertical ? slot * step : 0;
	*lx = g->ob.x + g->ob.w / 2.0 + (x - g->ob.x - cx - ox) / sc;
	*ly = g->ob.y + g->ob.h / 2.0 + (y - g->ob.y - cy - oy) / sc;
}

/* inner, a box inside outer, carried along when outer is drawn as dst */
static ly_box carry(ly_box outer, ly_box inner, ly_box dst)
{
	double sx = outer.w > 0 ? (double)dst.w / outer.w : 1.0;
	double sy = outer.h > 0 ? (double)dst.h / outer.h : 1.0;
	int x0 = dst.x + round_i((inner.x - outer.x) * sx);
	int y0 = dst.y + round_i((inner.y - outer.y) * sy);
	int x1 = dst.x + round_i((inner.x + inner.w - outer.x) * sx);
	int y1 = dst.y + round_i((inner.y + inner.h - outer.y) * sy);
	return (ly_box){ x0, y0, x1 - x0, y1 - y0 };
}

/* a clip that clips nothing: the dragged thumbnail may cross screens */
static const ly_box ANYWHERE = { -(1 << 28), -(1 << 28), 1 << 29, 1 << 29 };

static ly_box inset(ly_box b, int d)
{
	return (ly_box){ b.x + d, b.y + d, b.w - 2 * d, b.h - 2 * d };
}

static bool clip_box(ly_box b, ly_box clip, ly_box *out)
{
	int x0 = b.x > clip.x ? b.x : clip.x;
	int y0 = b.y > clip.y ? b.y : clip.y;
	int x1 = b.x + b.w < clip.x + clip.w ? b.x + b.w : clip.x + clip.w;
	int y1 = b.y + b.h < clip.y + clip.h ? b.y + b.h : clip.y + clip.h;
	if (x1 - x0 < 1 || y1 - y0 < 1)
		return false;
	*out = (ly_box){ x0, y0, x1 - x0, y1 - y0 };
	return true;
}

static bool in_box(ly_box b, double x, double y)
{
	return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

/* ── scene helpers ─────────────────────────────────────────────────────── */

static uint32_t fade(uint32_t rgba, double a)
{
	uint32_t al = (uint32_t)((rgba & 0xff) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5);
	return (rgba & 0xffffff00) | al;
}

static struct wlr_scene_rect *rect(struct wlr_scene_tree *parent, uint32_t rgba,
                                   int radius)
{
	float c[4];
	ui_color(rgba, c);
	struct wlr_scene_rect *r = wlr_scene_rect_create(parent, 1, 1, c);
#ifdef ARO_EFFECTS
	if (r && radius > 0)
		wlr_scene_rect_set_corner_radius(r, radius);
#else
	(void)radius;
#endif
	return r;
}

static void rect_color(struct wlr_scene_rect *r, uint32_t rgba)
{
	float c[4];
	ui_color(rgba, c);
	wlr_scene_rect_set_color(r, c);
}

/* clipped to the output, so a strip never draws on the screen next door */
static void rect_place(struct wlr_scene_rect *r, ly_box b, ly_box clip)
{
	ly_box v;
	if (!clip_box(b, clip, &v)) {
		wlr_scene_node_set_enabled(&r->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&r->node, true);
	wlr_scene_node_set_position(&r->node, v.x, v.y);
	wlr_scene_rect_set_size(r, v.w, v.h);
}

static void buf_place(struct wlr_scene_buffer *sb, struct wlr_fbox src,
                      ly_box b, ly_box clip)
{
	ly_box vis;
	if (b.w < 1 || b.h < 1 || !clip_box(b, clip, &vis)) {
		wlr_scene_node_set_enabled(&sb->node, false);
		return;
	}
	struct wlr_fbox cut = {
		src.x + (double)(vis.x - b.x) / b.w * src.width,
		src.y + (double)(vis.y - b.y) / b.h * src.height,
		(double)vis.w / b.w * src.width,
		(double)vis.h / b.h * src.height,
	};
	wlr_scene_node_set_enabled(&sb->node, true);
	wlr_scene_buffer_set_source_box(sb, &cut);
	wlr_scene_buffer_set_dest_size(sb, vis.w, vis.h);
	wlr_scene_node_set_position(&sb->node, vis.x, vis.y);
}

static void snap_place(struct wlr_scene_buffer *sb, struct aro_view *v,
                       ly_box b, ly_box clip)
{
	struct wlr_fbox src;
	if (!ui_snapshot_src(v, &src)) {
		wlr_scene_node_set_enabled(&sb->node, false);
		return;
	}
	buf_place(sb, src, b, clip);
}

/* a wallpaper: a mapped background-layer surface on this output */
static bool is_wall(struct aro_layer *l, struct aro_output *o)
{
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	return ls->output == o->wlr_output &&
	       ls->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND &&
	       ls->surface->mapped && ls->surface->buffer;
}

static bool wall_box(struct aro_layer *l, ly_box *out, struct wlr_fbox *src)
{
	struct wlr_surface *surf = l->layer_surface->surface;
	int x, y;
	if (!surf->buffer || !wlr_scene_node_coords(&l->scene->tree->node, &x, &y))
		return false;
	*out = (ly_box){ x, y, surf->current.width, surf->current.height };
	*src = (struct wlr_fbox){ 0, 0, surf->buffer->base.width,
	                          surf->buffer->base.height };
	return out->w > 0 && out->h > 0 && src->width > 0 && src->height > 0;
}

static void schedule_all(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

static void set_layers(struct aro_server *s, bool on)
{
	wlr_scene_node_set_enabled(&s->l_tiled->node, on);
	wlr_scene_node_set_enabled(&s->l_preview->node, on);
	wlr_scene_node_set_enabled(&s->l_float->node, on);
	wlr_scene_node_set_enabled(&s->l_fullscreen->node, on);
}

/* ── state ─────────────────────────────────────────────────────────────── */

static struct ov_output *ov_find(struct aro_server *s, struct aro_output *o)
{
	for (int i = 0; i < s->overview.nouts; i++)
		if (s->overview.outs[i].output == o)
			return &s->overview.outs[i];
	return NULL;
}

static int card_slot(const struct ov_output *oo, int ws)
{
	for (int i = 0; i < oo->ncards; i++)
		if (oo->cards[i].ws == ws)
			return i;
	return -1;
}

static bool shown_view(struct aro_view *v, struct aro_output *o)
{
	return v->mapped && v->output == o;
}

/* the window to land on in a workspace: most recently used */
static struct aro_view *pick_on_ws(struct aro_server *s, struct aro_output *o,
                                   int ws)
{
	if (s->focused && s->focused->output == o && s->focused->workspace == ws &&
	    s->focused->mapped)
		return s->focused;
	struct aro_view *v;
	wl_list_for_each(v, &s->switcher.mru, mru_link)
		if (shown_view(v, o) && v->workspace == ws)
			return v;
	return NULL;
}

static void out_free(struct ov_output *oo)
{
	for (int i = 0; i < oo->ncards; i++)
		qtext_finish(&oo->cards[i].label);
	oo->ncards = 0;
	free(oo->items);
	oo->items = NULL;
	oo->nitems = 0;
	if (oo->tree)
		wlr_scene_node_destroy(&oo->tree->node);
	oo->tree = NULL;
}

/* the drag's own trees; its thumbnail nodes go with them */
static void drag_reset(struct aro_overview *ov)
{
	if (ov->drag_tree)
		wlr_scene_node_destroy(&ov->drag_tree->node);
	if (ov->drop_tree)
		wlr_scene_node_destroy(&ov->drop_tree->node);
	ov->drag_tree = ov->drop_tree = NULL;
	ov->drop_edge = ov->drop_fill = NULL;
	ov->dragging = ov->drop_shown = false;
	ov->drag_view = ov->drop_target = NULL;
	ov->drop_out = NULL;
}

static void all_free(struct aro_overview *ov)
{
	drag_reset(ov);
	for (int i = 0; i < ov->nouts; i++)
		out_free(&ov->outs[i]);
	free(ov->outs);
	ov->outs = NULL;
	ov->nouts = 0;
}

static bool item_add(struct aro_server *s, struct ov_output *oo,
                     struct aro_view *v, bool chrome)
{
	const struct q_theme *th = &s->cfg.theme;
	const int r = th->radius, ri = th->radius - th->border > 0
	                              ? th->radius - th->border : 0;
	struct ov_item *it = &oo->items[oo->nitems];
	*it = (struct ov_item){ .view = v, .chrome = chrome };
	if (chrome) {
		it->edge = rect(oo->tree, th->line, r);
		it->bg = rect(oo->tree, th->frame, ri);
		if (!it->edge || !it->bg)
			return false;
	}
	it->snap = ui_snapshot_create(oo->tree, v, 1, 1, chrome ? ri : 0);
	if (chrome) {
		it->ring = rect(oo->tree, th->accent_soft, 0);
		if (!it->ring)
			return false;
	}
	oo->nitems++;
	return true;
}

static bool out_build(struct aro_server *s, struct ov_output *oo,
                      struct aro_output *o)
{
	const struct q_theme *th = &s->cfg.theme;
	oo->output = o;
	oo->tree = wlr_scene_tree_create(s->l_overview);
	if (!oo->tree)
		return false;
#ifdef ARO_EFFECTS
	oo->blur = wlr_scene_blur_create(oo->tree, 1, 1);
	if (!oo->blur)
		return false;
#endif
	oo->backdrop = rect(oo->tree, fade(th->overview_tint, 0), 0);
	if (!oo->backdrop)
		return false;

	int count[ARO_MAX_WS] = { 0 };
	int nviews = 0;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (shown_view(v, o) && v->workspace >= 0 && v->workspace < ARO_MAX_WS) {
			count[v->workspace]++;
			nviews++;
		}
	}

	/* the same workspaces the bar shows */
	for (int ws = 0; ws < ARO_MAX_WS; ws++) {
		if (!(ws < s->cfg.workspaces || ws == o->cur_ws || count[ws]))
			continue;
		struct ov_card *c = &oo->cards[oo->ncards];
		*c = (struct ov_card){ .ws = ws };
#ifdef ARO_EFFECTS
		float none[4] = { 0 };
		c->shadow = wlr_scene_shadow_create(oo->tree, 1, 1, th->radius,
		                                    TH_SHADOW_BLUR, none);
		if (!c->shadow)
			return false;
#endif
		c->edge = rect(oo->tree, th->accent, th->radius);
		c->bg = rect(oo->tree, fade(th->bg, 0), th->radius);
		if (!c->edge || !c->bg)
			return false;
		struct aro_layer *l;
		wl_list_for_each(l, &s->layers, link) {
			if (c->nwall == OV_MAX_WALL || !is_wall(l, o))
				continue;
			struct wlr_scene_buffer *w = wlr_scene_buffer_create(oo->tree,
				&l->layer_surface->surface->buffer->base);
			if (!w)
				return false;
#ifdef ARO_EFFECTS
			wlr_scene_buffer_set_corner_radius(w, th->radius);
#endif
			c->wall[c->nwall] = w;
			c->wall_src[c->nwall++] = l;
		}
		oo->ncards++;           /* before qtext_init: out_free finishes it */
		if (!qtext_init(&c->label, oo->tree, th->font_small))
			return false;
		char num[8];
		snprintf(num, sizeof num, "%d", ws + 1);
		qtext_set(&c->label, num, ws == o->cur_ws ? th->ink : th->dim,
		          o->scale, 200);
		qtext_show(&c->label, false);
	}

	oo->items = calloc(nviews > 0 ? nviews : 1, sizeof *oo->items);
	if (!oo->items)
		return false;

	/* stacking: tiled, then floating, then fullscreen */
	for (int pass = 0; pass < 3; pass++) {
		wl_list_for_each(v, &s->views, link) {
			if (!shown_view(v, o) || v->workspace < 0 ||
			    v->workspace >= ARO_MAX_WS)
				continue;
			int kind = v->fullscreen ? 2 : v->floating ? 1 : 0;
			if (kind != pass)
				continue;
			if (!item_add(s, oo, v, kind != 2))
				return false;
		}
	}

	int at = card_slot(oo, o->cur_ws);
	oo->c = oo->c_from = oo->c_to = at < 0 ? 0 : at;
	return true;
}

static bool all_build(struct aro_server *s, struct aro_output *skip)
{
	struct aro_overview *ov = &s->overview;
	int n = wl_list_length(&s->outputs);
	ov->outs = calloc(n > 0 ? n : 1, sizeof *ov->outs);
	if (!ov->outs)
		return false;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o == skip)
			continue;
		struct ov_output *oo = &ov->outs[ov->nouts++];
		if (!out_build(s, oo, o))
			return false;
	}
	return true;
}

/* ── selection ─────────────────────────────────────────────────────────── */

static void scroll_to(struct ov_output *oo, int slot, uint32_t now)
{
	if (slot < 0 || oo->c_to == slot)
		return;
	oo->c_from = oo->c;
	oo->c_to = slot;
	oo->c_start = now;
}

static void select_view(struct aro_server *s, struct aro_view *v)
{
	struct aro_overview *ov = &s->overview;
	ov->sel_view = v;
	ov->sel_out = v->output;
	ov->sel_ws = v->workspace;
	struct ov_output *oo = ov_find(s, v->output);
	if (oo)
		scroll_to(oo, card_slot(oo, v->workspace), aro_now_ms());
	schedule_all(s);
}

static void select_ws(struct aro_server *s, struct aro_output *o, int ws)
{
	struct aro_overview *ov = &s->overview;
	struct ov_output *oo = ov_find(s, o);
	if (!oo || card_slot(oo, ws) < 0)
		return;
	struct aro_view *v = pick_on_ws(s, o, ws);
	if (v) {
		select_view(s, v);
		return;
	}
	ov->sel_view = NULL;
	ov->sel_out = o;
	ov->sel_ws = ws;
	scroll_to(oo, card_slot(oo, ws), aro_now_ms());
	schedule_all(s);
}

/* after a rebuild: keep what was selected if it is still there */
static void select_fix(struct aro_server *s)
{
	struct aro_overview *ov = &s->overview;
	if (ov->sel_view && ov->sel_view->mapped && ov_find(s, ov->sel_view->output)) {
		select_view(s, ov->sel_view);
		return;
	}
	struct aro_output *o = ov->sel_out;
	if (!o || !ov_find(s, o))
		o = aro_focused_output(s);
	if (!o || !ov_find(s, o))
		o = ov->nouts ? ov->outs[0].output : NULL;
	ov->sel_view = NULL;
	ov->sel_out = o;
	if (!o)
		return;
	if (card_slot(ov_find(s, o), ov->sel_ws) < 0)
		ov->sel_ws = o->cur_ws;
	select_ws(s, o, ov->sel_ws);
}

/* the selection's box, at rest in the strip */
static bool sel_box(struct aro_server *s, struct ov_output *oo, ly_box *out)
{
	struct aro_overview *ov = &s->overview;
	struct ov_geom g = geom_for(s, oo->output);
	if (ov->sel_view) {
		int slot = card_slot(oo, ov->sel_view->workspace);
		if (slot < 0)
			return false;
		*out = ov_map(&g, aro_view_box(ov->sel_view), slot, 1.0);
		return true;
	}
	int slot = card_slot(oo, ov->sel_ws);
	if (slot < 0)
		return false;
	*out = ov_map(&g, oo->output->box, slot, 1.0);
	return true;
}

/* hjkl: every window on the output, and every empty workspace */
static void nav(struct aro_server *s, ly_edge e)
{
	struct aro_overview *ov = &s->overview;
	struct ov_output *oo = ov->sel_out ? ov_find(s, ov->sel_out) : NULL;
	ly_box from;
	if (!oo || !sel_box(s, oo, &from))
		return;

	int cap = oo->nitems + oo->ncards;
	ly_box *boxes = calloc(cap > 0 ? cap : 1, sizeof *boxes);
	struct aro_view **views = calloc(cap > 0 ? cap : 1, sizeof *views);
	int *ws = calloc(cap > 0 ? cap : 1, sizeof *ws);
	if (!boxes || !views || !ws)
		goto out;

	struct ov_geom g = geom_for(s, oo->output);
	int n = 0;
	for (int i = 0; i < oo->nitems; i++) {
		struct aro_view *v = oo->items[i].view;
		int slot = card_slot(oo, v->workspace);
		if (slot < 0 || v == ov->sel_view)
			continue;
		boxes[n] = ov_map(&g, aro_view_box(v), slot, 1.0);
		views[n] = v;
		ws[n++] = v->workspace;
	}
	for (int i = 0; i < oo->ncards; i++) {
		struct ov_card *c = &oo->cards[i];
		bool empty = true;
		for (int j = 0; j < oo->nitems && empty; j++)
			if (oo->items[j].view->workspace == c->ws)
				empty = false;
		if (!empty || (!ov->sel_view && c->ws == ov->sel_ws))
			continue;
		boxes[n] = ov_map(&g, oo->output->box, i, 1.0);
		views[n] = NULL;
		ws[n++] = c->ws;
	}

	int at = ly_pick(boxes, n, from, e);
	if (at >= 0) {
		if (views[at])
			select_view(s, views[at]);
		else
			select_ws(s, oo->output, ws[at]);
	}
out:
	free(boxes);
	free(views);
	free(ws);
}

static void step_ws(struct aro_server *s, int dir)
{
	struct aro_overview *ov = &s->overview;
	struct ov_output *oo = ov->sel_out ? ov_find(s, ov->sel_out) : NULL;
	if (!oo)
		return;
	int ws = ov->sel_view ? ov->sel_view->workspace : ov->sel_ws;
	int slot = card_slot(oo, ws) + dir;
	if (slot >= 0 && slot < oo->ncards)
		select_ws(s, oo->output, oo->cards[slot].ws);
}

/* ── open and close ────────────────────────────────────────────────────── */

static void drag_stop(struct aro_server *s);

static void close_begin(struct aro_server *s)
{
	struct aro_overview *ov = &s->overview;
	drag_stop(s);
	uint32_t now = aro_now_ms();
	ov->open = false;
	ov->pressed = false;
	ov->p_from = ov->p;
	ov->p_to = 0;
	ov->p_start = now;
	for (int i = 0; i < ov->nouts; i++) {
		struct ov_output *oo = &ov->outs[i];
		scroll_to(oo, card_slot(oo, oo->output->cur_ws), now);
		for (int j = 0; j < oo->ncards; j++)
			qtext_show(&oo->cards[j].label, false);
	}
	schedule_all(s);
}

static void close_done(struct aro_server *s)
{
	struct aro_overview *ov = &s->overview;
	all_free(ov);
	ov->shown = ov->open = false;
	ov->p = 0;
	set_layers(s, true);
	aro_arrange(s);
}

static void ov_commit(struct aro_server *s)
{
	struct aro_overview *ov = &s->overview;
	struct aro_view *v = ov->sel_view;
	struct aro_output *o = ov->sel_out;
	int ws = ov->sel_ws;

	if (v && v->mapped && v->output) {
		view_raise_and_focus(s, v);
		mru_touch(s);
	} else if (o && ov_find(s, o)) {
		s->focused_output = o;
		struct q_bind b = { .action = Q_WORKSPACE, .num = ws };
		aro_run_action(s, &b);
	}
	close_begin(s);
}

static void ov_open(struct aro_server *s)
{
	struct aro_overview *ov = &s->overview;
	if (aro_locked(s) || prompt_active(s) || switcher_active(s))
		return;

	if (!ov->shown) {
		if (!all_build(s, NULL)) {
			wlr_log(WLR_ERROR, "could not build the overview");
			all_free(ov);
			return;
		}
		ov->shown = true;
		ov->p = 0;
		set_layers(s, false);
		if (s->seat->pointer_state.button_count == 0)
			wlr_seat_pointer_notify_clear_focus(s->seat);
		wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
	}
	ov->open = true;
	ov->p_from = ov->p;
	ov->p_to = 1;
	ov->p_start = aro_now_ms();
	ov->pressed = false;
	ov->scroll = 0;

	ov->sel_view = NULL;
	ov->sel_out = aro_focused_output(s);
	ov->sel_ws = ov->sel_out ? ov->sel_out->cur_ws : 0;
	if (s->focused && s->focused->mapped && s->focused->output == ov->sel_out &&
	    s->focused->workspace == ov->sel_out->cur_ws)
		ov->sel_view = s->focused;
	select_fix(s);
	schedule_all(s);
}

void overview_toggle(struct aro_server *s)
{
	if (s->overview.open)
		ov_commit(s);
	else
		ov_open(s);
}

bool overview_active(struct aro_server *s)
{
	return s->overview.open;
}

bool overview_shown(struct aro_server *s)
{
	return s->overview.shown;
}

void overview_rebuild(struct aro_server *s)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->shown)
		return;

	if (ov->dragging)
		wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");

	/* strip positions survive a rebuild */
	int n = ov->nouts;
	struct ov_output *keep = calloc(n > 0 ? n : 1, sizeof *keep);
	if (!keep)
		return;
	for (int i = 0; i < n; i++)
		keep[i] = ov->outs[i];

	all_free(ov);
	if (!all_build(s, NULL)) {
		wlr_log(WLR_ERROR, "could not rebuild the overview");
		free(keep);
		all_free(ov);
		ov->open = ov->shown = false;
		set_layers(s, true);
		aro_arrange(s);
		return;
	}
	for (int i = 0; i < n; i++) {
		struct ov_output *oo = ov_find(s, keep[i].output);
		if (!oo)
			continue;
		oo->c = keep[i].c;
		oo->c_from = keep[i].c_from;
		oo->c_to = keep[i].c_to;
		oo->c_start = keep[i].c_start;
	}
	free(keep);
	if (ov->open)
		select_fix(s);
	schedule_all(s);
}

void overview_output_gone(struct aro_server *s, struct aro_output *o)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->shown || !ov_find(s, o))
		return;
	if (ov->sel_out == o) {
		ov->sel_out = NULL;
		ov->sel_view = NULL;
	}
	all_free(ov);
	if (!all_build(s, o)) {
		all_free(ov);
		ov->open = ov->shown = false;
		set_layers(s, true);
		return;
	}
	if (ov->open)
		select_fix(s);
}

void overview_finish(struct aro_server *s)
{
	all_free(&s->overview);
	s->overview.open = s->overview.shown = false;
}

/* ── drawing ───────────────────────────────────────────────────────────── */

static double advance(double from, double to, uint32_t start, uint32_t now,
                      int ms, bool *moving)
{
	if (from == to)
		return to;
	uint32_t el = now - start;
	if (ms <= 0 || el >= (uint32_t)ms)
		return to;
	*moving = true;
	return lerp(from, to, anim_ease_eval(&OV_EASE, (double)el / ms));
}

#ifdef ARO_EFFECTS
/* a shadow cannot be clipped, so one that would reach another screen goes */
static bool on_other_output(struct aro_server *s, struct aro_output *o,
                            ly_box b)
{
	struct aro_output *other;
	ly_box dummy;
	wl_list_for_each(other, &s->outputs, link)
		if (other != o && clip_box(b, other->box, &dummy))
			return true;
	return false;
}

static void shadow_place(struct aro_server *s, struct wlr_scene_shadow *sh,
                         struct aro_output *o, ly_box card, double p)
{
	const int sig = TH_SHADOW_BLUR, dy = TH_SHADOW_Y;
	ly_box b = { card.x - sig, card.y - sig + dy,
	             card.w + 2 * sig, card.h + 2 * sig };
	if (card.w < 1 || card.h < 1 || on_other_output(s, o, b)) {
		wlr_scene_node_set_enabled(&sh->node, false);
		return;
	}
	float c[4];
	ui_color(fade(TH_SHADOW_COLOR, p), c);
	wlr_scene_node_set_enabled(&sh->node, true);
	wlr_scene_node_set_position(&sh->node, b.x, b.y);
	wlr_scene_shadow_set_size(sh, b.w, b.h);
	wlr_scene_shadow_set_color(sh, c);
	/* not under the card: it is see-through while it fades in */
	wlr_scene_shadow_set_clipped_region(sh, (struct clipped_region){
		.area = { sig, sig - dy, card.w, card.h },
		.corners = corner_radii_all(s->cfg.theme.radius),
	});
}
#endif

/* the dragged thumbnail: its size when it tore loose, under the pointer */
static ly_box drag_box(const struct aro_overview *ov)
{
	return (ly_box){ round_i(ov->drag_x - ov->grab_dx),
	                 round_i(ov->drag_y - ov->grab_dy),
	                 ov->drag_from.w, ov->drag_from.h };
}

static void draw_output(struct aro_server *s, struct ov_output *oo)
{
	const struct q_theme *th = &s->cfg.theme;
	struct aro_overview *ov = &s->overview;
	struct aro_output *o = oo->output;
	const struct ov_geom g = geom_for(s, o);
	const ly_box clip = o->box;
	const double p = ov->p;
	const int bw = th->border;

#ifdef ARO_EFFECTS
	wlr_scene_node_set_position(&oo->blur->node, o->box.x, o->box.y);
	wlr_scene_blur_set_size(oo->blur, o->box.w, o->box.h);
	wlr_scene_blur_set_strength(oo->blur, (float)p);
	wlr_scene_blur_set_alpha(oo->blur, (float)p);
#endif
	rect_color(oo->backdrop, fade(th->overview_tint, p));
	rect_place(oo->backdrop, o->box, clip);

	for (int i = 0; i < oo->ncards; i++) {
		struct ov_card *c = &oo->cards[i];
		ly_box b = ov_map(&g, o->box, i - oo->c, p);
		c->drawn = b;
#ifdef ARO_EFFECTS
		shadow_place(s, c->shadow, o, b, p);
#endif
		bool sel = ov->open && !ov->sel_view && ov->sel_out == o &&
		           ov->sel_ws == c->ws;
		wlr_scene_node_set_enabled(&c->edge->node, sel);
		if (sel)
			rect_place(c->edge, b, clip);
		rect_color(c->bg, fade(th->bg, p));
		rect_place(c->bg, sel ? inset(b, bw) : b, clip);

		ly_box inner;
		bool any = clip_box(sel ? inset(b, bw) : b, clip, &inner);
		for (int k = 0; k < c->nwall; k++) {
			ly_box wb;
			struct wlr_fbox src;
			if (!any || !wall_box(c->wall_src[k], &wb, &src)) {
				wlr_scene_node_set_enabled(&c->wall[k]->node, false);
				continue;
			}
			buf_place(c->wall[k], src, ov_map(&g, wb, i - oo->c, p), inner);
		}

		bool label = ov->open && p >= 1.0;
		qtext_show(&c->label, label && in_box(clip, b.x + b.w / 2, b.y + b.h));
		if (label)
			qtext_move(&c->label, b.x + (b.w - c->label.w) / 2,
			           b.y + b.h + th->text_pad / 2);
	}

	for (int i = 0; i < oo->nitems; i++) {
		struct ov_item *it = &oo->items[i];
		struct aro_view *v = it->view;
		int slot = card_slot(oo, v->workspace);
		if (slot < 0 || !v->mapped || v->output != o) {
			if (it->edge) {
				wlr_scene_node_set_enabled(&it->edge->node, false);
				wlr_scene_node_set_enabled(&it->bg->node, false);
				wlr_scene_node_set_enabled(&it->ring->node, false);
			}
			if (it->snap)
				wlr_scene_node_set_enabled(&it->snap->node, false);
			it->drawn = (ly_box){ 0 };
			continue;
		}

		const bool dragged = ov->dragging && v == ov->drag_view;
		const ly_box cl = dragged ? ANYWHERE : clip;
		ly_box real = aro_view_box(v);
		ly_box b = dragged ? drag_box(ov) : ov_map(&g, real, slot - oo->c, p);
		it->drawn = dragged ? (ly_box){ 0 } : b;        /* not a drop target */
		if (!it->chrome) {
			if (it->snap)
				snap_place(it->snap, v, b, cl);
			continue;
		}

		bool sel = ov->sel_view == v;
		rect_color(it->edge, sel ? th->accent : th->line);
		rect_color(it->bg, sel ? th->frame_on : th->frame);
		rect_place(it->edge, b, cl);
		rect_place(it->bg, inset(b, bw), cl);

		ly_box cb;
		ui_frame_content_box(v, real, &cb);
		if (it->snap)
			snap_place(it->snap, v, dragged ? carry(real, cb, b)
			           : ov_map(&g, cb, slot - oo->c, p), cl);

		ly_box in = inset(b, bw);
		wlr_scene_node_set_enabled(&it->ring->node, sel);
		if (sel)
			rect_place(it->ring, (ly_box){ in.x, in.y, in.w, 1 }, cl);
	}
}

/* ── dragging ──────────────────────────────────────────────────────────── */

/* where a drop at x, y would land; false: nowhere new */
static bool drop_find(struct aro_server *s, double x, double y, ly_box *show)
{
	struct aro_overview *ov = &s->overview;
	struct aro_view *v = ov->drag_view;
	ov->drop_out = NULL;
	ov->drop_target = NULL;
	if (!v)
		return false;

	for (int i = 0; i < ov->nouts; i++) {
		struct ov_output *oo = &ov->outs[i];
		if (!in_box(oo->output->box, x, y))
			continue;
		for (int j = 0; j < oo->ncards; j++) {
			struct ov_card *c = &oo->cards[j];
			if (!in_box(c->drawn, x, y))
				continue;
			ov->drop_out = oo->output;
			ov->drop_ws = c->ws;

			/* tiled: beside the window under the pointer */
			const bool tiled = !v->floating && !v->fullscreen;
			for (int k = 0; tiled && k < oo->nitems; k++) {
				struct ov_item *it = &oo->items[k];
				struct aro_view *t = it->view;
				if (t == v || t->workspace != c->ws || t->floating ||
				    t->fullscreen || !t->node || !in_box(it->drawn, x, y))
					continue;
				ov->drop_target = t;
				ov->drop_side = aro_nearest_edge(it->drawn, x, y);
				*show = aro_drop_slot(it->drawn, ov->drop_side);
				return true;
			}
			/* its own workspace, off any window: stays put */
			if (tiled && oo->output == v->output && c->ws == v->workspace)
				return false;
			*show = c->drawn;
			return true;
		}
		return false;
	}
	return false;
}

/* the indicator: the same hairline and wash as outside the overview */
static bool drop_draw(struct aro_server *s, uint32_t now)
{
	const struct q_theme *th = &s->cfg.theme;
	struct aro_overview *ov = &s->overview;
	ly_box to;
	if (!drop_find(s, ov->drag_x, ov->drag_y, &to)) {
		wlr_scene_node_set_enabled(&ov->drop_tree->node, false);
		ov->drop_shown = false;
		return false;
	}
	if (!ov->drop_shown) {
		anim_box_set(&ov->drop_geo, to);
		ov->drop_shown = true;
		wlr_scene_node_set_enabled(&ov->drop_tree->node, true);
	} else {
		anim_box_to(&ov->drop_geo, to, now, th->drop_ms, &OV_EASE);
	}
	bool moving = anim_box_tick(&ov->drop_geo, now);

	const ly_box b = ov->drop_geo.cur, clip = ov->drop_out->box;
	const int bw = th->border;
	rect_place(ov->drop_edge, b, clip);
	rect_place(ov->drop_fill, inset(b, bw), clip);
#ifdef ARO_EFFECTS
	/* hollow; the region is relative to what rect_place kept */
	ly_box vis;
	if (clip_box(b, clip, &vis))
		wlr_scene_rect_set_clipped_region(ov->drop_edge, (struct clipped_region){
			.area = { b.x + bw - vis.x, b.y + bw - vis.y,
			          b.w - 2 * bw, b.h - 2 * bw },
			.corners = corner_radii_all(th->radius - bw > 0
			                            ? th->radius - bw : 0),
		});
#endif
	return moving;
}

static bool drag_begin(struct aro_server *s)
{
	const struct q_theme *th = &s->cfg.theme;
	struct aro_overview *ov = &s->overview;
	struct aro_view *v = ov->press_view;
	struct ov_output *oo = v && v->mapped ? ov_find(s, v->output) : NULL;
	struct ov_item *it = NULL;
	for (int i = 0; oo && i < oo->nitems; i++)
		if (oo->items[i].view == v)
			it = &oo->items[i];
	if (!it || it->drawn.w < 1 || it->drawn.h < 1)
		return false;

	const int ri = th->radius - th->border > 0 ? th->radius - th->border : 0;
	ov->drop_tree = wlr_scene_tree_create(s->l_overview);
	ov->drag_tree = wlr_scene_tree_create(s->l_overview);
	if (ov->drop_tree) {
		ov->drop_edge = rect(ov->drop_tree, th->drop_line, th->radius);
		ov->drop_fill = rect(ov->drop_tree, th->drop_fill, ri);
	}
	if (!ov->drag_tree || !ov->drop_edge || !ov->drop_fill) {
		drag_reset(ov);
		return false;
	}
	wlr_scene_node_set_enabled(&ov->drop_tree->node, false);

	/* over every card and every screen; same order as before */
	if (it->edge)
		wlr_scene_node_reparent(&it->edge->node, ov->drag_tree);
	if (it->bg)
		wlr_scene_node_reparent(&it->bg->node, ov->drag_tree);
	if (it->snap)
		wlr_scene_node_reparent(&it->snap->node, ov->drag_tree);
	if (it->ring)
		wlr_scene_node_reparent(&it->ring->node, ov->drag_tree);

	ov->dragging = true;
	ov->drag_view = v;
	ov->drag_from = it->drawn;
	ov->grab_dx = ov->press_x - it->drawn.x;
	ov->grab_dy = ov->press_y - it->drawn.y;
	ov->drag_x = ov->press_x;
	ov->drag_y = ov->press_y;
	/* selected, but the strip does not scroll to it */
	ov->sel_view = v;
	ov->sel_out = v->output;
	ov->sel_ws = v->workspace;
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "grabbing");
	return true;
}

/* cut short: the rebuild puts the thumbnail back */
static void drag_stop(struct aro_server *s)
{
	if (s->overview.dragging)
		overview_rebuild(s);
}

static void drop_finish(struct aro_server *s, double x, double y)
{
	struct aro_overview *ov = &s->overview;
	struct aro_view *v = ov->drag_view;
	ov->drag_x = x;
	ov->drag_y = y;

	ly_box show;
	const bool ok = drop_find(s, x, y, &show);
	struct aro_output *o = ov->drop_out;
	const int ws = ov->drop_ws;
	struct aro_view *target = ov->drop_target;
	const ly_edge side = ov->drop_side;

	/* floating: lands where its thumbnail was let go */
	ly_box fb = { 0 };
	bool have_fb = false;
	struct ov_output *to = ok ? ov_find(s, o) : NULL;
	if (to && v->floating && !v->fullscreen) {
		const struct ov_geom g = geom_for(s, o);
		const ly_box b = drag_box(ov);
		const ly_box ub = aro_output_usable(o);
		const int keep = 48;            /* enough of it stays reachable */
		double lx, ly;
		ov_unmap(&g, b.x, b.y, card_slot(to, ws) - to->c, ov->p, &lx, &ly);
		fb = (ly_box){ round_i(lx), round_i(ly), v->fbox.w, v->fbox.h };
		if (fb.x > ub.x + ub.w - keep)
			fb.x = ub.x + ub.w - keep;
		if (fb.x + fb.w < ub.x + keep)
			fb.x = ub.x + keep - fb.w;
		if (fb.y > ub.y + ub.h - keep)
			fb.y = ub.y + ub.h - keep;
		if (fb.y < ub.y)
			fb.y = ub.y;
		have_fb = true;
	}

	/* the strip stays put: another drop may follow */
	const int n = ov->nouts;
	struct aro_output **outs = calloc(n > 0 ? n : 1, sizeof *outs);
	double *at = calloc(n > 0 ? n : 1, sizeof *at);
	for (int i = 0; outs && at && i < n; i++) {
		outs[i] = ov->outs[i].output;
		at[i] = ov->outs[i].c_to;
	}

	if (ok)
		aro_view_drop(s, v, o, ws, target, side, have_fb ? &fb : NULL);
	overview_rebuild(s);            /* ends the drag, draws it anew */

	for (int i = 0; outs && at && i < n; i++) {
		struct ov_output *oo = ov_find(s, outs[i]);
		if (!oo || oo->ncards < 1)
			continue;
		double c = at[i] < oo->ncards - 1 ? at[i] : oo->ncards - 1;
		oo->c_from = oo->c;
		oo->c_to = c;
		oo->c_start = aro_now_ms();
	}
	free(outs);
	free(at);
}

void overview_pointer_motion(struct aro_server *s, double x, double y)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->open || !ov->pressed)
		return;
	if (!ov->dragging) {
		const double tear = s->cfg.theme.drag_tear;
		double ax = x - ov->press_x, ay = y - ov->press_y;
		if (ax < 0)
			ax = -ax;
		if (ay < 0)
			ay = -ay;
		if (!ov->press_view || (ax < tear && ay < tear))
			return;
		if (!drag_begin(s)) {
			ov->press_view = NULL;  /* not a click any more either */
			return;
		}
	}
	ov->drag_x = x;
	ov->drag_y = y;
	schedule_all(s);
}

bool overview_tick(struct aro_server *s, uint32_t now)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->shown)
		return false;

	const int ms = s->cfg.theme.overview_ms;
	bool moving = false;
	ov->p = advance(ov->p_from, ov->p_to, ov->p_start, now, ms, &moving);
	for (int i = 0; i < ov->nouts; i++) {
		struct ov_output *oo = &ov->outs[i];
		oo->c = advance(oo->c_from, oo->c_to, oo->c_start, now, ms, &moving);
	}

	if (!moving && !ov->open && ov->p <= 0.0) {
		close_done(s);
		return false;
	}
	for (int i = 0; i < ov->nouts; i++)
		draw_output(s, &ov->outs[i]);
	if (ov->dragging && drop_draw(s, now))
		moving = true;
	return moving;
}

static void send_done(struct wlr_surface *surface, int sx, int sy, void *data)
{
	(void)sx;
	(void)sy;
	wlr_surface_send_frame_done(surface, data);
}

/* hidden windows get no frame callbacks from the scene; keep them drawing */
void overview_frame_done(struct aro_server *s, struct aro_output *o,
                         struct timespec *now)
{
	struct ov_output *oo = s->overview.shown ? ov_find(s, o) : NULL;
	if (!oo)
		return;
	for (int i = 0; i < oo->nitems; i++) {
		struct wlr_surface *surf = view_surface(oo->items[i].view);
		if (surf)
			wlr_surface_for_each_surface(surf, send_done, now);
	}
}

void overview_view_commit(struct aro_server *s, struct aro_view *v)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->shown || !v->output)
		return;
	struct ov_output *oo = ov_find(s, v->output);
	if (!oo)
		return;
	for (int i = 0; i < oo->nitems; i++) {
		struct ov_item *it = &oo->items[i];
		if (it->view != v)
			continue;
		if (!it->snap) {
			overview_rebuild(s);    /* its first buffer */
			return;
		}
		ui_snapshot_update(it->snap, v);
		wlr_output_schedule_frame(v->output->wlr_output);
		return;
	}
}

/* animated wallpapers stay animated */
void overview_layer_commit(struct aro_server *s, struct wlr_surface *surface)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->shown)
		return;
	for (int i = 0; i < ov->nouts; i++) {
		struct ov_output *oo = &ov->outs[i];
		for (int j = 0; j < oo->ncards; j++) {
			struct ov_card *c = &oo->cards[j];
			for (int k = 0; k < c->nwall; k++) {
				if (c->wall_src[k]->layer_surface->surface != surface)
					continue;
				if (surface->buffer)
					wlr_scene_buffer_set_buffer_with_damage(c->wall[k],
						&surface->buffer->base, NULL);
				wlr_output_schedule_frame(oo->output->wlr_output);
			}
		}
	}
}

/* ── input ─────────────────────────────────────────────────────────────── */

void overview_key(struct aro_server *s, uint32_t mods, xkb_keysym_t sym)
{
	struct aro_overview *ov = &s->overview;
	if (!ov->open)
		return;

	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	                      WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	mods &= care;
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->sym != sym || b->mods != mods)
			continue;
		switch (b->action) {
		case Q_OVERVIEW:
			ov_commit(s);
			return;
		case Q_FOCUS:
			nav(s, (ly_edge)b->num);
			return;
		case Q_WORKSPACE:
			if (ov->sel_out)
				select_ws(s, ov->sel_out, b->num);
			return;
		case Q_CLOSE:
			if (ov->sel_view)
				view_close(ov->sel_view);
			return;
		default:
			break;
		}
	}

	switch (sym) {
	case XKB_KEY_Escape:
		close_begin(s);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		ov_commit(s);
		break;
	case XKB_KEY_h: case XKB_KEY_Left:  nav(s, LY_LEFT);  break;
	case XKB_KEY_l: case XKB_KEY_Right: nav(s, LY_RIGHT); break;
	case XKB_KEY_k: case XKB_KEY_Up:    nav(s, LY_UP);    break;
	case XKB_KEY_j: case XKB_KEY_Down:  nav(s, LY_DOWN);  break;
	default:
		if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9 && ov->sel_out)
			select_ws(s, ov->sel_out, (int)(sym - XKB_KEY_1));
		break;
	}
}

/* what is under the pointer: a window, else a workspace */
static bool hit(struct aro_server *s, double x, double y,
                struct aro_output **out, struct aro_view **view, int *ws)
{
	struct aro_overview *ov = &s->overview;
	for (int i = 0; i < ov->nouts; i++) {
		struct ov_output *oo = &ov->outs[i];
		if (!in_box(oo->output->box, x, y))
			continue;
		*out = oo->output;
		for (int j = oo->nitems - 1; j >= 0; j--) {
			if (in_box(oo->items[j].drawn, x, y)) {
				*view = oo->items[j].view;
				*ws = oo->items[j].view->workspace;
				return true;
			}
		}
		for (int j = 0; j < oo->ncards; j++) {
			if (in_box(oo->cards[j].drawn, x, y)) {
				*view = NULL;
				*ws = oo->cards[j].ws;
				return true;
			}
		}
		return false;
	}
	return false;
}

void overview_pointer_button(struct aro_server *s, double x, double y,
                             bool pressed)
{
	struct aro_overview *ov = &s->overview;
	struct aro_output *o = NULL;
	struct aro_view *v = NULL;
	int ws = -1;
	bool on = hit(s, x, y, &o, &v, &ws);

	if (pressed) {
		ov->pressed = true;
		ov->press_out = on ? o : NULL;
		ov->press_view = v;
		ov->press_ws = ws;
		ov->press_x = x;
		ov->press_y = y;
		return;
	}
	if (!ov->pressed)
		return;
	ov->pressed = false;
	if (ov->dragging) {
		drop_finish(s, x, y);
		return;
	}
	if (!on) {
		if (!ov->press_out)
			close_begin(s);         /* backdrop to backdrop */
		return;
	}
	if (o != ov->press_out || v != ov->press_view || ws != ov->press_ws)
		return;
	if (v)
		select_view(s, v);
	else
		select_ws(s, o, ws);
	ov_commit(s);
}

void overview_pointer_axis(struct aro_server *s, double delta, int discrete)
{
	struct aro_overview *ov = &s->overview;
	ov->scroll += discrete ? discrete / 120.0 : delta / 30.0;
	while (ov->scroll >= 1.0) {
		ov->scroll -= 1.0;
		step_ws(s, 1);
	}
	while (ov->scroll <= -1.0) {
		ov->scroll += 1.0;
		step_ws(s, -1);
	}
}
