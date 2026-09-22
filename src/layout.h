/*
 * aro — layout.h
 *
 * The layout tree. Split-on-demand, i3 style: every window is a leaf, and
 * splitting a leaf wraps it in a container holding the old leaf and a new one.
 *
 * This file knows nothing about Wayland, wlroots or rendering. It takes a
 * rectangle and hands back rectangles. That is the whole contract, and it is
 * why the tree can be driven from a terminal harness before the compositor
 * exists.
 *
 * Leaf pointers are stable: splitting a leaf never invalidates a pointer to
 * it, so the compositor can keep `view->node` across any number of splits.
 */
#ifndef ARO_LAYOUT_H
#define ARO_LAYOUT_H

#include <stdbool.h>

typedef enum { LY_LEAF, LY_SPLIT } ly_kind;
typedef enum { LY_ROW, LY_COL } ly_dir;          /* ROW: side by side. COL: stacked. */
typedef enum { LY_LEFT, LY_RIGHT, LY_UP, LY_DOWN } ly_edge;

typedef struct { int x, y, w, h; } ly_box;

/* Spacing, in pixels. The compositor fills this from theme.h. */
typedef struct {
	int gap;                /* between siblings */
	int outer_gap;          /* between the tree and the screen edge */
	int min;                /* a leaf never arranges smaller than this */
} ly_metrics;

typedef struct ly_node {
	ly_kind kind;
	struct ly_node *parent;
	ly_box box;             /* filled by ly_arrange() */

	void *user;             /* LY_LEAF: whatever the compositor wants */

	ly_dir dir;             /* LY_SPLIT */
	double ratio;           /* LY_SPLIT: share given to child a, 0..1 */
	struct ly_node *a, *b;  /* LY_SPLIT */
} ly_node;

/* lifecycle ------------------------------------------------------------- */
ly_node *ly_leaf(void *user);
void     ly_free(ly_node *n);                    /* frees the whole subtree */

/* structure ------------------------------------------------------------- */

/* Split `target`, returning the NEW leaf. `target` stays valid and keeps its
 * payload; it simply gains a parent. Pass the address of your root: if target
 * was the root, the new split node replaces it. */
ly_node *ly_split(ly_node **root, ly_node *target, ly_dir dir, void *user);

/* Remove `leaf` and collapse its parent. Returns the leaf that should take
 * focus, or NULL if the tree is now empty. */
ly_node *ly_close(ly_node **root, ly_node *leaf);

/* Exchange the payloads of two leaves — this is how "move window" works. */
void ly_swap(ly_node *x, ly_node *y);

/* geometry -------------------------------------------------------------- */

/* Compute every node's box inside `area`. Call before anything that needs
 * geometry, including ly_focus() — direction is resolved spatially, not by
 * walking the tree, so that focus behaves the way the screen looks. */
void ly_arrange(ly_node *root, ly_box area, const ly_metrics *m);

/* queries --------------------------------------------------------------- */
ly_node *ly_first_leaf(ly_node *n);
int      ly_collect(ly_node *root, ly_node **out, int max);   /* leaves, in tree order */
int      ly_count(ly_node *root);

/* The nearest leaf in a direction, by box centres. NULL at the edge. */
ly_node *ly_focus(ly_node *root, ly_node *from, ly_edge e);

/* Nudge the boundary on one side of `leaf`. Walks up to the nearest ancestor
 * splitting along the right axis. Returns false if there is none. */
bool ly_resize(ly_node *leaf, ly_edge e, double amount);

#endif
