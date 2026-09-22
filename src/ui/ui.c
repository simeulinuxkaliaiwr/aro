/* ui.c: frame rendering */
/* scene.h must come first */
#include "scene.h"

#include "aro.h"
#include "text.h"
#include "theme.h"

#include <stdlib.h>


#include <wlr/util/box.h>

void ui_color(uint32_t rgba, float out[4])
{
	float a = (float)(rgba & 0xff) / 255.0f;
	/* convert to premultiplied alpha */
	out[0] = (float)((rgba >> 24) & 0xff) / 255.0f * a;
	out[1] = (float)((rgba >> 16) & 0xff) / 255.0f * a;
	out[2] = (float)((rgba >> 8) & 0xff) / 255.0f * a;
	out[3] = a;
}

static void set_radius(struct wlr_scene_rect *rect, int radius)
{
#ifdef ARO_EFFECTS
	if (rect && radius > 0)
		wlr_scene_rect_set_corner_radius(rect, radius);
#else
	(void)rect;
	(void)radius;
#endif
}

/* hollow out border ring */
static void clip_border(struct aro_view *v, int inner_w, int inner_h)
{
	const struct q_theme *th = &v->server->cfg.theme;
#ifdef ARO_EFFECTS
	wlr_scene_rect_set_clipped_region(v->frame, (struct clipped_region){
		.area = { th->border, th->border, inner_w, inner_h },
		.corners = corner_radii_all(th->radius - th->border > 0
		                            ? th->radius - th->border : 0),
	});
#else
	(void)v; (void)inner_w; (void)inner_h;
#endif
}

#ifdef ARO_EFFECTS
static void round_buffer(struct wlr_scene_buffer *buffer, int sx, int sy,
                         void *data)
{
	(void)sx; (void)sy;
	wlr_scene_buffer_set_corner_radius(buffer, *(int *)data);
}
#endif

/* round client content corners */
void ui_frame_clip_content(struct aro_view *v)
{
	const struct q_theme *th = &v->server->cfg.theme;
#ifdef ARO_EFFECTS
	int radius = th->radius - th->border > 0 ? th->radius - th->border : 0;
	if (radius > 0)
		wlr_scene_node_for_each_buffer(&v->content->node, round_buffer, &radius);
#else
	(void)v;
#endif
}

bool ui_frame_create(struct aro_view *v, struct wlr_scene_tree *parent)
{
	const struct q_theme *th = &v->server->cfg.theme;
	float col[4];

	v->frame_tree = wlr_scene_tree_create(parent);
	if (!v->frame_tree)
		return false;

	ui_color(th->line, col);
	v->frame = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	ui_color(th->frame, col);
	v->bg = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	ui_color(th->frame, col);
	v->header = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	/* ring must be created before header occlusion */
	ui_color(th->accent_soft, col);
	v->ring = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	/* title above header */
	if (!qtext_init(&v->title, v->frame_tree, th->font))
		return false;

	v->content = wlr_scene_tree_create(v->frame_tree);

	/* popups above content */
	v->popups = wlr_scene_tree_create(v->frame_tree);

	if (!v->frame || !v->bg || !v->ring || !v->header || !v->content ||
	    !v->popups)
		return false;

	set_radius(v->frame, th->radius);
	set_radius(v->bg, th->radius - th->border > 0 ? th->radius - th->border : 0);

	wlr_scene_node_set_enabled(&v->ring->node, false);
	return true;
}

/* hide chrome in fullscreen */
void ui_frame_fullscreen(struct aro_view *v, bool fullscreen)
{
	const struct q_theme *th = &v->server->cfg.theme;
	wlr_scene_node_set_enabled(&v->frame->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->bg->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->header->node, !fullscreen);
	qtext_show(&v->title, !fullscreen);
	wlr_scene_node_set_enabled(&v->ring->node,
	                           !fullscreen && v->server->focused == v);

#ifdef ARO_EFFECTS
	/* no rounded corners in fullscreen */
	int radius = fullscreen ? 0 : th->radius - th->border;
	if (radius < 0)
		radius = 0;
	wlr_scene_node_for_each_buffer(&v->content->node, round_buffer, &radius);
#endif
}

