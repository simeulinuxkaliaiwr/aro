/* protocols.c: smaller Wayland protocols that need little of aro's own state */
#include "scene.h"

#include "protocols.h"
#include "aro.h"

#include <wlr/backend.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
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

#include <drm_fourcc.h>
#include <math.h>
#include <time.h>
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
	"hyprland_toplevel_export_manager_v1",
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

/* screen capture through ext-image-copy-capture: whole outputs, and single windows.
 * A window is drawn from its own surfaces rather than a scene node, as wlroots'
 * scene-node capture cannot read SceneFX's scene; the same code serves every build */
struct view_capture {
	struct wlr_ext_image_capture_source_v1 base;
	struct aro_view *view;
	struct wlr_swapchain *swapchain;
	int w, h;
	bool want;                      /* a client waits for a frame */
	bool sent;                      /* one went out since the start */
	struct wl_event_source *idle;
	struct wl_listener commit;
};

static void capture_src_destroy(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, capture_src_destroy);
	(void)data;
	wl_list_remove(&v->capture_src_destroy.link);
	v->capture_src = NULL;
}

/* the window's size in buffer pixels, at its screen's scale */
void capture_size(struct aro_view *v, int *w, int *h, float *scale)
{
	struct wlr_box g;
	view_geometry(v, &g);
	*scale = v->output && v->output->wlr_output ? v->output->wlr_output->scale : 1.0f;
	*w = (int)ceilf((float)g.width * *scale);
	*h = (int)ceilf((float)g.height * *scale);
	if (*w < 1)
		*w = 1;
	if (*h < 1)
		*h = 1;
}

/* buffers the window's size: the screen's own format, which the renderer draws into
 * anyway; else plain linear ARGB */
struct wlr_swapchain *capture_swapchain(struct aro_view *v, int w, int h)
{
	struct aro_server *s = v->server;
	struct wlr_output *out = v->output ? v->output->wlr_output : NULL;
	struct wlr_swapchain *sc = NULL;
	if (out && out->swapchain)
		sc = wlr_swapchain_create(s->allocator, w, h, &out->swapchain->format);
	if (!sc) {
		struct wlr_drm_format_set set = { 0 };
		if (wlr_drm_format_set_add(&set, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR))
			sc = wlr_swapchain_create(s->allocator, w, h, wlr_drm_format_set_get(&set, DRM_FORMAT_ARGB8888));
		wlr_drm_format_set_finish(&set);
	}
	return sc;
}

/* a swapchain the window's size; clients hear of new constraints */
static bool capture_resize(struct view_capture *vc)
{
	struct aro_server *s = vc->view->server;
	int w, h;
	float scale;
	capture_size(vc->view, &w, &h, &scale);
	if (vc->swapchain && w == vc->w && h == vc->h)
		return false;
	struct wlr_swapchain *sc = capture_swapchain(vc->view, w, h);
	if (!sc)
		return false;
	if (vc->swapchain)
		wlr_swapchain_destroy(vc->swapchain);
	vc->swapchain = sc;
	vc->w = w;
	vc->h = h;
	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&vc->base, sc, s->renderer);
	return true;
}

/* the frame goes out from the event loop, never inside a request */
static void capture_idle(void *data)
{
	struct view_capture *vc = data;
	vc->idle = NULL;
	if (!vc->want)
		return;
	if (capture_resize(vc))
		return;                 /* new constraints: the client asks again */
	vc->want = false;
	vc->sent = true;
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, (unsigned)vc->w, (unsigned)vc->h);
	struct wlr_ext_image_capture_source_v1_frame_event ev = { .damage = &damage };
	wl_signal_emit_mutable(&vc->base.events.frame, &ev);
	pixman_region32_fini(&damage);
}

static void capture_schedule(struct view_capture *vc)
{
	if (!vc->idle)
		vc->idle = wl_event_loop_add_idle(vc->view->server->loop, capture_idle, vc);
}

/* new content: a waiting client gets it */
static void capture_commit(struct wl_listener *l, void *data)
{
	struct view_capture *vc = wl_container_of(l, vc, commit);
	(void)data;
	if (vc->want)
		capture_schedule(vc);
}

