/*
 * aro — layout_harness.c
 *
 * Drives the layout tree with the same keys the compositor will bind, and
 * draws the result as ASCII. This is the fast loop: no wlroots, no session,
 * no reboot. If the layout feels wrong here, it will feel wrong on screen.
 *
 *   ninja -C build && ./build/aro-layout
 *   echo "v s l q" | ./build/aro-layout      # scriptable too
 */
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 104
#define ROWS 30

static char canvas[ROWS][COLS + 1];

static void canvas_clear(void)
{
	for (int y = 0; y < ROWS; y++) {
		memset(canvas[y], ' ', COLS);
		canvas[y][COLS] = '\0';
	}
}

static void put(int x, int y, char c)
{
	if (x >= 0 && x < COLS && y >= 0 && y < ROWS)
		canvas[y][x] = c;
}

static void draw_frame(ly_node *n, bool focused, int idx)
{
	ly_box b = n->box;
	if (b.w < 2 || b.h < 2)
		return;

	char h = focused ? '=' : '-';
	char v = focused ? '#' : '|';
	char c = focused ? '#' : '+';

	for (int x = b.x; x < b.x + b.w; x++) {
		put(x, b.y, h);
		put(x, b.y + b.h - 1, h);
	}
	for (int y = b.y; y < b.y + b.h; y++) {
		put(b.x, y, v);
		put(b.x + b.w - 1, y, v);
	}
	put(b.x, b.y, c);
	put(b.x + b.w - 1, b.y, c);
	put(b.x, b.y + b.h - 1, c);
	put(b.x + b.w - 1, b.y + b.h - 1, c);

	/* header strip: the label, like the pane titles in splits */
	char label[64];
	snprintf(label, sizeof label, " %d:%s %dx%d ", idx,
	         (const char *)n->user, b.w, b.h);
	for (int i = 0; label[i] && b.x + 2 + i < b.x + b.w - 1; i++)
		put(b.x + 2 + i, b.y, label[i]);
}

static void render(ly_node *root, ly_node *focus)
{
	canvas_clear();
	ly_arrange(root, (ly_box){ 0, 0, COLS, ROWS },
	           &(ly_metrics){ .gap = 1, .outer_gap = 1, .min = 6 });

	ly_node *buf[256];
	int n = ly_collect(root, buf, 256);
	for (int i = 0; i < n; i++)
		draw_frame(buf[i], buf[i] == focus, i + 1);

	printf("\x1b[2J\x1b[H");
	for (int y = 0; y < ROWS; y++)
		puts(canvas[y]);
	printf("\n  %d frame(s)   focus: %s\n", n,
	       focus ? (const char *)focus->user : "-");
	printf("  v split right  s split down  hjkl focus  HJKL resize  "
	       "q close  x quit\n\n> ");
	fflush(stdout);
}

int main(void)
{
	static int serial = 0;
	char names[256][12];

	snprintf(names[serial], sizeof names[0], "win%u", (unsigned)(serial & 0xff));
	ly_node *root = ly_leaf(names[serial]);
	ly_node *focus = root;
	serial++;

	render(root, focus);

	int ch;
	while ((ch = getchar()) != EOF) {
		if (ch == '\n' || ch == ' ')
			continue;
		if (ch == 'x')
			break;

		switch (ch) {
		case 'v':
		case 's': {
			if (serial >= 256)
				break;
			snprintf(names[serial], sizeof names[0], "win%u", (unsigned)(serial & 0xff));
			ly_node *fresh = ly_split(&root, focus,
			                          ch == 'v' ? LY_ROW : LY_COL,
			                          names[serial]);
			if (fresh) {
				focus = fresh;
				serial++;
			}
			break;
		}
		case 'h': case 'j': case 'k': case 'l': {
			ly_edge e = ch == 'h' ? LY_LEFT : ch == 'l' ? LY_RIGHT
			          : ch == 'k' ? LY_UP   : LY_DOWN;
			ly_node *next = ly_focus(root, focus, e);
			if (next)
				focus = next;
			break;
		}
		case 'H': case 'J': case 'K': case 'L': {
			ly_edge e = ch == 'H' ? LY_LEFT : ch == 'L' ? LY_RIGHT
			          : ch == 'K' ? LY_UP   : LY_DOWN;
			ly_resize(focus, e, 0.04);
			break;
		}
		case 'q':
			focus = ly_close(&root, focus);
			if (!root) {
				printf("\nlast frame closed\n");
				return 0;
			}
			break;
		default:
			break;
		}
		render(root, focus);
	}

	ly_free(root);
	return 0;
}
