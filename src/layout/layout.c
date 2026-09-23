/* layout.c: tiling tree */
#include "layout.h"

#include <stdlib.h>

/* lifecycle */

static ly_node *node_new(ly_kind k)
{
	ly_node *n = calloc(1, sizeof *n);
	if (n)
		n->kind = k;
	return n;
}

ly_node *ly_leaf(void *user)
{
	ly_node *n = node_new(LY_LEAF);
	if (n)
		n->user = user;
	return n;
}

void ly_free(ly_node *n)
{
	if (!n)
		return;
	if (n->kind == LY_SPLIT) {
		ly_free(n->a);
		ly_free(n->b);
	}
	free(n);
}

/* structure */

static void replace_child(ly_node **root, ly_node *old, ly_node *fresh)
{
	ly_node *p = old->parent;
	fresh->parent = p;
	if (!p)
		*root = fresh;
	else if (p->a == old)
		p->a = fresh;
	else
		p->b = fresh;
}

ly_node *ly_split(ly_node **root, ly_node *target, ly_dir dir, void *user)
{
	if (!root || !target)
		return NULL;

	ly_node *fresh = ly_leaf(user);
	if (!fresh)
		return NULL;
	ly_node *sp = node_new(LY_SPLIT);
	if (!sp) {
		free(fresh);
		return NULL;
	}

	sp->dir = dir;
	sp->ratio = 0.5;

	/* target stays valid; new leaf is returned */
	replace_child(root, target, sp);
	sp->a = target;
	sp->b = fresh;
	target->parent = sp;
	fresh->parent = sp;

	return fresh;
}

ly_node *ly_close(ly_node **root, ly_node *leaf)
{
	if (!root || !leaf)
		return NULL;

	ly_node *p = leaf->parent;
	if (!p) {                       /* the last window */
		free(leaf);
		*root = NULL;
		return NULL;
	}

	ly_node *sib = (p->a == leaf) ? p->b : p->a;
	replace_child(root, p, sib);    /* sib takes the parent's place */
	free(leaf);
	free(p);
	return ly_first_leaf(sib);
}

void ly_swap(ly_node *x, ly_node *y)
{
	if (!x || !y || x == y)
		return;
	void *t = x->user;
	x->user = y->user;
	y->user = t;
}

/* geometry */

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void arrange(ly_node *n, ly_box b, const ly_metrics *m)
{
	n->box = b;
	if (n->kind == LY_LEAF)
		return;

	int gap = m->gap;

	if (n->dir == LY_ROW) {
		int avail = b.w - gap;
		if (avail < 0)
			avail = 0;
		int aw = (int)(avail * n->ratio + 0.5);
		/* clamp split ratio */
		aw = clampi(aw, (avail < m->min * 2) ? avail / 2 : m->min,
		            (avail < m->min * 2) ? avail / 2 : avail - m->min);
		arrange(n->a, (ly_box){ b.x, b.y, aw, b.h }, m);
		arrange(n->b, (ly_box){ b.x + aw + gap, b.y, avail - aw, b.h }, m);
	} else {
		int avail = b.h - gap;
		if (avail < 0)
			avail = 0;
		int ah = (int)(avail * n->ratio + 0.5);
		ah = clampi(ah, (avail < m->min * 2) ? avail / 2 : m->min,
		            (avail < m->min * 2) ? avail / 2 : avail - m->min);
		arrange(n->a, (ly_box){ b.x, b.y, b.w, ah }, m);
		arrange(n->b, (ly_box){ b.x, b.y + ah + gap, b.w, avail - ah }, m);
	}
}

void ly_arrange(ly_node *root, ly_box area, const ly_metrics *m)
{
	if (!root)
		return;
	int g = m->outer_gap;
	ly_box b = { area.x + g, area.y + g, area.w - 2 * g, area.h - 2 * g };
	if (b.w < 0)
		b.w = 0;
	if (b.h < 0)
		b.h = 0;
	arrange(root, b, m);
}

/* queries */

ly_node *ly_first_leaf(ly_node *n)
{
	while (n && n->kind == LY_SPLIT)
		n = n->a;
	return n;
}

static int collect(ly_node *n, ly_node **out, int max, int i)
{
	if (!n || i >= max)
		return i;
	if (n->kind == LY_LEAF) {
		out[i++] = n;
		return i;
	}
	i = collect(n->a, out, max, i);
	return collect(n->b, out, max, i);
}

int ly_collect(ly_node *root, ly_node **out, int max)
{
	return collect(root, out, max, 0);
}

int ly_count(ly_node *n)
{
	if (!n)
		return 0;
	return n->kind == LY_LEAF ? 1 : ly_count(n->a) + ly_count(n->b);
}

int ly_pick(const ly_box *boxes, int n, ly_box from, ly_edge e)
{
	double cx = from.x + from.w / 2.0;
	double cy = from.y + from.h / 2.0;

	int best = -1;
	double best_score = 0;

	for (int i = 0; i < n; i++) {
		const ly_box *c = &boxes[i];
		double dx = (c->x + c->w / 2.0) - cx;
		double dy = (c->y + c->h / 2.0) - cy;

		bool ok;
		switch (e) {
		case LY_LEFT:  ok = dx < -1; break;
		case LY_RIGHT: ok = dx > 1;  break;
		case LY_UP:    ok = dy < -1; break;
		case LY_DOWN:  ok = dy > 1;  break;
		default:       ok = false;
		}
		if (!ok)
			continue;

		bool horiz = (e == LY_LEFT || e == LY_RIGHT);
		double along = horiz ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
		double off   = horiz ? (dy < 0 ? -dy : dy) : (dx < 0 ? -dx : dx);

		/* spatial focus scoring */
		double score = along + off * 2.0;
		if (best < 0 || score < best_score) {
			best = i;
			best_score = score;
		}
	}
	return best;
}

ly_node *ly_focus(ly_node *root, ly_node *from, ly_edge e)
{
	if (!root || !from)
		return NULL;

	ly_node *buf[256];
	int n = ly_collect(root, buf, 256);

	/* `from` itself is skipped: its own centre is never "that way" */
	ly_box boxes[256];
	for (int i = 0; i < n; i++)
		boxes[i] = buf[i]->box;

	int at = ly_pick(boxes, n, from->box, e);
	return at >= 0 && buf[at] != from ? buf[at] : NULL;
}

bool ly_resize(ly_node *leaf, ly_edge e, double amount)
{
	if (!leaf)
		return false;

	ly_dir want = (e == LY_LEFT || e == LY_RIGHT) ? LY_ROW : LY_COL;

	ly_node *n = leaf, *p = leaf->parent;
	while (p && p->dir != want) {
		n = p;
		p = p->parent;
	}
	if (!p)
		return false;

	double d = (e == LY_RIGHT || e == LY_DOWN) ? amount : -amount;
	if (p->b == n)
		d = -d;                 /* growing right means shrinking a */

	double r = p->ratio + d;
	p->ratio = r < 0.08 ? 0.08 : (r > 0.92 ? 0.92 : r);
	return true;
}
