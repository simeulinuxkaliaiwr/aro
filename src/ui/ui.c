/* ui.c: frame rendering */
/* scene.h must come first */
#include "scene.h"

#include "aro.h"
#include "text.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

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
	const int hh = v->server->cfg.theme.header_h;
	if (v->group)
		return hh > 0 ? hh : 24;        /* tabs need somewhere to go */
	const bool none = v->no_header ||
		(v->csd && v->server->cfg.header == Q_HEADER_AUTO);
	return none ? 0 : hh;
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

/* this window's opacity: a rule's, else the config's; fullscreen is solid */
static float frame_opacity(struct aro_view *v, bool f)
{
	if (v->fullscreen)
		return 1.0f;
	const struct aro_config *c = &v->server->cfg;
	const double r = v->rule_opacity[f ? 0 : 1];
	return (float)(r > 0 ? r : f ? c->opacity : c->opacity_unfocused);
}

/* what shows behind the content: window_background, else the frame colour */
static uint32_t frame_bg(struct aro_view *v, bool focused)
{
	const struct aro_config *c = &v->server->cfg;
	return c->win_bg_set ? c->win_bg : focused ? c->theme.frame_on : c->theme.frame;
}

/* how see-through headers are; only SceneFX can hollow the border out from under them */
float ui_header_alpha(struct aro_server *s)
{
#ifdef ARO_EFFECTS
	return (float)s->cfg.header_opacity;
#else
	(void)s;
	return 1.0f;
#endif
}

static float header_alpha(struct aro_view *v)
{
	return ui_header_alpha(v->server);
}

/* ui_color, faded by a */
static void color_faded(uint32_t rgba, float a, float out[4])
{
	ui_color(rgba, out);
	for (int i = 0; i < 4; i++)
		out[i] *= a;
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

struct paint {
	int radius;
	float alpha;
};

static void paint_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	(void)sx; (void)sy;
	const struct paint *p = data;
#ifdef ARO_EFFECTS
	wlr_scene_buffer_set_corner_radius(buffer, p->radius);
#endif
	wlr_scene_buffer_set_opacity(buffer, p->alpha);
}

static void paint_content(struct aro_view *v, float alpha)
{
	struct paint p = { v->fullscreen ? 0 : inner_radius(v), alpha };
	wlr_scene_node_for_each_buffer(&v->content->node, paint_buffer, &p);
}

static void hold_opacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	(void)sx; (void)sy;
	wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

/* before each frame: a surface commit puts its buffer back to opaque */
void ui_frame_hold_opacity(struct aro_view *v)
{
	float a = frame_opacity(v, v->server->focused == v);
	if (a < 1.0f)
		wlr_scene_node_for_each_buffer(&v->content->node, hold_opacity, &a);
}

/* round client content corners and fade it; new buffers come with each commit */
void ui_frame_clip_content(struct aro_view *v)
{
	paint_content(v, frame_opacity(v, v->server->focused == v));
}

/* one highlight for every header, redrawn when header_gloss changes */
struct wlr_buffer *ui_gloss_buffer(struct aro_server *s)
{
	if (s->gloss_amount == s->cfg.header_gloss)
		return s->gloss_buf;
	if (s->gloss_buf)
		wlr_buffer_drop(s->gloss_buf);
	s->gloss_buf = ui_gloss_render(s->cfg.header_gloss);
	s->gloss_amount = s->cfg.header_gloss;
	return s->gloss_buf;
}

static void gloss_sync(struct aro_view *v)
{
	struct wlr_buffer *buf = ui_gloss_buffer(v->server);
	wlr_scene_buffer_set_buffer(v->gloss, buf);
	if (!buf)
		wlr_scene_node_set_enabled(&v->gloss->node, false);
}

/* the highlight over the header; a header cut at the screen's edge shows its part of it.
 * The scene drops its buffer once uploaded, so the server's copy says whether there is one */
