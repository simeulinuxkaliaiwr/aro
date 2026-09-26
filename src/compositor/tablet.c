/* tablet.c: drawing tablets; pen data for apps that take it, a mouse for the rest */
#include "scene.h"

#include "tablet.h"
#include "aro.h"

#include <math.h>
#include <stdlib.h>

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/util/log.h>

struct aro_tablet {
	struct wl_list link;            /* aro_tablets.list */
	struct wlr_input_device *dev;
	struct wlr_tablet_v2_tablet *v2;
	struct wl_listener destroy;
};

struct aro_tool {
	struct aro_server *server;
	struct wlr_tablet_v2_tablet_tool *v2;
	struct wl_listener set_cursor;
	struct wl_listener destroy;
};

static struct aro_tablet *find_tablet(struct aro_server *s, struct wlr_tablet *t)
{
	struct aro_tablet *at;
	wl_list_for_each(at, &s->tablets.list, link)
		if (at->dev == &t->base)
			return at;
	return NULL;
}

static void tool_set_cursor(struct wl_listener *l, void *data)
{
	struct aro_tool *tool = wl_container_of(l, tool, set_cursor);
	struct wlr_tablet_v2_event_cursor *ev = data;
	struct wlr_surface *focused = tool->v2->focused_surface;
	if (focused && ev->seat_client->client == wl_resource_get_client(focused->resource))
		wlr_cursor_set_surface(tool->server->cursor, ev->surface,
		                       ev->hotspot_x, ev->hotspot_y);
}

static void tool_destroy(struct wl_listener *l, void *data)
{
	struct aro_tool *tool = wl_container_of(l, tool, destroy);
	struct wlr_tablet_tool *t = data;
	t->data = NULL;
	wl_list_remove(&tool->set_cursor.link);
	wl_list_remove(&tool->destroy.link);
	free(tool);
}

static struct aro_tool *get_tool(struct aro_server *s, struct wlr_tablet_tool *t)
{
	if (t->data)
		return t->data;
	struct aro_tool *tool = calloc(1, sizeof *tool);
	if (!tool)
		return NULL;
	tool->server = s;
	tool->v2 = wlr_tablet_tool_create(s->tablets.manager, s->seat, t);
	if (!tool->v2) {
		free(tool);
		return NULL;
	}
	tool->set_cursor.notify = tool_set_cursor;
	wl_signal_add(&tool->v2->events.set_cursor, &tool->set_cursor);
	tool->destroy.notify = tool_destroy;
	wl_signal_add(&t->events.destroy, &tool->destroy);
	t->data = tool;
	return tool;
}

/* move the cursor to the pen, then say whether the surface below takes tablet input */
static struct wlr_surface *follow_pen(struct aro_server *s, struct aro_tablet *at,
                                      struct aro_tool *tool, double x, double y,
                                      uint32_t time)
{
	wlr_cursor_warp_absolute(s->cursor, at->dev, x, y);
	double sx, sy;
	struct wlr_surface *surf = aro_surface_at(s, &sx, &sy);
	bool tablet_app = surf && wlr_surface_accepts_tablet_v2(surf, at->v2);

	/* a lifted pen follows the app under it; a pressed one stays with its app */
	if (tool->v2->focused_surface && tool->v2->focused_surface != surf &&
	    !tool->v2->is_down)
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
	if (!tablet_app) {
		if (!tool->v2->focused_surface)
			aro_pointer_moved(s, time);
		return NULL;
	}
	if (!tool->v2->focused_surface)
		wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->v2, at->v2, surf);
	wlr_tablet_v2_tablet_tool_notify_motion(tool->v2, sx, sy);
	return surf;
}

static void on_proximity(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, tablets.proximity);
	struct wlr_tablet_tool_proximity_event *ev = data;
	struct aro_tablet *at = find_tablet(s, ev->tablet);
	struct aro_tool *tool = get_tool(s, ev->tool);
	if (!at || !tool)
		return;
	if (ev->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
		return;
	}
	follow_pen(s, at, tool, ev->x, ev->y, ev->time_msec);
}

