/*
 * aro — anim.h
 *
 * The layout tree says where a frame belongs. This says how it gets there.
 *
 * Pure arithmetic, no wlroots: it can be unit-tested, and it is the piece
 * most likely to need tuning by feel rather than by reasoning.
 */
#ifndef ARO_ANIM_H
#define ARO_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

/* A CSS-style cubic-bezier with implicit endpoints (0,0) and (1,1).
 * y may exceed 1 — that overshoot is the spring. */
typedef struct {
	double x1, y1, x2, y2;
} anim_ease;

/* Progress 0..1 in, eased value out (which may briefly exceed 1). */
double anim_ease_eval(const anim_ease *e, double t);

typedef struct {
	ly_box from, to, cur;
	uint32_t start_ms, dur_ms;
	anim_ease ease;
	bool active;
} anim_box;

/* Start (or retarget) an animation toward `to`. Retargeting mid-flight starts
 * from wherever the box currently is, so interrupted moves stay continuous
 * instead of snapping back. */
void anim_box_to(anim_box *a, ly_box to, uint32_t now, uint32_t dur,
                 const anim_ease *ease);

/* Place instantly, no animation. */
void anim_box_set(anim_box *a, ly_box b);

/* Advance. Returns true while still moving. */
bool anim_box_tick(anim_box *a, uint32_t now);

#endif
