/* arrange.c: where windows go: tiling, monocle, the scroll strip, workspace slides */

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "config.h"
#include "aro.h"
#include "core.h"
#include "idle.h"
#include "ipc.h"

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>

/* tiling area after exclusive zones and bar */
ly_box usable_area(struct aro_output *o)
{
	ly_box u = o->usable;
	if (u.w <= 0 || u.h <= 0)
		u = o->box;
	u.h -= bar_height(&o->bar);
	return u;
}

ly_box aro_output_usable(struct aro_output *o)
{
	return usable_area(o);
}

enum q_layout ws_layout(struct aro_server *s, struct aro_output *o,
                               int ws)
{
	if (o && ws >= 0 && ws < ARO_MAX_WS &&
	    o->ws_layout[ws] != Q_LAYOUT_INHERIT)
		return (enum q_layout)o->ws_layout[ws];
	return config_ws_layout(&s->cfg, ws);
}

bool ws_monocle(struct aro_server *s, struct aro_output *o, int ws)
{
	return o && ws_layout(s, o, ws) == Q_LAYOUT_MONOCLE;
}

/* what a lone tiled window gets: the usable area inside the outer gap */
static ly_box monocle_box(struct aro_output *o)
{
	ly_box b = usable_area(o);
	int g = o->server->cfg.theme.outer_gap;
	b.x += g;
	b.y += g;
	b.w = b.w > 2 * g ? b.w - 2 * g : 1;
	b.h = b.h > 2 * g ? b.h - 2 * g : 1;
	return b;
}

/* layout = scroll: side-by-side splits make columns on a strip, stacked splits share one */
static bool node_under(ly_node *x, ly_node *anc)
{
	for (; x; x = x->parent)
		if (x == anc)
			return true;
	return false;
}

/* the column a leaf lives in: go down through side-by-side splits only */
static ly_node *scroll_column(ly_node *root, ly_node *leaf)
{
	ly_node *n = root;
	while (n && n->kind == LY_SPLIT && n->dir == LY_ROW)
		n = node_under(leaf, n->a) ? n->a : n->b;
	return n;
}

static int scroll_columns(ly_node *n, ly_node **out, int max, int count)
{
	if (!n || count >= max)
		return count;
	if (n->kind == LY_SPLIT && n->dir == LY_ROW) {
		count = scroll_columns(n->a, out, max, count);
		return scroll_columns(n->b, out, max, count);
	}
	out[count] = n;
	return count + 1;
}

/* the windows of a column, top to bottom, sharing its height by the tree's ratios */
static void scroll_stack(ly_node *n, ly_box b, int gap)
{
	if (n->kind != LY_SPLIT) {
		n->box = b;
		return;
	}
	int room = b.h - gap;
	int ha = (int)(room * n->ratio + 0.5);
	ha = ha < 1 ? 1 : ha > room - 1 ? room - 1 : ha;
	scroll_stack(n->a, (ly_box){ b.x, b.y, b.w, ha }, gap);
	scroll_stack(n->b, (ly_box){ b.x, b.y + ha + gap, b.w, room - ha }, gap);
}

static void scroll_place(struct aro_server *s, struct aro_output *o, int ws)
{
	ly_node *cols[256];
	int n = scroll_columns(o->ws[ws], cols, 256, 0);
	const int g = s->cfg.theme.outer_gap, gap = s->cfg.theme.gap;
	ly_box u = usable_area(o);
	ly_box view = { u.x + g, u.y + g, u.w - 2 * g, u.h - 2 * g };
	if (n == 0 || view.w < 1 || view.h < 1)
		return;

	int x[256], w[256], f = -1, end = 0;
	for (int i = 0; i < n; i++) {
		/* a column is as wide as its widest window asked to be */
		ly_node *leaves[256];
		int nl = ly_collect(cols[i], leaves, 256);
		double share = 0;
		for (int k = 0; k < nl; k++) {
			struct aro_view *v = leaves[k]->user;
			if (v && v->scroll_w > share)
				share = v->scroll_w;
			if (v && v == s->focused)
				f = i;
		}
		w[i] = (int)((share > 0 ? share : s->cfg.scroll_width) * view.w + 0.5);
		if (w[i] < s->cfg.theme.min)
			w[i] = s->cfg.theme.min;
		if (w[i] > view.w)
			w[i] = view.w;
		x[i] = end;
		end += w[i] + gap;
	}
	const int total = end - gap;

	/* move just enough to show the focused column, its neighbours peeking in */
	double off = o->scroll[ws];
	if (f >= 0) {
		int peek = s->cfg.scroll_peek;
		int left = f > 0 ? peek + gap : 0;
		int right = f < n - 1 ? peek + gap : 0;
		if (w[f] + left + right > view.w)
			left = right = 0;       /* no room to peek beside a wide column */
		if (x[f] + w[f] + right > off + view.w)
			off = x[f] + w[f] + right - view.w;
		if (x[f] - left < off)
			off = x[f] - left;      /* last: a wide column shows its left edge */
	}
	double max = total > view.w ? total - view.w : 0;
	off = off > max ? max : off < 0 ? 0 : off;
	o->scroll[ws] = off;

	for (int i = 0; i < n; i++)
		scroll_stack(cols[i], (ly_box){ view.x + x[i] - (int)off, view.y, w[i], view.h }, gap);
}

