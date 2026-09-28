/* touch.c: touchscreens; touch for apps that take it, a mouse for the rest */
#include "scene.h"

#include "touch.h"
#include "aro.h"
#include "core.h"
#include "idle.h"
#include "overview.h"
#include "picker.h"
#include "prompt.h"

#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/util/log.h>

struct aro_touch_dev {
	struct wl_list link;            /* aro_touch.devices */
	struct aro_server *server;
	struct wl_listener destroy;
};

static int point_find(struct aro_touch *t, int32_t id)
{
	for (int i = 0; i < ARO_TOUCH_POINTS; i++)
		if (t->points[i].used && t->points[i].id == id)
			return i;
	return -1;
}

/* where the finger is, in layout coordinates */
static void finger_at(struct aro_server *s, struct wlr_touch *touch,
                      double x, double y, double *lx, double *ly)
{
	wlr_cursor_absolute_to_layout_coords(s->cursor, &touch->base, x, y, lx, ly);
}

/* the cursor goes where the finger is, as if a mouse had moved there */
static void mouse_follow(struct aro_server *s, struct wlr_touch *touch,
                         double x, double y, uint32_t time)
{
	wlr_cursor_warp_absolute(s->cursor, &touch->base, x, y);
	aro_pointer_moved(s, time);
}

static void on_down(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, touch.down);
	struct wlr_touch_down_event *ev = data;
	struct aro_touch *t = &s->touch;
	idle_activity(s);

	double lx, ly, sx = 0, sy = 0;
	finger_at(s, ev->touch, ev->x, ev->y, &lx, &ly);
	struct wlr_surface *surface = NULL;
	struct aro_view *v = NULL;
	/* aro's own screens take a mouse, never touch */
	bool aro_ui = !aro_locked(s) && (overview_active(s) || prompt_active(s) ||
	                                   picker_active(s));
	if (!aro_ui)
		v = view_at(s, lx, ly, &surface, &sx, &sy);

	int slot = -1;
	for (int i = 0; i < ARO_TOUCH_POINTS && slot < 0; i++)
		if (!t->points[i].used)
			slot = i;

	if (surface && slot >= 0 && wlr_surface_accepts_touch(surface, s->seat)) {
		if (v && !aro_locked(s)) {
			struct aro_output *o = output_at(s, lx, ly);
			if (o)
				s->focused_output = o;
			aro_focus(s, v);
		}
		t->points[slot].used = true;
		t->points[slot].id = ev->touch_id;
		t->points[slot].ox = lx - sx;
		t->points[slot].oy = ly - sy;
		wlr_seat_touch_notify_down(s->seat, surface, ev->time_msec,
		                           ev->touch_id, sx, sy);
		return;
	}

	/* the first finger elsewhere is a mouse with its left button held */
	if (t->mouse)
		return;
	t->mouse = true;
	t->mouse_id = ev->touch_id;
	mouse_follow(s, ev->touch, ev->x, ev->y, ev->time_msec);
	aro_pointer_button(s, ev->time_msec, BTN_LEFT, true);
}

static void on_motion(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, touch.motion);
	struct wlr_touch_motion_event *ev = data;
	struct aro_touch *t = &s->touch;
	idle_activity(s);

	if (t->mouse && ev->touch_id == t->mouse_id) {
		mouse_follow(s, ev->touch, ev->x, ev->y, ev->time_msec);
		return;
	}
	int i = point_find(t, ev->touch_id);
	if (i < 0)
		return;
	double lx, ly;
	finger_at(s, ev->touch, ev->x, ev->y, &lx, &ly);
	wlr_seat_touch_notify_motion(s->seat, ev->time_msec, ev->touch_id,
	                             lx - t->points[i].ox, ly - t->points[i].oy);
}

