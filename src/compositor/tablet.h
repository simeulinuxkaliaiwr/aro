/* tablet.h: drawing tablets; pen data for apps that take it, a mouse for the rest */
#ifndef ARO_TABLET_H
#define ARO_TABLET_H

#include <wayland-server-core.h>

struct aro_server;
struct wlr_input_device;

struct aro_tablets {
	struct wlr_tablet_manager_v2 *manager;
	struct wl_list list;            /* aro_tablet.link */
	struct wl_listener axis, proximity, tip, button;
};

void tablet_init(struct aro_server *s);

/* a tablet was plugged in */
void tablet_add(struct aro_server *s, struct wlr_input_device *dev);

void tablet_finish(struct aro_server *s);

#endif
