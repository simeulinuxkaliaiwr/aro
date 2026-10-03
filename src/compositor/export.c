/* export.c: Hyprland's toplevel export, one frame of one window per request.
 * Quickshell's window previews (ScreencopyView on a toplevel) speak only this */
#include "scene.h"

#include "protocols.h"
#include "aro.h"

#include "hyprland-toplevel-export-v1-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>

#include <drm_fourcc.h>
#include <stdlib.h>
#include <time.h>

struct export_frame {
	struct wl_list link;            /* aro_server.exports */
	struct wl_resource *resource;
	struct aro_view *view;          /* NULL once the window is gone */
	int w, h;
	struct wlr_buffer *buffer;      /* the client's, locked from copy on */
	bool damage;                    /* the copy waits for the window to draw */
	bool done;                      /* ready or failed went out */
	struct wl_listener commit;      /* linked while damage is awaited */
	struct wl_event_source *idle;
};

static const struct hyprland_toplevel_export_frame_v1_interface frame_impl;

static void frame_unwait(struct export_frame *f)
{
	if (f->damage) {
		wl_list_remove(&f->commit.link);
		f->damage = false;
	}
	if (f->idle) {
		wl_event_source_remove(f->idle);
		f->idle = NULL;
	}
}

static void frame_fail(struct export_frame *f)
{
	frame_unwait(f);
	if (!f->done)
		hyprland_toplevel_export_frame_v1_send_failed(f->resource);
	f->done = true;
}

/* the window into the client's buffer: straight in if it is a GPU buffer, else drawn
 * into one of aro's and read back */
static bool frame_render(struct export_frame *f)
{
	struct aro_view *v = f->view;
	struct aro_server *s = v->server;
	struct wlr_dmabuf_attributes dmabuf;
	if (wlr_buffer_get_dmabuf(f->buffer, &dmabuf))
		return capture_draw(v, f->buffer);

	if (!v->export_sc || v->export_sc->width != f->w || v->export_sc->height != f->h) {
		if (v->export_sc)
			wlr_swapchain_destroy(v->export_sc);
		v->export_sc = capture_swapchain(v, f->w, f->h);
	}
	struct wlr_buffer *tmp = v->export_sc ? wlr_swapchain_acquire(v->export_sc) : NULL;
	if (!tmp)
		return false;
	bool ok = false;
	struct wlr_texture *tex = NULL;
	void *data;
	uint32_t format;
	size_t stride;
	if (capture_draw(v, tmp) && (tex = wlr_texture_from_buffer(s->renderer, tmp)) &&
	    wlr_buffer_begin_data_ptr_access(f->buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
	                                     &data, &format, &stride)) {
		ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
			.data = data, .format = format, .stride = (uint32_t)stride,
		});
		wlr_buffer_end_data_ptr_access(f->buffer);
	}
	if (tex)
		wlr_texture_destroy(tex);
	wlr_buffer_unlock(tmp);
	return ok;
}

static void frame_idle(void *data)
{
	struct export_frame *f = data;
	f->idle = NULL;
	if (!f->view || !frame_render(f)) {
		frame_fail(f);
		return;
	}
	frame_unwait(f);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	hyprland_toplevel_export_frame_v1_send_damage(f->resource, 0, 0, (uint32_t)f->w, (uint32_t)f->h);
	hyprland_toplevel_export_frame_v1_send_flags(f->resource, 0);
	hyprland_toplevel_export_frame_v1_send_ready(f->resource,
		(uint32_t)((uint64_t)now.tv_sec >> 32), (uint32_t)now.tv_sec, (uint32_t)now.tv_nsec);
	f->done = true;
}

/* the window drew: the waiting copy goes out */
static void frame_commit(struct wl_listener *l, void *data)
{
	struct export_frame *f = wl_container_of(l, f, commit);
	(void)data;
	if (!f->idle)
		f->idle = wl_event_loop_add_idle(f->view->server->loop, frame_idle, f);
}

