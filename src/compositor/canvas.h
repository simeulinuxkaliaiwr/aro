/* canvas.h: layout = canvas, floating windows on an endless plane, the screen a camera */
#ifndef ARO_CANVAS_H
#define ARO_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

struct aro_server;
struct aro_output;
struct aro_view;
struct wlr_surface;

bool ws_canvas(struct aro_server *s, struct aro_output *o, int ws);
/* on the plane of a canvas workspace: moves when the camera does */
bool view_on_plane(struct aro_view *v);

/* tiled windows on a canvas float where they are; floated ones off a canvas tile again */
void canvas_reconcile(struct aro_server *s);
/* a new window on a canvas: b, moved off any window already in that spot */
ly_box canvas_spot(struct aro_view *v, ly_box b);

/* the plane moves by dx, dy under the camera; windows follow at once */
void canvas_pan(struct aro_server *s, struct aro_output *o, double dx, double dy);
/* the camera a third of the screen that way, or back home when home */
void canvas_glide(struct aro_server *s, struct aro_output *o, ly_edge e, bool home);
/* bring v into view, flying; *dx, *dy (may be NULL) say how far it will move */
void canvas_show(struct aro_server *s, struct aro_view *v, int *dx, int *dy);
/* per frame; true while still moving */
bool canvas_tick(struct aro_server *s, struct aro_output *o, uint32_t now);

/* the empty desktop of a canvas workspace under the cursor, or NULL;
 * surface is what view_at found there */
struct aro_output *canvas_under_cursor(struct aro_server *s, struct wlr_surface *surface);
/* dragging the desktop */
void canvas_pan_begin(struct aro_server *s, struct aro_output *o);
void canvas_pan_motion(struct aro_server *s);
void canvas_pan_end(struct aro_server *s);
/* a wheel or a two-finger scroll over the desktop */
void canvas_scroll(struct aro_server *s, struct aro_output *o, bool horizontal,
                   double delta, bool finger);

/* a touchpad swipe on a canvas moves the plane */
bool canvas_swipe_begin(struct aro_server *s, uint32_t time);
void canvas_swipe_update(struct aro_server *s, double dx, double dy, uint32_t time);
void canvas_swipe_end(struct aro_server *s, bool cancelled);

/* a dragged window let go over another screen moves there; a tiling one keeps it floating */
void canvas_drag_end(struct aro_server *s, struct aro_view *v);

/* the dots on each screen showing a canvas, where the camera has them */
void canvas_grid_sync(struct aro_server *s);

/* an output going away: no grab or swipe may keep pointing at it */
void canvas_forget_output(struct aro_server *s, struct aro_output *o);

#endif
