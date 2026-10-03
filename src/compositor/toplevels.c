/* toplevels.c: window lists for taskbars, docks and screen-sharing pickers, and app icons */

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "aro.h"
#include "protocols.h"
#include "core.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>

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
	capture_view_gone(v);           /* while the surface is still there */
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

/* ── app icons ─────────────────────────────────────────────────────────── */

/* a theme icon name, looked up as a PNG in hicolor or pixmaps */
static struct wlr_buffer *icon_by_name(const char *name)
{
	if (!name || !*name || strchr(name, '/'))
		return NULL;
	static const char *const sizes[] = {
		"48x48", "64x64", "32x32", "128x128", "256x256", "24x24", "16x16",
	};
	/* $XDG_DATA_HOME, then $XDG_DATA_DIRS, with their usual defaults */
	char dirs[4096];
	const char *dh = getenv("XDG_DATA_HOME"), *dd = getenv("XDG_DATA_DIRS");
	const char *home = getenv("HOME");
	if (dh && *dh)
		snprintf(dirs, sizeof dirs, "%s:", dh);
	else
		snprintf(dirs, sizeof dirs, "%s/.local/share:", home ? home : "");
	size_t n = strlen(dirs);
	snprintf(dirs + n, sizeof dirs - n, "%s", dd && *dd ? dd : "/usr/local/share:/usr/share");
	char path[4096];
	for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
		char *save = NULL, list[4096];
		snprintf(list, sizeof list, "%s", dirs);
		for (char *d = strtok_r(list, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
			snprintf(path, sizeof path, "%s/icons/hicolor/%s/apps/%s.png", d, sizes[i], name);
			struct wlr_buffer *b = ui_png_load(path);
			if (b)
				return b;
		}
	}
	snprintf(path, sizeof path, "/usr/share/pixmaps/%s.png", name);
	return ui_png_load(path);
}

static void set_icon(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, set_toplevel_icon);
	struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event *ev = data;
	struct aro_view *v, *found = NULL;
	wl_list_for_each(v, &s->views, link)
		if (v->toplevel == ev->toplevel)
			found = v;
	if (!found)
		return;
	if (!ev->icon) {
		ui_frame_icon(found, NULL);
		return;
	}

	/* the smallest image that is sharp at twice the header's size, else the largest */
	const int want = (s->cfg.theme.header_h - 10) * 2;
	struct wlr_xdg_toplevel_icon_v1_buffer *ib, *best = NULL;
	wl_list_for_each(ib, &ev->icon->buffers, link) {
		int w = ib->buffer->width;
		int bw = best ? best->buffer->width : 0;
		if (!best || (bw < want ? w > bw : (w >= want && w < bw)))
			best = ib;
	}
	if (best) {
		ui_frame_icon(found, best->buffer);
		return;
	}
	struct wlr_buffer *named = icon_by_name(ev->icon->name);
	ui_frame_icon(found, named);
	if (named)
		wlr_buffer_drop(named);         /* the scene holds its own lock */
}

void toplevel_icons_init(struct aro_server *s)
{
	struct wlr_xdg_toplevel_icon_manager_v1 *m =
		wlr_xdg_toplevel_icon_manager_v1_create(s->display, 1);
	if (!m)
		return;
	/* the sizes a header wants: as drawn, and for a doubled screen */
	int sz = s->cfg.theme.header_h - 10;
	int sizes[] = { sz > 8 ? sz : 16, sz > 8 ? sz * 2 : 32 };
	wlr_xdg_toplevel_icon_manager_v1_set_sizes(m, sizes, 2);
	s->set_toplevel_icon.notify = set_icon;
	wl_signal_add(&m->events.set_icon, &s->set_toplevel_icon);
	s->toplevel_icons = m;
}

void toplevel_icons_finish(struct aro_server *s)
{
	if (s->toplevel_icons)
		wl_list_remove(&s->set_toplevel_icon.link);
	s->toplevel_icons = NULL;
}

