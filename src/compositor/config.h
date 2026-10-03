/* config.h: runtime config types */
#ifndef ARO_CONFIG_H
#define ARO_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"

/* split layout mode */
/* title header mode */
enum q_header {
	Q_HEADER_ALWAYS = 0,
	Q_HEADER_AUTO,
};

/* workspace switch animation */
enum q_slide {
	Q_SLIDE_HORIZONTAL = 0,
	Q_SLIDE_VERTICAL,
	Q_SLIDE_OFF,
};

enum q_layout {
	Q_LAYOUT_MANUAL = 0,
	Q_LAYOUT_DWINDLE,
	Q_LAYOUT_MONOCLE,       /* one window at a time, the tree kept underneath */
	Q_LAYOUT_SCROLL,        /* columns on an endless strip, the screen a window onto it */
};
#define Q_LAYOUT_INHERIT (-1)   /* use global layout */
#define Q_LAYOUT_TOGGLE  (-1)

/* bar = auto | true | false */
enum q_bar {
	Q_BAR_OFF = 0,
	Q_BAR_ON = 1,
	Q_BAR_AUTO = 2,         /* hidden while another bar is up */
};

/* `wallpaper = auto | none | <path>`; aro runs aropaper (wallpaper.c) */
enum q_wallpaper {
	Q_WALLPAPER_AUTO = 0,   /* aro's own art */
	Q_WALLPAPER_NONE,       /* nothing: swaybg, or a plain background */
	Q_WALLPAPER_FILE,       /* wallpaper_file */
};

enum q_action {
	Q_NONE = 0,
	Q_SPAWN,        /* arg: shell command */
	Q_CLOSE,
	Q_QUIT,
	Q_FOCUS,        /* num: ly_edge */
	Q_MOVE,         /* num: ly_edge */
	Q_RESIZE,       /* num: ly_edge */
	Q_FLOAT,
	Q_FULLSCREEN,
	Q_WORKSPACE,    /* num: index, 0-based */
	Q_SENDTO,       /* num: index, 0-based */
	Q_SPLIT,        /* num: ly_dir */
	Q_SWITCH,       /* num: +1 next, -1 previous */
	Q_LAYOUT,       /* num: enum q_layout, or Q_LAYOUT_TOGGLE */
	Q_OVERVIEW,
	Q_MOVE_WS,      /* num: ly_edge; the current workspace to that monitor */
	Q_STICKY,
	Q_MAXIMIZE,     /* layout = scroll: the column fills the screen, or goes back */
	Q_WIDTH,        /* layout = scroll, num: +1 next preset width, -1 previous */
	Q_SCRATCH,      /* the focused window into the scratchpad, or out */
	Q_SCRATCH_SHOW, /* arg: app_id glob, or NULL for any */
	Q_GROUP,        /* num: ly_edge; into the neighbour's tile as a tab */
	Q_UNGROUP,      /* out of its group, into a tile of its own */
	Q_TAB,          /* num: +1 next tab, -1 previous */
	Q_WALLPAPERS,   /* the wallpaper picker */
};

struct q_bind {
	uint32_t mods;          /* WLR_MODIFIER_*, with `mod` already resolved */
	xkb_keysym_t sym;       /* level-0 keysym, as the key handler compares */
	uint32_t button;        /* a mouse button (BTN_*) instead of a key; else 0 */
	enum q_action action;
	char *arg;              /* Q_SPAWN and Q_SCRATCH_SHOW; owned */
	int num;
};

/* window rule */
#define Q_RULE_UNSET (-1)

/* a rule's length: pixels, or a share of the screen when pct */
struct q_len {
	double v;
	bool pct, set;
};

