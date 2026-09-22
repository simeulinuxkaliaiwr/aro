/*
 * aro — prompt.c
 *
 * The card is built from a frame's own pieces — accent border, a 1px ring
 * just inside it, the focused frame background, the same radius — so it
 * reads as a frame that asked you something, not as a dialog from somewhere
 * else. The pills are the same construction again, smaller.
 *
 * API RISK: wlr_scene_* calls as used elsewhere in the project;
 * wlr_scene_node_raise_to_top is the only one not already used in notify.c
 * or ui.c.
 */
/* strdup: build trap 7. Before any include, including scene.h — the macro
 * has to be seen before the first system header, which scene.h pulls in. */
#define _POSIX_C_SOURCE 200809L

/* scene.h first: it decides which scene implementation the build uses,
 * and that only works if it is included before any wlroots header. */
#include "scene.h"

#include "prompt.h"
#include "aro.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* ── small helpers ─────────────────────────────────────────────────────── */

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

static int inner_radius(const struct q_theme *th)
{
	int r = th->radius - th->border;
	return r > 0 ? r : 0;
}

static struct wlr_scene_rect *rect(struct wlr_scene_tree *parent, uint32_t rgba)
{
	float c[4];
	ui_color(rgba, c);
	return wlr_scene_rect_create(parent, 1, 1, c);
}

static void rect_color(struct wlr_scene_rect *r, uint32_t rgba)
{
	float c[4];
	ui_color(rgba, c);
	wlr_scene_rect_set_color(r, c);
}

static int max_i(int a, int b)
{
	return a > b ? a : b;
}

static void schedule_all(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

static char *dup_or_empty(const char *s)
{
	return strdup(s ? s : "");
}

/* ── colours ───────────────────────────────────────────────────────────── */

/*
 * The primary pill carries the accent on its border and its label — it is
 * what Enter does, so it is the "focused" answer, and focus is the only
 * thing that gets colour. Hover lifts the background one step, from the
 * unfocused frame colour to the border colour: visible, and still grey.
 */
static void pill_colors(struct aro_server *s, struct prompt_pill *p,
                        bool hover)
{
	const struct q_theme *th = &s->cfg.theme;
	rect_color(p->edge, p->primary ? th->accent : th->line);
	rect_color(p->bg, hover ? th->line : th->frame);
}

/* ── layout ────────────────────────────────────────────────────────────── */

static void pill_place(const struct q_theme *th, struct prompt_pill *p)
{
	const int bw = th->border, px = th->text_pad;
	ly_box b = p->box;

	wlr_scene_node_set_position(&p->edge->node, b.x, b.y);
	wlr_scene_rect_set_size(p->edge, b.w, b.h);
	wlr_scene_node_set_position(&p->bg->node, b.x + bw, b.y + bw);
	wlr_scene_rect_set_size(p->bg, max_i(b.w - bw * 2, 1), max_i(b.h - bw * 2, 1));

	qtext_move(&p->label, b.x + px, b.y + (b.h - p->label.h) / 2);
	qtext_move(&p->hint, b.x + px + p->label.w + px,
	           b.y + (b.h - p->hint.h) / 2);
}

static int pill_width(const struct q_theme *th, const struct prompt_pill *p)
{
	/* pad, label, pad, hint, pad */
	return th->text_pad * 3 + p->label.w + p->hint.w;
}

/*
 * Everything is sized from the text, so the card fits its words at any font
 * and any scale rather than at the one they were tuned on.
 *
 *   ┌──────────────────────────────────┐
 *   │  Exit aro?                    │   title, font
 *   │  3 windows will close.           │   detail, font_small, dim
 *   │                                  │
 *   │             [ Cancel n ][ Exit y ]│   pills, right-aligned
 *   └──────────────────────────────────┘
 */
static void prompt_layout(struct aro_server *s)
{
	struct aro_prompt *p = &s->prompt;
	const struct q_theme *th = &s->cfg.theme;
	const int bw = th->border;
	const int pad = th->text_pad * 2;
	const int line_gap = th->text_pad / 2;
	const int pill_h = max_i(p->yes.label.h, p->yes.hint.h) + th->text_pad;

	const int yes_w = pill_width(th, &p->yes);
	const int no_w = pill_width(th, &p->no);
	const int row_w = no_w + th->gap + yes_w;

	int content_w = max_i(max_i(p->title_t.w, p->detail_t.w), row_w);
	content_w = max_i(content_w, 280);      /* a card, not a tooltip */

	const int w = bw * 2 + pad * 2 + content_w;
	const int h = bw * 2 + pad + p->title_t.h + line_gap + p->detail_t.h +
	              pad + pill_h + pad;

	ly_box ob = p->output->box;
	p->final = (ly_box){ ob.x + (ob.w - w) / 2, ob.y + (ob.h - h) / 2, w, h };

	qtext_move(&p->title_t, bw + pad, bw + pad);
	qtext_move(&p->detail_t, bw + pad, bw + pad + p->title_t.h + line_gap);

	const int row_y = h - bw - pad - pill_h;
	p->yes.box = (ly_box){ w - bw - pad - yes_w, row_y, yes_w, pill_h };
	p->no.box = (ly_box){ p->yes.box.x - th->gap - no_w, row_y, no_w, pill_h };
	pill_place(th, &p->yes);
	pill_place(th, &p->no);

	/* The card tree sits still at the final origin; tick moves the rects
	 * inside it, so the content never moves while the frame springs. */
	wlr_scene_node_set_position(&p->card->node, p->final.x, p->final.y);

	/* One scrim over the union of every output: the keyboard is taken
	 * everywhere, so the dimming should say so everywhere. */
	int x0 = ob.x, y0 = ob.y, x1 = ob.x + ob.w, y1 = ob.y + ob.h;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->box.x < x0) x0 = o->box.x;
		if (o->box.y < y0) y0 = o->box.y;
		if (o->box.x + o->box.w > x1) x1 = o->box.x + o->box.w;
		if (o->box.y + o->box.h > y1) y1 = o->box.y + o->box.h;
	}
	wlr_scene_node_set_position(&p->scrim->node, x0, y0);
	wlr_scene_rect_set_size(p->scrim, max_i(x1 - x0, 1), max_i(y1 - y0, 1));
}

