/* overview.h: zoomed-out view of every workspace */
#ifndef ARO_OVERVIEW_H
#define ARO_OVERVIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "layout.h"
#include "text.h"

struct aro_server;
struct aro_output;
struct aro_view;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wlr_scene_shadow;
struct wlr_surface;
struct aro_layer;

#define OV_MAX_WALL 4

struct ov_item {
	struct aro_view *view;
	int slot;                       /* index into the output's cards */
	struct wlr_scene_rect *edge, *bg, *ring;
	struct wlr_scene_buffer *snap;
	bool chrome;                    /* false for fullscreen */
	ly_box drawn;
};

struct ov_card {
	int ws;
	struct wlr_scene_shadow *shadow;        /* effects only */
	struct wlr_scene_rect *edge, *bg;
	/* the output's background layer, inside the card */
	struct wlr_scene_buffer *wall[OV_MAX_WALL];
	struct aro_layer *wall_src[OV_MAX_WALL];
	int nwall;
	struct qtext label;
	ly_box drawn;
};

struct ov_output {
	struct aro_output *output;
	struct wlr_scene_tree *tree;
	struct wlr_scene_blur *blur;            /* effects only */
	struct wlr_scene_rect *backdrop;
	struct ov_card cards[ARO_MAX_WS];
	int ncards;
	struct ov_item *items;
	int nitems;
	double c, c_from, c_to;         /* strip position, in cards */
	uint32_t c_start;
};

struct aro_overview {
	bool open;                      /* taking input */
	bool shown;                     /* drawn; also while closing */
	double p, p_from, p_to;         /* 0 normal, 1 zoomed out */
	uint32_t p_start;

	struct ov_output *outs;
	int nouts;

	/* selection: a window, or an empty workspace */
	struct aro_output *sel_out;
	struct aro_view *sel_view;
	int sel_ws;

	/* click acts on release over the same target */
	struct aro_view *press_view;
	struct aro_output *press_out;
	int press_ws;
	bool pressed;
	double scroll;
};

void overview_toggle(struct aro_server *s);
bool overview_active(struct aro_server *s);     /* open, taking input */
bool overview_shown(struct aro_server *s);      /* drawn, maybe closing */

bool overview_tick(struct aro_server *s, uint32_t now);
void overview_frame_done(struct aro_server *s, struct aro_output *o,
                         struct timespec *now);

void overview_key(struct aro_server *s, uint32_t mods, xkb_keysym_t sym);
void overview_pointer_button(struct aro_server *s, double x, double y,
                             bool pressed);
void overview_pointer_axis(struct aro_server *s, double delta, int discrete);

void overview_view_commit(struct aro_server *s, struct aro_view *v);
void overview_layer_commit(struct aro_server *s, struct wlr_surface *surface);
void overview_rebuild(struct aro_server *s);    /* map, unmap, reload */
void overview_output_gone(struct aro_server *s, struct aro_output *o);
void overview_finish(struct aro_server *s);

#endif
