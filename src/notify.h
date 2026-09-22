/*
 * aro — notify.h
 *
 * The compositor's own on-screen messages.
 *
 * NOT a notification daemon: this does not implement the freedesktop
 * notification protocol and applications cannot post to it. It exists so
 * that aro can tell you something about aro — a config file that did
 * not parse, an output that vanished — without the message going only to a
 * log nobody is reading.
 *
 * Toasts stack down the top-right corner of the focused output, spring in
 * like every other frame, and expire on their own.
 */
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

/*
 * Post a message. Formats like printf. Safe to call before any output
 * exists — the toast is simply dropped, since there is nowhere to draw it.
 *
 * An INFO message expires on its own. An ERROR does NOT: it describes the
 * state of something that is still wrong, not an event that happened, and
 * six seconds is less than the time it takes to look back at your editor.
 * Errors stay until notify_clear_errors() says the problem is gone.
 */
void notify(struct aro_server *s, enum notify_level level,
            const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Drop every error currently on screen. Called before posting a fresh set,
 * so the toasts always reflect the state of the file rather than a history
 * of everything that has ever been wrong with it. */
void notify_clear_errors(struct aro_server *s);

/* Advance every toast's animation; returns true while any is still moving. */
bool notify_tick(struct aro_server *s, uint32_t now);

/* Re-apply colours and fonts after a config reload. */
void notify_retheme(struct aro_server *s);

void notify_finish(struct aro_server *s);

#endif
