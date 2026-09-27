/* output.c: monitors, their modes, and moving windows off one that goes */

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "config.h"
#include "aro.h"
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm_fourcc.h>
#include <wlr/render/color.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>

/* ── output ────────────────────────────────────────────────────────────── */

/* a fullscreen app that asked for it presents at once, tearing, instead of waiting for vsync */
static bool output_wants_tearing(struct aro_server *s, struct aro_output *o)
{
	if (!s->cfg.allow_tearing || !s->tearing_mgr)
		return false;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->fullscreen || v->output != o || !view_visible(v))
			continue;
		struct wlr_surface *surf = view_surface(v);
		return surf && wlr_tearing_control_manager_v1_surface_hint_from_surface(
			s->tearing_mgr, surf) == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
	}
	return false;
}

/* one frame; a tearing flip when wanted, a normal one if the output refuses it */
static void output_commit(struct aro_server *s, struct aro_output *o,
                          struct wlr_scene_output *so)
{
	if (!output_wants_tearing(s, o)) {
		wlr_scene_output_commit(so, NULL);
		return;
	}
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	if (wlr_scene_output_build_state(so, &st, NULL)) {
		st.tearing_page_flip = true;
		if (!wlr_output_test_state(o->wlr_output, &st))
			st.tearing_page_flip = false;
		wlr_output_commit_state(o->wlr_output, &st);
	}
	wlr_output_state_finish(&st);
}

static void output_frame(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, frame);
	struct aro_server *s = o->server;
	(void)data;

	uint32_t now = aro_now_ms();
	slides_reap(s, now);
	/* any output's slide: its windows can cross onto this one */
	bool moving = slides_active(s);

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!view_on_screen(v))
			continue;
		if (anim_box_tick(&v->geo, now))
			moving = true;
		ui_frame_geometry(v, view_draw_box(v, now));
		ui_frame_hold_opacity(v);
	}

	if (notify_tick(s, now))
		moving = true;
	if (prompt_tick(s, now))
		moving = true;
	if (overview_tick(s, now))
		moving = true;
	if (ghost_tick(s, o, now))
		moving = true;

	/* animate drop preview */
	if (s->preview.active) {
		if (anim_box_tick(&s->preview.geo, now))
			moving = true;
		ui_preview_geometry(&s->preview, s->preview.geo.cur);
	}

	struct wlr_scene_output *so =
		wlr_scene_get_scene_output(s->scene, o->wlr_output);
	if (so) {
		output_commit(s, o, so);
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		wlr_scene_output_send_frame_done(so, &ts);
		overview_frame_done(s, o, &ts);
	}

	if (moving)
		wlr_output_schedule_frame(o->wlr_output);
}

static void output_request_state(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr_output, ev->state);
	o->scale = o->wlr_output->scale > 0 ? o->wlr_output->scale : 1.0f;
	output_refresh_box(o);
	update_backdrop(o->server);
	aro_arrange(o->server);
	output_mgr_update(o->server);
}

/* evacuate output on destroy */
void output_destroy(struct wl_listener *l, void *data)
{
	struct aro_output *o = wl_container_of(l, o, destroy);
	struct aro_server *s = o->server;
	(void)data;

	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);

	struct aro_output *dest = NULL;
	bar_return_cancel(o);   /* before free */
	extws_output_gone(o);
	if (o->enabled) {
		dest = output_evacuate(s, o);
		bar_finish(&o->bar);
	}

	/* toplevel handles drop a destroyed output themselves; forget it too,
	 * so a new output at the same address is not taken for this one */
	struct aro_view *fv;
	wl_list_for_each(fv, &s->views, link)
		if (fv->ftl_output == o->wlr_output)
			fv->ftl_output = NULL;
	free(o);

	if (dest) {
		update_backdrop(s);
		aro_arrange(s);
	}
	output_mgr_update(s);
	lid_update(s);
}