/* the width share of v's column: its widest window's */
double column_share(struct aro_server *s, struct aro_view *v)
{
	ly_node *leaves[256];
	int n = ly_collect(scroll_column(v->output->ws[v->workspace], v->node), leaves, 256);
	double share = 0;
	for (int i = 0; i < n; i++) {
		struct aro_view *lv = leaves[i]->user;
		if (lv && lv->scroll_w > share)
			share = lv->scroll_w;
	}
	return share > 0 ? share : s->cfg.scroll_width;
}

void column_set_share(struct aro_view *v, double share, double prev)
{
	ly_node *leaves[256];
	int n = ly_collect(scroll_column(v->output->ws[v->workspace], v->node), leaves, 256);
	for (int i = 0; i < n; i++) {
		struct aro_view *lv = leaves[i]->user;
		if (lv) {
			lv->scroll_w = share;
			lv->scroll_w_prev = prev;
		}
	}
}

/* for a drop or a new window beside v on a scroll workspace: its whole column */
ly_node *scroll_beside(struct aro_server *s, struct aro_output *o, int ws, ly_node *leaf)
{
	if (!leaf || ws_layout(s, o, ws) != Q_LAYOUT_SCROLL)
		return leaf;
	return scroll_column(o->ws[ws], leaf);
}

/* a drop beside a window on a strip lands beside its column: preview that */
ly_box drop_target_box(struct aro_server *s, struct aro_view *t, ly_edge e)
{
	ly_box b = t->node->box;
	if ((e == LY_LEFT || e == LY_RIGHT) && ws_layout(s, t->output, t->workspace) == Q_LAYOUT_SCROLL) {
		ly_node *col = scroll_column(t->output->ws[t->workspace], t->node);
		ly_node *first = ly_first_leaf(col), *last = ly_last_leaf(col);
		int y1 = last->box.y + last->box.h;
		b = (ly_box){ first->box.x, first->box.y, first->box.w, y1 - first->box.y };
	}
	return b;
}

bool aro_same_column(struct aro_view *a, struct aro_view *b)
{
	if (!a->node || !b->node || a->output != b->output || a->workspace != b->workspace ||
	    ws_layout(a->server, a->output, a->workspace) != Q_LAYOUT_SCROLL)
		return a == b;
	ly_node *root = a->output->ws[a->workspace];
	return scroll_column(root, a->node) == scroll_column(root, b->node);
}

/* lay out one workspace's tree, then the scroll strip if that is its layout */
void ws_arrange(struct aro_server *s, struct aro_output *o, int ws)
{
	if (!o || ws < 0 || ws >= ARO_MAX_WS || !o->ws[ws])
		return;
	ly_arrange(o->ws[ws], usable_area(o), &(ly_metrics){
		.gap = s->cfg.theme.gap, .outer_gap = s->cfg.theme.outer_gap,
		.min = s->cfg.theme.min });
	if (ws_layout(s, o, ws) == Q_LAYOUT_SCROLL)
		scroll_place(s, o, ws);
}

bool aro_view_clipped(struct aro_view *v)
{
	return v->node && !v->fullscreen && v->output &&
	       ws_layout(v->server, v->output, v->workspace) == Q_LAYOUT_SCROLL;
}

