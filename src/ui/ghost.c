/* ghost.c: a closed window's last frame, shrinking and fading out */
/* scene.h must come first */
#include "scene.h"

#include "ghost.h"
#include "aro.h"
#include "theme.h"

#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

struct aro_ghost {
	struct wl_list link;            /* aro_server.ghosts */
	struct aro_output *output;
	struct wlr_scene_tree *tree;

	struct wlr_scene_rect *edge[4]; /* one hollow rect with scenefx */
	struct wlr_scene_rect *bg;
	struct wlr_scene_rect *ring;    /* focused only */
	struct wlr_scene_buffer *snap;
	struct qtext title;
	bool has_title;

	uint32_t edge_col, bg_col, ring_col;
	bool fullscreen;                /* no chrome, fade only */
	int border, header_h, text_pad, radius;

	ly_box from;
	double scale;                   /* where the shrink ends */
	uint32_t start_ms, dur_ms;
};

#ifdef ARO_EFFECTS
#define NEDGE 1
#else
#define NEDGE 4
#endif

static const anim_ease EASE = { TH_EASE_FLAT_X1, TH_EASE_FLAT_Y1,
                                TH_EASE_FLAT_X2, TH_EASE_FLAT_Y2 };

static int max_i(int a, int b) { return a > b ? a : b; }

static struct wlr_scene_rect *rect(struct wlr_scene_tree *parent, uint32_t rgba)
{
	float c[4];
	ui_color(rgba, c);
	return wlr_scene_rect_create(parent, 1, 1, c);
}

/* premultiplied, so every channel scales */
static void rect_alpha(struct wlr_scene_rect *r, uint32_t rgba, float a)
{
	float c[4];
	ui_color(rgba, c);
	for (int i = 0; i < 4; i++)
		c[i] *= a;
	wlr_scene_rect_set_color(r, c);
}

static void rect_place(struct wlr_scene_rect *r, int x, int y, int w, int h)
{
	wlr_scene_node_set_position(&r->node, x, y);
	wlr_scene_rect_set_size(r, max_i(w, 1), max_i(h, 1));
}

static void set_radius(struct wlr_scene_rect *r, int radius)
{
#ifdef ARO_EFFECTS
	if (r && radius > 0)
		wlr_scene_rect_set_corner_radius(r, radius);
#else
	(void)r;
	(void)radius;
#endif
}

static void ghost_free(struct aro_ghost *g)
{
	if (g->has_title)
		qtext_finish(&g->title);
	if (g->tree)
		wlr_scene_node_destroy(&g->tree->node);
	wl_list_remove(&g->link);
	free(g);
}

/* p: time elapsed, 0 to 1; the shrink eases, the fade is linear */
static void ghost_place(struct aro_ghost *g, double p)
{
	const double k = 1.0 - (1.0 - g->scale) * anim_ease_eval(&EASE, p);
	const float a = (float)(1.0 - p);

	ly_box b;
	b.w = max_i((int)(g->from.w * k + 0.5), 1);
	b.h = max_i((int)(g->from.h * k + 0.5), 1);
	b.x = g->from.x + (g->from.w - b.w) / 2;
	b.y = g->from.y + (g->from.h - b.h) / 2;
	wlr_scene_node_set_position(&g->tree->node, b.x, b.y);

	if (g->fullscreen) {
		wlr_scene_buffer_set_dest_size(g->snap, b.w, b.h);
		wlr_scene_buffer_set_opacity(g->snap, a);
		return;
	}

	const int bw = g->border, hh = g->header_h;
	const int iw = max_i(b.w - bw * 2, 1), ih = max_i(b.h - bw * 2, 1);

#ifdef ARO_EFFECTS
	if (g->edge[0]) {
		rect_place(g->edge[0], 0, 0, b.w, b.h);
		wlr_scene_rect_set_clipped_region(g->edge[0], (struct clipped_region){
			.area = { bw, bw, iw, ih },
			.corners = corner_radii_all(g->radius),
		});
	}
#else
	if (g->edge[0]) {
		rect_place(g->edge[0], 0, 0, b.w, bw);
		rect_place(g->edge[1], 0, b.h - bw, b.w, bw);
		rect_place(g->edge[2], 0, bw, bw, ih);
		rect_place(g->edge[3], b.w - bw, bw, bw, ih);
	}
#endif
	for (int i = 0; i < 4; i++)
		if (g->edge[i])
			rect_alpha(g->edge[i], g->edge_col, a);

	rect_place(g->bg, bw, bw, iw, ih);
	rect_alpha(g->bg, g->bg_col, a);

	if (g->ring) {
		rect_place(g->ring, bw, bw, iw, 1);
		rect_alpha(g->ring, g->ring_col, a);
	}

	wlr_scene_node_set_position(&g->snap->node, bw, bw + hh);
	wlr_scene_buffer_set_dest_size(g->snap, iw, max_i(ih - hh, 1));
	wlr_scene_buffer_set_opacity(g->snap, a);

	if (g->has_title) {
		qtext_move(&g->title, bw + g->text_pad, bw + (hh - g->title.h) / 2);
		if (g->title.node)
			wlr_scene_buffer_set_opacity(g->title.node, a);
	}
}

