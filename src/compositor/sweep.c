/* sweep.c: draw a box, and the next window opens in it at that size */
#define _POSIX_C_SOURCE 200809L /* strdup */

/* scene.h must come first */
#include "scene.h"

#include "sweep.h"
#include "core.h"

#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

/*
 * The box is drawn before the window exists, so the window's very first
 * configure already carries the box's size: it draws once, at the right
 * size, where the box was. A drag shorter than SWEEP_CLICK is a click.
 */
#define SWEEP_CLICK   8         /* pixels */
#define SWEEP_WAIT_MS 30000     /* a box nothing takes goes away */

static void schedule_all(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

static void mark_show(struct aro_server *s, bool on)
{
	ui_preview_show(&s->sweep_mark, on);
	if (on && s->sweep_mark.tree)
		wlr_scene_node_raise_to_top(&s->sweep_mark.tree->node);
	schedule_all(s);
}

/* (x, y) moved inside the usable part of the screen the press was on */
static void clamp_point(struct aro_server *s, double *x, double *y)
{
	struct aro_output *o = output_at(s, s->sweep.x0, s->sweep.y0);
	if (!o)
		return;
	ly_box u = usable_area(o);
	*x = *x < u.x ? u.x : *x > u.x + u.w ? u.x + u.w : *x;
	*y = *y < u.y ? u.y : *y > u.y + u.h ? u.y + u.h : *y;
}

/* the box from the press to (x, y) */
static ly_box box_to(struct aro_server *s, double x, double y)
{
	struct aro_sweep *w = &s->sweep;
	clamp_point(s, &x, &y);
	int x0 = (int)w->x0, y0 = (int)w->y0, x1 = (int)x, y1 = (int)y;
	return (ly_box){
		x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
		x0 < x1 ? x1 - x0 : x0 - x1, y0 < y1 ? y1 - y0 : y0 - y1,
	};
}

/* give the pointer back to whatever is under it */
static void pointer_back(struct aro_server *s)
{
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
	pointer_motion_common(s, aro_now_ms());
}

static void wait_stop(struct aro_server *s)
{
	if (s->sweep.timer)
		wl_event_source_timer_update(s->sweep.timer, 0);
}

static int wait_expired(void *data)
{
	struct aro_server *s = data;
	if (s->sweep.state == SWEEP_WAITING) {
		wlr_log(WLR_INFO, "sweep: no window took the box");
		sweep_cancel(s);
	}
	return 0;
}

void sweep_init(struct aro_server *s)
{
	s->sweep = (struct aro_sweep){ 0 };
	s->sweep.timer = wl_event_loop_add_timer(s->loop, wait_expired, s);
	if (ui_preview_create(&s->sweep_mark, s))
		wlr_scene_node_reparent(&s->sweep_mark.tree->node, s->l_float);
}

void sweep_finish(struct aro_server *s)
{
	if (s->sweep.timer)
		wl_event_source_remove(s->sweep.timer);
	s->sweep.timer = NULL;
	free(s->sweep.cmd);
	s->sweep.cmd = NULL;
	ui_preview_finish(&s->sweep_mark);
}

bool sweep_active(struct aro_server *s)
{
	return s->sweep.state == SWEEP_ARMED || s->sweep.state == SWEEP_DRAWING;
}

void sweep_start(struct aro_server *s, const char *cmd)
{
	if (aro_locked(s) || prompt_active(s) || picker_active(s) || overview_active(s) ||
	    s->cursor_mode != ARO_CURSOR_PASSTHROUGH || !s->sweep_mark.tree)
		return;

	/* a second sweep replaces a box still waiting */
	sweep_cancel(s);
	s->sweep.cmd = cmd && *cmd ? strdup(cmd) : NULL;
	s->sweep.state = SWEEP_ARMED;
	wlr_seat_pointer_notify_clear_focus(s->seat);
	wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "crosshair");
	wlr_log(WLR_INFO, "sweep: armed%s%s", cmd ? " for " : "", cmd ? cmd : "");
}

void sweep_cancel(struct aro_server *s)
{
	struct aro_sweep *w = &s->sweep;
	bool had_pointer = sweep_active(s);
	if (w->state == SWEEP_OFF)
		return;
	w->state = SWEEP_OFF;
	free(w->cmd);
	w->cmd = NULL;
	wait_stop(s);
	mark_show(s, false);
	if (had_pointer)
		pointer_back(s);
}