/* adopt parked workspaces */
static void output_adopt_parked(struct aro_server *s, struct aro_output *o)
{
	if (!s->parked)
		return;
	s->parked = false;

	/* output starts empty */
	for (int i = 0; i < ARO_MAX_WS; i++) {
		o->ws[i] = s->orphan_ws[i];
		s->orphan_ws[i] = NULL;
		o->ws_layout[i] = s->orphan_ws_layout[i];
		s->orphan_ws_layout[i] = Q_LAYOUT_INHERIT;
	}
	o->cur_ws = s->orphan_cur_ws;

	struct aro_view *v;
	wl_list_for_each(v, &s->views, link) {
		/* only adopt mapped views */
		if (v->output || !v->mapped)
			continue;
		v->output = o;
		if (v->workspace < 0 || v->workspace >= ARO_MAX_WS)
			v->workspace = o->cur_ws;
		v->fbox.x += o->box.x;
		v->fbox.y += o->box.y;
		v->pre_fs.x += o->box.x;
		v->pre_fs.y += o->box.y;
		view_set_visible(v, v->workspace == o->cur_ws);
	}

	/* place adopted views before drawing */
	ly_node *root = o->ws[o->cur_ws];
	if (root)
		ws_arrange(s, o, o->cur_ws);
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && v->output == o)
			anim_box_set(&v->geo, view_target(v));

	aro_focus(s, output_pick_view(s, o));
	wlr_log(WLR_INFO, "output %s adopted the parked workspaces",
	        o->wlr_output->name);
}

/* ── output configuration ──────────────────────────────────────────────── */
/* output config paths */

/* why this output cannot show HDR, or NULL if it can */
static const char *hdr_unsupported(struct aro_server *s, struct wlr_output *wo)
{
	if (!s->renderer->features.output_color_transform)
		return "the renderer cannot (start aro with WLR_RENDERER=vulkan and -Deffects=false)";
	if (!(wo->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020) ||
	    !(wo->supported_transfer_functions & WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ))
		return "the monitor does not report HDR";
	return NULL;
}

/* output request defaults */
struct out_req {
	int enabled;                    /* -1 leave, 0, 1 */
	struct wlr_output_mode *mode;   /* an exact mode (protocol) */
	bool custom;                    /* mode_w x mode_h @ mode_mhz, as given */
	int mode_w, mode_h, mode_mhz;   /* 0 leave; -1 preferred; else look up */
	bool has_pos;
	bool pos_auto;                  /* place it to the right of the rest */
	int x, y;
	float scale;                    /* 0 leave; -1 work it out from the DPI */
	int transform;                  /* -1 leave */
	int adaptive_sync;              /* -1 leave */
	int hdr;                        /* -1 leave */
};

#define OUT_REQ_NONE { .enabled = -1, .transform = -1, .adaptive_sync = -1, .hdr = -1 }

/* notify output management clients */
void output_mgr_update(struct aro_server *s)
{
	if (!s->output_mgr)
		return;
	struct wlr_output_configuration_v1 *cfg =
		wlr_output_configuration_v1_create();
	if (!cfg)
		return;

	struct wl_list *lists[2] = { &s->outputs, &s->outputs_off };
	for (int i = 0; i < 2; i++) {
		struct aro_output *o;
		wl_list_for_each(o, lists[i], link) {
			struct wlr_output_configuration_head_v1 *h =
				wlr_output_configuration_head_v1_create(cfg, o->wlr_output);
			if (!h)
				continue;
			/* report layout-enabled state */
			h->state.enabled = o->enabled;
			if (o->enabled) {
				h->state.x = o->box.x;
				h->state.y = o->box.y;
			}
		}
	}
	/* takes ownership of cfg */
	wlr_output_manager_v1_set_configuration(s->output_mgr, cfg);
}

struct aro_output *output_from_wlr(struct aro_server *s,
                                          struct wlr_output *wo)
{
	struct wl_list *lists[2] = { &s->outputs, &s->outputs_off };
	for (int i = 0; i < 2; i++) {
		struct aro_output *o;
		wl_list_for_each(o, lists[i], link)
			if (o->wlr_output == wo)
				return o;
	}
	return NULL;
}

/* find output mode */
static struct wlr_output_mode *output_find_mode(struct wlr_output *wo,
                                                int w, int h, int mhz)
{
	struct wlr_output_mode *best = NULL, *m;
	wl_list_for_each(m, &wo->modes, link) {
		if (m->width != w || m->height != h)
			continue;
		if (!best) {
			best = m;
		} else if (mhz) {
			if (abs(m->refresh - mhz) < abs(best->refresh - mhz))
				best = m;
		} else if (m->refresh > best->refresh ||
		           (m->refresh == best->refresh && m->preferred)) {
			best = m;
		}
	}
	if (best && mhz && abs(best->refresh - mhz) > 1000)
		return NULL;
	return best;
}