void ghost_init(struct aro_server *s)
{
	wl_list_init(&s->ghosts);
}

void ghost_spawn(struct aro_server *s, struct aro_view *v, ly_box from)
{
	const struct q_theme *th = &s->cfg.theme;
	const double scale = th->open_scale;

	if (TH_CLOSE_MS <= 0 || scale >= 1.0 || scale <= 0.0)
		return;
	if (!v->output || !v->frame_tree || !v->frame_tree->node.parent ||
	    from.w <= 0 || from.h <= 0)
		return;

	struct aro_ghost *g = calloc(1, sizeof *g);
	if (!g)
		return;
	wl_list_init(&g->link);
	g->tree = wlr_scene_tree_create(v->frame_tree->node.parent);
	if (!g->tree) {
		free(g);
		return;
	}
	wlr_scene_node_raise_to_top(&g->tree->node);

	const bool focused = s->focused == v;
	const int inner = th->radius - th->border > 0 ? th->radius - th->border : 0;
	ly_box cb;
	ui_frame_content_box(v, from, &cb);

	g->output = v->output;
	g->from = from;
	g->fullscreen = v->fullscreen;
	g->scale = v->fullscreen ? 1.0 : scale;
	g->border = th->border;
	g->header_h = cb.y - from.y - th->border;
	g->text_pad = th->text_pad;
	g->radius = inner;
	g->edge_col = focused ? th->accent : th->line;
	g->bg_col = focused ? th->frame_on : th->frame;
	g->ring_col = th->accent_soft;
	g->start_ms = aro_now_ms();
	g->dur_ms = TH_CLOSE_MS;

	/* stacked like a frame: border, bg, ring, content, title */
	if (!g->fullscreen) {
		for (int i = 0; i < NEDGE && g->border > 0; i++)
			g->edge[i] = rect(g->tree, g->edge_col);
		set_radius(g->edge[0], th->radius);
		g->bg = rect(g->tree, g->bg_col);
		set_radius(g->bg, inner);
		if (focused)
			g->ring = rect(g->tree, g->ring_col);
	}

	int sw = g->fullscreen ? from.w : cb.w, sh = g->fullscreen ? from.h : cb.h;
	g->snap = ui_snapshot_create(g->tree, v, sw, sh, g->fullscreen ? 0 : inner);

	bool ok = g->snap != NULL;
	if (!g->fullscreen) {
		ok = ok && g->bg && (g->ring || !focused);
		for (int i = 0; i < NEDGE && g->border > 0; i++)
			ok = ok && g->edge[i];
	}
	if (!ok) {
		ghost_free(g);          /* never drew, or out of memory */
		return;
	}

	if (!g->fullscreen && g->header_h > 0 &&
	    qtext_init(&g->title, g->tree, th->font)) {
		g->has_title = true;
		/* ellipsized to the smallest the frame gets */
		int end_w = (int)(from.w * g->scale);
		int avail = max_i(end_w - th->border * 2 - th->text_pad * 2, 1);
		const char *title = view_title(v);
		qtext_set(&g->title, title ? title : "",
		          focused ? th->accent : th->dim, v->output->scale, avail);
	}

	wl_list_insert(s->ghosts.prev, &g->link);
	ghost_place(g, 0.0);
	wlr_output_schedule_frame(g->output->wlr_output);
}

bool ghost_tick(struct aro_server *s, struct aro_output *o, uint32_t now)
{
	bool more = false;
	struct aro_ghost *g, *tmp;
	wl_list_for_each_safe(g, tmp, &s->ghosts, link) {
		uint32_t el = now - g->start_ms;
		if (el >= g->dur_ms) {
			ghost_free(g);
			continue;
		}
		/* focusing the next window raises it; in monocle it would cover us */
		wlr_scene_node_raise_to_top(&g->tree->node);
		ghost_place(g, (double)el / g->dur_ms);
		if (g->output == o)
			more = true;
	}
	return more;
}

void ghost_drop(struct aro_server *s, struct aro_output *o)
{
	struct aro_ghost *g, *tmp;
	wl_list_for_each_safe(g, tmp, &s->ghosts, link)
		if (!o || g->output == o)
			ghost_free(g);
}
