/* touch.h: touchscreens; touch for apps that take it, a mouse for the rest */
#ifndef ARO_TOUCH_H
#define ARO_TOUCH_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct aro_server;
struct wlr_input_device;

#define ARO_TOUCH_POINTS 16

struct aro_touch {
	struct wl_list devices;         /* aro_touch_dev.link */
	/* a finger on a touch app: where its surface sat when it went down */
	struct {
		bool used;
		int32_t id;
		double ox, oy;
	} points[ARO_TOUCH_POINTS];
	bool mouse;                     /* a finger is being a mouse */
	int32_t mouse_id;
	struct wl_listener down, up, motion, cancel, frame;
};

void touch_init(struct aro_server *s);

/* a touchscreen was plugged in */
void touch_add(struct aro_server *s, struct wlr_input_device *dev);

/* any touchscreens: the seat says it has touch */
bool touch_present(struct aro_server *s);

void touch_finish(struct aro_server *s);

#endif
