/* switcher.h: mod+tab window switcher and the recently-used order */
#ifndef ARO_SWITCHER_H
#define ARO_SWITCHER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"
#include "text.h"

struct aro_server;
struct aro_output;
struct aro_view;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;

struct switcher_tile {
	struct wlr_scene_rect *edge, *bg, *ring;
	struct wlr_scene_buffer *preview;       /* NULL: no buffer to show */
	struct qtext title;
	ly_box box;                             /* relative to the card */
};

struct aro_switcher {
	/* recently used, most recent first: aro_view.mru_link */
	struct wl_list mru;
	struct aro_view *pending;               /* focused, not yet committed */
	struct wl_event_source *debounce;

	/* while mod+tab is held */
	bool active;
	struct aro_view **items;                /* snapshot at open; [0] is focused */
	int n, sel;
	struct aro_output *output;              /* the card is centred here */
	struct wl_event_source *open_timer;

	/* the card; NULL until the open delay has passed */
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *edge, *bg;
	struct switcher_tile *tiles;            /* items[first .. first+count) */
	int first, count;
};

void switcher_init(struct aro_server *s);
void switcher_finish(struct aro_server *s);

/* the bind: open on the first press, move the selection after that */
void switcher_step(struct aro_server *s, int dir);
bool switcher_active(struct aro_server *s);

/* a key pressed while open, other than a switch bind */
void switcher_key(struct aro_server *s, xkb_keysym_t sym);
/* releasing mod commits */
void switcher_modifiers(struct aro_server *s, uint32_t mods);

void switcher_retheme(struct aro_server *s);
void switcher_output_gone(struct aro_server *s, struct aro_output *o);

/* recently-used order */
void mru_add(struct aro_server *s, struct aro_view *v);         /* on map */
void mru_remove(struct aro_server *s, struct aro_view *v);      /* on unmap */
void mru_focus(struct aro_server *s, struct aro_view *v);       /* debounced */
void mru_touch(struct aro_server *s);   /* input into the focused window */

#endif