/* ── building and tearing down ─────────────────────────────────────────── */

static void teardown_nodes(struct aro_prompt *p)
{
	/* qtext_finish before the tree goes, the order notify.c uses */
	if (p->tree) {
		qtext_finish(&p->title_t);
		qtext_finish(&p->detail_t);
		qtext_finish(&p->yes.label);
		qtext_finish(&p->yes.hint);
		qtext_finish(&p->no.label);
		qtext_finish(&p->no.hint);
		wlr_scene_node_destroy(&p->tree->node);
	}
	p->tree = NULL;
	p->scrim = p->edge = p->bg = p->ring = NULL;
	p->card = p->content = NULL;
	memset(&p->title_t, 0, sizeof p->title_t);
	memset(&p->detail_t, 0, sizeof p->detail_t);
	memset(&p->yes, 0, sizeof p->yes);
	memset(&p->no, 0, sizeof p->no);
}

static void free_strings(struct aro_prompt *p)
{
	free(p->title);
	free(p->detail);
	free(p->yes_label);
	free(p->no_label);
	p->title = p->detail = p->yes_label = p->no_label = NULL;
}

/*
 * Build the card from the strings already in *p. On failure everything
 * built so far is destroyed; like notify.c, a qtext that was initialised
 * but never set has nothing of its own to free, so none is finished here.
 */
