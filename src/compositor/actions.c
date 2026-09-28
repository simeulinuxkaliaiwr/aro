/* actions.c: what binds do, from quitting to switching layouts */

/* scene.h must come first */
#include "scene.h"

#include "config.h"
#include "aro.h"
#include "core.h"

#include <stdio.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* ── quitting ──────────────────────────────────────────────────────────── */

static void quit_now(struct aro_server *s)
{
	wl_display_terminate(s->display);
}

/* ask before quitting */
static void quit_ask(struct aro_server *s)
{
	if (prompt_active(s))
		return;

	struct aro_output *o = aro_focused_output(s);
	if (!o) {
		quit_now(s);            /* nowhere to ask: nothing to lose either */
		return;
	}

	/* end drag before prompt */
	if (s->cursor_mode != ARO_CURSOR_PASSTHROUGH)
		grab_end(s);

	int n = 0;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped)
			n++;

	char detail[64];
	if (n == 0)
		snprintf(detail, sizeof detail, "Nothing is open.");
	else
		snprintf(detail, sizeof detail, "%d open window%s will close.",
		         n, n == 1 ? "" : "s");

	/* don't quit if prompt fails */
	if (!prompt_open(s, o, "Exit aro?", detail, "Exit", "Cancel",
	                 quit_now)) {
		notify(s, NOTIFY_INFO, "could not show the exit prompt; "
		       "set confirm_quit = false to quit without it");
		return;
	}

	/* clear pointer focus under prompt */
	wlr_seat_pointer_clear_focus(s->seat);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
}

/* restore pointer focus after prompt */
void prompt_after(struct aro_server *s, bool was_active)
{
	if (was_active && !prompt_active(s))
		pointer_motion_common(s, aro_now_ms());
}

/*
 * mod+hjkl and friends on a floating window. Floating windows live
 * outside the tree, so they get their own versions:
 *   focus   the nearest floating window that way on this workspace, by the
 *           tree's own spatial rule (ly_pick); none that way does nothing,
 *           since jumping screens from a dialog would surprise
 *   move    nudge by resize_step of the usable area, kept on screen
 *   resize  grow (right, down) or shrink (left, up) the far edges
 * Reaching the tiled windows from here is mod+tab's job.
 */
static void float_directional(struct aro_server *s, struct aro_view *f,
                              enum q_action action, ly_edge e)
{
	struct aro_output *o = f->output;
	const struct q_theme *th = &s->cfg.theme;

	if (action == Q_FOCUS) {
		struct aro_view *cand[64];
		ly_box boxes[64];
		int n = 0;
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link) {
			if (n == 64)
				break;
			if (v == f || !v->floating || v->fullscreen || !view_visible(v) ||
			    v->output != o)
				continue;
			cand[n] = v;
			boxes[n++] = view_target(v);
		}
		int at = ly_pick(boxes, n, view_target(f), e);
		if (at >= 0) {
			aro_focus(s, cand[at]);
			cursor_warp_to_view(s, cand[at]);
		}
		return;
	}

	ly_box u = usable_area(o);
	int sx = (int)(u.w * th->resize_step + 0.5), sy = (int)(u.h * th->resize_step + 0.5);
	if (sx < 1)
		sx = 1;
	if (sy < 1)
		sy = 1;
	int dx = e == LY_LEFT ? -sx : e == LY_RIGHT ? sx : 0;
	int dy = e == LY_UP ? -sy : e == LY_DOWN ? sy : 0;
	ly_box b = f->fbox;

	if (action == Q_MOVE) {
		b.x += dx;
		b.y += dy;
		/* stay on screen: flush against an edge, not past it */
		if (b.x + b.w > u.x + u.w)
			b.x = u.x + u.w - b.w;
		if (b.y + b.h > u.y + u.h)
			b.y = u.y + u.h - b.h;
		if (b.x < u.x)
			b.x = u.x;
		if (b.y < u.y)
			b.y = u.y;
	} else {
		b.w += dx;
		b.h += dy;
		if (b.w < th->float_min_w)
			b.w = th->float_min_w;
		if (b.h < th->float_min_h)
			b.h = th->float_min_h;
		if (b.x + b.w > u.x + u.w)
			b.w = u.x + u.w - b.x > th->float_min_w ? u.x + u.w - b.x : b.w;
		if (b.y + b.h > u.y + u.h)
			b.h = u.y + u.h - b.y > th->float_min_h ? u.y + u.h - b.y : b.h;
		/* we chose a size, so the client stops choosing it: the same
		 * hand-over as dragging an edge */
		f->float_follow = false;
	}

	if (b.x == f->fbox.x && b.y == f->fbox.y &&
	    b.w == f->fbox.w && b.h == f->fbox.h)
		return;
	f->fbox = b;
	aro_arrange(s);
}