/* automatic scale */
static float output_auto_scale(struct wlr_output *wo)
{
	int w = wo->width, h = wo->height;
	if (w <= 0 || h <= 0) {
		struct wlr_output_mode *m = wlr_output_preferred_mode(wo);
		if (!m)
			return 1.0f;
		w = m->width;
		h = m->height;
	}
	if (wo->phys_width <= 0 || wo->phys_height <= 0)
		return 1.0f;

	/* the diagonal, so a rotated or oddly shaped panel still reads right */
	double px = sqrt((double)w * w + (double)h * h);
	double mm = sqrt((double)wo->phys_width * wo->phys_width +
	                 (double)wo->phys_height * wo->phys_height);
	double dpi = px / (mm / 25.4);
	return dpi >= 192 ? 2.0f : 1.0f;
}

/* evacuate output */
struct aro_output *output_evacuate(struct aro_server *s,
                                          struct aro_output *o)
{
	/* the views below are rehomed or parked and made visible or hidden
	 * there; only the flag is left. No slide_finish(): this runs from
	 * output_destroy, and scheduling a frame on a dying output is not
	 * something to rely on */
	o->slide.active = false;

	/* dismiss prompt on output loss */
	prompt_output_gone(s, o);
	switcher_output_gone(s, o);
	overview_output_gone(s, o);
	ghost_drop(s, o);

	/* clear grabs on output loss */
	if (s->grabbed && s->grabbed->output == o)
		grab_forget(s, s->grabbed);

	if (s->focused_output == o)
		s->focused_output = NULL;
	struct aro_output *dest = aro_focused_output(s);   /* now another */
	s->focused_output = dest;

	struct aro_view *v, *tmp;
	wl_list_for_each_safe(v, tmp, &s->views, link) {
		if (v->output != o)
			continue;

		if (!dest) {
			/* park views if no destination */
			v->output = NULL;
			v->fbox.x -= o->box.x;
			v->fbox.y -= o->box.y;
			v->pre_fs.x -= o->box.x;
			v->pre_fs.y -= o->box.y;
			view_set_visible(v, false);
			continue;
		}

		/* remove leaf before tree is freed */
		v->node = NULL;
		v->output = dest;
		v->workspace = dest->cur_ws;
		if (v->floating) {
			v->fbox.x += dest->box.x - o->box.x;
			v->fbox.y += dest->box.y - o->box.y;
		} else {
			v->node = tree_insert(s, dest, dest->cur_ws, v, NULL,
			                      LY_ROW, false);
		}
		view_set_visible(v, true);
	}

	if (dest) {
		for (int i = 0; i < ARO_MAX_WS; i++) {
			ly_free(o->ws[i]);
			o->ws[i] = NULL;
		}
	} else {
		for (int i = 0; i < ARO_MAX_WS; i++) {
			s->orphan_ws[i] = o->ws[i];
			o->ws[i] = NULL;
			s->orphan_ws_layout[i] = o->ws_layout[i];
		}
		s->orphan_cur_ws = o->cur_ws;
		s->parked = true;
		wlr_log(WLR_INFO, "last output gone — parking its workspaces");
	}
	o->cur_ws = 0;
	/* reset layout overrides */
	for (int i = 0; i < ARO_MAX_WS; i++)
		o->ws_layout[i] = Q_LAYOUT_INHERIT;

	if (s->focused && s->focused->output == NULL)
		aro_focus(s, dest ? output_pick_view(s, dest) : NULL);

	return dest;
}

/* disable output */
static void output_disable(struct aro_output *o)
{
	struct aro_server *s = o->server;
	struct wlr_output *wo = o->wlr_output;

	wl_list_remove(&o->link);
	wl_list_insert(&s->outputs_off, &o->link);
	o->enabled = false;

	struct aro_output *dest = output_evacuate(s, o);

	/* destroy layer surfaces on disabled output */
	struct aro_layer *l, *ltmp;
	wl_list_for_each_safe(l, ltmp, &s->layers, link) {
		if (l->layer_surface->output == wo)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	bar_return_cancel(o);
	bar_finish(&o->bar);
	memset(&o->bar, 0, sizeof o->bar);
	extws_output_gone(o);
	o->bar_snap = false;

	/* remove scene/layout entries */
	struct wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, wo);
	if (so)
		wlr_scene_output_destroy(so);
	wlr_output_layout_remove(s->output_layout, wo);

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, false);
	wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);

	if (dest) {
		update_backdrop(s);
		aro_arrange(s);
	}
	wlr_log(WLR_INFO, "output %s disabled", wo->name);
}

