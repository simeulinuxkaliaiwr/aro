/* bar.h: status bar */
#ifndef ARO_BAR_H
#define ARO_BAR_H

#include <stdbool.h>

#include "config.h"
#include "text.h"

struct aro_server;
struct aro_output;
struct wlr_scene_tree;
struct wlr_scene_rect;

struct bar_ws {
	struct wlr_scene_rect *pill;
	struct qtext label;
};

struct aro_bar {
	struct aro_output *output;   /* for the theme and the scene layer */
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *bg;
	struct bar_ws ws[ARO_MAX_WS];
	int nws;                        /* how many pills exist, = ARO_MAX_WS */
	int shown;                      /* how many are currently drawn */

	struct qtext title;
	struct qtext clock;

	int x, y, w;
	float scale;
};

bool bar_create(struct aro_bar *b, struct aro_output *o);
void bar_place(struct aro_bar *b, int x, int y, int w, float scale);
/* update bar from output state */
void bar_update(struct aro_bar *b, struct aro_output *o);
void bar_retheme(struct aro_bar *b);
void bar_finish(struct aro_bar *b);

#endif
