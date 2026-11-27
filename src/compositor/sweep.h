/* sweep.h: draw a box, and the next window opens in it at that size */
#ifndef ARO_SWEEP_H
#define ARO_SWEEP_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"

struct aro_server;
struct aro_output;
struct aro_view;
struct wl_event_source;

enum sweep_state {
	SWEEP_OFF,
	SWEEP_ARMED,            /* crosshair: waiting for the press */
	SWEEP_DRAWING,          /* button held: the box follows the pointer */
	SWEEP_WAITING,          /* drawn: the next new window takes it */
};

struct aro_sweep {
	enum sweep_state state;
	double x0, y0;                  /* where the press was */
	ly_box box;                     /* the frame, layout coordinates */
	char *cmd;                      /* run once the box is drawn; owned */
	struct aro_view *held;          /* sweep_new: the window waiting for this box */
	uint32_t toast;                 /* ...and the toast saying so */
	struct wl_event_source *timer;  /* a box nothing takes goes away */
};

void sweep_init(struct aro_server *s);
void sweep_finish(struct aro_server *s);

/* the sweep action: cmd may be NULL (the next window, whoever opens it) */
void sweep_start(struct aro_server *s, const char *cmd);
void sweep_cancel(struct aro_server *s);

/* armed or drawing: the pointer belongs to the sweep */
bool sweep_active(struct aro_server *s);
void sweep_pointer_motion(struct aro_server *s, double lx, double ly);
void sweep_pointer_button(struct aro_server *s, uint32_t button, bool pressed);
/* true if the key was the sweep's */
bool sweep_key(struct aro_server *s, xkb_keysym_t sym);

/* a new window takes the waiting box: fills *box (the frame) and returns true */
bool sweep_take(struct aro_server *s, struct aro_view *v, ly_box *box);

/* sweep_new: at its first commit, whether v waits for a box; true means it was told nothing */
bool sweep_hold(struct aro_server *s, struct aro_view *v);
/* a window going away must not be waited for */
void sweep_view_gone(struct aro_server *s, struct aro_view *v);

void sweep_retheme(struct aro_server *s);
void sweep_output_gone(struct aro_server *s, struct aro_output *o);

#endif
