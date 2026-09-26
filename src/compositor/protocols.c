/* protocols.c: smaller Wayland protocols that need little of aro's own state */
#include "scene.h"

#include "protocols.h"
#include "aro.h"

#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/util/log.h>
#ifdef ARO_XWAYLAND
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>
#include <wlr/xwayland/xwayland.h>
#endif

#include <stdlib.h>
#include <string.h>

/* explicit sync, which NVIDIA's driver needs; only when both ends can do timelines */
static void syncobj_init(struct aro_server *s)
{
	int drm_fd = wlr_renderer_get_drm_fd(s->renderer);
	if (drm_fd < 0 || !s->renderer->features.timeline ||
	    !s->backend->features.timeline) {
		wlr_log(WLR_INFO, "explicit sync: not supported by this renderer or backend");
		return;
	}
	if (wlr_linux_drm_syncobj_manager_v1_create(s->display, 1, drm_fd))
		wlr_log(WLR_INFO, "explicit sync: on");
}

struct inhibitor {
	struct wl_list link;            /* aro_server.kb_inhibitors */
	struct aro_server *server;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

void shortcuts_inhibit_sync(struct aro_server *s)
{
	struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
	struct inhibitor *in;
	wl_list_for_each(in, &s->kb_inhibitors, link) {
		bool want = in->inhibitor->surface == focused;
		if (want && !in->inhibitor->active)
			wlr_keyboard_shortcuts_inhibitor_v1_activate(in->inhibitor);
		else if (!want && in->inhibitor->active)
			wlr_keyboard_shortcuts_inhibitor_v1_deactivate(in->inhibitor);
	}
}

bool shortcuts_inhibited(struct aro_server *s)
{
	struct inhibitor *in;
	wl_list_for_each(in, &s->kb_inhibitors, link)
		if (in->inhibitor->active)
			return true;
	return false;
}

static void inhibitor_destroy(struct wl_listener *l, void *data)
{
	struct inhibitor *in = wl_container_of(l, in, destroy);
	(void)data;
	wl_list_remove(&in->destroy.link);
	wl_list_remove(&in->link);
	free(in);
}

static void new_inhibitor(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_kb_inhibitor);
	struct inhibitor *in = calloc(1, sizeof *in);
	if (!in)
		return;
	in->server = s;
	in->inhibitor = data;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&in->inhibitor->events.destroy, &in->destroy);
	wl_list_insert(&s->kb_inhibitors, &in->link);
	shortcuts_inhibit_sync(s);
}

static void shortcuts_init(struct aro_server *s)
{
	wl_list_init(&s->kb_inhibitors);
	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *m =
		wlr_keyboard_shortcuts_inhibit_v1_create(s->display);
	if (!m)
		return;
	s->new_kb_inhibitor.notify = new_inhibitor;
	wl_signal_add(&m->events.new_inhibitor, &s->new_kb_inhibitor);
	s->inhibit_mgr = m;
}

/* what a sandboxed app (Flatpak and the like) must not reach: capture, input, control */
static const char *const privileged[] = {
	"zwlr_screencopy_manager_v1",
	"zwlr_export_dmabuf_manager_v1",
	"ext_image_copy_capture_manager_v1",
	"ext_output_image_capture_source_manager_v1",
	"ext_foreign_toplevel_image_capture_source_manager_v1",
	"zwlr_data_control_manager_v1",
	"ext_data_control_manager_v1",
	"zwp_virtual_keyboard_manager_v1",
	"zwlr_virtual_pointer_manager_v1",
	"zwp_input_method_manager_v2",
	"zwp_keyboard_shortcuts_inhibit_manager_v1",
	"zwlr_layer_shell_v1",
	"ext_session_lock_manager_v1",
	"zwlr_output_manager_v1",
	"zwlr_output_power_manager_v1",
	"zwlr_gamma_control_manager_v1",
	"zwlr_foreign_toplevel_manager_v1",
	"ext_foreign_toplevel_list_v1",
	"ext_workspace_manager_v1",
	"ext_idle_notifier_v1",
	"wp_drm_lease_device_v1",
	"wp_security_context_manager_v1",
};

static bool global_filter(const struct wl_client *client,
                          const struct wl_global *global, void *data)
{
	struct aro_server *s = data;
#ifdef ARO_XWAYLAND
	/* only XWayland itself may bind its shell */
	if (s->xwayland && s->xwayland->shell_v1 &&
	    global == s->xwayland->shell_v1->global)
		return s->xwayland->server && client == s->xwayland->server->client;
#endif
	if (!wlr_security_context_manager_v1_lookup_client(s->security_ctx, client))
		return true;            /* not sandboxed */
	const char *name = wl_global_get_interface(global)->name;
	for (size_t i = 0; i < sizeof privileged / sizeof privileged[0]; i++)
		if (!strcmp(name, privileged[i]))
			return false;
	return true;
}

static void security_init(struct aro_server *s)
{
	s->security_ctx = wlr_security_context_manager_v1_create(s->display);
	if (s->security_ctx)
		wl_display_set_global_filter(s->display, global_filter, s);
}

void protocols_init(struct aro_server *s)
{
	syncobj_init(s);
	shortcuts_init(s);
	security_init(s);
	wlr_xdg_wm_dialog_v1_create(s->display, 1);     /* modal dialogs; see xdg_type */
	s->tearing_mgr = wlr_tearing_control_manager_v1_create(s->display, 1);
}

void protocols_finish(struct aro_server *s)
{
	if (s->inhibit_mgr)
		wl_list_remove(&s->new_kb_inhibitor.link);
	s->inhibit_mgr = NULL;
}