static bool prompt_build(struct aro_server *s, bool animate)
{
	struct aro_prompt *p = &s->prompt;
	const struct q_theme *th = &s->cfg.theme;
	const float scale = p->output->scale;

	p->tree = wlr_scene_tree_create(s->l_notify);
	if (!p->tree)
		return false;

	/* toasts are siblings in the same layer and stack in creation order;
	 * one posted before the prompt must not sit on top of it */
	wlr_scene_node_raise_to_top(&p->tree->node);

	/* Creation order is stacking order: scrim, then the card over it;
	 * inside the card border, background, ring, then content on top. */
	p->scrim = rect(p->tree, animate ? (th->scrim & ~0xffu) : th->scrim);
	p->card = wlr_scene_tree_create(p->tree);
	if (!p->scrim || !p->card)
		goto fail;

	p->edge = rect(p->card, th->accent);
	p->bg = rect(p->card, th->frame_on);
	p->ring = rect(p->card, th->accent_soft);
	p->content = wlr_scene_tree_create(p->card);
	if (!p->edge || !p->bg || !p->ring || !p->content)
		goto fail;

	p->no.edge = rect(p->content, th->line);
	p->no.bg = rect(p->content, th->frame);
	p->yes.edge = rect(p->content, th->accent);
	p->yes.bg = rect(p->content, th->frame);
	if (!p->no.edge || !p->no.bg || !p->yes.edge || !p->yes.bg)
		goto fail;

	/* labels after their pill's rects, or the rects bury them */
	if (!qtext_init(&p->title_t, p->content, th->font) ||
	    !qtext_init(&p->detail_t, p->content, th->font_small) ||
	    !qtext_init(&p->no.label, p->content, th->font) ||
	    !qtext_init(&p->no.hint, p->content, th->font_small) ||
	    !qtext_init(&p->yes.label, p->content, th->font) ||
	    !qtext_init(&p->yes.hint, p->content, th->font_small))
		goto fail;

	p->yes.primary = true;
	p->no.primary = false;

	qtext_set(&p->title_t, p->title, th->ink, scale, 0);
	qtext_set(&p->detail_t, p->detail, th->dim, scale, 0);
	qtext_set(&p->yes.label, p->yes_label, th->accent, scale, 0);
	qtext_set(&p->yes.hint, "y", th->dim, scale, 0);
	qtext_set(&p->no.label, p->no_label, th->ink, scale, 0);
	qtext_set(&p->no.hint, "n", th->dim, scale, 0);

	set_radius(p->edge, th->radius);
	set_radius(p->bg, inner_radius(th));
	set_radius(p->yes.edge, th->radius);
	set_radius(p->yes.bg, inner_radius(th));
	set_radius(p->no.edge, th->radius);
	set_radius(p->no.bg, inner_radius(th));

	p->hover = 0;
	p->pressed = 0;
	pill_colors(s, &p->yes, false);
	pill_colors(s, &p->no, false);

	prompt_layout(s);

	const uint32_t now = aro_now_ms();
	const uint32_t fade = th->anim_focus_ms > 0 ? (uint32_t)th->anim_focus_ms : 1;
	if (animate) {
		/* grow from open_scale around the centre, as a new frame does */
		const double os = th->open_scale;
		const ly_box f = p->final;
		anim_box_set(&p->geo, (ly_box){
			f.x + (int)(f.w * (1 - os) / 2),
			f.y + (int)(f.h * (1 - os) / 2),
			max_i((int)(f.w * os), 1),
			max_i((int)(f.h * os), 1),
		});
		anim_box_to(&p->geo, f, now, th->anim_ms, &(anim_ease){
			TH_EASE_X1, TH_EASE_Y1, TH_EASE_X2, TH_EASE_Y2 });
		p->content_shown = false;
		p->scrim_full = false;
		p->opened_ms = now;
	} else {
		anim_box_set(&p->geo, p->final);
		p->content_shown = true;
		p->scrim_full = true;
		p->opened_ms = now - fade;
	}
	wlr_scene_node_set_enabled(&p->content->node, p->content_shown);

	p->active = true;
	schedule_all(s);
	return true;

fail:
	wlr_scene_node_destroy(&p->tree->node);
	p->tree = NULL;         /* teardown_nodes must not finish the qtexts */
	teardown_nodes(p);
	return false;
}

bool prompt_open(struct aro_server *s, struct aro_output *o,
                 const char *title, const char *detail,
                 const char *yes_label, const char *no_label,
                 void (*on_yes)(struct aro_server *s))
{
	struct aro_prompt *p = &s->prompt;
	if (p->active || !o || !s->l_notify)
		return false;

	p->title = dup_or_empty(title);
	p->detail = dup_or_empty(detail);
	p->yes_label = dup_or_empty(yes_label);
	p->no_label = dup_or_empty(no_label);
	if (!p->title || !p->detail || !p->yes_label || !p->no_label) {
		free_strings(p);
		return false;
	}
	p->output = o;
	p->on_yes = on_yes;

	if (!prompt_build(s, true)) {
		wlr_log(WLR_ERROR, "could not build the prompt");
		free_strings(p);
		p->output = NULL;
		p->on_yes = NULL;
		return false;
	}
	return true;
}

void prompt_close(struct aro_server *s)
{
	struct aro_prompt *p = &s->prompt;
	if (!p->active)
		return;
	teardown_nodes(p);
	free_strings(p);
	p->active = false;
	p->output = NULL;
	p->on_yes = NULL;
	schedule_all(s);
}

bool prompt_active(struct aro_server *s)
{
	return s->prompt.active;
}

/* Close first, THEN act: the callback may well end the process, and a
 * callback that opens another prompt must find this one gone. */
static void answer_yes(struct aro_server *s)
{
	void (*cb)(struct aro_server *) = s->prompt.on_yes;
	prompt_close(s);
	if (cb)
		cb(s);
}

/* ── input ─────────────────────────────────────────────────────────────── */

void prompt_key(struct aro_server *s, xkb_keysym_t sym)
{
	if (!s->prompt.active)
		return;

	switch (sym) {
	case XKB_KEY_y:
	case XKB_KEY_Y:
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		answer_yes(s);
		break;
	case XKB_KEY_n:
	case XKB_KEY_N:
	case XKB_KEY_Escape:
		prompt_close(s);
		break;
	default:
		break;          /* swallowed: the card is modal */
	}
}

