/* text.h: text rendering helper */
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

/* render text at output scale */
void qtext_set(struct qtext *t, const char *text, uint32_t color,
               float scale, int max_w);

/* font is borrowed from config */
void qtext_set_font(struct qtext *t, const char *font);

void qtext_move(struct qtext *t, int x, int y);
void qtext_show(struct qtext *t, bool visible);
void qtext_finish(struct qtext *t);

#endif