/* apply output config */
static bool output_configure(struct aro_output *o, const struct out_req *r,
                             bool test, char *why, size_t why_len)
{
	struct aro_server *s = o->server;
	struct wlr_output *wo = o->wlr_output;
	const bool enable = r->enabled < 0 ? o->enabled : r->enabled == 1;
	why[0] = '\0';

	if (!enable) {
		if (test)
			return true;
		if (o->enabled) {
			output_disable(o);
		} else {
			/* ensure disabled output is off */
			struct wlr_output_state st;
			wlr_output_state_init(&st);
			wlr_output_state_set_enabled(&st, false);
			wlr_output_commit_state(wo, &st);
			wlr_output_state_finish(&st);
		}
		output_mgr_update(s);
		return true;
	}

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);

	if (r->mode) {
		wlr_output_state_set_mode(&st, r->mode);
	} else if (r->custom) {
		wlr_output_state_set_custom_mode(&st, r->mode_w, r->mode_h, r->mode_mhz);
	} else if (r->mode_w > 0) {
		struct wlr_output_mode *m =
			output_find_mode(wo, r->mode_w, r->mode_h, r->mode_mhz);
		if (m) {
			wlr_output_state_set_mode(&st, m);
		} else if (wl_list_empty(&wo->modes)) {
			/* headless outputs accept custom mode */
			wlr_output_state_set_custom_mode(&st, r->mode_w, r->mode_h,
			                                 r->mode_mhz);
		} else {
			if (r->mode_mhz)
				snprintf(why, why_len, "no %dx%d mode at %.3g Hz",
				         r->mode_w, r->mode_h, r->mode_mhz / 1000.0);
			else
				snprintf(why, why_len, "no %dx%d mode",
				         r->mode_w, r->mode_h);
			wlr_output_state_finish(&st);
			return false;
		}
	} else if (r->mode_w < 0 || !o->enabled) {
		/* use preferred mode */
		struct wlr_output_mode *m = wlr_output_preferred_mode(wo);
		if (m)
			wlr_output_state_set_mode(&st, m);
	}
	if (r->scale > 0)
		wlr_output_state_set_scale(&st, r->scale);
	else if (r->scale < 0)
		wlr_output_state_set_scale(&st, output_auto_scale(wo));
	if (r->transform >= 0)
		wlr_output_state_set_transform(&st, (enum wl_output_transform)r->transform);
	if (r->adaptive_sync >= 0)
		wlr_output_state_set_adaptive_sync_enabled(&st, r->adaptive_sync == 1);
	if (r->hdr >= 0) {
		/* HDR: PQ in BT.2020, ten bits a channel; off: back to plain sRGB */
		const char *no = r->hdr == 1 ? hdr_unsupported(s, wo) : NULL;
		if (no)
			wlr_log(WLR_INFO, "%s: hdr stays off: %s", wo->name, no);
		const bool on = r->hdr == 1 && !no;
		static const struct wlr_output_image_description pq = {
			.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
			.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
		};
		if (on || wo->image_description) {
			wlr_output_state_set_image_description(&st, on ? &pq : NULL);
			wlr_output_state_set_render_format(&st, on ? DRM_FORMAT_XRGB2101010
			                                           : DRM_FORMAT_XRGB8888);
		}
	}

	bool ok = wlr_output_test_state(wo, &st);
	if (ok && !test)
		ok = wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);
	if (!ok) {
		snprintf(why, why_len, "the output refused that combination%s",
		         r->adaptive_sync == 1 ? " (adaptive_sync?)" : r->hdr == 1 ? " (hdr?)" : "");
		return false;
	}
	if (test)
		return true;

	/* ── placement ── */
	const bool was_on = o->enabled;
	const ly_box before = o->box;

	struct wlr_output_layout_output *lo =
		wlr_output_layout_get(s->output_layout, wo);
	if (r->has_pos)
		lo = wlr_output_layout_add(s->output_layout, wo, r->x, r->y);
	else if (r->pos_auto || !lo)
		lo = wlr_output_layout_add_auto(s->output_layout, wo);
	if (!was_on && lo) {
		struct wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, wo);
		if (!so)
			so = wlr_scene_output_create(s->scene, wo);
		if (so)
			wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
	}

	o->scale = wo->scale > 0 ? wo->scale : 1.0f;

	if (!was_on) {
		wl_list_remove(&o->link);
		wl_list_insert(&s->outputs, &o->link);
		o->enabled = true;
		o->lid_off = false;     /* whoever turned it on wins over the lid */

		/* refresh box before bar */
		output_refresh_box(o);

		/* create bar */
		if (!bar_create(&o->bar, o))
			wlr_log(WLR_ERROR, "could not build a bar for %s", wo->name);

		if (!s->focused_output)
			s->focused_output = o;
	}

	update_backdrop(s);

	if (!was_on) {
		output_adopt_parked(s, o);      /* after the boxes and layers are final */
	} else if (before.x != o->box.x || before.y != o->box.y) {
		/* shift floating views when output moves */
		int dx = o->box.x - before.x, dy = o->box.y - before.y;
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link) {
			if (v->output != o)
				continue;
			v->fbox.x += dx;
			v->fbox.y += dy;
			v->pre_fs.x += dx;
			v->pre_fs.y += dy;
		}
	}

	aro_arrange(s);

	/* snap views after output geometry change */
	if (was_on && !box_eq(before, o->box)) {
		slide_finish(o);        /* its span was the old width */
		struct aro_view *v;
		wl_list_for_each(v, &s->views, link)
			if (v->output == o && view_visible(v))
				anim_box_set(&v->geo, view_target(v));
	}

	output_mgr_update(s);
	wlr_log(WLR_INFO, "output %s: %dx%d at %d,%d, scale %.2f",
	        wo->name, o->box.w, o->box.h, o->box.x, o->box.y, o->scale);
	return true;
}

