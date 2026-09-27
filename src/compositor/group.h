/* group.h: tabbed groups, several tiled windows sharing one leaf of the tree */
#ifndef ARO_GROUP_H
#define ARO_GROUP_H

#include <stdbool.h>

#include "layout.h"
#include "text.h"

#define ARO_GROUP_MAX 16

struct aro_server;
struct aro_view;
struct wlr_scene_tree;
struct wlr_scene_rect;

/* every member's node is the same leaf, whose user is the active one */
struct aro_group {
	struct aro_view *v[ARO_GROUP_MAX];      /* in tab order */
	int n;
	struct aro_view *active;

	/* the tabs, drawn in the active member's header */
	struct wlr_scene_tree *tabs;
	struct qtext label[ARO_GROUP_MAX];
	struct wlr_scene_rect *sep[ARO_GROUP_MAX];
	struct wlr_scene_rect *mark;            /* under the active tab */
	int nlabels;
	int edge[ARO_GROUP_MAX + 1];            /* tab boundaries, frame-relative */
};

/* a group member hidden behind another tab */
bool view_tab_hidden(const struct aro_view *v);

/* out of the tree, or out of its group; the leaf focus should go to, or NULL */
ly_node *view_detach(struct aro_view *v);

/* v, out of the tree, into into's tile as the active tab; false if full */
bool group_join(struct aro_server *s, struct aro_view *v, struct aro_view *into);

/* show v's tab, hiding the rest */
void group_activate(struct aro_server *s, struct aro_view *v);

/* the next (dir > 0) or previous tab of v's group, wrapping */
struct aro_view *group_step(struct aro_view *v, int dir);

/* the tab under x, a frame-relative position in the header; -1 if none */
int group_tab_at(struct aro_view *v, int x);

#endif