/* final target geometry */
ly_box view_target(struct aro_view *v)
{
	if (v->fullscreen)
		return v->output ? v->output->box : (ly_box){ 0, 0, 1, 1 };
	if (v->floating)
		return v->fbox;
	if (v->node && ws_monocle(v->server, v->output, v->workspace))
		return monocle_box(v->output);
	if (v->node)
		return v->node->box;
	return (ly_box){ 0, 0, 1, 1 };
}

/* workspace root; parked views use orphan root */
ly_node **view_ws_root(struct aro_view *v)
{
	if (v->output)
		return &v->output->ws[v->workspace];
	return &v->server->orphan_ws[v->workspace];
}

/* runtime override, else config */
/* insert into a workspace tree; forced dir beats dwindle */
ly_node *tree_insert(struct aro_server *s, struct aro_output *o,
                            int ws, struct aro_view *v, ly_node *target,
                            ly_dir dir, bool forced)
{
	ly_node **root = &o->ws[ws];
	ly_node *leaf;

	if (!*root) {
		leaf = *root = ly_leaf(v);
	} else {
		bool dwindle = ws_layout(s, o, ws) == Q_LAYOUT_DWINDLE;
		bool scroll = ws_layout(s, o, ws) == Q_LAYOUT_SCROLL;
		/* dwindle continues the spiral; a scroll strip grows at its end */
		if (!target)
			target = dwindle || scroll ? ly_last_leaf(*root) : ly_first_leaf(*root);
		if (dwindle && !forced)
			dir = target->box.w > target->box.h ? LY_ROW : LY_COL;
		/* side by side on a strip is a new column: beside the whole column */
		if (dir == LY_ROW)
			target = scroll_beside(s, o, ws, target);
		leaf = ly_split(root, target, dir, v);
	}
	/* hidden trees too, so dwindle reads real boxes */
	if (leaf)
		ws_arrange(s, o, ws);
	return leaf;
}

bool box_eq(ly_box a, ly_box b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* retarget only when the target changed */
static void view_retarget(anim_box *g, ly_box t, uint32_t now, uint32_t dur,
                          const anim_ease *ease)
{
	if (box_eq(g->active ? g->to : g->cur, t))
		return;
	anim_box_to(g, t, now, dur, ease);
}

/* mapped, on an output, and on its current workspace */
bool view_visible(struct aro_view *v)
{
	return v->mapped && v->output && v->workspace == v->output->cur_ws;
}

/* ── workspace slide ───────────────────────────────────────────────────── */
/*
 * Drawing only. view_visible() stays the truth for focus, arrange, rules,
 * idle and hit testing: a window on the outgoing workspace is drawn while
 * it leaves, and is otherwise already gone.
 */

/* on the workspace sliding out of this output */
static bool view_leaving(struct aro_view *v)
{
	struct aro_output *o = v->output;
	return v->mapped && o && o->slide.active &&
	       v->workspace == o->slide.out_ws && v->workspace != o->cur_ws;
}

/* drawn this frame: visible, or on its way out */
bool view_on_screen(struct aro_view *v)
{
	return view_visible(v) || view_leaving(v);
}

/* the outgoing workspace's offset now; the incoming one is this + span */
static int slide_offset(const struct aro_output *o, uint32_t now)
{
	uint32_t el = now - o->slide.start_ms;
	double t = el >= o->slide.dur_ms ? 1.0
		: anim_ease_eval(&FLAT, (double)el / (double)o->slide.dur_ms);
	double off = o->slide.from + (o->slide.to - o->slide.from) * t;
	return (int)(off + (off >= 0 ? 0.5 : -0.5));
}

/* where to draw a window this frame: geo.cur, plus the slide */
ly_box view_draw_box(struct aro_view *v, uint32_t now)
{
	ly_box b = v->geo.cur;
	struct aro_output *o = v->output;
	if (!o || !o->slide.active || v->sticky)
		return b;               /* a sticky window is on both workspaces: it stays put */

	int off;
	if (view_visible(v))
		off = slide_offset(o, now) + o->slide.span;
	else if (view_leaving(v))
		off = slide_offset(o, now);
	else
		return b;

	if (o->slide.vertical)
		b.y += off;
	else
		b.x += off;
	return b;
}

/* end a slide now: the outgoing workspace is hidden, the incoming one
 * is drawn where it belongs on the next frame */
void slide_finish(struct aro_output *o)
{
	if (!o->slide.active)
		return;
	o->slide.active = false;

	struct aro_view *v;
	wl_list_for_each(v, &o->server->views, link) {
		if (v->output == o && v->workspace != o->cur_ws)
			wlr_scene_node_set_enabled(&v->frame_tree->node, false);
	}
	wlr_output_schedule_frame(o->wlr_output);
}

/* finish every slide whose time is up; an output that is not drawing
 * (DPMS) would otherwise never finish its own */
void slides_reap(struct aro_server *s, uint32_t now)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->slide.active && now - o->slide.start_ms >= o->slide.dur_ms)
			slide_finish(o);
	}
}