static void on_up(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, touch.up);
	struct wlr_touch_up_event *ev = data;
	struct aro_touch *t = &s->touch;
	idle_activity(s);

	if (t->mouse && ev->touch_id == t->mouse_id) {
		t->mouse = false;
		aro_pointer_button(s, ev->time_msec, BTN_LEFT, false);
		return;
	}
	int i = point_find(t, ev->touch_id);
	if (i < 0)
		return;
	t->points[i].used = false;
	wlr_seat_touch_notify_up(s->seat, ev->time_msec, ev->touch_id);
}

static void on_cancel(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, touch.cancel);
	struct wlr_touch_cancel_event *ev = data;
	struct aro_touch *t = &s->touch;

	if (t->mouse && ev->touch_id == t->mouse_id) {
		t->mouse = false;
		aro_pointer_button(s, ev->time_msec, BTN_LEFT, false);
		return;
	}
	int i = point_find(t, ev->touch_id);
	if (i < 0)
		return;
	t->points[i].used = false;
	struct wlr_touch_point *p = wlr_seat_touch_get_point(s->seat, ev->touch_id);
	if (p && p->client)
		wlr_seat_touch_notify_cancel(s->seat, p->client);
}

static void on_frame(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, touch.frame);
	(void)data;
	wlr_seat_touch_notify_frame(s->seat);
}

static void dev_destroy(struct wl_listener *l, void *data)
{
	struct aro_touch_dev *d = wl_container_of(l, d, destroy);
	struct aro_server *s = d->server;
	(void)data;
	wl_list_remove(&d->destroy.link);
	wl_list_remove(&d->link);
	free(d);
	if (!touch_present(s)) {
		/* the fingers went with it: a held mouse button must not stay held */
		struct aro_touch *t = &s->touch;
		if (t->mouse)
			aro_pointer_button(s, aro_now_ms(), BTN_LEFT, false);
		t->mouse = false;
		memset(t->points, 0, sizeof t->points);
	}
	seat_update_caps(s);
}

void touch_add(struct aro_server *s, struct wlr_input_device *dev)
{
	struct aro_touch_dev *d = calloc(1, sizeof *d);
	if (!d)
		return;
	d->server = s;
	d->destroy.notify = dev_destroy;
	wl_signal_add(&dev->events.destroy, &d->destroy);
	wl_list_insert(&s->touch.devices, &d->link);
	wlr_cursor_attach_input_device(s->cursor, dev);

	/* a built-in screen's touch belongs to that screen, not the whole layout */
	struct wlr_touch *touch = wlr_touch_from_input_device(dev);
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		if (touch->output_name && o->wlr_output->name &&
		    !strcmp(touch->output_name, o->wlr_output->name))
			wlr_cursor_map_input_to_output(s->cursor, dev, o->wlr_output);
	wlr_log(WLR_INFO, "touch: %s", dev->name ? dev->name : "unnamed");
}

bool touch_present(struct aro_server *s)
{
	return !wl_list_empty(&s->touch.devices);
}

void touch_init(struct aro_server *s)
{
	struct aro_touch *t = &s->touch;
	wl_list_init(&t->devices);
	t->down.notify = on_down;
	wl_signal_add(&s->cursor->events.touch_down, &t->down);
	t->up.notify = on_up;
	wl_signal_add(&s->cursor->events.touch_up, &t->up);
	t->motion.notify = on_motion;
	wl_signal_add(&s->cursor->events.touch_motion, &t->motion);
	t->cancel.notify = on_cancel;
	wl_signal_add(&s->cursor->events.touch_cancel, &t->cancel);
	t->frame.notify = on_frame;
	wl_signal_add(&s->cursor->events.touch_frame, &t->frame);
}

void touch_finish(struct aro_server *s)
{
	struct aro_touch *t = &s->touch;
	wl_list_remove(&t->down.link);
	wl_list_remove(&t->up.link);
	wl_list_remove(&t->motion.link);
	wl_list_remove(&t->cancel.link);
	wl_list_remove(&t->frame.link);
	struct aro_touch_dev *d, *tmp;
	wl_list_for_each_safe(d, tmp, &t->devices, link) {
		wl_list_remove(&d->destroy.link);
		wl_list_remove(&d->link);
		free(d);
	}
}