/* diff monitor block against last applied */
static bool monitor_req(const struct q_monitor_set *m,
                        const struct q_monitor_set *last, struct out_req *r)
{
	*r = (struct out_req)OUT_REQ_NONE;
	bool any = false;

	if (m->enabled != Q_RULE_UNSET &&
	    (!last || last->enabled != m->enabled)) {
		r->enabled = m->enabled;
		any = true;
	}
	if (m->mode_w && (!last || last->mode_w != m->mode_w ||
	                  last->mode_h != m->mode_h ||
	                  last->mode_mhz != m->mode_mhz)) {
		r->mode_w = m->mode_w;
		r->mode_h = m->mode_h;
		r->mode_mhz = m->mode_mhz;
		any = true;
	}
	if (m->has_pos && (!last || !last->has_pos ||
	                   last->x != m->x || last->y != m->y)) {
		r->has_pos = true;
		r->x = m->x;
		r->y = m->y;
		any = true;
	}
	if (m->pos_auto && (!last || !last->pos_auto)) {
		r->pos_auto = true;
		any = true;
	}
	if (m->scale != 0 && (!last || last->scale != m->scale)) {
		/* auto scale */
		r->scale = (float)m->scale;
		any = true;
	}
	if (m->transform != Q_RULE_UNSET &&
	    (!last || last->transform != m->transform)) {
		r->transform = m->transform;
		any = true;
	}
	if (m->adaptive_sync != Q_RULE_UNSET &&
	    (!last || last->adaptive_sync != m->adaptive_sync)) {
		r->adaptive_sync = m->adaptive_sync;
		any = true;
	}
	if (m->hdr != Q_RULE_UNSET && (!last || last->hdr != m->hdr)) {
		r->hdr = m->hdr;
		any = true;
	}
	return any;
}

static void monitor_eval(struct aro_output *o, struct q_monitor_set *out)
{
	struct wlr_output *wo = o->wlr_output;
	char desc[256];
	snprintf(desc, sizeof desc, "%s %s %s",
	         wo->make ? wo->make : "", wo->model ? wo->model : "",
	         wo->serial ? wo->serial : "");
	config_monitor_eval(&o->server->cfg, wo->name, desc, out);
}

