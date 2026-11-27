/* map.h: layout = canvas, mod+o: the plane drawn as a map, its windows joined by soft lines */
#ifndef ARO_MAP_H
#define ARO_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"
#include "text.h"

struct aro_server;
struct aro_output;
struct aro_view;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;

struct map_item {
	struct aro_view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *edge;    /* the card's frame; the accent when chosen */
	struct ui_snap *snap;           /* the window, live */
	struct qtext label;
	ly_box at;                      /* its card, on the map */
};

struct aro_map {
	bool open;                      /* taking input */
	bool shown;                     /* drawn; also while closing */
	double p, p_from, p_to;         /* 0 the screen, 1 the map */
	uint32_t p_start;

	struct aro_output *o;
	int ws;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *scrim;
	struct wlr_scene_buffer *art;   /* the dots, the lines, where the screen is */
	struct map_item *items;
	int n;

	/* plane to map: m = o + (b - x0) * k */
	double k, ox, oy;
	int x0, y0;

	int sel;                        /* chosen card, or -1 */
	bool pressed;
	double press_x, press_y;
};

void map_init(struct aro_server *s);
void map_finish(struct aro_server *s);

/* whether mod+o shows the map here: the focused screen shows a canvas */
bool map_wants(struct aro_server *s);
void map_toggle(struct aro_server *s);
void map_close(struct aro_server *s);

bool map_active(struct aro_server *s);  /* open, taking input */
bool map_tick(struct aro_server *s, uint32_t now);

void map_pointer_motion(struct aro_server *s, double x, double y);
void map_pointer_button(struct aro_server *s, uint32_t button, bool pressed);
void map_key(struct aro_server *s, uint32_t mods, xkb_keysym_t sym);

void map_view_commit(struct aro_server *s, struct aro_view *v);
void map_view_gone(struct aro_server *s, struct aro_view *v);
void map_output_gone(struct aro_server *s, struct aro_output *o);

#endif
