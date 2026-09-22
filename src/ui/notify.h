/* notify.h: compositor toasts */
#ifndef ARO_NOTIFY_H
#define ARO_NOTIFY_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "anim.h"
#include "text.h"

struct aro_server;
struct wlr_scene_tree;
struct wlr_scene_rect;

enum notify_level {
	NOTIFY_INFO,
	NOTIFY_ERROR,
};

struct aro_notification {
	struct wl_list link;            /* aro_server.notifications */
	struct aro_server *server;

	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *bg;
	struct wlr_scene_rect *edge;    /* accent, or urgent for an error */
	struct qtext text;

	anim_box geo;
	struct wl_event_source *timer;  /* NULL for an error: it does not expire */
	enum notify_level level;
};

/* post toast; errors persist until cleared */
void notify(struct aro_server *s, enum notify_level level,
            const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* clear error toasts */
void notify_clear_errors(struct aro_server *s);

/* tick toast animations */
bool notify_tick(struct aro_server *s, uint32_t now);

/* retheme toasts */
void notify_retheme(struct aro_server *s);

void notify_finish(struct aro_server *s);

#endif
