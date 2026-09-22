/*
 * aro — prompt.h
 *
 * A modal yes/no card: "Exit aro?", with a scrim over the desktop.
 *
 * Generic on purpose. The caller supplies the words and what YES does; this
 * file owns how it looks, how it animates and how it answers keys and
 * clicks. Quitting is the first caller, not the only possible one.
 *
 * While a prompt is up it takes the keyboard and the pointer — every key,
 * every click, releases included, so no client ever sees half a keystroke.
 * The session lock still wins: the card lives in the notify layer, below
 * the overlay, and aro.c routes nothing to it while locked.
 *
 * Answers:
 *   yes  y, Enter, keypad Enter, or a click on the primary pill
 *   no   n, Escape, a click on the other pill, or a click on the scrim
 */
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

/*
 * Put a card up on `o`. Strings are copied. Returns false if it could not
 * be built, in which case nothing is on screen and nothing was grabbed —
 * the caller decides whether that means "go ahead" or "do nothing".
 * Opening while one is already up returns false and changes nothing.
 */
bool prompt_open(struct aro_server *s, struct aro_output *o,
                 const char *title, const char *detail,
                 const char *yes_label, const char *no_label,
                 void (*on_yes)(struct aro_server *s));

void prompt_close(struct aro_server *s);
bool prompt_active(struct aro_server *s);

/* Routing. Each consumes what it is given; the caller forwards nothing to
 * clients while prompt_active() is true. A level-0 keysym, as bindings use. */
void prompt_key(struct aro_server *s, xkb_keysym_t sym);
void prompt_pointer_motion(struct aro_server *s, double lx, double ly);
void prompt_pointer_button(struct aro_server *s, double lx, double ly,
                           bool pressed);

/* Advance the animation; true while anything is still moving. */
bool prompt_tick(struct aro_server *s, uint32_t now);

/* After a config reload: rebuilt in the new theme, without animating. */
void prompt_retheme(struct aro_server *s);

/* The output the card is on is going away. */
void prompt_output_gone(struct aro_server *s, struct aro_output *o);

void prompt_finish(struct aro_server *s);

#endif