/* content box inside frame */
void ui_frame_content_box(struct aro_view *v, ly_box b, ly_box *out)
{
	const struct q_theme *th = &v->server->cfg.theme;
	const int bw = th->border;
	const bool no_header =
		v->csd && v->server->cfg.header == Q_HEADER_AUTO;
	const int hh = no_header ? 0 : th->header_h;

	int cw = b.w - bw * 2;
	int ch = b.h - bw * 2 - hh;
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;
	*out = (ly_box){ b.x + bw, b.y + bw + hh, cw, ch };
}

void ui_frame_geometry(struct aro_view *v, ly_box b)
{
	const struct q_theme *th = &v->server->cfg.theme;
	const int bw = th->border;

	/* skip header for CSD in auto mode */
	const bool no_header =
		v->csd && v->server->cfg.header == Q_HEADER_AUTO;
	const int hh = no_header ? 0 : th->header_h;

	/* ignore zero-size geometry */
	if (b.w <= 0 || b.h <= 0)
		return;

	/* surface geometry offset for CSD */
	struct wlr_box g;
	view_geometry(v, &g);

	if (v->fullscreen) {
		wlr_scene_node_set_position(&v->frame_tree->node, b.x, b.y);
		wlr_scene_node_set_position(&v->content->node, 0, 0);
		wlr_scene_node_set_position(&v->popups->node, 0, 0);
		if (v->surface_tree)
			wlr_scene_subsurface_tree_set_clip(&v->surface_tree->node,
				&(struct wlr_box){ g.x, g.y, b.w, b.h });
		view_configure(v, b.x, b.y, b.w, b.h);
		return;
	}

	int inner_w = b.w - bw * 2;
	int inner_h = b.h - bw * 2;
	if (inner_w < 1)
		inner_w = 1;
	if (inner_h < 1)
		inner_h = 1;

	wlr_scene_node_set_position(&v->frame_tree->node, b.x, b.y);

	wlr_scene_rect_set_size(v->frame, b.w, b.h);

	wlr_scene_node_set_position(&v->bg->node, bw, bw);
	wlr_scene_rect_set_size(v->bg, inner_w, inner_h);

	/* place inner ring */
	wlr_scene_node_set_position(&v->ring->node, bw, bw);
	wlr_scene_rect_set_size(v->ring, inner_w, 1);

	wlr_scene_node_set_enabled(&v->header->node, hh > 0);
	qtext_show(&v->title, hh > 0 && !v->fullscreen);
	wlr_scene_node_set_position(&v->header->node, bw, bw);
	wlr_scene_rect_set_size(v->header, inner_w, hh < inner_h ? hh : inner_h);

	ly_box cb;
	ui_frame_content_box(v, b, &cb);
	const int cw = cb.w;
	const int ch = cb.h;

	clip_border(v, inner_w, inner_h);

	wlr_scene_node_set_position(&v->content->node, bw, bw + hh);

	/* popups follow content origin */
	wlr_scene_node_set_position(&v->popups->node, bw, bw + hh);

	/* verify wlroots/scenefx calls against current headers */
	/* clip actual surface tree, not wrapper */
	if (v->surface_tree)
		wlr_scene_subsurface_tree_set_clip(&v->surface_tree->node,
		                                   &(struct wlr_box){ g.x, g.y, cw, ch });

	/* don't re-render title every frame */
	qtext_move(&v->title, bw + th->text_pad, bw + (hh - v->title.h) / 2);

	/* configure client size */
	/* X11 needs absolute position */
	/* don't fight client-sized floats */
	/* safety check for stale float_follow */
	if (!(v->floating && v->float_follow))
		view_configure(v, cb.x, cb.y, cb.w, cb.h);
}

void ui_frame_focus(struct aro_view *v, bool focused)
{
	const struct q_theme *th = &v->server->cfg.theme;
	float col[4];

	ui_color(focused ? th->accent : th->line, col);
	wlr_scene_rect_set_color(v->frame, col);

	ui_color(focused ? th->frame_on : th->frame, col);
	wlr_scene_rect_set_color(v->bg, col);
	wlr_scene_rect_set_color(v->header, col);

	wlr_scene_node_set_enabled(&v->ring->node, focused);

	/* title color follows focus */
	const char *title = view_title(v);
	qtext_set(&v->title, title ? title : "", focused ? th->accent : th->dim,
	          v->title.scale, v->title.max_w);
}

