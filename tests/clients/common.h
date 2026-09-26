/* common.h: what every test client needs from a running aro */
#ifndef ARO_TEST_COMMON_H
#define ARO_TEST_COMMON_H

#include <stdint.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"

struct globals {
	struct wl_display *dpy;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct ext_workspace_manager_v1 *workspaces;
};

/* connect and bind everything aro offers; exits on failure */
void connect_globals(struct globals *g);

/* a solid-colour ARGB buffer */
struct wl_buffer *solid_buffer(struct globals *g, int w, int h, uint32_t argb);

/* one roundtrip; exits with a message if aro has gone away */
void roundtrip(struct globals *g);

/* print to stderr and exit 1 */
_Noreturn void fail(const char *fmt, ...);

#endif
