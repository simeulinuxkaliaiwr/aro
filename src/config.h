/*
 * aro — config.h
 *
 * A plain `key = value` file, read once at startup. No dependency, no
 * tokenizer worth the name: every line is a key, an equals sign and the rest
 * of the line.
 *
 * theme.h still holds every default; this is what the compositor reads at
 * runtime. A missing config file is not an error — the defaults apply and
 * the built-in bindings are installed.
 */
#ifndef ARO_CONFIG_H
#define ARO_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"

/*
 * How the direction of a new split is chosen.
 *
 * MANUAL is i3's rule: a new window opens beside the focused one, to the
 * right unless mod+v/mod+s said otherwise for the next window only.
 *
 * DWINDLE is Hyprland's: the split follows the shape of the frame being
 * split — along its longer axis — so a full-screen window halves
 * left/right, each of those halves top/bottom, and so on, spiralling
 * without anyone having to say which way. A one-shot mod+v or mod+s still
 * overrides it for the next window.
 */
/*
 * Whether to draw our own title strip on a window that already draws one.
 *
 * ALWAYS is the default and the splits look: every frame gets a header,
 * including GTK windows that draw their own — you end up with two title
 * bars stacked, which is the price of a consistent strip everywhere.
 *
 * AUTO drops ours when the client decorates itself. Tidier per window,
 * inconsistent across the desktop, since whether a frame has a header then
 * depends on the toolkit the app happens to use.
 */
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

/*
 * A window rule: "windows that look like this get treated like that".
 *
 *   rule = app_id:mpv  workspace 3
 *   rule = app_id:firefox  title:Picture-in-Picture  float
 *
 * Matchers are globs (* and ?, \ escapes), case-insensitive, and every
 * matcher on a line must match. A missing matcher matches anything; a line
 * needs at least one, so a rule can never silently apply to every window.
 * For X11 windows app_id matches the WM_CLASS class.
 *
 * Each action field is -1 when the rule says nothing about it. Every
 * matching rule applies, in file order, so for a field two rules both set,
 * the later line wins — the same way a later key overrides an earlier one
 * everywhere else in this file.
 */
#define Q_RULE_UNSET (-1)

struct q_rule {
	char *app_id;           /* glob, NULL = any; owned */
	char *title;            /* glob, NULL = any; owned */
	int floating;           /* 1 float, 0 tile */
	int workspace;          /* 0-based */
	int fullscreen;         /* 1 only: a rule can ask for it, not forbid it */
};

/* What the rules say about one window, merged. Same -1 convention. */
struct q_rule_result {
	int floating, workspace, fullscreen;
};

/*
 * Everything that used to be a #define in theme.h. The macros are still
 * there and are still the single source of truth for the DEFAULTS — this
 * struct is what the running compositor actually reads.
 *
 * Colours are 0xRRGGBBAA, as in theme.h; ui.c premultiplies them.
 */
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

/*
 * The hard ceiling on workspaces. Workspaces are created on demand — you
 * never declare how many you want — but the arrays behind them are fixed,
 * because a pill row you cannot count at a glance is not a useful bar and a
 * genuinely unbounded count buys nothing over a number nobody reaches.
 */
#define ARO_MAX_WS 32

struct aro_config {
	uint32_t modkey;        /* WLR_MODIFIER_LOGO or WLR_MODIFIER_ALT */

	/* How many pills the bar always shows, even when empty. Beyond this
	 * they appear as they are used and vanish when they empty. */
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

	char **exec;            /* run once at startup; owned */
	int nexec;
	char **exec_always;     /* run at startup AND on every reload; owned */
	int nexec_always;

	/*
	 * Problems found while parsing, ready to be shown to the user. The
	 * parser cannot reach the compositor to put them on screen, and a log
	 * file nobody is tailing is not telling anyone anything.
	 */
	char **errors;          /* owned */
	int nerrors;
};

/* theme.h defaults plus the built-in bindings. Always call this first. */
void config_defaults(struct aro_config *c);

/*
 * Read `path`, or the usual XDG location when path is NULL. Returns false
 * only if the file exists and could not be read — a missing file is fine.
 * Bad lines are reported with their line number and skipped, never fatal:
 * a typo in a config file should not cost you your session.
 *
 * Any `bind` line replaces the built-in bindings wholesale the first time
 * one is seen, so a config that sets bindings starts from a clean slate
 * rather than fighting the defaults.
 */
bool config_load(struct aro_config *c, const char *path);

/* Where config_load(NULL) reads from. Caller frees; NULL if unknowable. */
char *config_path(void);

/*
 * Change the modifier `mod` stands for, remapping every binding that already
 * uses it. Needed because bindings are parsed with `mod` resolved to
 * whatever the modifier was at the time — so a `mod = alt` line halfway down
 * a file, or a -m flag after the file is read, has to fix up what came
 * before it rather than silently leaving those bindings on the old key.
 */
void config_set_modkey(struct aro_config *c, uint32_t modkey);

/*
 * Every rule that matches this window, merged in file order. NULL app_id or
 * title matches as the empty string, so a pattern of * still matches it and
 * anything more specific does not.
 */
void config_rules_eval(const struct aro_config *c, const char *app_id,
                       const char *title, struct q_rule_result *out);

void config_finish(struct aro_config *c);

/* fork twice and run `cmd` under /bin/sh, so we never collect zombies */
void config_spawn(const char *cmd);

#endif