static bool ws_empty(struct aro_server *s, struct aro_output *o, int ws)
{
	if (o->ws[ws])
		return false;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && !v->stashed && v->output == o && v->workspace == ws)
			return false;
	return true;
}

/* the current workspace, windows and layout, to the monitor in direction e */
/* f, as a tab, into the tile that way */
static void group_toward(struct aro_server *s, struct aro_view *f, ly_edge e)
{
	if (!f || !f->node || f->fullscreen || !f->output)
		return;
	ly_node *t = ly_focus(f->output->ws[f->workspace], f->node, e);
	struct aro_view *into = t && t != f->node ? t->user : NULL;
	if (!into || (into->group && into->group->n == ARO_GROUP_MAX))
		return;
	view_detach(f);
	if (!group_join(s, f, into))
		f->node = tree_insert(s, f->output, f->workspace, f, NULL, LY_ROW, false);
	aro_focus(s, f);
	aro_arrange(s);
}

/* f out of its group, into a tile beside it */
static void ungroup(struct aro_server *s, struct aro_view *f)
{
	if (!f || !f->group || !f->node || f->fullscreen)
		return;
	ly_node *leaf = view_detach(f);
	f->node = tree_insert(s, f->output, f->workspace, f, leaf, LY_ROW, true);
	view_set_visible(f, f->workspace == f->output->cur_ws);
	aro_focus(s, f);
	aro_arrange(s);
}

static void workspace_move_to_output(struct aro_server *s, ly_edge e)
{
	struct aro_output *o = aro_focused_output(s);
	struct aro_output *dest = o ? output_toward(s, o, e) : NULL;
	if (!dest)
		return;

	/* the same number there if it is free, else the first empty one */
	const int from = o->cur_ws;
	int to = ws_empty(s, dest, from) ? from : -1;
	for (int i = 0; to < 0 && i < ARO_MAX_WS; i++)
		if (ws_empty(s, dest, i))
			to = i;
	if (to < 0)
		return;

	dest->ws[to] = o->ws[from];
	o->ws[from] = NULL;
	dest->ws_layout[to] = o->ws_layout[from];
	o->ws_layout[from] = Q_LAYOUT_INHERIT;
	dest->scroll[to] = o->scroll[from];
	o->scroll[from] = 0;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->output != o || v->workspace != from || v->sticky || v->stashed)
			continue;
		v->output = dest;
		v->workspace = to;
		if (v->floating) {
			v->fbox.x += dest->box.x - o->box.x;
			v->fbox.y += dest->box.y - o->box.y;
		}
	}

	struct aro_view *keep = s->focused;
	s->focused_output = dest;
	if (dest->cur_ws != to) {
		workspace_show(s, to);
	} else {
		wl_list_for_each(v, &s->views, link)
			if (v->output == dest && v->workspace == to)
				view_set_visible(v, true);
	}
	if (keep && keep->output == dest)
		aro_focus(s, keep);
	aro_arrange(s);
}