/* apply monitor block */
static void monitor_apply(struct aro_output *o, bool initial)
{
	struct aro_server *s = o->server;
	const char *name = o->wlr_output->name;

	struct q_monitor_set m;
	monitor_eval(o, &m);

	struct out_req r;
	const bool fresh = initial || !o->mon_applied;
	bool any = monitor_req(&m, fresh ? NULL : &o->mon_last, &r);
	if (initial && r.enabled < 0)
		r.enabled = 1;

	/* never disable last active output */
	bool refused = false;
	if (r.enabled == 0) {
		int lit = wl_list_length(&s->outputs) - (o->enabled ? 1 : 0);
		struct aro_output *lo;
		wl_list_for_each(lo, &s->outputs_off, link)
			lit += lo->lid_off;     /* lid_update turns it back on */
		if (lit == 0) {
			notify(s, NOTIFY_ERROR,
			       "monitor %s: not turning off the only screen that is on",
			       name);
			r.enabled = initial ? 1 : -1;
			refused = true;
		}
	}

	if (!initial && !any)
		return;

	char why[128];
	bool ok = output_configure(o, &r, false, why, sizeof why);
	if (!ok) {
		if (initial) {
			notify(s, NOTIFY_ERROR, "monitor %s: %s — using its "
			       "preferred mode instead", name, why);
			struct out_req plain = OUT_REQ_NONE;
			plain.enabled = 1;
			if (!output_configure(o, &plain, false, why, sizeof why))
				wlr_log(WLR_ERROR, "output %s would not come on: %s",
				        name, why);
		} else {
			notify(s, NOTIFY_ERROR, "monitor %s: %s", name, why);
		}
	}

	if (ok && !refused) {
		if (o->enabled) {
			o->mon_last = m;
		} else {
			/* remember only enabled while off */
			if (fresh)
				config_monitor_unset(&o->mon_last);
			o->mon_last.enabled = m.enabled;
		}
		o->mon_applied = true;
	}
}

/* reapply monitor blocks */
void monitors_reapply(struct aro_server *s)
{
	int n = wl_list_length(&s->outputs) + wl_list_length(&s->outputs_off);
	if (n == 0)
		return;
	struct aro_output **all = calloc(n, sizeof *all);
	if (!all)
		return;

	/* snapshot outputs before reapplying */
	int i = 0;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		all[i++] = o;
	wl_list_for_each(o, &s->outputs_off, link)
		all[i++] = o;

	for (int pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			struct q_monitor_set m;
			monitor_eval(all[i], &m);
			if ((m.enabled == 0) != (pass == 1))
				continue;
			monitor_apply(all[i], false);
		}
	}
	free(all);
	lid_update(s);
}

/* apply output management config */
static void output_mgr_apply_or_test(struct aro_server *s,
                                     struct wlr_output_configuration_v1 *cfg,
                                     bool test)
{
	bool ok = true;
	struct wlr_output_configuration_head_v1 *h;

	int lit = 0;
	wl_list_for_each(h, &cfg->heads, link)
		if (h->state.enabled)
			lit++;
	if (lit == 0) {
		wlr_log(WLR_ERROR, "output management: refusing to turn every "
		        "output off");
		ok = false;
	}

	/* enable before disable */
	for (int pass = 0; ok && pass < 2; pass++) {
		wl_list_for_each(h, &cfg->heads, link) {
			if (h->state.enabled != (pass == 0))
				continue;
			struct aro_output *o = output_from_wlr(s, h->state.output);
			if (!o) {
				ok = false;
				break;
			}

			struct out_req r = OUT_REQ_NONE;
			r.enabled = h->state.enabled;
			if (h->state.enabled) {
				if (h->state.mode) {
					r.mode = h->state.mode;
				} else if (h->state.custom_mode.width > 0) {
					r.custom = true;
					r.mode_w = h->state.custom_mode.width;
					r.mode_h = h->state.custom_mode.height;
					r.mode_mhz = h->state.custom_mode.refresh;
				}
				r.has_pos = true;
				r.x = h->state.x;
				r.y = h->state.y;
				r.scale = h->state.scale;
				r.transform = h->state.transform;
				r.adaptive_sync = h->state.adaptive_sync_enabled;
			}

			char why[128];
			if (!output_configure(o, &r, test, why, sizeof why)) {
				wlr_log(WLR_ERROR, "output management: %s: %s",
				        o->wlr_output->name, why);
				ok = false;
				break;
			}
		}
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(cfg);
	else
		wlr_output_configuration_v1_send_failed(cfg);
	wlr_output_configuration_v1_destroy(cfg);

	/* update client state even on failure */
	output_mgr_update(s);
}

void output_mgr_apply(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, output_mgr_apply);
	output_mgr_apply_or_test(s, data, false);
}

