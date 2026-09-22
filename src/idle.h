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
 */
#ifndef ARO_IDLE_H
#define ARO_IDLE_H

#include <wayland-server-core.h>

struct aro_server;

struct aro_inhibitor {
	struct wl_list link;            /* aro_server.inhibitors */
	struct aro_server *server;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

void idle_init(struct aro_server *s);
void idle_finish(struct aro_server *s);

/*
 * Called from every input path. Cheap by design — it runs on every pointer
 * motion event, so it must stay a function call and a timestamp.
 */
void idle_activity(struct aro_server *s);

#endif
