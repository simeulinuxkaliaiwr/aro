/* globals: print every protocol aro advertises, one per line */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
	(void)data; (void)r; (void)name;
	printf("%s %u\n", iface, version);
}

static void reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fputs("cannot connect to aro\n", stderr);
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_listener, NULL);
	if (wl_display_roundtrip(dpy) < 0)
		return 1;
	wl_display_disconnect(dpy);
	return 0;
}