struct q_rule {
	char *app_id;           /* glob, NULL = any; owned */
	char *title;            /* glob, NULL = any; owned */
	char *type;             /* glob against the window type; owned */
	int floating;           /* 1 float, 0 tile */
	int workspace;          /* 0-based */
	int fullscreen;         /* 1 only: a rule can ask for it, not forbid it */
	int scratch;            /* 1 only: starts hidden in the scratchpad */
	int sticky;             /* 1 only */
	struct q_len size[2];   /* floating size, frame included */
	struct q_len pos[2];    /* floating top-left, from the usable area's */
	int center;             /* 1 only */
	char *monitor;          /* glob on name or "make model serial"; owned */
	int no_border, no_header, no_radius;    /* 1 only */
	double opacity[2];      /* focused, unfocused; 0 unset */
};

/* merged rule result */
struct q_rule_result {
	int floating, workspace, fullscreen, scratch, sticky, center;
	struct q_len size[2], pos[2];
	const char *monitor;    /* the rule's; only good until the next reload */
	unsigned monitor_hash;  /* what changes are told by; 0 unset */
	int no_border, no_header, no_radius;
	double opacity[2];
};

/* monitor block */
#define Q_MON_AUTO (-2)

struct q_monitor_set {
	int enabled;            /* -1 unset, 0, 1 (auto = 1) */
	int mode_w, mode_h;     /* 0 unset; -1 x -1 = preferred (auto) */
	int mode_mhz;           /* refresh in mHz; 0 = the best at that size */
	bool has_pos;           /* a fixed x,y was given */
	bool pos_auto;          /* ...or `position = auto` */
	int x, y;               /* layout coordinates, when has_pos */
	double scale;           /* 0 unset, Q_MON_AUTO = from the panel's DPI */
	int transform;          /* -1 unset, else an enum wl_output_transform
	                         * (auto = NORMAL) */
	int adaptive_sync;      /* -1 unset, 0, 1 (auto = 0, the default) */
	int hdr;                /* -1 unset, 0, 1 (auto = 0) */
};

struct q_monitor {
	char *match;            /* glob; owned */
	int line;               /* where the block opened, for messages */
	struct q_monitor_set set;
};

/* runtime theme values */
struct q_theme {
	int gap, outer_gap, border, radius, min;
	int header_h, bar_h, bar_pad, text_pad;
	int float_min_w, float_min_h;
	double float_scale;

	int anim_ms, anim_fs_ms, anim_focus_ms, drop_ms;
	int ws_slide_ms;                /* 0 = no slide */
	double open_scale;

	int drag_tear, resize_zone;
	double resize_step;

	uint32_t bg, frame, frame_on, line;
	uint32_t accent, accent_soft, ink, dim;
	uint32_t bar_bg, urgent, drop_fill, drop_line;
	uint32_t notify_bg;
	uint32_t scrim;                 /* behind a prompt */
	int notify_ms, notify_max_w;
	int switch_delay_ms, switch_debounce_ms, switch_preview_h;
	double overview_zoom;
	int overview_ms;
	uint32_t overview_tint;

	char *font, *font_small;        /* owned */
};

/* fixed workspace limit */
#define ARO_MAX_WS 32

struct aro_config {
	uint32_t modkey;        /* WLR_MODIFIER_LOGO or WLR_MODIFIER_ALT */

	/* initial pill count */
	int workspaces;

	bool focus_follows_mouse;
	bool confirm_quit;      /* ask before the quit bind ends the session */
	bool allow_tearing;     /* fullscreen apps that ask may skip vsync */
	enum q_layout layout;
	/* per-workspace layout */
	int ws_layout[ARO_MAX_WS];
	enum q_header header;
	enum q_slide ws_slide;  /* how a workspace switch moves */

	/*
	 * `bar = false` removes our bar. It is carried out by setting
	 * theme.bar_h to 0 once the file is read, so everything that makes
	 * room for the bar stops doing so without knowing why. bar_height = 0
	 * means the same thing.
	 */
	enum q_bar bar;
	bool bar_battery;       /* the battery left of the clock, when there is one */