static void capture_start(struct wlr_ext_image_capture_source_v1 *src, bool with_cursors)
{
	struct view_capture *vc = wl_container_of(src, vc, base);
	(void)with_cursors;
	vc->sent = false;
	capture_resize(vc);
}

static void capture_stop(struct wlr_ext_image_capture_source_v1 *src)
{
	struct view_capture *vc = wl_container_of(src, vc, base);
	vc->want = false;
}

/* the first frame at once; after that, when the window draws something new */
static void capture_request(struct wlr_ext_image_capture_source_v1 *src, bool schedule_frame)
{
	struct view_capture *vc = wl_container_of(src, vc, base);
	(void)schedule_frame;
	vc->want = true;
	if (!vc->sent)
		capture_schedule(vc);
}

struct capture_pass {
	struct wlr_render_pass *pass;
	int gx, gy;
	float scale;
};

static void capture_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct capture_pass *d = data;
	struct wlr_texture *tex = wlr_surface_get_texture(surface);
	if (!tex)
		return;
	struct wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	wlr_render_pass_add_texture(d->pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.src_box = src,
		.dst_box = {
			(int)lroundf((float)(sx - d->gx) * d->scale),
			(int)lroundf((float)(sy - d->gy) * d->scale),
			(int)lroundf((float)surface->current.width * d->scale),
			(int)lroundf((float)surface->current.height * d->scale),
		},
		.transform = surface->current.transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	});
}

bool capture_draw(struct aro_view *v, struct wlr_buffer *buf)
{
	struct wlr_surface *surface = view_surface(v);
	if (!surface)
		return false;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(v->server->renderer, buf, NULL);
	if (!pass)
		return false;
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { 0, 0, buf->width, buf->height },
		.color = { 0, 0, 0, 0 },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	struct wlr_box g;
	view_geometry(v, &g);
	int w, h;
	struct capture_pass d = { .pass = pass, .gx = g.x, .gy = g.y };
	capture_size(v, &w, &h, &d.scale);
	wlr_surface_for_each_surface(surface, capture_surface, &d);
	return wlr_render_pass_submit(pass);
}

static void capture_copy(struct wlr_ext_image_capture_source_v1 *src,
                         struct wlr_ext_image_copy_capture_frame_v1 *frame,
                         struct wlr_ext_image_capture_source_v1_frame_event *ev)
{
	struct view_capture *vc = wl_container_of(src, vc, base);
	struct aro_server *s = vc->view->server;
	(void)ev;
	struct wlr_buffer *buf = vc->swapchain ? wlr_swapchain_acquire(vc->swapchain) : NULL;
	if (!buf || !capture_draw(vc->view, buf)) {
		if (buf)
			wlr_buffer_unlock(buf);
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}
	/* as wlroots' own output source does: a failed copy has already failed the frame */
	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buf, s->renderer)) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
	}
	wlr_buffer_unlock(buf);
}

static const struct wlr_ext_image_capture_source_v1_interface capture_impl = {
	.start = capture_start,
	.stop = capture_stop,
	.request_frame = capture_request,
	.copy_frame = capture_copy,
};

void capture_view_gone(struct aro_view *v)
{
	if (!v->capture_src)
		return;
	struct view_capture *vc = wl_container_of(v->capture_src, vc, base);
	if (vc->idle)
		wl_event_source_remove(vc->idle);
	wl_list_remove(&vc->commit.link);
	wlr_ext_image_capture_source_v1_finish(&vc->base);      /* clears v->capture_src */
	if (vc->swapchain)
		wlr_swapchain_destroy(vc->swapchain);
	free(vc);
}

static void new_capture_request(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_capture_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req = data;

	struct aro_view *v, *found = NULL;
	wl_list_for_each(v, &s->views, link)
		if (v->ext_ftl && v->ext_ftl == req->toplevel_handle) {
			found = v;
			break;
		}
	struct wlr_surface *surface = found ? view_surface(found) : NULL;
	if (!surface)
		return;
	if (!found->capture_src) {
		struct view_capture *vc = calloc(1, sizeof *vc);
		if (!vc)
			return;
		vc->view = found;
		wlr_ext_image_capture_source_v1_init(&vc->base, &capture_impl);
		vc->commit.notify = capture_commit;
		wl_signal_add(&surface->events.commit, &vc->commit);
		found->capture_src = &vc->base;
		found->capture_src_destroy.notify = capture_src_destroy;
		wl_signal_add(&vc->base.events.destroy, &found->capture_src_destroy);
		capture_resize(vc);
	}
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req,
		found->capture_src);
}

