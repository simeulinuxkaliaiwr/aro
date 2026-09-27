/* gestures.c: touchpad gestures, passed to apps except aro's workspace swipe */
#include "scene.h"

#include "gestures.h"
#include "aro.h"

#include <math.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/util/log.h>

/* touchpad units a swipe must travel to count, about a third of a typical pad */
#define SWIPE_DISTANCE 100.0

static struct aro_output *output_under_cursor(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		ly_box b = o->box;
		if (s->cursor->x >= b.x && s->cursor->x < b.x + b.w &&
		    s->cursor->y >= b.y && s->cursor->y < b.y + b.h)
			return o;
	}
	return NULL;
}

static void swipe_begin(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.swipe_begin);
	struct wlr_pointer_swipe_begin_event *ev = data;
	struct aro_gestures *g = &s->gestures;

	g->swiping = s->cfg.gesture_fingers > 0 && !aro_locked(s) &&
	             ev->fingers == (uint32_t)s->cfg.gesture_fingers;
	g->dx = g->dy = 0;
	if (!g->swiping)
		wlr_pointer_gestures_v1_send_swipe_begin(g->proto, s->seat,
		                                         ev->time_msec, ev->fingers);
}

static void swipe_update(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.swipe_update);
	struct wlr_pointer_swipe_update_event *ev = data;
	struct aro_gestures *g = &s->gestures;

	if (g->swiping) {
		g->dx += ev->dx;
		g->dy += ev->dy;
		return;
	}
	wlr_pointer_gestures_v1_send_swipe_update(g->proto, s->seat,
	                                          ev->time_msec, ev->dx, ev->dy);
}

static void swipe_end(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.swipe_end);
	struct wlr_pointer_swipe_end_event *ev = data;
	struct aro_gestures *g = &s->gestures;

	if (!g->swiping) {
		wlr_pointer_gestures_v1_send_swipe_end(g->proto, s->seat,
		                                       ev->time_msec, ev->cancelled);
		return;
	}
	g->swiping = false;

	struct aro_output *o = output_under_cursor(s);
	if (ev->cancelled || !o)
		return;

	/* sideways, when workspaces slide up and down: a scroll strip's next column */
	struct aro_view *f = s->focused;
	if (s->cfg.ws_slide == Q_SLIDE_VERTICAL && fabs(g->dx) > fabs(g->dy)) {
		if (fabs(g->dx) >= SWIPE_DISTANCE && f && f->output == o &&
		    f->workspace == o->cur_ws && aro_ws_layout(o, o->cur_ws) == Q_LAYOUT_SCROLL)
			aro_run_action(s, &(struct q_bind){ .action = Q_FOCUS,
				.num = g->dx < 0 ? LY_RIGHT : LY_LEFT });
		return;
	}

	/* along the axis workspaces slide on; fingers moving left bring the next one in */
	double d = s->cfg.ws_slide == Q_SLIDE_VERTICAL ? g->dy : g->dx;
	if (fabs(d) < SWIPE_DISTANCE)
		return;
	int ws = o->cur_ws + (d < 0 ? 1 : -1);
	if (ws < 0 || ws >= ARO_MAX_WS)
		return;
	s->focused_output = o;
	aro_run_action(s, &(struct q_bind){ .action = Q_WORKSPACE, .num = ws });
}

static void pinch_begin(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.pinch_begin);
	struct wlr_pointer_pinch_begin_event *ev = data;
	wlr_pointer_gestures_v1_send_pinch_begin(s->gestures.proto, s->seat,
	                                         ev->time_msec, ev->fingers);
}

static void pinch_update(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.pinch_update);
	struct wlr_pointer_pinch_update_event *ev = data;
	wlr_pointer_gestures_v1_send_pinch_update(s->gestures.proto, s->seat,
		ev->time_msec, ev->dx, ev->dy, ev->scale, ev->rotation);
}

static void pinch_end(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.pinch_end);
	struct wlr_pointer_pinch_end_event *ev = data;
	wlr_pointer_gestures_v1_send_pinch_end(s->gestures.proto, s->seat,
	                                       ev->time_msec, ev->cancelled);
}

static void hold_begin(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.hold_begin);
	struct wlr_pointer_hold_begin_event *ev = data;
	wlr_pointer_gestures_v1_send_hold_begin(s->gestures.proto, s->seat,
	                                        ev->time_msec, ev->fingers);
}

static void hold_end(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, gestures.hold_end);
	struct wlr_pointer_hold_end_event *ev = data;
	wlr_pointer_gestures_v1_send_hold_end(s->gestures.proto, s->seat,
	                                      ev->time_msec, ev->cancelled);
}

void gestures_init(struct aro_server *s)
{
	struct aro_gestures *g = &s->gestures;
	g->proto = wlr_pointer_gestures_v1_create(s->display);
	if (!g->proto) {
		wlr_log(WLR_ERROR, "pointer gestures unavailable");
		return;
	}
	g->swipe_begin.notify = swipe_begin;
	wl_signal_add(&s->cursor->events.swipe_begin, &g->swipe_begin);
	g->swipe_update.notify = swipe_update;
	wl_signal_add(&s->cursor->events.swipe_update, &g->swipe_update);
	g->swipe_end.notify = swipe_end;
	wl_signal_add(&s->cursor->events.swipe_end, &g->swipe_end);
	g->pinch_begin.notify = pinch_begin;
	wl_signal_add(&s->cursor->events.pinch_begin, &g->pinch_begin);
	g->pinch_update.notify = pinch_update;
	wl_signal_add(&s->cursor->events.pinch_update, &g->pinch_update);
	g->pinch_end.notify = pinch_end;
	wl_signal_add(&s->cursor->events.pinch_end, &g->pinch_end);
	g->hold_begin.notify = hold_begin;
	wl_signal_add(&s->cursor->events.hold_begin, &g->hold_begin);
	g->hold_end.notify = hold_end;
	wl_signal_add(&s->cursor->events.hold_end, &g->hold_end);
}

void gestures_finish(struct aro_server *s)
{
	struct aro_gestures *g = &s->gestures;
	if (!g->proto)
		return;
	wl_list_remove(&g->swipe_begin.link);
	wl_list_remove(&g->swipe_update.link);
	wl_list_remove(&g->swipe_end.link);
	wl_list_remove(&g->pinch_begin.link);
	wl_list_remove(&g->pinch_update.link);
	wl_list_remove(&g->pinch_end.link);
	wl_list_remove(&g->hold_begin.link);
	wl_list_remove(&g->hold_end.link);
	g->proto = NULL;
}
