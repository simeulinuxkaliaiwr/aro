/* anim.c: easing and animated boxes */
#include "anim.h"

/* ── cubic bezier ──────────────────────────────────────────────────────── */
/* cubic-bezier evaluation */

static double bez(double a, double b, double t)
{
	double mt = 1.0 - t;
	/* 3(1-t)^2 t a + 3(1-t) t^2 b + t^3 */
	return 3.0 * mt * mt * t * a + 3.0 * mt * t * t * b + t * t * t;
}

static double bez_slope(double a, double b, double t)
{
	double mt = 1.0 - t;
	return 3.0 * mt * mt * a
	     + 6.0 * mt * t * (b - a)
	     + 3.0 * t * t * (1.0 - b);
}

double anim_ease_eval(const anim_ease *e, double x)
{
	if (x <= 0.0)
		return 0.0;
	if (x >= 1.0)
		return 1.0;

	double t = x;
	for (int i = 0; i < 8; i++) {
		double err = bez(e->x1, e->x2, t) - x;
		if (err < 1e-6 && err > -1e-6)
			return bez(e->y1, e->y2, t);
		double d = bez_slope(e->x1, e->x2, t);
		if (d < 1e-6 && d > -1e-6)
			break;
		t -= err / d;
	}

	double lo = 0.0, hi = 1.0;
	t = x;
	for (int i = 0; i < 24; i++) {
		double v = bez(e->x1, e->x2, t);
		if (v < x)
			lo = t;
		else
			hi = t;
		t = (lo + hi) / 2.0;
	}
	return bez(e->y1, e->y2, t);
}

/* ── animated boxes ────────────────────────────────────────────────────── */

static int lerp(int a, int b, double t)
{
	return (int)(a + (b - a) * t + (t >= 0 ? 0.5 : -0.5));
}

static bool box_eq(ly_box a, ly_box b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void anim_box_set(anim_box *a, ly_box b)
{
	a->from = a->to = a->cur = b;
	a->active = false;
}

void anim_box_to(anim_box *a, ly_box to, uint32_t now, uint32_t dur,
                 const anim_ease *ease)
{
	if (box_eq(a->to, to) && a->active)
		return;                 /* already heading there */
	if (box_eq(a->cur, to)) {
		a->to = to;
		a->active = false;
		return;
	}
	if (dur == 0) {
		anim_box_set(a, to);
		return;
	}

	/* retarget from current position */
	a->from = a->cur;
	a->to = to;
	a->start_ms = now;
	a->dur_ms = dur;
	if (ease)
		a->ease = *ease;
	a->active = true;
}

bool anim_box_tick(anim_box *a, uint32_t now)
{
	if (!a->active)
		return false;

	uint32_t elapsed = now - a->start_ms;
	if (elapsed >= a->dur_ms) {
		a->cur = a->to;
		a->active = false;
		return false;
	}

	double p = (double)elapsed / (double)a->dur_ms;
	double t = anim_ease_eval(&a->ease, p);

	a->cur.x = lerp(a->from.x, a->to.x, t);
	a->cur.y = lerp(a->from.y, a->to.y, t);
	a->cur.w = lerp(a->from.w, a->to.w, t);
	a->cur.h = lerp(a->from.h, a->to.h, t);

	/* clamp tiny sizes */
	if (a->cur.w < 1)
		a->cur.w = 1;
	if (a->cur.h < 1)
		a->cur.h = 1;
	return true;
}
