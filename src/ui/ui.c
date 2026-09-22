/*
 * aro — ui.c
 *
 * The only file that knows what a frame looks like. Everything else moves
 * rectangles around; this turns a rectangle into the splits look.
 *
 * Frame anatomy, outside in:
 *
 *   frame   the border, `border` thick. Focus lives here.
 *   bg      inset by the border, the frame background.
 *   ring    a 1px hairline just inside the border, accent at low alpha.
 *           This is the inner glow from the start page and it is what stops
 *           the border reading as a flat outline.
 *   header  a `header_height` strip at the top of bg.
 *   content the client's surface tree, below the header.
 *
 * API RISK: every wlr_scene_* call below is written against wlroots 0.19 from
 * memory and has not been compiled. The scenefx calls under ARO_EFFECTS
 * are the least certain of all — check them against scenefx's headers before
 * trusting this file.
 */
/* scene.h first: it decides which scene implementation the build uses,
 * and that only works if it is included before any wlroots header. */
#include "scene.h"

#include "aro.h"
#include "text.h"
#include "theme.h"

#include <stdlib.h>


#include <wlr/util/box.h>

void ui_color(uint32_t rgba, float out[4])
{
	float a = (float)(rgba & 0xff) / 255.0f;
	/* wlroots wants premultiplied alpha */
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

/*
 * Punch the interior out of the border rect so it is a true ring rather than
 * a filled rectangle sitting behind everything else. Matters the moment any
 * part of the frame stops being opaque, and it saves the overdraw regardless.
 */
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

/*
 * The client's own buffers are square, so without this their bottom corners
 * poke out of our rounded frame. Only the bottom two: the header strip
 * covers the top edge.
 */
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

	/* AFTER the header: scene siblings stack in creation order, and an
	 * opaque header strip drawn later would bury this hairline. */
	ui_color(th->accent_soft, col);
	v->ring = wlr_scene_rect_create(v->frame_tree, 1, 1, col);

	/* the title sits above the header strip, below the client */
	if (!qtext_init(&v->title, v->frame_tree, th->font))
		return false;

	v->content = wlr_scene_tree_create(v->frame_tree);

	/* after content so menus draw over the client, and outside it so the
	 * content clip does not cut them off at the frame edge */
	v->popups = wlr_scene_tree_create(v->frame_tree);

	if (!v->frame || !v->bg || !v->ring || !v->header || !v->content ||
	    !v->popups)
		return false;

	set_radius(v->frame, th->radius);
	set_radius(v->bg, th->radius - th->border > 0 ? th->radius - th->border : 0);

	wlr_scene_node_set_enabled(&v->ring->node, false);
	return true;
}

/*
 * Fullscreen is the absence of the frame. The client asked for the whole
 * screen and a border, a header strip and a rounded corner are all lies about
 * how much of it we gave them — so the chrome is disabled rather than resized
 * to nothing, and ui_frame_geometry stops insetting the content.
 */
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
	/* square corners: a rounded edge against the screen edge shows desktop */
	int radius = fullscreen ? 0 : th->radius - th->border;
	if (radius < 0)
		radius = 0;
	wlr_scene_node_for_each_buffer(&v->content->node, round_buffer, &radius);
#endif
}

