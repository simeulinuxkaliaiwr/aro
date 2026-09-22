/* prompt.h: modal yes/no card */
#ifndef ARO_PROMPT_H
#define ARO_PROMPT_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "anim.h"
#include "layout.h"
#include "text.h"

struct aro_server;
struct aro_output;
struct wlr_scene_tree;
struct wlr_scene_rect;

struct prompt_pill {
	struct wlr_scene_rect *edge, *bg;
	struct qtext label, hint;       /* "Exit" and, dimmer, "y" */
	ly_box box;                     /* relative to the card's final box */
	bool primary;                   /* the Enter answer: gets the accent */
};

struct aro_prompt {
	bool active;
	struct aro_output *output;   /* the card is centred on this one */
	void (*on_yes)(struct aro_server *s);

	/* owned; kept so a config reload can rebuild the card in the new
	 * theme without the caller having to ask again */
	char *title, *detail, *yes_label, *no_label;

	struct wlr_scene_tree *tree;    /* in l_notify, raised to the top */
	struct wlr_scene_rect *scrim;   /* over every output */
	struct wlr_scene_tree *card;    /* fixed at the final box's origin */
	struct wlr_scene_rect *edge, *bg, *ring;
	struct wlr_scene_tree *content; /* text and pills, shown once landed */
	struct qtext title_t, detail_t;
	struct prompt_pill yes, no;

	ly_box final;                   /* where the card settles */
	anim_box geo;                   /* where the card is now */
	bool content_shown;
	bool scrim_full;
	uint32_t opened_ms;

	int hover;                      /* 0 none, 1 yes, 2 no */
	int pressed;                    /* what the button went down on;
	                                   -1 is the scrim */
};

/* open prompt */
bool prompt_open(struct aro_server *s, struct aro_output *o,
                 const char *title, const char *detail,
                 const char *yes_label, const char *no_label,
                 void (*on_yes)(struct aro_server *s));

void prompt_close(struct aro_server *s);
bool prompt_active(struct aro_server *s);

/* input routing */
void prompt_key(struct aro_server *s, xkb_keysym_t sym);
void prompt_pointer_motion(struct aro_server *s, double lx, double ly);
void prompt_pointer_button(struct aro_server *s, double lx, double ly,
                           bool pressed);

/* tick prompt animation */
bool prompt_tick(struct aro_server *s, uint32_t now);

/* retheme prompt */
void prompt_retheme(struct aro_server *s);

/* prompt output removed */
void prompt_output_gone(struct aro_server *s, struct aro_output *o);

void prompt_finish(struct aro_server *s);

#endif
