/* idle.h: idle notify / inhibit / output power */
#ifndef ARO_IDLE_H
#define ARO_IDLE_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct aro_server;
struct wlr_surface;

struct aro_inhibitor {
	struct wl_list link;            /* aro_server.inhibitors */
	struct aro_server *server;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
	/* watch inhibitor surface map/unmap */
	struct wl_listener surface_map;
	struct wl_listener surface_unmap;
};

void idle_init(struct aro_server *s);
void idle_finish(struct aro_server *s);

/* report user activity */
void idle_activity(struct aro_server *s);

/* re-evaluate inhibitors */
void idle_update(struct aro_server *s);

/* surface visibility check, implemented in aro.c */
bool aro_surface_visible(struct aro_server *s, struct wlr_surface *surface);

#endif
