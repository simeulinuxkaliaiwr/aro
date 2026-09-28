/* picker.h: the wallpaper picker, a strip of wallpapers to choose from */
#ifndef ARO_PICKER_H
#define ARO_PICKER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"
#include "text.h"

struct aro_server;
struct aro_output;
struct wlr_buffer;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wl_event_source;

/* cards drawn at once: the selection and five either side */
#define PK_SLOTS 11
#define PK_QUERY_MAX 128

enum pk_state {
	PK_WAIT,        /* no thumbnail yet; one is being made */
	PK_READY,       /* the thumbnail is on disk */
	PK_FAILED,      /* aropaper could not read it: left out */
};

struct pk_item {
	char *path;
	char *name;                     /* what the picker shows */
	char *thumb;                    /* in the cache */
	enum pk_state state;
	int run;                        /* the thumbnailer run asked for it; 0 none */
	struct wlr_buffer *buf;         /* loaded while near the selection */
	int vi;                         /* index in the view; -1 filtered out */
};

struct pk_card {
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *edge, *bg;
	struct wlr_scene_buffer *img;
	int vi;                         /* the view index shown; -1 hidden */
	ly_box box;                     /* where, for clicks */
};

/* aropaper --thumbnails; outlives the picker so a started batch finishes */
struct pk_thumbnailer {
	pid_t pid;
	int pidfd, out;
	int run;
	struct wl_event_source *exit_src, *out_src;
	char buf[8192];
	size_t len;
};

struct aro_picker {
	bool open;                      /* taking input */
	bool shown;                     /* drawn; also while closing */
	double p, p_from, p_to;         /* 0 hidden, 1 open */
	uint32_t p_start;
	struct aro_output *output;

	struct pk_item *items;          /* every wallpaper found, by name */
	int n;
	int *view;                      /* the ones matching the query */
	int nview;
	int sel;                        /* into view */
	int current;                    /* the item on screen now; -1 none */
	char query[PK_QUERY_MAX];

	double pos, pos_from, pos_to;   /* the strip, in cards */
	uint32_t pos_start;

	struct wlr_scene_tree *tree;
	struct wlr_scene_blur *blur;    /* effects only */
	struct wlr_scene_rect *scrim;
	struct wlr_scene_tree *strip;
	struct pk_card cards[PK_SLOTS];
	struct qtext name, info, search, hint;
	char *title_font;               /* owned: font, larger */

	int pressed;                    /* view index under the press; -1 none,
	                                   -2 the backdrop */
	double scroll;                  /* a smooth wheel, until it makes a step */

	char *cache;                    /* the thumbnail directory; owned */
	struct pk_thumbnailer thumbs;
	int next_run;
};

void picker_init(struct aro_server *s);
void picker_toggle(struct aro_server *s);
bool picker_active(struct aro_server *s);       /* open, taking input */

/* a key press: raw is level 0, for binds; typed is what it types */
void picker_key(struct aro_server *s, uint32_t mods, xkb_keysym_t raw,
                xkb_keysym_t typed);
void picker_pointer_motion(struct aro_server *s, double lx, double ly);
void picker_pointer_button(struct aro_server *s, double lx, double ly, bool pressed);
void picker_pointer_axis(struct aro_server *s, double delta, int discrete);

bool picker_tick(struct aro_server *s, uint32_t now);
void picker_retheme(struct aro_server *s);
void picker_output_gone(struct aro_server *s, struct aro_output *o);
void picker_finish(struct aro_server *s);

#endif