void output_mgr_test(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, output_mgr_test);
	output_mgr_apply_or_test(s, data, true);
}

/* new outputs start off until monitor blocks apply */
void new_output(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_output);
	struct wlr_output *wlr_output = data;

	/* a VR headset is not part of the desktop: only lent to a VR runtime */
	if (wlr_output->non_desktop) {
		if (s->drm_lease && wlr_drm_lease_v1_manager_offer_output(s->drm_lease, wlr_output))
			wlr_log(WLR_INFO, "output: %s offered for lease (non-desktop)", wlr_output->name);
		return;
	}

	wlr_output_init_render(wlr_output, s->allocator, s->renderer);

	struct aro_output *o = calloc(1, sizeof *o);
	if (!o)
		return;
	o->server = s;
	o->wlr_output = wlr_output;
	o->scale = 1.0f;
	o->cur_ws = 0;
	for (int i = 0; i < ARO_MAX_WS; i++)
		o->ws_layout[i] = Q_LAYOUT_INHERIT;

	o->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &o->destroy);
	wl_list_insert(&s->outputs_off, &o->link);

	/* log output identifiers */
	wlr_log(WLR_INFO, "output: name=\"%s\" desc=\"%s %s %s\"",
	        wlr_output->name,
	        wlr_output->make ? wlr_output->make : "",
	        wlr_output->model ? wlr_output->model : "",
	        wlr_output->serial ? wlr_output->serial : "");

	monitor_apply(o, true);
	output_mgr_update(s);
	lid_update(s);
}

/* output containing a point */
struct aro_output *output_at(struct aro_server *s, double x, double y)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (x >= o->box.x && x < o->box.x + o->box.w &&
		    y >= o->box.y && y < o->box.y + o->box.h)
			return o;
	}
	return aro_focused_output(s);
}

/* cache output box in layout coordinates */
void output_refresh_box(struct aro_output *o)
{
	struct wlr_box box = { 0 };
	wlr_output_layout_get_box(o->server->output_layout, o->wlr_output, &box);
	o->box = (ly_box){ box.x, box.y, box.width, box.height };
}

/* resize root backdrop to cover the layout */
void update_backdrop(struct aro_server *s)
{
	if (!s->root_bg)
		return;
	struct wlr_box box = { 0 };
	wlr_output_layout_get_box(s->output_layout, NULL, &box);
	if (box.width <= 0 || box.height <= 0)
		return;
	wlr_scene_node_set_position(&s->root_bg->node, box.x, box.y);
	wlr_scene_rect_set_size(s->root_bg, box.width, box.height);

	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		output_refresh_box(o);
	/* also places the bars */
	arrange_layers(s);

	/* lock uses refreshed boxes too */
	lock_arrange(s);
}

/* built-in panels, by connector name */
static bool output_is_internal(const struct wlr_output *wo)
{
	return !strncmp(wo->name, "eDP-", 4) || !strncmp(wo->name, "LVDS-", 5) ||
	       !strncmp(wo->name, "DSI-", 4);
}

/* lid shut with another screen on: built-in panels off, else back on */
void lid_update(struct aro_server *s)
{
	bool other = false;
	struct aro_output *o, *tmp;
	wl_list_for_each(o, &s->outputs, link)
		other |= !output_is_internal(o->wlr_output);

	if (s->lid_closed && s->cfg.lid_switch && other) {
		wl_list_for_each_safe(o, tmp, &s->outputs, link) {
			if (!output_is_internal(o->wlr_output))
				continue;
			struct out_req r = OUT_REQ_NONE;
			r.enabled = 0;
			char why[128];
			output_configure(o, &r, false, why, sizeof why);
			o->lid_off = true;
			wlr_log(WLR_INFO, "lid: %s off", o->wlr_output->name);
		}
		return;
	}
	wl_list_for_each_safe(o, tmp, &s->outputs_off, link) {
		if (!o->lid_off)
			continue;
		wlr_log(WLR_INFO, "lid: %s back on", o->wlr_output->name);
		monitor_apply(o, true);         /* its whole monitor block */
		o->lid_off = false;
	}
}