	enum q_wallpaper wallpaper;
	char *wallpaper_file;   /* Q_WALLPAPER_FILE only; owned, `~` unexpanded */
	char **wallpaper_dirs;  /* the picker's folders, `~` unexpanded; owned */
	int nwallpaper_dirs;

	/* touchpad (libinput) */
	bool tp_tap, tp_natural_scroll, tp_dwt;
	int gesture_fingers;    /* a swipe with this many changes workspace; 0 = off */

	/* layout = scroll */
	double scroll_width;    /* a new column's width, as a share of the screen */
	int scroll_peek;        /* pixels of the neighbouring columns left showing */
	double scroll_presets[8];       /* what the width action steps through, ascending */
	int nscroll_presets;
	double tp_speed;        /* -1..1 */

	/* mice and trackpoints (libinput) */
	enum q_accel { Q_ACCEL_DEFAULT, Q_ACCEL_FLAT, Q_ACCEL_ADAPTIVE } ms_accel;
	double ms_speed;        /* -1..1 */
	bool ms_natural_scroll;

	/* key repeat: keys per second, and ms before it starts */
	int kb_repeat_rate, kb_repeat_delay;

	bool lid_switch;        /* closing the lid turns the built-in screen off */

	/* see-through windows */
	double opacity, opacity_unfocused;      /* 0.05..1 */
	double header_gloss;    /* 0..1: the glassy highlight across headers */
	double header_opacity;  /* 0.05..1: below 1, headers show a blur of what is behind */
	bool win_bg_set;        /* else the frame colour, as the header */
	uint32_t win_bg;        /* behind a window's content */
	bool blur;              /* behind see-through windows; effects builds only */
	int blur_passes, blur_radius;
	char *blur_layers;      /* namespace globs, space-separated; owned */
	char *overview_layers;  /* the same, kept sharp over the overview; owned */

	/* xkb: NULL means the system default, i.e. whatever XKB_DEFAULT_* say */
	char *xkb_layout, *xkb_variant, *xkb_options, *xkb_model, *xkb_rules;

	/* cursor: NULL / 0 means whatever XCURSOR_THEME / XCURSOR_SIZE say */
	char *cursor_theme;     /* owned */
	int cursor_size;

	struct q_theme theme;

	struct q_bind *binds;
	int nbinds, bind_cap;

	struct q_rule *rules;   /* in file order; owned */
	int nrules;

	struct q_monitor *monitors;     /* in file order; owned */
	int nmonitors;

	char **exec;            /* run once at startup; owned */
	int nexec;
	char **exec_always;     /* run at startup AND on every reload; owned */
	int nexec_always;

	/* parse errors to show on screen */
	char **errors;          /* owned */
	int nerrors;
};

/* defaults + built-in bindings */
void config_defaults(struct aro_config *c);

/* case-insensitive glob with * and ?, as rules match */
bool glob_match(const char *p, const char *s);

/* the action half of a bind line; also what IPC dispatch uses.
 * arg may be NULL; for Q_SPAWN the command is arg itself */
bool config_parse_action(const char *name, const char *arg,
                         enum q_action *action, int *num);

/* load config; bad lines are reported and skipped */
bool config_load(struct aro_config *c, const char *path);

/* config path */
char *config_path(void);

/* remap bindings when modkey changes */
void config_set_modkey(struct aro_config *c, uint32_t modkey);

/* workspace layout from config */
enum q_layout config_ws_layout(const struct aro_config *c, int ws);
const char *config_layout_name(enum q_layout l);

/* evaluate window rules */
void config_rules_eval(const struct aro_config *c, const char *app_id,
                       const char *title, const char *type,
                       struct q_rule_result *out);

/* evaluate monitor blocks */
void config_monitor_eval(const struct aro_config *c, const char *name,
                         const char *desc, struct q_monitor_set *out);

/* all fields unset */
void config_monitor_unset(struct q_monitor_set *m);

void config_finish(struct aro_config *c);

/* spawn without zombies */
void config_spawn(const char *cmd);

#endif