static void capture_init(struct aro_server *s)
{
	wlr_ext_image_copy_capture_manager_v1_create(s->display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(s->display, 1);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *m =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(s->display, 1);
	if (m) {
		s->new_capture_request.notify = new_capture_request;
		wl_signal_add(&m->events.new_request, &s->new_capture_request);
		s->capture_toplevels = true;
	}
}

/* VR headsets: aro lends non-desktop outputs to runtimes like Monado or SteamVR */
static void lease_request(struct wl_listener *l, void *data)
{
	struct wlr_drm_lease_request_v1 *req = data;
	(void)l;
	if (!wlr_drm_lease_request_v1_grant(req))
		wlr_drm_lease_request_v1_reject(req);
}

static void lease_init(struct aro_server *s)
{
	s->drm_lease = wlr_drm_lease_v1_manager_create(s->display, s->backend);
	if (!s->drm_lease)
		return;         /* nested or headless: no DRM to lend */
	s->lease_request.notify = lease_request;
	wl_signal_add(&s->drm_lease->events.request, &s->lease_request);
	wlr_log(WLR_INFO, "VR headsets: can be leased");
}

/* apps say what colour space they draw in; only if the renderer can convert */
void colour_init(struct aro_server *s)
{
	const struct wlr_renderer *r = s->renderer;
	wlr_log(WLR_INFO, "renderer colour transforms: input %s, output %s",
	        r->features.input_color_transform ? "yes" : "no",
	        r->features.output_color_transform ? "yes" : "no");
	if (!r->features.input_color_transform)
		return;

	size_t ntf = 0, nprim = 0;
	enum wp_color_manager_v1_transfer_function *tf =
		wlr_color_manager_v1_transfer_function_list_from_renderer(s->renderer, &ntf);
	enum wp_color_manager_v1_primaries *prim =
		wlr_color_manager_v1_primaries_list_from_renderer(s->renderer, &nprim);
	static const enum wp_color_manager_v1_render_intent intents[] = {
		WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
	};
	struct wlr_color_manager_v1 *cm = NULL;
	if (tf && prim)
		cm = wlr_color_manager_v1_create(s->display, 1, &(struct wlr_color_manager_v1_options){
			.features = { .parametric = true, .set_mastering_display_primaries = true },
			.render_intents = intents,
			.render_intents_len = sizeof intents / sizeof *intents,
			.transfer_functions = tf,
			.transfer_functions_len = ntf,
			.primaries = prim,
			.primaries_len = nprim,
		});
	free(tf);
	free(prim);
	if (!cm) {
		wlr_log(WLR_ERROR, "colour management: could not start");
		return;
	}
	wlr_scene_set_color_manager_v1(s->scene, cm);
	wlr_log(WLR_INFO, "colour management: on");
}

void protocols_init(struct aro_server *s)
{
	syncobj_init(s);
	shortcuts_init(s);
	security_init(s);
	wlr_xdg_wm_dialog_v1_create(s->display, 1);     /* modal dialogs; see xdg_type */
	s->tearing_mgr = wlr_tearing_control_manager_v1_create(s->display, 1);
	capture_init(s);
	export_init(s);
	bgeffect_init(s);
	lease_init(s);
}

void protocols_finish(struct aro_server *s)
{
	if (s->inhibit_mgr)
		wl_list_remove(&s->new_kb_inhibitor.link);
	s->inhibit_mgr = NULL;
	if (s->capture_toplevels)
		wl_list_remove(&s->new_capture_request.link);
	s->capture_toplevels = false;
	if (s->drm_lease)
		wl_list_remove(&s->lease_request.link);
	s->drm_lease = NULL;
}
