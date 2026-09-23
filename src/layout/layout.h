/* layout.h: tiling tree API */
#ifndef ARO_LAYOUT_H
#define ARO_LAYOUT_H

#include <stdbool.h>

typedef enum { LY_LEAF, LY_SPLIT } ly_kind;
typedef enum { LY_ROW, LY_COL } ly_dir;          /* ROW: side by side. COL: stacked. */
typedef enum { LY_LEFT, LY_RIGHT, LY_UP, LY_DOWN } ly_edge;

typedef struct { int x, y, w, h; } ly_box;

/* layout spacing */
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

/* lifecycle */
ly_node *ly_leaf(void *user);
void     ly_free(ly_node *n);                    /* frees the whole subtree */

/* structure */

/* split target and return new leaf */
ly_node *ly_split(ly_node **root, ly_node *target, ly_dir dir, void *user);

/* remove leaf and collapse parent */
ly_node *ly_close(ly_node **root, ly_node *leaf);

/* swap leaf payloads */
void ly_swap(ly_node *x, ly_node *y);

/* geometry */

/* compute boxes */
void ly_arrange(ly_node *root, ly_box area, const ly_metrics *m);

/* queries */
ly_node *ly_first_leaf(ly_node *n);
int      ly_collect(ly_node *root, ly_node **out, int max);   /* leaves, in tree order */
int      ly_count(ly_node *root);

/* spatial focus */
ly_node *ly_focus(ly_node *root, ly_node *from, ly_edge e);

/*
 * The same rule on bare boxes, for things outside the tree (floating
 * windows): the index of the box nearest `from` in direction e, sideways
 * drift counting double. -1 if nothing lies that way.
 */
int ly_pick(const ly_box *boxes, int n, ly_box from, ly_edge e);

/* resize boundary */
bool ly_resize(ly_node *leaf, ly_edge e, double amount);

#endif