/* set current workspace layout; matching config clears override */
static void layout_set(struct aro_server *s, int want)
{
	struct aro_output *o = aro_focused_output(s);
	if (!o)
		return;
	const int ws = o->cur_ws;
	enum q_layout cur = ws_layout(s, o, ws);
	/* toggle cycles manual, dwindle, monocle, scroll */
	enum q_layout next = want == Q_LAYOUT_TOGGLE
	                   ? (cur == Q_LAYOUT_MANUAL  ? Q_LAYOUT_DWINDLE
	                   :  cur == Q_LAYOUT_DWINDLE ? Q_LAYOUT_MONOCLE
	                   :  cur == Q_LAYOUT_MONOCLE ? Q_LAYOUT_SCROLL
	                   :                            Q_LAYOUT_MANUAL)
	                   : (enum q_layout)want;

	o->ws_layout[ws] = next == config_ws_layout(&s->cfg, ws)
	                 ? Q_LAYOUT_INHERIT : (int)next;
	wlr_log(WLR_INFO, "layout: workspace %d on %s is %s", ws + 1,
	        o->wlr_output->name, config_layout_name(next));
	if (next != cur) {
		notify(s, NOTIFY_INFO, "Workspace %d: %s", ws + 1,
		       config_layout_name(next));
		aro_arrange(s);         /* monocle and scroll place windows differently */
	}
}

/* monocle focus: h/k previous, j/l next, in tree order; NULL at the ends */
static ly_node *monocle_step(ly_node *root, ly_node *from, ly_edge e)
{
	ly_node *leaves[256];
	int n = ly_collect(root, leaves, 256);
	int step = e == LY_LEFT || e == LY_UP ? -1 : 1;
	for (int i = 0; i < n; i++)
		if (leaves[i] == from)
			return i + step >= 0 && i + step < n ? leaves[i + step] : NULL;
	return NULL;
}