static bool in_box(ly_box b, double x, double y)
{
	return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

/* 1 yes, 2 no, 0 neither — in layout coordinates, against where the pills
 * settle rather than where the springing card happens to be */
static int pill_at(struct aro_prompt *p, double lx, double ly)
{
	double x = lx - p->final.x, y = ly - p->final.y;
	if (!p->content_shown)
		return 0;
	if (in_box(p->yes.box, x, y))
		return 1;
	if (in_box(p->no.box, x, y))
		return 2;
	return 0;
}

void prompt_pointer_motion(struct aro_server *s, double lx, double ly)
{
	struct aro_prompt *p = &s->prompt;
	if (!p->active)
		return;

	int at = pill_at(p, lx, ly);
	if (at == p->hover)
		return;
	p->hover = at;
	pill_colors(s, &p->yes, at == 1);
	pill_colors(s, &p->no, at == 2);
	schedule_all(s);
}

/*
 * Answers happen on RELEASE, over the same thing the press landed on —
 * the usual button contract: press, change your mind, drag off, let go,
 * nothing happens. That includes the scrim: acting on its press would close
 * the card and hand the release to whatever window is underneath, a release
 * with no press that the client never asked for.
 */
void prompt_pointer_button(struct aro_server *s, double lx, double ly,
                           bool pressed)
{
	struct aro_prompt *p = &s->prompt;
	if (!p->active)
		return;

	int at = pill_at(p, lx, ly);
	bool on_card = in_box(p->final, lx, ly);

	if (pressed) {
		p->pressed = at ? at : (on_card ? 0 : -1);
		return;
	}

	int was = p->pressed;
	p->pressed = 0;
	if (was == 1 && at == 1)
		answer_yes(s);
	else if (was == 2 && at == 2)
		prompt_close(s);
	else if (was == -1 && !on_card)
		prompt_close(s);        /* clicked away: the same as Escape */
}

/* ── animation ─────────────────────────────────────────────────────────── */

bool prompt_tick(struct aro_server *s, uint32_t now)
{
	struct aro_prompt *p = &s->prompt;
	if (!p->active)
		return false;

	const struct q_theme *th = &s->cfg.theme;
	const int bw = th->border;
	bool moving = anim_box_tick(&p->geo, now);

	ly_box b = p->geo.cur;
	const int ox = b.x - p->final.x, oy = b.y - p->final.y;
	const int iw = max_i(b.w - bw * 2, 1), ih = max_i(b.h - bw * 2, 1);

	wlr_scene_node_set_position(&p->edge->node, ox, oy);
	wlr_scene_rect_set_size(p->edge, max_i(b.w, 1), max_i(b.h, 1));
	wlr_scene_node_set_position(&p->bg->node, ox + bw, oy + bw);
	wlr_scene_rect_set_size(p->bg, iw, ih);
	wlr_scene_node_set_position(&p->ring->node, ox + bw, oy + bw);
	wlr_scene_rect_set_size(p->ring, iw, 1);

	/* The words appear once the frame has grown past them — before that
	 * they would hang outside a card still too small to hold them. */
	if (!p->content_shown && b.w >= p->final.w && b.h >= p->final.h) {
		p->content_shown = true;
		wlr_scene_node_set_enabled(&p->content->node, true);
	}

	if (!p->scrim_full) {
		const uint32_t fade =
			th->anim_focus_ms > 0 ? (uint32_t)th->anim_focus_ms : 1;
		const uint32_t el = now - p->opened_ms;
		const uint32_t a_full = th->scrim & 0xffu;
		if (el >= fade) {
			rect_color(p->scrim, th->scrim);
			p->scrim_full = true;
		} else {
			uint32_t a = a_full * el / fade;
			rect_color(p->scrim, (th->scrim & ~0xffu) | a);
			moving = true;
		}
	}

	/* A spring that settled undershooting by a pixel would otherwise
	 * never show the content at all. */
	if (!moving && !p->content_shown) {
		p->content_shown = true;
		wlr_scene_node_set_enabled(&p->content->node, true);
	}
	return moving;
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

/*
 * Rebuilt rather than recoloured: fonts can change size, and a qtext
 * borrows its font pointer from the config that a reload just freed.
 */
void prompt_retheme(struct aro_server *s)
{
	struct aro_prompt *p = &s->prompt;
	if (!p->active)
		return;

	teardown_nodes(p);
	p->active = false;
	if (!prompt_build(s, false)) {
		/* no card, but still modal would be the worst of both */
		free_strings(p);
		p->output = NULL;
		p->on_yes = NULL;
		schedule_all(s);
	}
}

void prompt_output_gone(struct aro_server *s, struct aro_output *o)
{
	if (s->prompt.active && s->prompt.output == o)
		prompt_close(s);
}

void prompt_finish(struct aro_server *s)
{
	prompt_close(s);
}