/* update title using target width */
void ui_frame_title(struct aro_view *v, int frame_w, float scale)
{
	const struct q_theme *th = &v->server->cfg.theme;
	const char *title = view_title(v);
	int avail = frame_w - th->border * 2 - th->text_pad * 2;
	if (avail < 1)
		avail = 1;

	if (v->csd && v->server->cfg.header == Q_HEADER_AUTO)
		return;         /* no header to put a title in */

	bool focused = v->server->focused == v;
	qtext_set(&v->title, title ? title : "", focused ? th->accent : th->dim,
	          scale, avail);
	qtext_move(&v->title, th->border + th->text_pad,
	           th->border + (th->header_h - v->title.h) / 2);
}

/* ── the drop indicator ────────────────────────────────────────────────── */
/* drop preview styling */
bool ui_preview_create(struct aro_preview *p, struct aro_server *s)
{
	const struct q_theme *th = &s->cfg.theme;
	float col[4];

	p->server = s;
	p->tree = wlr_scene_tree_create(s->l_preview);
	if (!p->tree)
		return false;

	ui_color(th->drop_line, col);
	p->edge = wlr_scene_rect_create(p->tree, 1, 1, col);

	ui_color(th->drop_fill, col);
	p->fill = wlr_scene_rect_create(p->tree, 1, 1, col);

	if (!p->edge || !p->fill)
		return false;

	set_radius(p->edge, th->radius);
	set_radius(p->fill, th->radius - th->border > 0 ? th->radius - th->border : 0);

	p->active = false;
	wlr_scene_node_set_enabled(&p->tree->node, false);
	return true;
}

void ui_preview_show(struct aro_preview *p, bool visible)
{
	if (!p->tree)
		return;
	p->active = visible;
	wlr_scene_node_set_enabled(&p->tree->node, visible);
}

void ui_preview_geometry(struct aro_preview *p, ly_box b)
{
	const struct q_theme *th = &p->server->cfg.theme;
	const int bw = th->border;

	if (!p->tree)
		return;

	int inner_w = b.w - bw * 2;
	int inner_h = b.h - bw * 2;
	if (inner_w < 1)
		inner_w = 1;
	if (inner_h < 1)
		inner_h = 1;

	wlr_scene_node_set_position(&p->tree->node, b.x, b.y);
	wlr_scene_rect_set_size(p->edge, b.w, b.h);
	wlr_scene_node_set_position(&p->fill->node, bw, bw);
	wlr_scene_rect_set_size(p->fill, inner_w, inner_h);

#ifdef ARO_EFFECTS
	/* hollow preview border */
	wlr_scene_rect_set_clipped_region(p->edge, (struct clipped_region){
		.area = { bw, bw, inner_w, inner_h },
		.corners = corner_radii_all(th->radius - bw > 0 ? th->radius - bw : 0),
	});
#endif
}

void ui_preview_finish(struct aro_preview *p)
{
	if (p->tree)
		wlr_scene_node_destroy(&p->tree->node);
	p->tree = NULL;
	p->active = false;
}

/* retheme frame */
void ui_frame_retheme(struct aro_view *v)
{
	const struct q_theme *th = &v->server->cfg.theme;

	set_radius(v->frame, th->radius);
	set_radius(v->bg, th->radius - th->border > 0 ? th->radius - th->border : 0);

	qtext_set_font(&v->title, th->font);
	ui_frame_focus(v, v->server->focused == v);
}

void ui_preview_retheme(struct aro_preview *p)
{
	const struct q_theme *th = &p->server->cfg.theme;
	float col[4];

	if (!p->tree)
		return;

	ui_color(th->drop_line, col);
	wlr_scene_rect_set_color(p->edge, col);
	ui_color(th->drop_fill, col);
	wlr_scene_rect_set_color(p->fill, col);

	set_radius(p->edge, th->radius);
	set_radius(p->fill, th->radius - th->border > 0 ? th->radius - th->border : 0);
}