bool slides_active(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		if (o->slide.active)
			return true;
	return false;
}

/*
 * Begin (or redirect) a slide from workspace old to ws. Called before
 * cur_ws changes. A higher number comes in from the right, or from below.
 * If a slide is already running, the workspace that was coming in is
 * where it is now, and that is where the new slide starts from, so
 * reversing mid-way retraces instead of jumping.
 */
static void slide_start(struct aro_output *o, int old, int ws)
{
	struct aro_server *s = o->server;
	const bool vertical = s->cfg.ws_slide == Q_SLIDE_VERTICAL;
	const uint32_t now = aro_now_ms();

	if (s->cfg.ws_slide == Q_SLIDE_OFF || s->cfg.theme.ws_slide_ms <= 0 ||
	    !o->wlr_output->enabled || overview_shown(s)) {
		o->slide.active = false;
		return;
	}

	int base = 0;           /* where old is drawn right now */
	if (o->slide.active && o->slide.vertical == vertical &&
	    now - o->slide.start_ms < o->slide.dur_ms)
		base = slide_offset(o, now) + o->slide.span;

	const int size = vertical ? o->box.h : o->box.w;
	const int sgn = ws > old ? 1 : -1;

	o->slide.active = true;
	o->slide.out_ws = old;
	o->slide.vertical = vertical;
	o->slide.span = sgn * size;
	o->slide.from = base;
	o->slide.to = -sgn * size;
	o->slide.start_ms = now;
	o->slide.dur_ms = (uint32_t)s->cfg.theme.ws_slide_ms;
}

/* monocle draws one tiled window: the focused one, else whichever was shown */
void monocle_sync(struct aro_server *s, struct aro_output *o)
{
	if (!o)
		return;
	const int ws = o->cur_ws;
	const bool mono = ws_monocle(s, o, ws);
	struct aro_view *top = NULL, *v;

	if (mono) {
		struct aro_view *f = s->focused;
		if (f && f->node && f->output == o && f->workspace == ws)
			top = f;
		else
			wl_list_for_each(v, &s->views, link)
				if (v->mapped && v->node && v->output == o &&
				    v->workspace == ws && v->frame_tree->node.enabled) {
					top = v;
					break;
				}
		if (!top && o->ws[ws]) {
			ly_node *first = ly_first_leaf(o->ws[ws]);
			top = first ? first->user : NULL;
		}
	}

	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped || !v->node || v->output != o || v->workspace != ws)
			continue;
		wlr_scene_node_set_enabled(&v->frame_tree->node,
		                           !mono || v == top || v->fullscreen);
	}
}

void aro_arrange(struct aro_server *s)
{
	uint32_t now = aro_now_ms();
	slides_reap(s, now);
	ipc_notify(s);          /* whatever moved, subscribers hear once aro is idle */

	/* arrange each output's current workspace */
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		ws_arrange(s, o, o->cur_ws);
		monocle_sync(s, o);
	}

	/* retarget visible views */
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_visible(v))
			continue;

		/* don't fight an active drag */
		if (s->grabbed == v && s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
			continue;

		ly_box t = view_target(v);

		/* fullscreen uses flat easing */
		if (v->fullscreen)
			view_retarget(&v->geo, t, now, s->cfg.theme.anim_fs_ms, &FLAT);
		else
			view_retarget(&v->geo, t, now, s->cfg.theme.anim_ms, &SPRING);

		if (!v->fullscreen)
			ui_frame_title(v, t.w, v->output->scale);
	}

	/* bar came or went: place, do not spring */
	wl_list_for_each(o, &s->outputs, link) {
		if (!o->bar_snap)
			continue;
		o->bar_snap = false;
		wl_list_for_each(v, &s->views, link) {
			if (v->output != o || !view_visible(v))
				continue;
			if (s->grabbed == v && s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
				continue;
			anim_box_set(&v->geo, view_target(v));
		}
	}

	wl_list_for_each(o, &s->outputs, link) {
		if (o->bar.tree)
			bar_update(&o->bar, o);
		wlr_output_schedule_frame(o->wlr_output);
	}

	/* recheck idle inhibitors */
	idle_update(s);

	/* taskbars see outputs and fullscreen changes; arrange follows all of them */
	struct aro_view *fv;
	wl_list_for_each(fv, &s->views, link)
		ftl_sync_view(fv);
	extws_sync(s);
}

