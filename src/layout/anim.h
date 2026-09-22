/* anim.h: animation helpers */
#ifndef ARO_ANIM_H
#define ARO_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

/* cubic-bezier control points */
typedef struct {
	double x1, y1, x2, y2;
} anim_ease;

/* eased progress */
double anim_ease_eval(const anim_ease *e, double t);

typedef struct {
	ly_box from, to, cur;
	uint32_t start_ms, dur_ms;
	anim_ease ease;
	bool active;
} anim_box;

/* start/retarget animation */
void anim_box_to(anim_box *a, ly_box to, uint32_t now, uint32_t dur,
                 const anim_ease *ease);

/* set immediately */
void anim_box_set(anim_box *a, ly_box b);

/* advance animation */
bool anim_box_tick(anim_box *a, uint32_t now);

#endif