void ui_gloss_place(struct aro_server *s, struct wlr_scene_buffer *g,
                    ly_box want, ly_box got, bool shown)
{
	shown = shown && s->gloss_buf && got.w > 0 && got.h > 0 && want.h > 0;
	wlr_scene_node_set_enabled(&g->node, shown);
	if (!shown)
		return;
	const double bh = UI_GLOSS_H;
	wlr_scene_node_set_position(&g->node, got.x, got.y);
	wlr_scene_buffer_set_dest_size(g, got.w, got.h);
	wlr_scene_buffer_set_source_box(g, &(struct wlr_fbox){
		0, bh * (got.y - want.y) / want.h, 1, bh * got.h / want.h });
}

static void place_gloss(struct aro_view *v, ly_box want, ly_box got, bool shown)
{
	ui_gloss_place(v->server, v->gloss, want, got, shown);
}

/* a see-through header: the window background starts under it, and it takes the top corners */
static void round_header(struct aro_view *v, bool glass)
{
#ifdef ARO_EFFECTS
	const int r = inner_radius(v);
	wlr_scene_rect_set_corner_radii(v->bg, glass ? corner_radii_new(0, 0, r, r) : corner_radii_all(r));
	wlr_scene_rect_set_corner_radii(v->header, glass ? corner_radii_new(r, r, 0, 0) : corner_radii_none());
	wlr_scene_buffer_set_corner_radii(v->gloss, glass ? corner_radii_new(r, r, 0, 0) : corner_radii_none());
#else
	(void)v;
	(void)glass;
#endif
}

bool ui_frame_create(struct aro_view *v, struct wlr_scene_tree *parent)
{
	const struct q_theme *th = &v->server->cfg.theme;
	float col[4];

	v->frame_tree = wlr_scene_tree_create(parent);
	if (!v->frame_tree)
		return false;

#ifdef ARO_EFFECTS
	/* first, so it is under everything else in the frame */
	v->blur = wlr_scene_blur_create(v->frame_tree, 1, 1);
	if (!v->blur)
		return false;
	wlr_scene_node_set_enabled(&v->blur->node, false);
#endif

	ui_color(th->line, col);
	v->frame = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	ui_color(th->frame, col);
	v->bg = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	ui_color(th->frame, col);
	v->header = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	/* right above the header: icons and tabs go above it */
	v->gloss = wlr_scene_buffer_create(v->frame_tree, NULL);

	/* ring must be created before header occlusion */
	ui_color(th->accent_soft, col);
	v->ring = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	/* title above header */
	if (!qtext_init(&v->title, v->frame_tree, th->font))
		return false;

	v->content = wlr_scene_tree_create(v->frame_tree);

	/* popups above content */
	v->popups = wlr_scene_tree_create(v->frame_tree);

	if (!v->frame || !v->bg || !v->ring || !v->header || !v->gloss || !v->content ||
	    !v->popups)
		return false;
	gloss_sync(v);

	set_radius(v->frame, ui_frame_radius(v));
	set_radius(v->bg, inner_radius(v));

	wlr_scene_node_set_enabled(&v->ring->node, false);
	return true;
}

#ifdef ARO_EFFECTS
/* see-through: faded, or showing what is behind through window_background */
static bool frame_see_through(struct aro_view *v, bool focused)
{
	return frame_opacity(v, focused) < 1.0f || (!v->fullscreen && (frame_bg(v, focused) & 0xff) < 0xff) ||
	       (!v->fullscreen && frame_header(v) > 0 && header_alpha(v) < 1.0f);
}

/* the blur covers the frame, or the part of it that may be drawn */
static void place_blur(struct aro_view *v, ly_box vis, int radius)
{
	wlr_scene_node_set_position(&v->blur->node, vis.x, vis.y);
	wlr_scene_blur_set_size(v->blur, vis.w, vis.h);
	wlr_scene_blur_set_corner_radius(v->blur, radius);
}
#endif

