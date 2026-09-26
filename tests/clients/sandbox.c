/* sandbox: connect the way a Flatpak app does, through a security context, and list what it sees */
#define _GNU_SOURCE /* pipe2 */

#include "common.h"
#include "security-context-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static struct wp_security_context_manager_v1 *mgr;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, wp_security_context_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &wp_security_context_manager_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void sandboxed_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)r; (void)name; (void)v;
	printf("%s\n", iface);
}

static const struct wl_registry_listener sandboxed_reg = { sandboxed_global, reg_remove };

int main(void)
{
	struct globals g;
	connect_globals(&g);
	wl_registry_add_listener(wl_display_get_registry(g.dpy), &reg, NULL);
	roundtrip(&g);
	if (!mgr)
		fail("aro does not offer security-context");

	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s/aro-sandbox", dir);
	unlink(addr.sun_path);
	int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
	    listen(listen_fd, 4) < 0)
		fail("cannot create the sandbox socket");

	/* aro stops listening once the other end of this pipe closes */
	int close_fds[2];
	if (pipe2(close_fds, O_CLOEXEC) < 0)
		fail("no pipe");

	struct wp_security_context_v1 *ctx =
		wp_security_context_manager_v1_create_listener(mgr, listen_fd, close_fds[0]);
	wp_security_context_v1_set_sandbox_engine(ctx, "org.aro.test");
	wp_security_context_v1_set_app_id(ctx, "aro-test-sandboxed");
	wp_security_context_v1_commit(ctx);
	close(listen_fd);
	close(close_fds[0]);
	roundtrip(&g);

	struct wl_display *app = wl_display_connect("aro-sandbox");
	if (!app)
		fail("cannot connect through the sandbox socket");
	wl_registry_add_listener(wl_display_get_registry(app), &sandboxed_reg, NULL);
	if (wl_display_roundtrip(app) < 0)
		fail("the sandboxed connection failed");
	wl_display_disconnect(app);

	close(close_fds[1]);
	unlink(addr.sun_path);
	return 0;
}
