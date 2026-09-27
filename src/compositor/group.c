/* group.c: tabbed groups; the tree keeps one leaf, the group decides which window shows */
#include "scene.h"

#include "group.h"
#include "aro.h"
#include "core.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

bool view_tab_hidden(const struct aro_view *v)
{
	return v->group && v->group->active != v;
}

static int index_of(const struct aro_group *g, const struct aro_view *v)
{
	for (int i = 0; i < g->n; i++)
		if (g->v[i] == v)
			return i;
	return -1;
}

/* is v's workspace the one on screen */
static bool on_shown_ws(const struct aro_view *v)
{
	return v->mapped && v->output && v->workspace == v->output->cur_ws;
}

void group_activate(struct aro_server *s, struct aro_view *v)
{
	struct aro_group *g = v->group;
	if (!g)
		return;
	struct aro_view *was = g->active;
	if (was && was != v && was->fullscreen)
		view_set_fullscreen(s, was, false);     /* a hidden tab cannot stay fullscreen */
	g->active = v;
	if (v->node)
		v->node->user = v;
	for (int i = 0; i < g->n; i++)
		view_set_visible(g->v[i], g->v[i] == v && on_shown_ws(v));
	if (was != v) {
		/* the new tab starts where the old one is drawn */
		if (was)
			v->geo = was->geo;
		else
			anim_box_set(&v->geo, view_target(v));
	}
	ui_frame_tabs(v);
}

/* drop a group of one, and the tabs with it */
static void group_dissolve(struct aro_group *g)
{
	struct aro_view *last = g->n == 1 ? g->v[0] : NULL;
	for (int i = 0; i < g->nlabels; i++)
		qtext_finish(&g->label[i]);
	if (g->tabs)
		wlr_scene_node_destroy(&g->tabs->node);
	free(g);
	if (last) {
		last->group = NULL;
		ui_frame_retheme(last);         /* its own title comes back */
	}
}

/* take v out of its group; the group, if any left, keeps the leaf */
static void group_remove(struct aro_view *v)
{
	struct aro_group *g = v->group;
	int i = index_of(g, v);
	memmove(&g->v[i], &g->v[i + 1], (size_t)(g->n - i - 1) * sizeof g->v[0]);
	g->n--;
	v->group = NULL;
	ui_frame_retheme(v);

	if (g->active == v) {
		struct aro_view *next = g->v[i < g->n ? i : g->n - 1];
		g->active = NULL;
		group_activate(v->server, next);
	} else {
		ui_frame_tabs(g->active);
	}
	if (g->n == 1)
		group_dissolve(g);
}

ly_node *view_detach(struct aro_view *v)
{
	if (!v->node)
		return NULL;
	ly_node *next;
	if (v->group) {
		next = v->node;         /* the group stays where it was */
		group_remove(v);
	} else {
		next = ly_close(view_ws_root(v), v->node);
	}
	v->node = NULL;
	return next;
}

bool group_join(struct aro_server *s, struct aro_view *v, struct aro_view *into)
{
	if (!into->node || v->node || v == into)
		return false;
	struct aro_group *g = into->group;
	if (!g) {
		g = calloc(1, sizeof *g);
		if (!g)
			return false;
		g->v[g->n++] = into;
		g->active = into;
		into->group = g;
	}
	if (g->n == ARO_GROUP_MAX)
		return false;
	g->v[g->n++] = v;
	v->group = g;
	v->node = into->node;
	v->output = into->output;
	v->workspace = into->workspace;
	group_activate(s, v);
	wlr_log(WLR_INFO, "group: %d tabs", g->n);
	return true;
}

struct aro_view *group_step(struct aro_view *v, int dir)
{
	struct aro_group *g = v->group;
	if (!g)
		return v;
	int i = index_of(g, g->active);
	return g->v[((i + (dir > 0 ? 1 : -1)) % g->n + g->n) % g->n];
}

int group_tab_at(struct aro_view *v, int x)
{
	struct aro_group *g = v->group;
	if (!g)
		return -1;
	for (int i = 0; i < g->n && i < g->nlabels; i++)
		if (x >= g->edge[i] && x < g->edge[i + 1])
			return i;
	return -1;
}