static void frame_copy(struct wl_client *client, struct wl_resource *resource,
                       struct wl_resource *buffer, int32_t ignore_damage)
{
	(void)client;
	struct export_frame *f = wl_resource_get_user_data(resource);
	if (f->buffer || f->done) {
		wl_resource_post_error(resource, HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_ERROR_ALREADY_USED,
			"frame already used");
		return;
	}
	f->buffer = wlr_buffer_try_from_resource(buffer);
	if (!f->buffer || f->buffer->width != f->w || f->buffer->height != f->h) {
		wl_resource_post_error(resource, HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_ERROR_INVALID_BUFFER,
			"buffer is not %dx%d", f->w, f->h);
		return;
	}
	struct wlr_surface *surface = f->view ? view_surface(f->view) : NULL;
	if (!surface) {
		frame_fail(f);
		return;
	}
	if (ignore_damage) {
		f->idle = wl_event_loop_add_idle(f->view->server->loop, frame_idle, f);
		return;
	}
	f->commit.notify = frame_commit;
	wl_signal_add(&surface->events.commit, &f->commit);
	f->damage = true;
}

static void frame_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_export_frame_v1_interface frame_impl = {
	.copy = frame_copy,
	.destroy = frame_destroy_req,
};

static void frame_resource_destroy(struct wl_resource *resource)
{
	struct export_frame *f = wl_resource_get_user_data(resource);
	frame_unwait(f);
	if (f->buffer)
		wlr_buffer_unlock(f->buffer);
	wl_list_remove(&f->link);
	free(f);
}

static void capture(struct wl_client *client, struct wl_resource *manager,
                    uint32_t id, struct aro_view *v)
{
	struct aro_server *s = wl_resource_get_user_data(manager);
	struct export_frame *f = calloc(1, sizeof *f);
	if (!f) {
		wl_client_post_no_memory(client);
		return;
	}
	f->resource = wl_resource_create(client, &hyprland_toplevel_export_frame_v1_interface,
		wl_resource_get_version(manager), id);
	if (!f->resource) {
		free(f);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(f->resource, &frame_impl, f, frame_resource_destroy);
	wl_list_insert(&s->exports, &f->link);

	if (!v || !view_surface(v)) {
		frame_fail(f);
		return;
	}
	f->view = v;
	float scale;
	capture_size(v, &f->w, &f->h, &scale);
	hyprland_toplevel_export_frame_v1_send_buffer(f->resource, WL_SHM_FORMAT_ARGB8888,
		(uint32_t)f->w, (uint32_t)f->h, (uint32_t)f->w * 4);
	if (wl_resource_get_version(f->resource) >= 2) {
		if (wlr_renderer_get_drm_fd(s->renderer) >= 0)
			hyprland_toplevel_export_frame_v1_send_linux_dmabuf(f->resource,
				DRM_FORMAT_ARGB8888, (uint32_t)f->w, (uint32_t)f->h);
		hyprland_toplevel_export_frame_v1_send_buffer_done(f->resource);
	}
}

/* Hyprland names windows by address; aro by the id aroctl shows */
static void capture_toplevel(struct wl_client *client, struct wl_resource *manager,
                             uint32_t id, int32_t overlay_cursor, uint32_t handle)
{
	struct aro_server *s = wl_resource_get_user_data(manager);
	(void)overlay_cursor;
	struct aro_view *v, *found = NULL;
	wl_list_for_each(v, &s->views, link)
		if (v->id == handle && v->ftl) {
			found = v;
			break;
		}
	capture(client, manager, id, found);
}

static void capture_toplevel_wlr(struct wl_client *client, struct wl_resource *manager,
                                 uint32_t id, int32_t overlay_cursor, struct wl_resource *handle)
{
	struct aro_server *s = wl_resource_get_user_data(manager);
	(void)overlay_cursor;
	struct wlr_foreign_toplevel_handle_v1 *ftl = wl_resource_get_user_data(handle);  /* NULL: gone */
	struct aro_view *v, *found = NULL;
	wl_list_for_each(v, &s->views, link)
		if (ftl && v->ftl == ftl) {
			found = v;
			break;
		}
	capture(client, manager, id, found);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_export_manager_v1_interface manager_impl = {
	.capture_toplevel = capture_toplevel,
	.destroy = manager_destroy,
	.capture_toplevel_with_wlr_toplevel_handle = capture_toplevel_wlr,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client,
		&hyprland_toplevel_export_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

void export_view_gone(struct aro_view *v)
{
	struct export_frame *f;
	wl_list_for_each(f, &v->server->exports, link)
		if (f->view == v) {
			frame_fail(f);
			f->view = NULL;
		}
	if (v->export_sc)
		wlr_swapchain_destroy(v->export_sc);
	v->export_sc = NULL;
}

void export_init(struct aro_server *s)
{
	wl_list_init(&s->exports);
	if (!wl_global_create(s->display, &hyprland_toplevel_export_manager_v1_interface, 2, s, manager_bind))
		wlr_log(WLR_ERROR, "window export: could not start");
}