/*
 * The client's box inside a frame box, in layout coordinates.
 *
 * The single place that knows how much of a frame is chrome — border on every
 * side, and a header strip unless the client draws its own and header = auto.
 * aro.c needs the same answer to size floats and to configure one
 * outside the per-frame pass; asking here keeps it from growing a second copy
 * of the header rule, which it once did and got wrong.
 */
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

	/* header = auto drops ours when the client draws its own */
	const bool no_header =
		v->csd && v->server->cfg.header == Q_HEADER_AUTO;
	const int hh = no_header ? 0 : th->header_h;

	/*
	 * A degenerate box means this frame has no geometry yet: anim_box is
	 * zero-initialised, and output_frame draws every mapped view whether
	 * or not arrange has reached it. Configuring from it sends the client
	 * a 0x0 size, which in xdg-shell means "you decide" — the client then
	 * commits 0x0 geometry and wlroots warns about it. Nothing useful can
	 * be drawn from this box either way, so leave the frame alone until
	 * there is a real one.
	 */
	if (b.w <= 0 || b.h <= 0)
		return;

	/*
	 * Where the window proper begins inside its surface. Non-zero for a
	 * GTK client-side-decorated window, whose surface carries invisible
	 * shadow margins around the part you can actually see.
	 *
	 * This is NOT used to position anything: wlr_scene_xdg_surface_create()
	 * already places the tree so the geometry origin sits at the node
	 * origin. Offsetting again moves the window right back out of its own
	 * frame by the width of the shadow. It is only needed for the clip
	 * box, which is in surface coordinates and therefore still starts at
	 * the geometry origin.
	 */
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

	/* the hairline sits on the inside edge of the border */
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

	/* popups are positioned in the client's coordinate space, so this
	 * tracks content exactly — it just is not clipped to it */
	wlr_scene_node_set_position(&v->popups->node, bw, bw + hh);

	/*
	 * Clip the client to the content box.
	 *
	 * The client answers set_size on its own schedule, so mid-resize its
	 * surface is still the size it was. Growing is covered by the frame
	 * drawing its own background under the gap; shrinking is the opposite
	 * problem — there is MORE surface than frame, and without a clip the
	 * overhang draws straight over the window next door. On a terminal
	 * that reads as the text duplicating, because you are seeing the
	 * neighbour and the overhang at the same time.
	 *
	 * API RISK: wlr_scene_subsurface_tree_set_clip, not compiled. NULL is
	 * meant to clear the clip.
	 */
	/*
	 * Clip the CLIENT'S tree, not the wrapper around it.
	 *
	 * wlr_scene_subsurface_tree_set_clip() walks a surface tree; handed
	 * the plain tree that contains one, it finds no surfaces and does
	 * nothing at all — silently. That is why a GTK window's shadow was
	 * still being drawn outside the frame and reading as a fat border.
	 *
	 * The box is in surface coordinates, so it starts at the geometry
	 * origin rather than at zero.
	 */
	if (v->surface_tree)
		wlr_scene_subsurface_tree_set_clip(&v->surface_tree->node,
		                                   &(struct wlr_box){ g.x, g.y, cw, ch });

	/* Reposition only. Re-rendering the title here would mean a fresh pango
	 * pass on every animation frame — ui_frame_title() is called from
	 * arrange instead, where the TARGET width is known and stable. */
	qtext_move(&v->title, bw + th->text_pad, bw + (hh - v->title.h) / 2);

	/* Ask the client to match. It answers on its own schedule, so during an
	 * animation the surface lags the frame — that is why the frame has its
	 * own background: the gap shows frame colour, not desktop. */
	/* the content box in layout coordinates: X11 needs where, not just how big */
	/*
	 * ...unless it is a float that now sizes itself. Configuring it here,
	 * every frame, is what pinned file pickers at half the screen: the
	 * client could never settle on the size it wanted because we restated
	 * ours sixty times a second. aro.c told it once; its commits move
	 * the frame now (view_float_follow), and dragging an edge hands the
	 * decision back by clearing the flag.
	 */
	/* floating is checked too, so a code path that flips a window back
	 * into the tree without clearing the flag cannot leave a tiled window
	 * that is never configured again */
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

	/* The title carries focus too. Re-read it from the shell rather than
	 * from the qtext's own buffer — belt and braces after the aliasing
	 * bug, and the client may have changed it since. */
	const char *title = view_title(v);
	qtext_set(&v->title, title ? title : "", focused ? th->accent : th->dim,
	          v->title.scale, v->title.max_w);
}

/* Called from arrange with the frame's target width, so the ellipsis is
 * computed against where the frame is going, not where it currently is. */
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
/*
 * Built from the same two pieces as a frame — a hairline rect with a wash
 * inside it — so a slot being previewed reads as a frame that has not arrived
 * yet rather than as a different kind of object. Accent, because it is the
 * thing you are currently pointing at, and focus is the only thing that gets
 * colour.
 */
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
	/* hollow out the hairline, exactly as the frame border does */
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

/*
 * Re-apply everything the theme decides, after a config reload.
 *
 * Sizes are left to the next arrange — they come from the tree. This is the
 * part arrange cannot do: colours, corner radii and the font, none of which
 * are recomputed on a geometry pass.
 */
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
