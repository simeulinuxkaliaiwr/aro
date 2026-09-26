/* switcher.c: mod+tab window switcher and the recently-used order */
/* scene.h must come first */
#include "scene.h"

#include "switcher.h"
#include "aro.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/*
 * Behaviour, from niri's documentation of its Recent Windows switcher
 * (docs only; niri is GPL and none of its code is used here):
 *   - most-recently-used order over every window, on every workspace;
 *   - focus is committed to that order only after a debounce, so windows
 *     passed through on the way somewhere do not count; typing into a
 *     window commits it at once;
 *   - the card appears after a short delay, so a quick mod+tab tap just
 *     swaps windows without anything flashing up;
 *   - previews are static: each window's last frame, never live.
 */

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

static void rect_place(struct wlr_scene_rect *r, int x, int y, int w, int h)
{
	wlr_scene_node_set_position(&r->node, x, y);
	wlr_scene_rect_set_size(r, w > 1 ? w : 1, h > 1 ? h : 1);
}

static int max_i(int a, int b) { return a > b ? a : b; }
static int min_i(int a, int b) { return a < b ? a : b; }

static void schedule_all(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

static bool listed(struct aro_view *v)
{
	return v->mapped && v->output;  /* parked windows have no screen */
}

/* ── recently used ─────────────────────────────────────────────────────── */

static void mru_front(struct aro_server *s, struct aro_view *v)
{
	if (!v || !v->mapped)
		return;
	wl_list_remove(&v->mru_link);
	wl_list_insert(&s->switcher.mru, &v->mru_link);
}

void mru_add(struct aro_server *s, struct aro_view *v)
{
	/* at the back: a window nobody has used yet */
	wl_list_remove(&v->mru_link);
	wl_list_insert(s->switcher.mru.prev, &v->mru_link);
}

static void switcher_view_gone(struct aro_server *s, struct aro_view *v);

void mru_remove(struct aro_server *s, struct aro_view *v)
{
	wl_list_remove(&v->mru_link);
	wl_list_init(&v->mru_link);
	if (s->switcher.pending == v)
		s->switcher.pending = NULL;
	switcher_view_gone(s, v);
}

void mru_touch(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	if (!w->pending)
		return;
	mru_front(s, w->pending);
	w->pending = NULL;
	if (w->debounce)
		wl_event_source_timer_update(w->debounce, 0);
}

static int debounce_done(void *data)
{
	mru_touch(data);
	return 0;
}

void mru_focus(struct aro_server *s, struct aro_view *v)
{
	struct aro_switcher *w = &s->switcher;
	if (!v || !v->mapped) {
		w->pending = NULL;
		return;
	}
	if (w->mru.next == &v->mru_link) {
		w->pending = NULL;              /* already the most recent */
		return;
	}
	w->pending = v;
	int ms = s->cfg.theme.switch_debounce_ms;
	if (ms <= 0 || !w->debounce)
		mru_touch(s);
	else
		wl_event_source_timer_update(w->debounce, ms);
}

/* ── the card ──────────────────────────────────────────────────────────── */

static void tiles_free(struct aro_switcher *w)
{
	if (w->tiles) {
		for (int i = 0; i < w->count; i++)
			qtext_finish(&w->tiles[i].title);
		free(w->tiles);
	}
	w->tiles = NULL;
	w->count = 0;
	if (w->tree)
		wlr_scene_node_destroy(&w->tree->node);
	w->tree = NULL;
	w->edge = w->bg = NULL;
}

static void tile_colors(struct aro_server *s, struct switcher_tile *t,
                        struct aro_view *v, bool selected)
{
	const struct q_theme *th = &s->cfg.theme;
	rect_color(t->edge, selected ? th->accent : th->line);
	rect_color(t->bg, selected ? th->frame_on : th->frame);
	wlr_scene_node_set_enabled(&t->ring->node, selected);

	const char *title = view_title(v);
	qtext_set(&t->title, title ? title : "", selected ? th->accent : th->dim,
	          t->title.scale, t->title.max_w);
}

/* width a preview wants at height h: the window's own shape, within reason */
static int preview_width(struct aro_view *v, int h)
{
	struct wlr_box g = { 0 };
	view_geometry(v, &g);
	double aspect = g.width > 0 && g.height > 0
	              ? (double)g.width / g.height : 16.0 / 10.0;
	if (aspect < 0.5)
		aspect = 0.5;
	if (aspect > 2.2)
		aspect = 2.2;
	return max_i((int)(h * aspect), 1);
}

/*
 * Which items fit: all of them if they can, shrunk to at most 60%; past
 * that, a run of neighbours around the selection.
 */
static void choose_range(struct aro_server *s, int max_row, int *ph,
                         int *first, int *count)
{
	struct aro_switcher *w = &s->switcher;
	const struct q_theme *th = &s->cfg.theme;
	const int bw = th->border, gap = th->gap;

	int total = 0;
	for (int i = 0; i < w->n; i++)
		total += preview_width(w->items[i], *ph) + bw * 2 + (i ? gap : 0);
	if (total > max_row) {
		int shrunk = (int)((double)*ph * max_row / total);
		*ph = max_i(shrunk, (*ph * 6) / 10);
	}

	int lo = w->sel, hi = w->sel;
	int used = preview_width(w->items[w->sel], *ph) + bw * 2;
	for (bool grew = true; grew;) {
		grew = false;
		if (hi + 1 < w->n) {
			int add = gap + preview_width(w->items[hi + 1], *ph) + bw * 2;
			if (used + add <= max_row) {
				used += add;
				hi++;
				grew = true;
			}
		}
		if (lo > 0) {
			int add = gap + preview_width(w->items[lo - 1], *ph) + bw * 2;
			if (used + add <= max_row) {
				used += add;
				lo--;
				grew = true;
			}
		}
	}
	*first = lo;
	*count = hi - lo + 1;
}

static bool card_build(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	const struct q_theme *th = &s->cfg.theme;
	struct aro_output *o = w->output;
	if (!o)
		return false;

	const int bw = th->border, pad = th->text_pad * 2, gap = th->gap;
	const int title_gap = th->text_pad / 2;
	const float scale = o->scale;
	const ly_box ob = o->box;

	/* half the screen at most, as in niri's max-scale */
	int ph = min_i(th->switch_preview_h, ob.h / 2);
	int max_row = ob.w * 9 / 10 - 2 * (bw + pad);
	choose_range(s, max_row, &ph, &w->first, &w->count);

	w->tree = wlr_scene_tree_create(s->l_notify);
	if (!w->tree)
		return false;
	wlr_scene_node_raise_to_top(&w->tree->node);

	/* the card is a frame that is not focused: grey border, the accent
	 * belongs to the selected window only */
	w->edge = rect(w->tree, th->line);
	w->bg = rect(w->tree, th->frame);
	w->tiles = calloc(w->count, sizeof *w->tiles);
	if (!w->edge || !w->bg || !w->tiles) {
		tiles_free(w);
		return false;
	}
	set_radius(w->edge, th->radius);
	set_radius(w->bg, inner_radius(th));

	int x = bw + pad, title_h = 0;
	for (int i = 0; i < w->count; i++) {
		struct switcher_tile *t = &w->tiles[i];
		struct aro_view *v = w->items[w->first + i];
		const int pw = preview_width(v, ph);

		t->box = (ly_box){ x, bw + pad, pw + bw * 2, ph + bw * 2 };
		t->edge = rect(w->tree, th->line);
		t->bg = rect(w->tree, th->frame);
		if (!t->edge || !t->bg) {
			tiles_free(w);
			return false;
		}
		set_radius(t->edge, th->radius);
		set_radius(t->bg, inner_radius(th));

		t->preview = ui_snapshot_create(w->tree, v, pw, ph, inner_radius(th));
		t->ring = rect(w->tree, th->accent_soft);   /* over the preview */
		if (!t->ring || !qtext_init(&t->title, w->tree, th->font_small)) {
			tiles_free(w);
			return false;
		}
		qtext_set(&t->title, "", th->dim, scale, t->box.w);

		rect_place(t->edge, t->box.x, t->box.y, t->box.w, t->box.h);
		rect_place(t->bg, t->box.x + bw, t->box.y + bw, pw, ph);
		rect_place(t->ring, t->box.x + bw, t->box.y + bw, pw, 1);
		if (t->preview)
			wlr_scene_node_set_position(&t->preview->node,
			                            t->box.x + bw, t->box.y + bw);

		tile_colors(s, t, v, w->first + i == w->sel);
		title_h = max_i(title_h, t->title.h);
		x += t->box.w + gap;
	}
	for (int i = 0; i < w->count; i++) {
		struct switcher_tile *t = &w->tiles[i];
		qtext_move(&t->title, t->box.x, t->box.y + t->box.h + title_gap);
	}

	const int cw = x - gap + pad + bw;
	const int ch = bw + pad + ph + bw * 2 + title_gap + title_h + pad + bw;
	rect_place(w->edge, 0, 0, cw, ch);
	rect_place(w->bg, bw, bw, cw - bw * 2, ch - bw * 2);
	wlr_scene_node_set_position(&w->tree->node,
	                            ob.x + (ob.w - cw) / 2, ob.y + (ob.h - ch) / 2);

	schedule_all(s);
	return true;
}

static void card_rebuild(struct aro_server *s)
{
	tiles_free(&s->switcher);
	if (!card_build(s))
		wlr_log(WLR_ERROR, "could not build the window switcher");
}

static void card_select(struct aro_server *s, int old)
{
	struct aro_switcher *w = &s->switcher;
	if (!w->tree)
		return;
	if (w->sel < w->first || w->sel >= w->first + w->count) {
		card_rebuild(s);                /* scrolled past the edge */
		return;
	}
	if (old >= w->first && old < w->first + w->count)
		tile_colors(s, &w->tiles[old - w->first], w->items[old], false);
	tile_colors(s, &w->tiles[w->sel - w->first], w->items[w->sel], true);
	schedule_all(s);
}

static int open_delay_done(void *data)
{
	struct aro_server *s = data;
	if (s->switcher.active && !s->switcher.tree)
		card_rebuild(s);
	return 0;
}

/* ── open, step, close ─────────────────────────────────────────────────── */

static void switcher_close(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	if (!w->active)
		return;
	tiles_free(w);
	free(w->items);
	w->items = NULL;
	w->n = w->sel = 0;
	w->output = NULL;
	w->active = false;
	if (w->open_timer)
		wl_event_source_timer_update(w->open_timer, 0);
	schedule_all(s);
}

static void switcher_commit(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	struct aro_view *v = w->active && w->sel < w->n ? w->items[w->sel] : NULL;
	switcher_close(s);
	if (v && listed(v)) {
		view_raise_and_focus(s, v);
		mru_touch(s);                   /* a deliberate choice: no debounce */
	}
}

/* the focused window first, then everything else in recently-used order */
static bool collect(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	int cap = wl_list_length(&w->mru) + 1;
	w->items = calloc(cap, sizeof *w->items);
	if (!w->items)
		return false;
	w->n = 0;

	if (s->focused && listed(s->focused))
		w->items[w->n++] = s->focused;
	struct aro_view *v;
	wl_list_for_each(v, &w->mru, mru_link) {
		if (v != s->focused && listed(v) && w->n < cap)
			w->items[w->n++] = v;
	}
	if (w->n < 2) {
		free(w->items);
		w->items = NULL;
		w->n = 0;
		return false;
	}
	return true;
}

void switcher_step(struct aro_server *s, int dir)
{
	struct aro_switcher *w = &s->switcher;
	if (aro_locked(s) || prompt_active(s) || overview_shown(s))
		return;

	if (!w->active) {
		if (!collect(s))
			return;
		w->active = true;
		w->sel = dir < 0 ? w->n - 1 : 1;
		w->output = aro_focused_output(s);
		int ms = s->cfg.theme.switch_delay_ms;
		if (ms <= 0 || !w->open_timer)
			card_rebuild(s);
		else
			wl_event_source_timer_update(w->open_timer, ms);
		return;
	}

	int old = w->sel;
	w->sel = ((w->sel + (dir < 0 ? -1 : 1)) % w->n + w->n) % w->n;
	card_select(s, old);
}

bool switcher_active(struct aro_server *s)
{
	return s->switcher.active;
}

void switcher_key(struct aro_server *s, xkb_keysym_t sym)
{
	if (!s->switcher.active)
		return;
	switch (sym) {
	case XKB_KEY_Escape:
		switcher_close(s);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		switcher_commit(s);
		break;
	case XKB_KEY_Left:
	case XKB_KEY_h:
		switcher_step(s, -1);
		break;
	case XKB_KEY_Right:
	case XKB_KEY_l:
		switcher_step(s, 1);
		break;
	default:
		break;                          /* swallowed while open */
	}
}

void switcher_modifiers(struct aro_server *s, uint32_t mods)
{
	if (s->switcher.active && !(mods & s->cfg.modkey))
		switcher_commit(s);
}

/* a listed window unmapped while the switcher is open */
static void switcher_view_gone(struct aro_server *s, struct aro_view *v)
{
	struct aro_switcher *w = &s->switcher;
	if (!w->active)
		return;

	int at = -1;
	for (int i = 0; i < w->n; i++)
		if (w->items[i] == v)
			at = i;
	if (at < 0)
		return;

	memmove(&w->items[at], &w->items[at + 1],
	        (size_t)(w->n - at - 1) * sizeof *w->items);
	w->n--;
	if (w->n < 2) {
		switcher_close(s);
		return;
	}
	if (at < w->sel || w->sel >= w->n)
		w->sel = w->sel > 0 ? w->sel - 1 : 0;
	if (w->tree)
		card_rebuild(s);
}

void switcher_output_gone(struct aro_server *s, struct aro_output *o)
{
	if (s->switcher.active && s->switcher.output == o)
		switcher_close(s);
}

/* fonts are borrowed from the config a reload just freed: rebuild */
void switcher_retheme(struct aro_server *s)
{
	if (s->switcher.tree)
		card_rebuild(s);
}

void switcher_init(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	wl_list_init(&w->mru);
	w->debounce = wl_event_loop_add_timer(s->loop, debounce_done, s);
	w->open_timer = wl_event_loop_add_timer(s->loop, open_delay_done, s);
}

void switcher_finish(struct aro_server *s)
{
	struct aro_switcher *w = &s->switcher;
	switcher_close(s);
	if (w->debounce)
		wl_event_source_remove(w->debounce);
	if (w->open_timer)
		wl_event_source_remove(w->open_timer);
	w->debounce = w->open_timer = NULL;
}
