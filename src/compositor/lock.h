/* lock.h: session lock state */
#ifndef ARO_LOCK_H
#define ARO_LOCK_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct aro_server;
struct aro_output;
struct wlr_session_lock_v1;
struct wlr_session_lock_surface_v1;
struct wlr_scene_tree;
struct wlr_scene_rect;

struct aro_lock {
	struct aro_server *server;
	struct wlr_session_lock_v1 *lock;

	struct wlr_scene_tree *tree;    /* the lock's surfaces live under here */
	struct wlr_scene_rect *blank;   /* solid black, under them, always */
	struct wl_list surfaces;

	/* abandoned lock: stay locked */
	bool abandoned;

	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

struct aro_lock_surface {
	struct wl_list link;
	struct aro_lock *lock;
	struct wlr_session_lock_surface_v1 *surface;
	struct wlr_scene_tree *tree;

	struct wl_listener map;
	struct wl_listener destroy;
};

void lock_init(struct aro_server *s);
void lock_finish(struct aro_server *s);

/* locked? */
bool aro_locked(struct aro_server *s);

/* arrange lock surfaces */
void lock_arrange(struct aro_server *s);

#endif