/* colours and opacity for focus; blur behind when it can be seen */
static void frame_paint(struct aro_view *v, bool focused)
{
	const struct q_theme *th = &v->server->cfg.theme;
	const float a = frame_opacity(v, focused);
	float col[4];

	color_faded(focused ? th->accent : th->line, a, col);
	wlr_scene_rect_set_color(v->frame, col);
	color_faded(frame_bg(v, focused), a, col);
	wlr_scene_rect_set_color(v->bg, col);
	color_faded(focused ? th->frame_on : th->frame, a * header_alpha(v), col);
	wlr_scene_rect_set_color(v->header, col);
	wlr_scene_buffer_set_opacity(v->gloss, a);
	color_faded(th->accent_soft, a, col);
	wlr_scene_rect_set_color(v->ring, col);
	if (v->title.node)
		wlr_scene_buffer_set_opacity(v->title.node, a);
	if (v->icon)
		wlr_scene_buffer_set_opacity(v->icon, a);
	paint_content(v, a);
#ifdef ARO_EFFECTS
	wlr_scene_node_set_enabled(&v->blur->node,
		v->server->cfg.blur && frame_see_through(v, focused));
#endif
}

/* hide chrome in fullscreen */
void ui_frame_fullscreen(struct aro_view *v, bool fullscreen)
{
	wlr_scene_node_set_enabled(&v->frame->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->bg->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->header->node, !fullscreen);
	if (fullscreen)
		wlr_scene_node_set_enabled(&v->gloss->node, false);   /* geometry brings it back */
	qtext_show(&v->title, !fullscreen && !v->group);
	if (v->group && v->group->tabs && v->group->tabs->node.parent == v->frame_tree)
		wlr_scene_node_set_enabled(&v->group->tabs->node, !fullscreen);
	if (v->icon)
		wlr_scene_node_set_enabled(&v->icon->node, !fullscreen);
	wlr_scene_node_set_enabled(&v->ring->node, !fullscreen &&
	                           v->server->focused == v && ui_frame_border(v) > 0);
	frame_paint(v, v->server->focused == v);        /* square and solid in fullscreen */
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
#ifdef ARO_EFFECTS
		place_blur(v, (ly_box){ 0, 0, b.w, b.h }, 0);
#endif
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
#ifdef ARO_EFFECTS
	place_blur(v, border, ui_frame_radius(v));
#endif
	/* behind a see-through header is the blur, not the window background */
	const bool glass = hh > 0 && header_alpha(v) < 1.0f;
	const int under = glass ? (hh < inner_h ? hh : inner_h) : 0;
	round_header(v, glass);
	place_rect(v->bg, (ly_box){ bw, bw + under, inner_w, inner_h - under }, vis);
	place_rect(v->ring, (ly_box){ bw, bw, inner_w, 1 }, vis);

	wlr_scene_node_set_enabled(&v->header->node, hh > 0);
	qtext_show(&v->title, hh > 0 && !v->fullscreen && !cut && !v->group);
	if (v->group && v->group->tabs && v->group->tabs->node.parent == v->frame_tree)
		wlr_scene_node_set_enabled(&v->group->tabs->node, !v->fullscreen && !cut);
	if (v->icon) {
		/* the icon goes where the title goes */
		const int isz = icon_size(th);
		wlr_scene_node_set_enabled(&v->icon->node,
			isz && hh >= isz && !v->fullscreen && !cut && !v->group);
		wlr_scene_node_set_position(&v->icon->node, bw + th->text_pad, bw + (hh - isz) / 2);
	}
	const ly_box hwant = { bw, bw, inner_w, hh < inner_h ? hh : inner_h };
	place_gloss(v, hwant, place_rect(v->header, hwant, vis), hh > 0);

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

	frame_paint(v, focused);
	wlr_scene_node_set_enabled(&v->ring->node, focused && ui_frame_border(v) > 0);
	if (v->group && v->group->active == v)
		ui_frame_tabs(v);

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
			wlr_scene_node_place_above(&v->icon->node, &v->gloss->node);
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

static struct wlr_scene_rect *tab_rect(struct wlr_scene_tree *parent)
{
	float none[4] = { 0 };
	return wlr_scene_rect_create(parent, 1, 1, none);
}

/* the group's tabs across v's header: equal widths, a line between, a mark under the shown one */
static void tabs_layout(struct aro_view *v, int frame_w, float scale)
{
	struct aro_group *g = v->group;
	const struct q_theme *th = &v->server->cfg.theme;
	if (!g->tabs) {
		g->tabs = wlr_scene_tree_create(v->frame_tree);
		g->mark = g->tabs ? tab_rect(g->tabs) : NULL;
		if (!g->mark)
			return;
	} else if (g->tabs->node.parent != v->frame_tree) {
		wlr_scene_node_reparent(&g->tabs->node, v->frame_tree);
	}
	wlr_scene_node_place_above(&g->tabs->node, &v->gloss->node);
	while (g->nlabels < g->n) {
		struct wlr_scene_rect *sep = tab_rect(g->tabs);
		if (!sep || !qtext_init(&g->label[g->nlabels], g->tabs, th->font))
			return;
		g->sep[g->nlabels++] = sep;
	}

	const struct aro_server *s = v->server;
	const bool focused = s->focused && s->focused->group == g;
	const float a = frame_opacity(v, focused);
	const int bw = ui_frame_border(v), hh = frame_header(v);
	const int inner = frame_w - bw * 2 > 1 ? frame_w - bw * 2 : 1;
	float col[4];
	for (int i = 0; i < g->nlabels; i++) {
		struct qtext *l = &g->label[i];
		if (i >= g->n) {
			qtext_show(l, false);
			wlr_scene_node_set_enabled(&g->sep[i]->node, false);
			continue;
		}
		struct aro_view *m = g->v[i];
		const int x0 = bw + inner * i / g->n, x1 = bw + inner * (i + 1) / g->n;
		g->edge[i] = x0;
		g->edge[i + 1] = x1;
		if (l->font && strcmp(l->font, th->font))
			qtext_set_font(l, th->font);
		const char *title = view_title(m);
		const int avail = x1 - x0 - th->text_pad * 2;
		qtext_set(l, title ? title : "", m == g->active ? (focused ? th->accent : th->ink) : th->dim,
		          scale, avail > 1 ? avail : 1);
		qtext_move(l, x0 + th->text_pad, bw + (hh - l->h) / 2);
		qtext_show(l, true);
		if (l->node)
			wlr_scene_buffer_set_opacity(l->node, a);

		/* a line between tabs */
		wlr_scene_node_set_enabled(&g->sep[i]->node, i > 0);
		color_faded(th->line, a, col);
		wlr_scene_rect_set_color(g->sep[i], col);
		wlr_scene_node_set_position(&g->sep[i]->node, x0, bw + 4);
		wlr_scene_rect_set_size(g->sep[i], 1, hh > 8 ? hh - 8 : 1);

		if (m == g->active) {
			color_faded(focused ? th->accent : th->line, a, col);
			wlr_scene_rect_set_color(g->mark, col);
			wlr_scene_node_set_position(&g->mark->node, x0, bw + hh - 2);
			wlr_scene_rect_set_size(g->mark, x1 - x0, 2);
		}
	}
}

/* a layout point in v's header */
bool ui_frame_in_header(struct aro_view *v, double lx, double ly)
{
	if (v->fullscreen)
		return false;
	const ly_box b = aro_view_box(v);
	const int bw = ui_frame_border(v), hh = frame_header(v);
	return hh > 0 && lx >= b.x && lx < b.x + b.w && ly >= b.y + bw && ly < b.y + bw + hh;
}

/* the tab under a layout point in v's header; -1 if none */
int ui_frame_tab_at(struct aro_view *v, double lx, double ly)
{
	if (!v->group || v->fullscreen)
		return -1;
	const ly_box b = aro_view_box(v);
	const int bw = ui_frame_border(v), hh = frame_header(v);
	if (ly < b.y + bw || ly >= b.y + bw + hh)
		return -1;
	return group_tab_at(v, (int)(lx - b.x));
}

/* redraw the tabs in v's header, v being the shown tab */
void ui_frame_tabs(struct aro_view *v)
{
	if (v->group && v->group->active == v && v->output)
		tabs_layout(v, aro_view_box(v).w, v->output->scale);
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
	if (v->group) {
		/* the tabs live in the shown tab's header, whoever's title changed */
		if (v->group->active == v)
			tabs_layout(v, frame_w, scale);
		else
			ui_frame_tabs(v->group->active);
		return;
	}
	if (hh == 0)
		return;         /* no header to put a title in */

	bool focused = v->server->focused == v;
	qtext_set(&v->title, title ? title : "", focused ? th->accent : th->dim,
	          scale, avail);
	qtext_move(&v->title, title_x(v), ui_frame_border(v) + (hh - v->title.h) / 2);
}

/* ── snapshots ─────────────────────────────────────────────────────────── */

/* a copy of a window: a buffer per surface, subsurfaces too (Firefox draws its page in one) */
#define SNAP_PARTS 16

struct snap_part {
	struct wlr_scene_buffer *buf;
	struct wlr_fbox src;            /* in buffer pixels */
	double x, y, w, h;              /* from the window's top-left, logical */
};

struct ui_snap {
	struct wlr_scene_tree *tree;
	struct wl_listener destroy;
	int radius, n;
	double gw, gh;                  /* the window's size when copied */
	struct ui_snap **owner;         /* cleared when this copy goes */
	struct snap_part part[SNAP_PARTS];
};

static void snap_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct ui_snap *s = wl_container_of(l, s, destroy);
	if (s->owner)
		*s->owner = NULL;
	wl_list_remove(&s->destroy.link);
	free(s);
}