static void on_axis(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, tablets.axis);
	struct wlr_tablet_tool_axis_event *ev = data;
	struct aro_tablet *at = find_tablet(s, ev->tablet);
	struct aro_tool *tool = get_tool(s, ev->tool);
	if (!at || !tool)
		return;

	double x = ev->updated_axes & WLR_TABLET_TOOL_AXIS_X ? ev->x : NAN;
	double y = ev->updated_axes & WLR_TABLET_TOOL_AXIS_Y ? ev->y : NAN;
	if (!follow_pen(s, at, tool, x, y, ev->time_msec))
		return;

	struct wlr_tablet_v2_tablet_tool *t = tool->v2;
	uint32_t a = ev->updated_axes;
	if (a & WLR_TABLET_TOOL_AXIS_PRESSURE)
		wlr_tablet_v2_tablet_tool_notify_pressure(t, ev->pressure);
	if (a & WLR_TABLET_TOOL_AXIS_DISTANCE)
		wlr_tablet_v2_tablet_tool_notify_distance(t, ev->distance);
	if (a & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
		wlr_tablet_v2_tablet_tool_notify_tilt(t, ev->tilt_x, ev->tilt_y);
	if (a & WLR_TABLET_TOOL_AXIS_ROTATION)
		wlr_tablet_v2_tablet_tool_notify_rotation(t, ev->rotation);
	if (a & WLR_TABLET_TOOL_AXIS_SLIDER)
		wlr_tablet_v2_tablet_tool_notify_slider(t, ev->slider);
	if (a & WLR_TABLET_TOOL_AXIS_WHEEL)
		wlr_tablet_v2_tablet_tool_notify_wheel(t, ev->wheel_delta, 0);
}

static void on_tip(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, tablets.tip);
	struct wlr_tablet_tool_tip_event *ev = data;
	struct aro_tool *tool = get_tool(s, ev->tool);
	if (!tool)
		return;
	bool down = ev->state == WLR_TABLET_TOOL_TIP_DOWN;

	/* no tablet app under the pen: the tip is a left click */
	if (!tool->v2->focused_surface) {
		aro_pointer_button(s, ev->time_msec, BTN_LEFT, down);
		return;
	}
	if (down) {
		aro_focus_at_cursor(s);
		wlr_tablet_v2_tablet_tool_notify_down(tool->v2);
	} else {
		wlr_tablet_v2_tablet_tool_notify_up(tool->v2);
	}
}

static void on_button(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, tablets.button);
	struct wlr_tablet_tool_button_event *ev = data;
	struct aro_tool *tool = get_tool(s, ev->tool);
	if (!tool)
		return;
	bool down = ev->state == WLR_BUTTON_PRESSED;

	/* the pen's side buttons are right and middle clicks outside tablet apps */
	if (!tool->v2->focused_surface) {
		aro_pointer_button(s, ev->time_msec,
		                   ev->button == BTN_STYLUS ? BTN_RIGHT : BTN_MIDDLE, down);
		return;
	}
	wlr_tablet_v2_tablet_tool_notify_button(tool->v2, ev->button,
		down ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
		     : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void tablet_destroy(struct wl_listener *l, void *data)
{
	struct aro_tablet *at = wl_container_of(l, at, destroy);
	(void)data;
	wl_list_remove(&at->destroy.link);
	wl_list_remove(&at->link);
	free(at);
}

void tablet_add(struct aro_server *s, struct wlr_input_device *dev)
{
	if (!s->tablets.manager)
		return;
	struct aro_tablet *at = calloc(1, sizeof *at);
	if (!at)
		return;
	at->dev = dev;
	at->v2 = wlr_tablet_create(s->tablets.manager, s->seat, dev);
	if (!at->v2) {
		free(at);
		return;
	}
	at->destroy.notify = tablet_destroy;
	wl_signal_add(&dev->events.destroy, &at->destroy);
	wl_list_insert(&s->tablets.list, &at->link);
	wlr_cursor_attach_input_device(s->cursor, dev);
	wlr_log(WLR_INFO, "tablet: %s", dev->name ? dev->name : "unnamed");
}

void tablet_init(struct aro_server *s)
{
	struct aro_tablets *t = &s->tablets;
	wl_list_init(&t->list);
	t->manager = wlr_tablet_v2_create(s->display);
	if (!t->manager)
		return;
	t->axis.notify = on_axis;
	wl_signal_add(&s->cursor->events.tablet_tool_axis, &t->axis);
	t->proximity.notify = on_proximity;
	wl_signal_add(&s->cursor->events.tablet_tool_proximity, &t->proximity);
	t->tip.notify = on_tip;
	wl_signal_add(&s->cursor->events.tablet_tool_tip, &t->tip);
	t->button.notify = on_button;
	wl_signal_add(&s->cursor->events.tablet_tool_button, &t->button);
}

void tablet_finish(struct aro_server *s)
{
	struct aro_tablets *t = &s->tablets;
	if (!t->manager)
		return;
	wl_list_remove(&t->axis.link);
	wl_list_remove(&t->proximity.link);
	wl_list_remove(&t->tip.link);
	wl_list_remove(&t->button.link);
	t->manager = NULL;
}
