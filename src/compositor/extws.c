/* extws.c: aro's workspaces over ext-workspace-v1, for waybar and other bars */
#include "scene.h"

#include "extws.h"
#include "aro.h"

#include <stdio.h>

#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* which output and slot a handle stands for; NULL if none */
static struct aro_output *handle_owner(struct aro_server *s,
                                       struct wlr_ext_workspace_handle_v1 *h,
                                       int *ws)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		for (int i = 0; i < ARO_MAX_WS; i++)
			if (o->extws[i] == h) {
				*ws = i;
				return o;
			}
	return NULL;
}

/* a bar asked for something; aro only offers activate */
static void handle_commit(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, extws_commit);
	struct wlr_ext_workspace_v1_commit_event *ev = data;

	struct wlr_ext_workspace_v1_request *r;
	wl_list_for_each(r, ev->requests, link) {
		if (r->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE ||
		    !r->activate.workspace)
			continue;
		int ws;
		struct aro_output *o = handle_owner(s, r->activate.workspace, &ws);
		if (!o)
			continue;
		s->focused_output = o;
		aro_run_action(s, &(struct q_bind){ .action = Q_WORKSPACE, .num = ws });
	}
}

void extws_init(struct aro_server *s)
{
	s->extws_mgr = wlr_ext_workspace_manager_v1_create(s->display, 1);
	if (!s->extws_mgr) {
		wlr_log(WLR_ERROR, "ext-workspace unavailable; bars will not see workspaces");
		return;
	}
	s->extws_commit.notify = handle_commit;
	wl_signal_add(&s->extws_mgr->events.commit, &s->extws_commit);
}

static void slot_drop(struct aro_output *o, int i)
{
	if (!o->extws[i])
		return;
	wlr_ext_workspace_handle_v1_destroy(o->extws[i]);
	o->extws[i] = NULL;
	o->extws_active[i] = false;
}

void extws_output_gone(struct aro_output *o)
{
	for (int i = 0; i < ARO_MAX_WS; i++)
		slot_drop(o, i);
	if (o->extws_group) {
		wlr_ext_workspace_group_handle_v1_destroy(o->extws_group);
		o->extws_group = NULL;
	}
}

static void output_sync(struct aro_server *s, struct aro_output *o)
{
	if (!o->extws_group) {
		o->extws_group = wlr_ext_workspace_group_handle_v1_create(s->extws_mgr, 0);
		if (!o->extws_group)
			return;
		wlr_ext_workspace_group_handle_v1_output_enter(o->extws_group,
		                                              o->wlr_output);
	}

	/* what the bar shows: the configured count, plus current and occupied ones */
	bool occupied[ARO_MAX_WS] = { 0 };
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && v->output == o && v->workspace >= 0 &&
		    v->workspace < ARO_MAX_WS)
			occupied[v->workspace] = true;

	int last = s->cfg.workspaces - 1;
	for (int i = 0; i < ARO_MAX_WS; i++)
		if (occupied[i] || o->ws[i] || i == o->cur_ws)
			last = i > last ? i : last;
	if (last >= ARO_MAX_WS)
		last = ARO_MAX_WS - 1;

	for (int i = 0; i < ARO_MAX_WS; i++) {
		if (i > last) {
			slot_drop(o, i);
			continue;
		}
		if (!o->extws[i]) {
			char id[64], name[8];
			snprintf(id, sizeof id, "%s:%d", o->wlr_output->name, i + 1);
			snprintf(name, sizeof name, "%d", i + 1);
			o->extws[i] = wlr_ext_workspace_handle_v1_create(s->extws_mgr, id,
				EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
			if (!o->extws[i])
				continue;
			uint32_t coord = (uint32_t)i;
			wlr_ext_workspace_handle_v1_set_group(o->extws[i], o->extws_group);
			wlr_ext_workspace_handle_v1_set_name(o->extws[i], name);
			wlr_ext_workspace_handle_v1_set_coordinates(o->extws[i], &coord, 1);
			o->extws_active[i] = false;
		}
		/* only changes go out: every arrange lands here */
		bool active = i == o->cur_ws;
		if (active != o->extws_active[i]) {
			wlr_ext_workspace_handle_v1_set_active(o->extws[i], active);
			o->extws_active[i] = active;
		}
	}
}

void extws_sync(struct aro_server *s)
{
	if (!s->extws_mgr)
		return;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		output_sync(s, o);
}

void extws_finish(struct aro_server *s)
{
	if (!s->extws_mgr)
		return;
	wl_list_remove(&s->extws_commit.link);
	s->extws_mgr = NULL;
}