struct snap_walk {
	struct ui_snap *s;
	int i, gx, gy;
};

/* show buf in part i; false: no node for it */
static bool snap_part_set(struct ui_snap *s, int i, struct wlr_buffer *buf)
{
	struct snap_part *p = &s->part[i];
	if (!p->buf)
		p->buf = wlr_scene_buffer_create(s->tree, buf);
	else if (p->buf->buffer != buf)
		wlr_scene_buffer_set_buffer(p->buf, buf);
	return p->buf != NULL;
}

/* drop the parts past n, and any a walk made but did not count */
static void snap_trim(struct ui_snap *s, int n)
{
	for (int i = n; i < SNAP_PARTS && s->part[i].buf; i++) {
		wlr_scene_node_destroy(&s->part[i].buf->node);
		s->part[i].buf = NULL;
	}
	s->n = n;
}

/* make s the same picture as from */
static void snap_copy(struct ui_snap *s, const struct ui_snap *from)
{
	int n = 0;
	for (int i = 0; i < from->n; i++) {
		const struct snap_part *f = &from->part[i];
		if (!snap_part_set(s, n, f->buf->buffer))
			break;
		struct wlr_scene_buffer *b = s->part[n].buf;
		s->part[n] = *f;
		s->part[n++].buf = b;
	}
	snap_trim(s, n);
	s->gw = from->gw;
	s->gh = from->gh;
}

