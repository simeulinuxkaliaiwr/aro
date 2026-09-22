/*
 * aro — text.h
 *
 * Turning a string into something the scene graph can draw.
 *
 * wlroots has no text. The standard route is: render with pango-cairo into
 * an image surface, wrap that surface in a wlr_buffer, hand the buffer to a
 * wlr_scene_buffer. This file is that route, behind an interface where you
 * only ever say "this node should say X".
 *
 * A qtext caches what it last drew and re-renders only when something it
 * actually depends on changes. That matters: text is the most expensive
 * thing on screen and geometry changes 60 times a second during animation.
 */
#ifndef ARO_TEXT_H
#define ARO_TEXT_H

#include <stdbool.h>
#include <stdint.h>

struct wlr_scene_tree;
struct wlr_scene_buffer;

struct qtext {
	struct wlr_scene_buffer *node;
	const char *font;               /* pango description, e.g. "Sans 10" */

	char *text;                     /* what is currently drawn */
	uint32_t color;
	float scale;
	int max_w;

	int w, h;                       /* logical size of the drawn result */
};

bool qtext_init(struct qtext *t, struct wlr_scene_tree *parent, const char *font);

/* Draw `text`, ellipsised at max_w (0 = no limit). scale is the output
 * scale, so the buffer is rendered at device resolution and displayed at
 * logical size — this is what keeps it sharp on HiDPI. */
void qtext_set(struct qtext *t, const char *text, uint32_t color,
               float scale, int max_w);

/* Swap the font. The pointer is borrowed, so it must outlive the qtext —
 * config strings do, since the config owns them for the process lifetime. */
void qtext_set_font(struct qtext *t, const char *font);

void qtext_move(struct qtext *t, int x, int y);
void qtext_show(struct qtext *t, bool visible);
void qtext_finish(struct qtext *t);

#endif
