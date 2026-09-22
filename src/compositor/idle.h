/*
 * aro — idle.h
 *
 * Three protocols that only make sense together:
 *
 *   idle-notify-v1          we tell clients how long the seat has been
 *                           idle; swayidle is the usual listener
 *   idle-inhibit-v1         clients tell US not to count them as idle,
 *                           which is how a video player keeps the screen on
 *   output-power-management the thing that actually turns the panel off
 *
 * The compositor owns none of the policy. It reports activity, honours
 * inhibitors, and switches outputs on and off when asked; deciding that
 * five minutes of nothing means lock the screen is swayidle's job.
 *
 * An inhibitor only counts while its surface is on screen — the protocol
 * says so, and without it a paused video on a workspace you left an hour
 * ago holds the screen awake forever. "On screen" is answered by aro.c,
 * which is the only file that knows about workspaces.
 */
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
	/* on the inhibiting surface itself: a surface can create its
	 * inhibitor before it maps, and nothing else would notice it map */
	struct wl_listener surface_map;
	struct wl_listener surface_unmap;
};

void idle_init(struct aro_server *s);
void idle_finish(struct aro_server *s);

/*
 * Called from every input path. Cheap by design — it runs on every pointer
 * motion event, so it must stay a function call and a timestamp.
 */
void idle_activity(struct aro_server *s);

/*
 * Re-decide whether any inhibitor counts. Called at the end of
 * aro_arrange(), which already runs on everything that can change what is
 * on screen, and by idle.c itself when an inhibitor or its surface comes
 * or goes. Walks inhibitors x views — both lists are short, and arrange is
 * event-driven, not per-frame.
 */
void idle_update(struct aro_server *s);

/*
 * Implemented in aro.c, where views and workspaces live; declared here
 * because this file is its only caller. True when the surface — or the
 * window it is a popup or subsurface of — is mapped and on screen.
 */
bool aro_surface_visible(struct aro_server *s, struct wlr_surface *surface);

#endif