/* from the surfaces, not the scene: a clipped or hidden window's nodes are cut or off */
static void snap_collect(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct snap_walk *w = data;
	struct ui_snap *s = w->s;
	if (!surface->buffer || w->i == SNAP_PARTS ||
	    surface->current.width <= 0 || surface->current.height <= 0)
		return;
	if (!snap_part_set(s, w->i, &surface->buffer->base))
		return;
	struct snap_part *p = &s->part[w->i];
	wlr_surface_get_buffer_source_box(surface, &p->src);
	p->x = sx - w->gx;
	p->y = sy - w->gy;
	p->w = surface->current.width;
	p->h = surface->current.height;
	w->i++;
}

/* copy the window's surfaces as they are now */
void ui_snap_sync(struct ui_snap *s, struct aro_view *v)
{
	struct wlr_surface *surface = view_surface(v);
	if (!s || !surface)
		return;
	struct wlr_box g = { 0 };
	view_geometry(v, &g);
	struct snap_walk w = { s, 0, g.x, g.y };
	wlr_surface_for_each_surface(surface, snap_collect, &w);
	/* a hidden window may have let go of some buffers: show it as it was */
	const struct ui_snap *last = v->snap_last;
	if (last && last != s && w.i < last->n) {
		snap_copy(s, last);
		return;
	}
	if (w.i == 0)
		return;         /* nothing drawn yet: keep the last copy */
	snap_trim(s, w.i);
	/* no geometry: the main surface's size */
	s->gw = g.width > 0 ? g.width : s->part[0].w;
	s->gh = g.height > 0 ? g.height : s->part[0].h;
}

