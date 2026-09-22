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

enum q_layout {
	Q_LAYOUT_MANUAL = 0,
	Q_LAYOUT_DWINDLE,
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
};

struct q_bind {
	uint32_t mods;          /* WLR_MODIFIER_*, with `mod` already resolved */
	xkb_keysym_t sym;       /* level-0 keysym, as the key handler compares */
	enum q_action action;
	char *arg;              /* Q_SPAWN only; owned */
	int num;
};

/* window rule */
#define Q_RULE_UNSET (-1)

struct q_rule {
	char *app_id;           /* glob, NULL = any; owned */
	char *title;            /* glob, NULL = any; owned */
	int floating;           /* 1 float, 0 tile */
	int workspace;          /* 0-based */
	int fullscreen;         /* 1 only: a rule can ask for it, not forbid it */
};

/* merged rule result */
struct q_rule_result {
	int floating, workspace, fullscreen;
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
	double open_scale;

	int drag_tear, resize_zone;
	double resize_step;

	uint32_t bg, frame, frame_on, line;
	uint32_t accent, accent_soft, ink, dim;
	uint32_t bar_bg, urgent, drop_fill, drop_line;
	uint32_t notify_bg;
	uint32_t scrim;                 /* behind a prompt */
	int notify_ms, notify_max_w;

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
	enum q_layout layout;
	enum q_header header;

	/* xkb: NULL means the system default, i.e. whatever XKB_DEFAULT_* say */
	char *xkb_layout, *xkb_variant, *xkb_options, *xkb_model, *xkb_rules;

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

/* load config; bad lines are reported and skipped */
bool config_load(struct aro_config *c, const char *path);

/* config path */
char *config_path(void);

/* remap bindings when modkey changes */
void config_set_modkey(struct aro_config *c, uint32_t modkey);

/* evaluate window rules */
void config_rules_eval(const struct aro_config *c, const char *app_id,
                       const char *title, struct q_rule_result *out);

/* evaluate monitor blocks */
void config_monitor_eval(const struct aro_config *c, const char *name,
                         const char *desc, struct q_monitor_set *out);

/* all fields unset */
void config_monitor_unset(struct q_monitor_set *m);

void config_finish(struct aro_config *c);

/* spawn without zombies */
void config_spawn(const char *cmd);

#endif