void run_action(struct aro_server *s, const struct q_bind *b)
{
	struct aro_view *f = s->focused;

	switch (b->action) {
	case Q_SPAWN:
		config_spawn(b->arg);
		return;
	case Q_CLOSE:
		if (f)
			view_close(f);
		return;
	case Q_QUIT:
		if (s->cfg.confirm_quit)
			quit_ask(s);
		else
			quit_now(s);
		return;
	case Q_FLOAT:
		if (f)
			view_set_floating(s, f, !f->floating);
		return;
	case Q_FULLSCREEN:
		if (f)
			view_set_fullscreen(s, f, !f->fullscreen);
		return;
	case Q_SWITCH:
		switcher_step(s, b->num);
		return;
	case Q_SPLIT:
		/* one-shot split */
		s->pending_split = (ly_dir)b->num;
		s->split_forced = true;
		return;
	case Q_LAYOUT:
		layout_set(s, b->num);
		return;
	case Q_OVERVIEW:
		if (s->cursor_mode == ARO_CURSOR_PASSTHROUGH)
			overview_toggle(s);
		return;
	case Q_MOVE_WS:
		workspace_move_to_output(s, (ly_edge)b->num);
		return;
	case Q_SCRATCH:
		scratch_toggle(s, f);
		return;
	case Q_GROUP:
		group_toward(s, f, (ly_edge)b->num);
		return;
	case Q_UNGROUP:
		ungroup(s, f);
		return;
	case Q_TAB:
		if (f && f->group)
			aro_focus(s, group_step(f, b->num));
		return;
	case Q_SCRATCH_SHOW:
		scratch_show(s, b->arg);
		return;
	case Q_WALLPAPERS:
		if (s->cursor_mode == ARO_CURSOR_PASSTHROUGH)
			picker_toggle(s);
		return;
	case Q_STICKY:
		if (f && !f->fullscreen) {
			if (!f->sticky)
				view_set_floating(s, f, true);
			f->sticky = !f->sticky;
			f->scratch = false;     /* a sticky window is always shown */
			wlr_log(WLR_INFO, "sticky: %s %s", view_app_id(f) ? view_app_id(f) : "?",
			        f->sticky ? "on" : "off");
		}
		return;
	case Q_MAXIMIZE:
		if (f && f->node && f->output && !f->fullscreen &&
		    ws_layout(s, f->output, f->workspace) == Q_LAYOUT_SCROLL) {
			const double share = column_share(s, f);
			if (share < 1.0)
				column_set_share(f, 1.0, share);
			else
				column_set_share(f, f->scroll_w_prev > 0 ? f->scroll_w_prev : s->cfg.scroll_width, 0);
			aro_arrange(s);
		}
		return;
	case Q_WIDTH:
		if (f && f->node && f->output && !f->fullscreen &&
		    ws_layout(s, f->output, f->workspace) == Q_LAYOUT_SCROLL &&
		    s->cfg.nscroll_presets > 0) {
			/* the next preset past the current width, wrapping round */
			const double *p = s->cfg.scroll_presets;
			const int n = s->cfg.nscroll_presets;
			const double cur = column_share(s, f);
			double to = b->num > 0 ? p[0] : p[n - 1];
			for (int i = 0; i < n; i++) {
				int k = b->num > 0 ? i : n - 1 - i;
				if (b->num > 0 ? p[k] > cur + 0.01 : p[k] < cur - 0.01) {
					to = p[k];
					break;
				}
			}
			column_set_share(f, to, 0);
			aro_arrange(s);
		}
		return;
	case Q_WORKSPACE:
		workspace_show(s, b->num);
		return;
	case Q_SENDTO:
		view_send_to(s, f, b->num);
		view_raise_and_focus(s, f);     /* follow it there */
		return;
	case Q_FOCUS:
	case Q_MOVE:
	case Q_RESIZE:
		break;                                  /* below */
	case Q_NONE:
		return;
	}

	/* the directional three; a floating window has its own */
	if (f && f->floating && !f->fullscreen && f->output) {
		float_directional(s, f, b->action, (ly_edge)b->num);
		return;
	}
	if (!f || !f->node || !f->output)
		return;

	ly_edge e = (ly_edge)b->num;
	struct aro_output *o = f->output;
	const bool mono = ws_monocle(s, o, o->cur_ws);
	const bool scroll = ws_layout(s, o, o->cur_ws) == Q_LAYOUT_SCROLL;
	const bool across = e == LY_LEFT || e == LY_RIGHT;

	if (b->action == Q_RESIZE) {
		if (mono)
			return;         /* would move boundaries nobody can see */
		if (scroll && across) {
			/* the whole column gets wider or narrower; up and down split it as usual */
			double share = column_share(s, f) +
			        (e == LY_RIGHT ? s->cfg.theme.resize_step : -s->cfg.theme.resize_step);
			share = share < 0.1 ? 0.1 : share > 1.0 ? 1.0 : share;
			column_set_share(f, share, 0);
			aro_arrange(s);
			return;
		}
		if (ly_resize(f->node, e, s->cfg.theme.resize_step))
			aro_arrange(s);
		return;
	}

	if (b->action == Q_MOVE) {
		ly_node *target = ly_focus(o->ws[o->cur_ws], f->node, e);
		if (target && target->user) {
			view_swap(f, target->user);
			aro_arrange(s);
		} else {
			/* move across outputs if possible */
			struct aro_output *dest = output_toward(s, o, e);
			if (dest)
				view_move_to_output(s, f, dest);
		}
		return;
	}

	/* monocle steps through windows in order; the others go by where windows are */
	ly_node *next = mono ? monocle_step(o->ws[o->cur_ws], f->node, e)
	                     : ly_focus(o->ws[o->cur_ws], f->node, e);
	if (next) {
		aro_focus(s, next->user);
		cursor_warp_to_view(s, next->user);
		return;
	}

	/* focus across outputs */
	struct aro_output *dest = output_toward(s, o, e);
	if (dest) {
		s->focused_output = dest;
		struct aro_view *cand = output_pick_view(s, dest);
		aro_focus(s, cand);
		cursor_warp_to_view(s, cand);
	}
}

/* match bindings */
bool handle_bind(struct aro_server *s, uint32_t mods, xkb_keysym_t sym)
{
	/* let lock screen receive keys; an app that took the shortcuts gets them too */
	if (aro_locked(s) || shortcuts_inhibited(s))
		return false;

	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	                      WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	mods &= care;

	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (!b->button && b->sym == sym && b->mods == mods) {
			run_action(s, b);
			return true;
		}
	}
	return false;
}

/* while the switcher is open only its own bind acts */
bool handle_switch_bind(struct aro_server *s, uint32_t mods,
                               xkb_keysym_t sym)
{
	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	                      WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	mods &= care;
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->action == Q_SWITCH && b->sym == sym && b->mods == mods) {
			switcher_step(s, b->num);
			return true;
		}
	}
	return false;
}
