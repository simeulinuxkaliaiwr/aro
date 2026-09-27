/* ui.c: frame rendering */
/* scene.h must come first */
#include "scene.h"

#include "aro.h"
#include "text.h"
#include "theme.h"

#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
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
	if (rect)
		wlr_scene_rect_set_corner_radius(rect, radius > 0 ? radius : 0);
#else
	(void)rect;
	(void)radius;
#endif
}

/* this window's border, header and corner radius; rules can take each away */
int ui_frame_border(struct aro_view *v)
{
	return v->no_border ? 0 : v->server->cfg.theme.border;
}

static int frame_header(struct aro_view *v)
{
	const bool none = v->no_header ||
		(v->csd && v->server->cfg.header == Q_HEADER_AUTO);
	return none ? 0 : v->server->cfg.theme.header_h;
}

int ui_frame_radius(struct aro_view *v)
{
	return v->no_radius ? 0 : v->server->cfg.theme.radius;
}

/* the radius inside the border: the content's corners */
static int inner_radius(struct aro_view *v)
{
	int r = ui_frame_radius(v) - ui_frame_border(v);
	return r > 0 ? r : 0;
}

/* hollow out border ring; at is where the border rect starts after edge clipping */
static void clip_border(struct aro_view *v, ly_box at, int inner_w, int inner_h)
{
#ifdef ARO_EFFECTS
	const int bw = ui_frame_border(v);
	wlr_scene_rect_set_clipped_region(v->frame, (struct clipped_region){
		.area = { bw - at.x, bw - at.y, inner_w, inner_h },
		.corners = corner_radii_all(inner_radius(v)),
	});
#else
	(void)v; (void)at; (void)inner_w; (void)inner_h;
#endif
}

static bool box_meet(ly_box a, ly_box b, ly_box *out)
{
	int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
	int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
	int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
	*out = x1 > x0 && y1 > y0 ? (ly_box){ x0, y0, x1 - x0, y1 - y0 } : (ly_box){ 0 };
	return out->w > 0;
}

/* a frame rect trimmed to what may be drawn; nothing left draws nothing */
static ly_box place_rect(struct wlr_scene_rect *r, ly_box want, ly_box vis)
{
	ly_box got;
	box_meet(want, vis, &got);
	wlr_scene_node_set_position(&r->node, got.x, got.y);
	wlr_scene_rect_set_size(r, got.w, got.h);
	return got;
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
#ifdef ARO_EFFECTS
	int radius = v->fullscreen ? 0 : inner_radius(v);
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

	set_radius(v->frame, ui_frame_radius(v));
	set_radius(v->bg, inner_radius(v));

	wlr_scene_node_set_enabled(&v->ring->node, false);
	return true;
}

/* hide chrome in fullscreen */
void ui_frame_fullscreen(struct aro_view *v, bool fullscreen)
{
	wlr_scene_node_set_enabled(&v->frame->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->bg->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->header->node, !fullscreen);
	qtext_show(&v->title, !fullscreen);
	if (v->icon)
		wlr_scene_node_set_enabled(&v->icon->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->ring->node, !fullscreen &&
	                           v->server->focused == v && ui_frame_border(v) > 0);

#ifdef ARO_EFFECTS
	/* no rounded corners in fullscreen */
	int radius = fullscreen ? 0 : inner_radius(v);
	wlr_scene_node_for_each_buffer(&v->content->node, round_buffer, &radius);
#endif
}

/* content box inside frame */
void ui_frame_content_box(struct aro_view *v, ly_box b, ly_box *out)
{
	const int bw = ui_frame_border(v);
	const int hh = frame_header(v);

	int cw = b.w - bw * 2;
	int ch = b.h - bw * 2 - hh;
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;
	*out = (ly_box){ b.x + bw, b.y + bw + hh, cw, ch };
}

/* an app icon's side in the header; 0 when the header is too short for one */
static int icon_size(const struct q_theme *th)
{
	int sz = th->header_h - 10;
	return sz >= 8 ? sz : 0;
}