void view_set_visible(struct aro_view *v, bool visible)
{
	wlr_scene_node_set_enabled(&v->frame_tree->node, visible);
}

/* show a workspace on the focused output */
void workspace_show(struct aro_server *s, int ws)
{
	struct aro_output *o = aro_focused_output(s);
	if (!o || ws < 0 || ws >= ARO_MAX_WS || ws == o->cur_ws)
		return;

	ghost_drop(s, o);       /* it would not slide with its workspace */

	/* the old workspace stays drawn while it slides out; anything left
	 * over from an earlier slide that is neither of these two is hidden */
	slide_start(o, o->cur_ws, ws);
	o->cur_ws = ws;

	/* sticky windows come along */
	struct aro_view *sv;
	wl_list_for_each(sv, &s->views, link)
		if (sv->sticky && sv->output == o)
			sv->workspace = ws;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->output != o)
			continue;
		if (v->workspace == ws)
			view_set_visible(v, true);
		else if (!o->slide.active || v->workspace != o->slide.out_ws)
			view_set_visible(v, false);
		/* sliding out keeps its state: hidden monocle windows stay hidden */
	}

	ly_node *root = o->ws[ws];

	/* place incoming windows before drawing */
	if (root) {
		ws_arrange(s, o, ws);
		ly_node *leaves[256];
		int n = ly_collect(root, leaves, 256);
		for (int i = 0; i < n; i++) {
			struct aro_view *iv = leaves[i]->user;
			if (iv && iv->mapped)
				anim_box_set(&iv->geo, view_target(iv));
		}
	}

	/* snap floating windows too */
	struct aro_view *fv;
	wl_list_for_each(fv, &s->views, link) {
		if (fv->mapped && fv->output == o && fv->workspace == ws &&
		    (fv->floating || fv->fullscreen))
			anim_box_set(&fv->geo, view_target(fv));
	}

	struct aro_view *next = NULL;
	/* a scroll strip comes back to the column you left, not its first */
	if (root && ws_layout(s, o, ws) == Q_LAYOUT_SCROLL) {
		struct aro_view *m;
		wl_list_for_each(m, &s->switcher.mru, mru_link)
			if (m->mapped && m->node && m->output == o && m->workspace == ws) {
				next = m;
				break;
			}
	}
	if (!next && root) {
		ly_node *first = ly_first_leaf(root);
		next = first ? first->user : NULL;
	}
	if (!next) {
		/* fallback focus for floating-only workspace */
		struct aro_view *cand;
		wl_list_for_each(cand, &s->views, link) {
			if (cand->mapped && cand->output == o && cand->workspace == ws) {
				next = cand;
				break;
			}
		}
	}
	aro_focus(s, next);
	aro_arrange(s);
}

void view_send_to(struct aro_server *s, struct aro_view *v, int ws)
{
	if (!v || ws < 0 || ws >= ARO_MAX_WS || ws == v->workspace)
		return;
	v->sticky = false;              /* sent to one workspace on purpose */

	ly_node *next = NULL;

	if (v->floating) {
		/* floating views are not in the tree */
		v->workspace = ws;
	} else {
		if (!v->node)
			return;
		next = ly_close(&v->output->ws[v->workspace], v->node);
		v->node = NULL;
		v->workspace = ws;

		v->node = tree_insert(s, v->output, ws, v, NULL, LY_ROW, false);
		if (!v->node)
			return;
	}

	view_set_visible(v, false);
	if (s->focused == v) {
		s->focused = NULL;
		aro_focus(s, next ? next->user : NULL);
	}
	aro_arrange(s);
}