void sweep_pointer_motion(struct aro_server *s, double lx, double ly)
{
	if (s->sweep.state != SWEEP_DRAWING)
		return;
	s->sweep.box = box_to(s, lx, ly);
	ui_preview_geometry(&s->sweep_mark, s->sweep.box);
	schedule_all(s);
}

/* a box too small for a floating window grows away from the press, on screen */
static void grow_to_min(struct aro_server *s, ly_box *b)
{
	const struct q_theme *th = &s->cfg.theme;
	if (b->w < th->float_min_w) {
		if (b->x < (int)s->sweep.x0)
			b->x = (int)s->sweep.x0 - th->float_min_w;
		b->w = th->float_min_w;
	}
	if (b->h < th->float_min_h) {
		if (b->y < (int)s->sweep.y0)
			b->y = (int)s->sweep.y0 - th->float_min_h;
		b->h = th->float_min_h;
	}
	struct aro_output *o = output_at(s, s->sweep.x0, s->sweep.y0);
	if (!o)
		return;
	ly_box u = usable_area(o);
	if (b->x + b->w > u.x + u.w)
		b->x = u.x + u.w - b->w;
	if (b->y + b->h > u.y + u.h)
		b->y = u.y + u.h - b->h;
	if (b->x < u.x)
		b->x = u.x;
	if (b->y < u.y)
		b->y = u.y;
}

static void drawn(struct aro_server *s)
{
	struct aro_sweep *w = &s->sweep;
	char *cmd = w->cmd;
	w->cmd = NULL;

	/* a click: the window opens where it always would */
	if (w->box.w < SWEEP_CLICK && w->box.h < SWEEP_CLICK) {
		sweep_cancel(s);
		if (cmd)
			config_spawn(cmd);
		free(cmd);
		return;
	}

	grow_to_min(s, &w->box);
	ui_preview_geometry(&s->sweep_mark, w->box);
	w->state = SWEEP_WAITING;
	if (w->timer)
		wl_event_source_timer_update(w->timer, SWEEP_WAIT_MS);
	pointer_back(s);
	schedule_all(s);
	wlr_log(WLR_INFO, "sweep: box %dx%d at %d,%d", w->box.w, w->box.h, w->box.x, w->box.y);
	if (cmd)
		config_spawn(cmd);
	free(cmd);
}

void sweep_pointer_button(struct aro_server *s, uint32_t button, bool pressed)
{
	struct aro_sweep *w = &s->sweep;

	/* any other button gives up */
	if (button != BTN_LEFT) {
		if (pressed)
			sweep_cancel(s);
		return;
	}
	if (pressed && w->state == SWEEP_ARMED) {
		w->x0 = s->cursor->x;
		w->y0 = s->cursor->y;
		clamp_point(s, &w->x0, &w->y0);         /* not on the bar */
		w->state = SWEEP_DRAWING;
		w->box = box_to(s, w->x0, w->y0);
		ui_preview_geometry(&s->sweep_mark, w->box);
		mark_show(s, true);
	} else if (!pressed && w->state == SWEEP_DRAWING) {
		w->box = box_to(s, s->cursor->x, s->cursor->y);
		drawn(s);
	}
}

bool sweep_key(struct aro_server *s, xkb_keysym_t sym)
{
	if (!sweep_active(s) || sym != XKB_KEY_Escape)
		return false;
	sweep_cancel(s);
	return true;
}

bool sweep_take(struct aro_server *s, struct aro_view *v, ly_box *box)
{
	/* dialogs and the like go with their parent, not into the box */
	const char *type = aro_view_type(v);
	if (s->sweep.state != SWEEP_WAITING || (type && strcmp(type, "normal")))
		return false;
	*box = s->sweep.box;
	s->sweep.state = SWEEP_OFF;
	wait_stop(s);
	mark_show(s, false);
	return true;
}

void sweep_retheme(struct aro_server *s)
{
	ui_preview_retheme(&s->sweep_mark);
	if (s->sweep.state == SWEEP_DRAWING || s->sweep.state == SWEEP_WAITING)
		ui_preview_geometry(&s->sweep_mark, s->sweep.box);
}

void sweep_output_gone(struct aro_server *s, struct aro_output *o)
{
	if (s->sweep.state != SWEEP_OFF && output_at(s, s->sweep.x0, s->sweep.y0) == o)
		sweep_cancel(s);
}