struct ui_snap *ui_snap_create(struct wlr_scene_tree *parent, struct aro_view *v,
                               int radius)
{
	struct ui_snap *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->tree = wlr_scene_tree_create(parent);
	if (!s->tree) {
		free(s);
		return NULL;
	}
	s->radius = radius;
	s->destroy.notify = snap_destroyed;
	wl_signal_add(&s->tree->node.events.destroy, &s->destroy);
	ui_snap_sync(s, v);
	if (s->n == 0) {
		wlr_scene_node_destroy(&s->tree->node);         /* frees s */
		return NULL;
	}
	return s;
}

/* remember how v looks before it is hidden */
void ui_snap_keep(struct aro_view *v)
{
	if (v->snap_last || !v->frame_tree)
		return;
	struct ui_snap *s = ui_snap_create(v->frame_tree, v, 0);
	if (!s)
		return;
	wlr_scene_node_set_enabled(&s->tree->node, false);
	s->owner = &v->snap_last;
	v->snap_last = s;
}

void ui_snap_forget(struct aro_view *v)
{
	if (v->snap_last)
		wlr_scene_node_destroy(&v->snap_last->tree->node);     /* clears snap_last */
}

struct wlr_scene_node *ui_snap_node(struct ui_snap *s)
{
	return &s->tree->node;
}

/* lay the copy out in box b, cut to clip (in the parent's coordinates) */
void ui_snap_place(struct ui_snap *s, ly_box b, ly_box clip)
{
	if (b.w < 1 || b.h < 1 || s->gw <= 0 || s->gh <= 0) {
		wlr_scene_node_set_enabled(&s->tree->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&s->tree->node, true);
	ly_box keep;
	if (!box_meet(b, clip, &keep)) {
		wlr_scene_node_set_enabled(&s->tree->node, false);
		return;
	}
	const double kx = b.w / s->gw, ky = b.h / s->gh;
	for (int i = 0; i < s->n; i++) {
		struct snap_part *p = &s->part[i];
		ly_box d = {
			b.x + (int)(p->x * kx + 0.5), b.y + (int)(p->y * ky + 0.5),
			(int)(p->w * kx + 0.5), (int)(p->h * ky + 0.5),
		};
		ly_box vis;
		if (d.w < 1 || d.h < 1 || !box_meet(d, keep, &vis)) {
			wlr_scene_node_set_enabled(&p->buf->node, false);
			continue;
		}
		struct wlr_fbox cut = {
			p->src.x + (double)(vis.x - d.x) / d.w * p->src.width,
			p->src.y + (double)(vis.y - d.y) / d.h * p->src.height,
			(double)vis.w / d.w * p->src.width,
			(double)vis.h / d.h * p->src.height,
		};
		wlr_scene_node_set_enabled(&p->buf->node, true);
		wlr_scene_buffer_set_source_box(p->buf, &cut);
		wlr_scene_buffer_set_dest_size(p->buf, vis.w, vis.h);
		wlr_scene_node_set_position(&p->buf->node, vis.x, vis.y);
#ifdef ARO_EFFECTS
		/* round what reaches the corners; a cut edge stays square */
		const bool whole = vis.x == b.x && vis.y == b.y && vis.w == b.w && vis.h == b.h;
		wlr_scene_buffer_set_corner_radius(p->buf, whole ? s->radius : 0);
#endif
	}
}

void ui_snap_opacity(struct ui_snap *s, float a)
{
	for (int i = 0; i < s->n; i++)
		wlr_scene_buffer_set_opacity(s->part[i].buf, a);
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
	gloss_sync(v);
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
