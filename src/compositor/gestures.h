/* gestures.h: touchpad gestures, passed to apps except aro's workspace swipe */
#ifndef ARO_GESTURES_H
#define ARO_GESTURES_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct aro_server;

struct aro_gestures {
	struct wlr_pointer_gestures_v1 *proto;
	bool swiping;                   /* this swipe is aro's, not the app's */
	double dx, dy;                  /* how far it has gone */

	struct wl_listener swipe_begin, swipe_update, swipe_end;
	struct wl_listener pinch_begin, pinch_update, pinch_end;
	struct wl_listener hold_begin, hold_end;
};

void gestures_init(struct aro_server *s);
void gestures_finish(struct aro_server *s);

#endif
