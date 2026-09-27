/* toplevels.c: window lists for taskbars, docks and screen-sharing pickers */

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "aro.h"
#include "core.h"

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>

/* ── foreign toplevel ──────────────────────────────────────────────────── */
/*
 * Two protocols: wlr-foreign-toplevel-management (waybar's taskbar; can
 * activate, close, fullscreen) and ext-foreign-toplevel-list (read-only).
 * Handles exist while a window is mapped. Minimize and maximize requests
 * are ignored: a tiling layout has neither.
 */

static void ftl_on_activate(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, ftl_activate);
	(void)data;
	if (aro_locked(v->server) || prompt_active(v->server))
		return;
	view_raise_and_focus(v->server, v);
}

static void ftl_on_close(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, ftl_close);
	(void)data;
	view_close(v);
}

static void ftl_on_fullscreen(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, ftl_fullscreen_req);
	const struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
	view_set_fullscreen(v->server, v, ev->fullscreen);
}

/* wlroots copies these; NULL is not accepted everywhere, "" is */
void ftl_update_ids(struct aro_view *v)
{
	const char *title = view_title(v), *app_id = view_app_id(v);
	title = title ? title : "";
	app_id = app_id ? app_id : "";

	if (v->ftl) {
		wlr_foreign_toplevel_handle_v1_set_title(v->ftl, title);
		wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, app_id);
	}
	if (v->ext_ftl) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title, .app_id = app_id,
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(v->ext_ftl, &st);
	}
}

void ftl_create(struct aro_view *v)
{
	struct aro_server *s = v->server;
	const char *title = view_title(v), *app_id = view_app_id(v);

	if (s->ext_ftl_list && !v->ext_ftl) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title ? title : "", .app_id = app_id ? app_id : "",
		};
		v->ext_ftl = wlr_ext_foreign_toplevel_handle_v1_create(s->ext_ftl_list, &st);
	}

	if (s->ftl_mgr && !v->ftl) {
		v->ftl = wlr_foreign_toplevel_handle_v1_create(s->ftl_mgr);
		if (v->ftl) {
			v->ftl_output = NULL;
			v->ftl_fullscreen = false;
			v->ftl_activate.notify = ftl_on_activate;
			wl_signal_add(&v->ftl->events.request_activate, &v->ftl_activate);
			v->ftl_close.notify = ftl_on_close;
			wl_signal_add(&v->ftl->events.request_close, &v->ftl_close);
			v->ftl_fullscreen_req.notify = ftl_on_fullscreen;
			wl_signal_add(&v->ftl->events.request_fullscreen,
			              &v->ftl_fullscreen_req);
		}
	}

	ftl_update_ids(v);
	ftl_sync_view(v);
}

void ftl_destroy(struct aro_view *v)
{
	struct aro_server *s = v->server;
	if (s->ftl_activated == v)
		s->ftl_activated = NULL;

	if (v->ftl) {
		wl_list_remove(&v->ftl_activate.link);
		wl_list_remove(&v->ftl_close.link);
		wl_list_remove(&v->ftl_fullscreen_req.link);
		wlr_foreign_toplevel_handle_v1_destroy(v->ftl);
		v->ftl = NULL;
	}
	v->ftl_output = NULL;
	if (v->ext_ftl) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(v->ext_ftl);
		v->ext_ftl = NULL;
	}
}

/* the output a window is on, and whether it is fullscreen */
void ftl_sync_view(struct aro_view *v)
{
	if (!v->ftl)
		return;

	struct wlr_output *wo = v->output && v->output->enabled
	                      ? v->output->wlr_output : NULL;
	if (wo != v->ftl_output) {
		if (v->ftl_output)
			wlr_foreign_toplevel_handle_v1_output_leave(v->ftl, v->ftl_output);
		if (wo)
			wlr_foreign_toplevel_handle_v1_output_enter(v->ftl, wo);
		v->ftl_output = wo;
	}
	if (v->fullscreen != v->ftl_fullscreen) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(v->ftl, v->fullscreen);
		v->ftl_fullscreen = v->fullscreen;
	}
}

void ftl_sync_activated(struct aro_server *s)
{
	struct aro_view *want = s->focused && s->focused->ftl ? s->focused : NULL;
	if (want == s->ftl_activated)
		return;
	if (s->ftl_activated && s->ftl_activated->ftl)
		wlr_foreign_toplevel_handle_v1_set_activated(s->ftl_activated->ftl, false);
	if (want)
		wlr_foreign_toplevel_handle_v1_set_activated(want->ftl, true);
	s->ftl_activated = want;
}

int clock_tick(void *data)
{
	struct aro_server *s = data;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->bar.tree)
			bar_update(&o->bar, o);
		wlr_output_schedule_frame(o->wlr_output);
	}

	wl_event_source_timer_update(s->clock_timer, 1000);
	return 0;
}