/* where the title starts: after the icon, when there is one */
static int title_x(struct aro_view *v)
{
	const struct q_theme *th = &v->server->cfg.theme;
	int x = ui_frame_border(v) + th->text_pad;
	if (v->icon && icon_size(th))
		x += icon_size(th) + th->text_pad / 2;
	return x;
}

void ui_frame_geometry(struct aro_view *v, ly_box b)
{
	const struct q_theme *th = &v->server->cfg.theme;
	const int bw = ui_frame_border(v);
	const int hh = frame_header(v);      /* none for CSD in auto mode */

	/* ignore zero-size geometry */
	if (b.w <= 0 || b.h <= 0)
		return;

	/* surface geometry offset for CSD */
	struct wlr_box g;
	view_geometry(v, &g);

	if (v->fullscreen) {
		wlr_scene_node_set_position(&v->frame_tree->node, b.x, b.y);
		wlr_scene_node_set_enabled(&v->content->node, true);   /* edge clipping may have hidden it */
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

	/* a scroll column is cut at its screen's edge, not drawn on the next screen */
	ly_box vis = { 0, 0, b.w, b.h };
	if (aro_view_clipped(v)) {
		ly_box ob = v->output->box;
		box_meet(vis, (ly_box){ ob.x - b.x, ob.y - b.y, ob.w, ob.h }, &vis);
	}
	const bool cut = vis.x != 0 || vis.y != 0 || vis.w != b.w || vis.h != b.h;

	ly_box border = place_rect(v->frame, (ly_box){ 0, 0, b.w, b.h }, vis);
	place_rect(v->bg, (ly_box){ bw, bw, inner_w, inner_h }, vis);
	place_rect(v->ring, (ly_box){ bw, bw, inner_w, 1 }, vis);

	wlr_scene_node_set_enabled(&v->header->node, hh > 0);
	qtext_show(&v->title, hh > 0 && !v->fullscreen && !cut);
	if (v->icon) {
		/* the icon goes where the title goes */
		const int isz = icon_size(th);
		wlr_scene_node_set_enabled(&v->icon->node, isz && hh >= isz && !v->fullscreen && !cut);
		wlr_scene_node_set_position(&v->icon->node, bw + th->text_pad, bw + (hh - isz) / 2);
	}
	place_rect(v->header, (ly_box){ bw, bw, inner_w, hh < inner_h ? hh : inner_h }, vis);

	ly_box cb;
	ui_frame_content_box(v, b, &cb);
	const int cw = cb.w;
	const int ch = cb.h;

	clip_border(v, border, inner_w, inner_h);

	wlr_scene_node_set_position(&v->content->node, bw, bw + hh);

	/* popups follow content origin */
	wlr_scene_node_set_position(&v->popups->node, bw, bw + hh);

	/* verify wlroots/scenefx calls against current headers */
	/* clip actual surface tree, not wrapper; and to the visible part */
	ly_box shown;
	bool any = box_meet((ly_box){ g.x, g.y, cw, ch },
	                    (ly_box){ vis.x - bw + g.x, vis.y - bw - hh + g.y, vis.w, vis.h },
	                    &shown);
	wlr_scene_node_set_enabled(&v->content->node, any);
	if (v->surface_tree && any)
		wlr_scene_subsurface_tree_set_clip(&v->surface_tree->node,
			&(struct wlr_box){ shown.x, shown.y, shown.w, shown.h });

	/* don't re-render title every frame */
	qtext_move(&v->title, title_x(v), bw + (hh - v->title.h) / 2);

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

	wlr_scene_node_set_enabled(&v->ring->node, focused && ui_frame_border(v) > 0);

	/* title color follows focus */
	const char *title = view_title(v);
	qtext_set(&v->title, title ? title : "", focused ? th->accent : th->dim,
	          v->title.scale, v->title.max_w);
}

/* the app's icon in the header; NULL takes it away. The scene keeps its own lock */
void ui_frame_icon(struct aro_view *v, struct wlr_buffer *buf)
{
	const struct q_theme *th = &v->server->cfg.theme;
	if (v->icon) {
		wlr_scene_node_destroy(&v->icon->node);
		v->icon = NULL;
	}
	if (buf && icon_size(th)) {
		v->icon = wlr_scene_buffer_create(v->frame_tree, buf);
		if (v->icon) {
			wlr_scene_node_place_above(&v->icon->node, &v->header->node);
			wlr_scene_buffer_set_dest_size(v->icon, icon_size(th), icon_size(th));
			wlr_scene_buffer_set_filter_mode(v->icon, WLR_SCALE_FILTER_BILINEAR);
			/* shown where and when the title is */
			const int bw = ui_frame_border(v);
			wlr_scene_node_set_position(&v->icon->node, bw + th->text_pad,
			                            bw + (th->header_h - icon_size(th)) / 2);
			wlr_scene_node_set_enabled(&v->icon->node,
				v->title.node && v->title.node->node.enabled);
		}
	}
	if (v->mapped && v->output)
		ui_frame_title(v, aro_view_box(v).w, v->output->scale);
}

/* update title using target width */
void ui_frame_title(struct aro_view *v, int frame_w, float scale)
{
	const struct q_theme *th = &v->server->cfg.theme;
	const char *title = view_title(v);
	int avail = frame_w - title_x(v) - ui_frame_border(v) - th->text_pad;
	if (avail < 1)
		avail = 1;

	const int hh = frame_header(v);
	if (hh == 0)
		return;         /* no header to put a title in */

	bool focused = v->server->focused == v;
	qtext_set(&v->title, title ? title : "", focused ? th->accent : th->dim,
	          scale, avail);
	qtext_move(&v->title, title_x(v), ui_frame_border(v) + (hh - v->title.h) / 2);
}

/* ── snapshots ─────────────────────────────────────────────────────────── */

/* buffer crop to the window geometry (no CSD shadow); false: no buffer */
bool ui_snapshot_src(struct aro_view *v, struct wlr_fbox *out)
{
	struct wlr_surface *surf = view_surface(v);
	if (!surf || !surf->buffer)
		return false;
	struct wlr_buffer *buf = &surf->buffer->base;
	*out = (struct wlr_fbox){ 0, 0, buf->width, buf->height };

	struct wlr_box g = { 0 };
	view_geometry(v, &g);
	float sc = surf->current.scale > 0 ? (float)surf->current.scale : 1.0f;
	if (g.width <= 0 || g.height <= 0)
		return true;
	struct wlr_fbox src = { g.x * sc, g.y * sc, g.width * sc, g.height * sc };
	if (src.x < 0)
		src.x = 0;
	if (src.y < 0)
		src.y = 0;
	if (src.x + src.width > buf->width)
		src.width = buf->width - src.x;
	if (src.y + src.height > buf->height)
		src.height = buf->height - src.y;
	if (src.width > 0 && src.height > 0)
		*out = src;
	return true;
}

/* the window's current buffer; the scene holds a lock on it */
struct wlr_scene_buffer *ui_snapshot_create(struct wlr_scene_tree *parent,
                                            struct aro_view *v, int w, int h,
                                            int radius)
{
	struct wlr_surface *surf = view_surface(v);
	if (!surf || !surf->buffer)
		return NULL;

	struct wlr_buffer *buf = &surf->buffer->base;
	struct wlr_scene_buffer *b = wlr_scene_buffer_create(parent, buf);
	if (!b)
		return NULL;

	struct wlr_fbox src;
	if (ui_snapshot_src(v, &src))
		wlr_scene_buffer_set_source_box(b, &src);
	wlr_scene_buffer_set_dest_size(b, w, h);
#ifdef ARO_EFFECTS
	if (radius > 0)
		wlr_scene_buffer_set_corner_radius(b, radius);
#else
	(void)radius;
#endif
	return b;
}

/* after a commit: show the new buffer */
void ui_snapshot_update(struct wlr_scene_buffer *b, struct aro_view *v)
{
	struct wlr_surface *surf = view_surface(v);
	if (b && surf && surf->buffer)
		wlr_scene_buffer_set_buffer_with_damage(b, &surf->buffer->base, NULL);
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

	set_radius(v->frame, ui_frame_radius(v));
	set_radius(v->bg, inner_radius(v));
	ui_frame_clip_content(v);

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
