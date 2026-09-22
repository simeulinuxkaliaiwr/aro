/*
 * aro — config.c
 */
/*
 * _GNU_SOURCE, not _POSIX_C_SOURCE: strsep() is a BSD extension rather than
 * POSIX, so restricting glibc to the POSIX namespace actively hides it.
 * _GNU_SOURCE implies everything _POSIX_C_SOURCE 200809L would have given.
 */
#define _GNU_SOURCE

#include "config.h"
#include "theme.h"

#include <ctype.h>
#include <stddef.h>   /* offsetof */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — not in string.h */
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-protocol.h>     /* enum wl_output_transform */
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

/* ── small helpers ─────────────────────────────────────────────────────── */

static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static bool parse_bool(const char *v, bool *out)
{
	if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
	    !strcasecmp(v, "on") || !strcmp(v, "1")) {
		*out = true;
		return true;
	}
	if (!strcasecmp(v, "false") || !strcasecmp(v, "no") ||
	    !strcasecmp(v, "off") || !strcmp(v, "0")) {
		*out = false;
		return true;
	}
	return false;
}

/* 0xRRGGBBAA, or #rrggbbaa, or bare rrggbbaa. Alpha may be omitted. */
static bool parse_color(const char *v, uint32_t *out)
{
	if (*v == '#')
		v++;
	else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		v += 2;

	size_t n = strlen(v);
	if (n != 6 && n != 8)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!isxdigit((unsigned char)v[i]))
			return false;

	unsigned long c = strtoul(v, NULL, 16);
	*out = (n == 6) ? (uint32_t)((c << 8) | 0xff) : (uint32_t)c;
	return true;
}

static bool parse_double(const char *v, double *out)
{
	char *end;
	double d = strtod(v, &end);
	if (end == v)
		return false;
	*out = d;
	return true;
}

static bool parse_int(const char *v, int *out)
{
	char *end;
	long n = strtol(v, &end, 0);
	if (end == v || *trim(end) != '\0')
		return false;
	*out = (int)n;
	return true;
}

/* ── bindings ──────────────────────────────────────────────────────────── */

static bool bind_add(struct aro_config *c, uint32_t mods, xkb_keysym_t sym,
                     enum q_action action, int num, const char *arg)
{
	if (c->nbinds == c->bind_cap) {
		int cap = c->bind_cap ? c->bind_cap * 2 : 32;
		struct q_bind *b = realloc(c->binds, cap * sizeof *b);
		if (!b)
			return false;
		c->binds = b;
		c->bind_cap = cap;
	}

	struct q_bind *b = &c->binds[c->nbinds];
	b->mods = mods;
	b->sym = sym;
	b->action = action;
	b->num = num;
	b->arg = arg ? strdup(arg) : NULL;
	if (arg && !b->arg)
		return false;

	c->nbinds++;
	return true;
}

/*
 * "mod+shift+h" -> mods and a level-0 keysym.
 *
 * Level-0 matters: the key handler compares unshifted symbols and reads
 * shift from the modifier mask, because translated symbols turn `e` into `E`
 * and `1` into `!`. So "shift+1" stores XKB_KEY_1 with the shift bit, not
 * XKB_KEY_exclam.
 */
static bool parse_combo(struct aro_config *c, char *spec,
                        uint32_t *mods, xkb_keysym_t *sym)
{
	*mods = 0;
	*sym = XKB_KEY_NoSymbol;

	char *save = NULL;
	for (char *tok = strtok_r(spec, "+", &save); tok;
	     tok = strtok_r(NULL, "+", &save)) {
		tok = trim(tok);
		if (!*tok)
			continue;

		if (!strcasecmp(tok, "mod"))
			*mods |= c->modkey;
		else if (!strcasecmp(tok, "super") || !strcasecmp(tok, "logo"))
			*mods |= WLR_MODIFIER_LOGO;
		else if (!strcasecmp(tok, "alt"))
			*mods |= WLR_MODIFIER_ALT;
		else if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control"))
			*mods |= WLR_MODIFIER_CTRL;
		else if (!strcasecmp(tok, "shift"))
			*mods |= WLR_MODIFIER_SHIFT;
		else
			*sym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	return *sym != XKB_KEY_NoSymbol;
}

static bool parse_edge(const char *s, int *out)
{
	if (!strcasecmp(s, "left"))  { *out = LY_LEFT;  return true; }
	if (!strcasecmp(s, "right")) { *out = LY_RIGHT; return true; }
	if (!strcasecmp(s, "up"))    { *out = LY_UP;    return true; }
	if (!strcasecmp(s, "down"))  { *out = LY_DOWN;  return true; }
	return false;
}

/* "mod+h, focus, left" — combo, action, optional argument */
static bool parse_bind(struct aro_config *c, char *value)
{
	char *combo = strsep(&value, ",");
	char *action = strsep(&value, ",");
	if (!combo || !action)
		return false;

	combo = trim(combo);
	action = trim(action);
	char *arg = value ? trim(value) : NULL;

	uint32_t mods;
	xkb_keysym_t sym;
	if (!parse_combo(c, combo, &mods, &sym))
		return false;

	int num = 0;

	if (!strcasecmp(action, "spawn") || !strcasecmp(action, "exec")) {
		if (!arg || !*arg)
			return false;
		return bind_add(c, mods, sym, Q_SPAWN, 0, arg);
	}
	if (!strcasecmp(action, "close"))
		return bind_add(c, mods, sym, Q_CLOSE, 0, NULL);
	if (!strcasecmp(action, "quit"))
		return bind_add(c, mods, sym, Q_QUIT, 0, NULL);
	if (!strcasecmp(action, "float"))
		return bind_add(c, mods, sym, Q_FLOAT, 0, NULL);
	if (!strcasecmp(action, "fullscreen"))
		return bind_add(c, mods, sym, Q_FULLSCREEN, 0, NULL);

	if (!strcasecmp(action, "focus") || !strcasecmp(action, "move") ||
	    !strcasecmp(action, "resize")) {
		if (!arg || !parse_edge(arg, &num))
			return false;
		enum q_action a = !strcasecmp(action, "focus")  ? Q_FOCUS
		                : !strcasecmp(action, "move")   ? Q_MOVE
		                                                : Q_RESIZE;
		return bind_add(c, mods, sym, a, num, NULL);
	}

	if (!strcasecmp(action, "workspace") || !strcasecmp(action, "sendto")) {
		if (!arg || !parse_int(arg, &num) || num < 1 || num > ARO_MAX_WS)
			return false;
		return bind_add(c, mods, sym,
		                !strcasecmp(action, "workspace") ? Q_WORKSPACE : Q_SENDTO,
		                num - 1, NULL);
	}

	if (!strcasecmp(action, "split")) {
		if (!arg)
			return false;
		if (!strcasecmp(arg, "right") || !strcasecmp(arg, "horizontal"))
			num = LY_ROW;
		else if (!strcasecmp(arg, "down") || !strcasecmp(arg, "vertical"))
			num = LY_COL;
		else
			return false;
		return bind_add(c, mods, sym, Q_SPLIT, num, NULL);
	}

	return false;
}

/* ── defaults ──────────────────────────────────────────────────────────── */

static void install_default_binds(struct aro_config *c)
{
	const uint32_t M = c->modkey;
	const uint32_t S = WLR_MODIFIER_SHIFT;
	const uint32_t C = WLR_MODIFIER_CTRL;

	bind_add(c, M, XKB_KEY_Return, Q_SPAWN, 0, "foot");
	bind_add(c, M, XKB_KEY_d, Q_SPAWN, 0, "fuzzel");
	bind_add(c, M, XKB_KEY_q, Q_CLOSE, 0, NULL);
	bind_add(c, M | S, XKB_KEY_e, Q_QUIT, 0, NULL);
	bind_add(c, M, XKB_KEY_space, Q_FLOAT, 0, NULL);
	bind_add(c, M, XKB_KEY_f, Q_FULLSCREEN, 0, NULL);
	bind_add(c, M, XKB_KEY_v, Q_SPLIT, LY_ROW, NULL);
	bind_add(c, M, XKB_KEY_s, Q_SPLIT, LY_COL, NULL);

	const xkb_keysym_t hjkl[4] = {
		XKB_KEY_h, XKB_KEY_j, XKB_KEY_k, XKB_KEY_l,
	};
	const int edge[4] = { LY_LEFT, LY_DOWN, LY_UP, LY_RIGHT };
	for (int i = 0; i < 4; i++) {
		bind_add(c, M,     hjkl[i], Q_FOCUS,  edge[i], NULL);
		bind_add(c, M | S, hjkl[i], Q_MOVE,   edge[i], NULL);
		bind_add(c, M | C, hjkl[i], Q_RESIZE, edge[i], NULL);
	}

	const xkb_keysym_t num[4] = {
		XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4,
	};
	for (int i = 0; i < 4; i++) {
		bind_add(c, M,     num[i], Q_WORKSPACE, i, NULL);
		bind_add(c, M | S, num[i], Q_SENDTO,    i, NULL);
	}
}

void config_set_modkey(struct aro_config *c, uint32_t modkey)
{
	if (modkey == c->modkey)
		return;

	for (int i = 0; i < c->nbinds; i++) {
		struct q_bind *b = &c->binds[i];
		if (b->mods & c->modkey) {
			b->mods &= ~c->modkey;
			b->mods |= modkey;
		}
	}
	c->modkey = modkey;
}

void config_defaults(struct aro_config *c)
{
	memset(c, 0, sizeof *c);

	/* Super by default. `mod = alt` in the config is how you test nested,
	 * where the host compositor grabs Super before we ever see it. */
	c->modkey = WLR_MODIFIER_LOGO;
	c->workspaces = TH_WORKSPACES;
	c->focus_follows_mouse = TH_FOCUS_FOLLOWS_MOUSE;
	c->confirm_quit = TH_CONFIRM_QUIT;
	c->layout = Q_LAYOUT_MANUAL;
	c->header = Q_HEADER_ALWAYS;

	c->theme = (struct q_theme){
		.gap = TH_GAP, .outer_gap = TH_OUTER_GAP, .border = TH_BORDER,
		.radius = TH_RADIUS, .min = TH_MIN,
		.header_h = TH_HEADER_H, .bar_h = TH_BAR_H,
		.bar_pad = TH_BAR_PAD, .text_pad = TH_TEXT_PAD,
		.float_min_w = TH_FLOAT_MIN_W, .float_min_h = TH_FLOAT_MIN_H,
		.float_scale = TH_FLOAT_SCALE,

		.anim_ms = TH_ANIM_MS, .anim_fs_ms = TH_ANIM_FS_MS,
		.anim_focus_ms = TH_ANIM_FOCUS_MS, .drop_ms = TH_DROP_MS,
		.open_scale = TH_OPEN_SCALE,

		.drag_tear = TH_DRAG_TEAR, .resize_zone = TH_RESIZE_ZONE,
		.resize_step = TH_RESIZE_STEP,

		.bg = TH_BG, .frame = TH_FRAME, .frame_on = TH_FRAME_ON,
		.line = TH_LINE, .accent = TH_ACCENT, .accent_soft = TH_ACCENT_SOFT,
		.ink = TH_INK, .dim = TH_DIM, .bar_bg = TH_BAR_BG,
		.urgent = TH_URGENT, .drop_fill = TH_DROP_FILL,
		.drop_line = TH_DROP_LINE,
		.notify_bg = TH_NOTIFY_BG, .notify_ms = TH_NOTIFY_MS,
		.scrim = TH_SCRIM,
		.notify_max_w = TH_NOTIFY_MAX_W,
	};
	c->theme.font = strdup(TH_FONT);
	c->theme.font_small = strdup(TH_FONT_SMALL);

	install_default_binds(c);
}

/* ── loading ───────────────────────────────────────────────────────────── */

static void clear_binds(struct aro_config *c)
{
	for (int i = 0; i < c->nbinds; i++)
		free(c->binds[i].arg);
	c->nbinds = 0;
}

/* Record a parse problem AND log it. Never fatal: a typo in a config file
 * should cost you that line, not your session. */
static void config_err(struct aro_config *c, int line, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	wlr_log(WLR_ERROR, "config:%d: %s", line, msg);

	char full[300];
	snprintf(full, sizeof full, "config:%d: %s", line, msg);

	char **e = realloc(c->errors, (c->nerrors + 1) * sizeof *e);
	if (!e)
		return;
	c->errors = e;
	c->errors[c->nerrors] = strdup(full);
	if (c->errors[c->nerrors])
		c->nerrors++;
}

static bool set_str(char **dst, const char *value)
{
	char *dup = strdup(value);
	if (!dup)
		return false;
	free(*dst);
	*dst = dup;
	return true;
}

static bool list_add(char ***list, int *n, const char *cmd)
{
	char **e = realloc(*list, (*n + 1) * sizeof *e);
	if (!e)
		return false;
	*list = e;
	(*list)[*n] = strdup(cmd);
	if (!(*list)[*n])
		return false;
	(*n)++;
	return true;
}

/* ── window rules ──────────────────────────────────────────────────────── */

/*
 * Glob match: * is any run of characters, ? is exactly one, \ makes the next
 * character literal. Case-insensitive, because app_ids are not consistently
 * cased across toolkits ("firefox", "org.gnome.Nautilus", "Gimp-2.10") and
 * a rule that silently fails over a capital letter is a rule nobody trusts.
 *
 * Hand-rolled rather than fnmatch(): FNM_CASEFOLD is a GNU extension, and
 * fnmatch's [] classes and / handling are file-name semantics nobody writing
 * a window rule is thinking about. Bytes, not codepoints — ? matches one byte
 * of a multi-byte character, which only matters for ? next to non-ASCII.
 */
static bool glob_match(const char *p, const char *s)
{
	const char *star = NULL, *resume = NULL;

	while (*s) {
		if (*p == '\\' && p[1]) {
			if (tolower((unsigned char)p[1]) == tolower((unsigned char)*s)) {
				p += 2;
				s++;
				continue;
			}
		} else if (*p == '?') {
			p++;
			s++;
			continue;
		} else if (*p == '*') {
			star = p++;
			resume = s;
			continue;
		} else if (*p && tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
			p++;
			s++;
			continue;
		}
		/* mismatch: let the last * swallow one more character, or fail */
		if (!star)
			return false;
		p = star + 1;
		s = ++resume;
	}
	while (*p == '*')
		p++;
	return *p == '\0';
}

/*
 * The next whitespace-separated word, in place. Double quotes group words
 * and are removed, anywhere in the word — so title:"Open File" and
 * "title:Open File" both give `title:Open File`. *bad is set by an
 * unterminated quote. Returns NULL at the end of the line.
 */
static char *next_word(char **cur, bool *bad)
{
	char *p = *cur;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (!*p) {
		*cur = p;
		return NULL;
	}

	char *start = p, *out = p;
	bool quoted = false;
	while (*p && (quoted || !isspace((unsigned char)*p))) {
		if (*p == '"') {
			quoted = !quoted;
			p++;
			continue;
		}
		*out++ = *p++;
	}
	if (quoted)
		*bad = true;
	if (*p)
		p++;            /* the separator; out may lag p, so write after */
	*out = '\0';
	*cur = p;
	return start;
}

static void rule_free(struct q_rule *r)
{
	free(r->app_id);
	free(r->title);
	r->app_id = r->title = NULL;
}

/*
 * "app_id:mpv title:*YouTube* workspace 3 float" — matchers and actions in
 * any order. Reports its own errors, because "bad value for 'rule'" says
 * nothing about which of six words was wrong.
 */
static void parse_rule(struct aro_config *c, int lineno, char *value)
{
	struct q_rule r = {
		.floating = Q_RULE_UNSET,
		.workspace = Q_RULE_UNSET,
		.fullscreen = Q_RULE_UNSET,
	};
	bool acts = false, bad_quote = false;
	char *cur = value, *w;

	while ((w = next_word(&cur, &bad_quote))) {
		if (!strncasecmp(w, "app_id:", 7) || !strncasecmp(w, "title:", 6)) {
			bool is_app = (w[0] == 'a' || w[0] == 'A');
			char *pat = strchr(w, ':') + 1;
			if (!*pat) {
				config_err(c, lineno, "rule: empty %s pattern",
				           is_app ? "app_id" : "title");
				goto fail;
			}
			if (!set_str(is_app ? &r.app_id : &r.title, pat))
				goto fail;
		} else if (!strcasecmp(w, "float")) {
			r.floating = 1;
			acts = true;
		} else if (!strcasecmp(w, "tile")) {
			r.floating = 0;
			acts = true;
		} else if (!strcasecmp(w, "fullscreen")) {
			r.fullscreen = 1;
			acts = true;
		} else if (!strcasecmp(w, "workspace")) {
			char *n = next_word(&cur, &bad_quote);
			int ws;
			if (!n || !parse_int(n, &ws) || ws < 1 || ws > ARO_MAX_WS) {
				config_err(c, lineno,
				           "rule: workspace needs a number from 1 to %d",
				           ARO_MAX_WS);
				goto fail;
			}
			r.workspace = ws - 1;
			acts = true;
		} else {
			config_err(c, lineno, "rule: unknown word '%s'", w);
			goto fail;
		}
	}

	if (bad_quote) {
		config_err(c, lineno, "rule: unterminated quote");
		goto fail;
	}
	if (!r.app_id && !r.title) {
		/* would match every window — much likelier a typo than a wish */
		config_err(c, lineno, "rule: needs app_id: or title:");
		goto fail;
	}
	if (!acts) {
		config_err(c, lineno,
		           "rule: no action (float, tile, fullscreen, workspace N)");
		goto fail;
	}

	struct q_rule *grown = realloc(c->rules, (c->nrules + 1) * sizeof *grown);
	if (!grown)
		goto fail;
	c->rules = grown;
	c->rules[c->nrules++] = r;
	return;

fail:
	rule_free(&r);
}

void config_rules_eval(const struct aro_config *c, const char *app_id,
                       const char *title, struct q_rule_result *out)
{
	*out = (struct q_rule_result){
		Q_RULE_UNSET, Q_RULE_UNSET, Q_RULE_UNSET,
	};
	const char *a = app_id ? app_id : "";
	const char *t = title ? title : "";

	for (int i = 0; i < c->nrules; i++) {
		const struct q_rule *r = &c->rules[i];
		if (r->app_id && !glob_match(r->app_id, a))
			continue;
		if (r->title && !glob_match(r->title, t))
			continue;
		if (r->floating != Q_RULE_UNSET)
			out->floating = r->floating;
		if (r->workspace != Q_RULE_UNSET)
			out->workspace = r->workspace;
		if (r->fullscreen != Q_RULE_UNSET)
			out->fullscreen = r->fullscreen;
	}
}

/* ── monitor blocks ────────────────────────────────────────────────────── */

void config_monitor_unset(struct q_monitor_set *m)
{
	*m = (struct q_monitor_set){
		.enabled = Q_RULE_UNSET,
		.transform = Q_RULE_UNSET,
		.adaptive_sync = Q_RULE_UNSET,
	};
}

void config_monitor_eval(const struct aro_config *c, const char *name,
                         const char *desc, struct q_monitor_set *out)
{
	config_monitor_unset(out);
	const char *n = name ? name : "";
	const char *d = desc ? desc : "";

	for (int i = 0; i < c->nmonitors; i++) {
		const struct q_monitor *m = &c->monitors[i];
		if (!glob_match(m->match, n) && !glob_match(m->match, d))
			continue;
		const struct q_monitor_set *b = &m->set;
		if (b->enabled != Q_RULE_UNSET)
			out->enabled = b->enabled;
		if (b->mode_w) {
			/* a mode is one thing: size and refresh travel together */
			out->mode_w = b->mode_w;
			out->mode_h = b->mode_h;
			out->mode_mhz = b->mode_mhz;
		}
		if (b->has_pos || b->pos_auto) {
			out->has_pos = b->has_pos;
			out->pos_auto = b->pos_auto;
			out->x = b->x;
			out->y = b->y;
		}
		if (b->scale != 0)
			out->scale = b->scale;
		if (b->transform != Q_RULE_UNSET)
			out->transform = b->transform;
		if (b->adaptive_sync != Q_RULE_UNSET)
			out->adaptive_sync = b->adaptive_sync;
	}
}

/* "1920x1080", "1920x1080@60", "2560x1440@143.912Hz", "preferred"/"auto" */
static bool parse_mode(const char *v, struct q_monitor_set *m)
{
	if (!strcasecmp(v, "preferred") || !strcasecmp(v, "auto")) {
		m->mode_w = m->mode_h = -1;
		m->mode_mhz = 0;
		return true;
	}

	char *end;
	long w = strtol(v, &end, 10);
	if (end == v || (*end != 'x' && *end != 'X'))
		return false;
	const char *hs = end + 1;
	long h = strtol(hs, &end, 10);
	if (end == hs)
		return false;

	double hz = 0;
	if (*end == '@') {
		const char *rs = end + 1;
		hz = strtod(rs, &end);
		if (end == rs || hz <= 0 || hz > 1000)
			return false;
		if (!strcasecmp(end, "hz"))
			end += 2;
	}
	if (*end)
		return false;
	if (w < 1 || w > 16384 || h < 1 || h > 16384)
		return false;

	m->mode_w = (int)w;
	m->mode_h = (int)h;
	m->mode_mhz = (int)(hz * 1000 + 0.5);
	return true;
}

/* "1920,0", "1920 0", "-1080,0", or "auto" — to the right of the others */
static bool parse_pos(const char *v, struct q_monitor_set *m)
{
	if (!strcasecmp(v, "auto")) {
		m->pos_auto = true;
		m->has_pos = false;
		return true;
	}

	char *end;
	long x = strtol(v, &end, 10);
	if (end == v)
		return false;
	const char *p = end;
	while (*p == ',' || isspace((unsigned char)*p))
		p++;
	if (p == end)
		return false;           /* needs a separator between the two */
	long y = strtol(p, &end, 10);
	if (end == p || *end)
		return false;
	if (x < -100000 || x > 100000 || y < -100000 || y > 100000)
		return false;
	m->has_pos = true;
	m->pos_auto = false;
	m->x = (int)x;
	m->y = (int)y;
	return true;
}

static bool parse_transform(const char *v, int *out)
{
	static const struct { const char *name; int t; } names[] = {
		{ "normal",      WL_OUTPUT_TRANSFORM_NORMAL },
		{ "auto",        WL_OUTPUT_TRANSFORM_NORMAL },
		{ "0",           WL_OUTPUT_TRANSFORM_NORMAL },
		{ "90",          WL_OUTPUT_TRANSFORM_90 },
		{ "180",         WL_OUTPUT_TRANSFORM_180 },
		{ "270",         WL_OUTPUT_TRANSFORM_270 },
		{ "flipped",     WL_OUTPUT_TRANSFORM_FLIPPED },
		{ "flipped-90",  WL_OUTPUT_TRANSFORM_FLIPPED_90 },
		{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
		{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (!strcasecmp(v, names[i].name)) {
			*out = names[i].t;
			return true;
		}
	}
	return false;
}

/* One `key = value` line inside a monitor block. Reports its own errors. */
static void monitor_key(struct aro_config *c, int lineno,
                        struct q_monitor_set *m, const char *key,
                        const char *value)
{
	bool ok, b;
	const bool is_auto = !strcasecmp(value, "auto");

	if (!strcasecmp(key, "enabled")) {
		/* `auto` is on: a screen you plugged in is a screen you want */
		if (is_auto) {
			m->enabled = 1;
			ok = true;
		} else if ((ok = parse_bool(value, &b))) {
			m->enabled = b;
		}
	} else if (!strcasecmp(key, "mode")) {
		ok = parse_mode(value, m);
	} else if (!strcasecmp(key, "position")) {
		ok = parse_pos(value, m);
	} else if (!strcasecmp(key, "scale")) {
		double d;
		if (is_auto) {
			m->scale = Q_MON_AUTO;
			ok = true;
		} else {
			ok = parse_double(value, &d) && d > 0 && d <= 10;
			if (ok)
				m->scale = d;
		}
	} else if (!strcasecmp(key, "transform")) {
		ok = parse_transform(value, &m->transform);
	} else if (!strcasecmp(key, "adaptive_sync")) {
		/* `auto` is off — the default everywhere that has to be asked */
		if (is_auto) {
			m->adaptive_sync = 0;
			ok = true;
		} else if ((ok = parse_bool(value, &b))) {
			m->adaptive_sync = b;
		}
	} else {
		config_err(c, lineno, "monitor: unknown key '%s' (enabled, mode, "
		           "position, scale, transform, adaptive_sync)", key);
		return;
	}

	if (!ok)
		config_err(c, lineno, "monitor: bad value for '%s'", key);
}

/*
 * The line that opens a block: everything after the word `monitor`.
 * `rest` is `eDP-1 {` or `"Dell *" {`. Returns the new block's index,
 * or -1 after reporting what was wrong — the caller then swallows the
 * block's body — or -2 for the one-line `monitor = ...` form, which has
 * no body to swallow.
 */
static int monitor_open(struct aro_config *c, int lineno, char *rest)
{
	char *p = trim(rest);
	size_t n = strlen(p);

	/* `monitor = eDP-1 ...` — the one-line form people will try first */
	if (*p == '=') {
		config_err(c, lineno, "monitor settings go in a block: "
		           "monitor NAME { ... }");
		return -2;
	}
	if (!n || p[n - 1] != '{') {
		config_err(c, lineno, "monitor: the line must end with '{'");
		return -1;
	}
	p[n - 1] = '\0';

	bool bad_quote = false;
	char *cur = p;
	char *pat = next_word(&cur, &bad_quote);
	if (bad_quote) {
		config_err(c, lineno, "monitor: unterminated quote");
		return -1;
	}
	if (!pat || !*pat) {
		config_err(c, lineno, "monitor: needs a name or pattern, "
		           "e.g. monitor eDP-1 {");
		return -1;
	}
	if (next_word(&cur, &bad_quote)) {
		config_err(c, lineno, "monitor: quote a pattern with spaces in it");
		return -1;
	}

	struct q_monitor *grown =
		realloc(c->monitors, (c->nmonitors + 1) * sizeof *grown);
	if (!grown)
		return -1;
	c->monitors = grown;

	struct q_monitor *m = &c->monitors[c->nmonitors];
	m->match = strdup(pat);
	if (!m->match)
		return -1;
	m->line = lineno;
	config_monitor_unset(&m->set);
	return c->nmonitors++;
}

/*
 * The theme keys, as a table rather than thirty branches. Every one is a
 * name, a type and where in q_theme it lands — adding a setting is one line
 * here and one default in config_defaults().
 */
enum key_type { K_INT, K_COLOR, K_DOUBLE, K_STR };

struct theme_key {
	const char *name;
	enum key_type type;
	size_t offset;
	int min, max;           /* K_INT only */
};

#define T(field) offsetof(struct q_theme, field)
static const struct theme_key theme_keys[] = {
	{ "gaps",            K_INT,    T(gap),           0, 200 },
	{ "outer_gaps",      K_INT,    T(outer_gap),     0, 200 },
	{ "border",          K_INT,    T(border),        0, 40 },
	{ "radius",          K_INT,    T(radius),        0, 64 },
	{ "min_size",        K_INT,    T(min),           1, 2000 },
	{ "header_height",   K_INT,    T(header_h),      0, 200 },
	{ "bar_height",      K_INT,    T(bar_h),         0, 200 },
	{ "bar_padding",     K_INT,    T(bar_pad),       0, 200 },
	{ "text_padding",    K_INT,    T(text_pad),      0, 200 },
	{ "float_min_width", K_INT,    T(float_min_w),   1, 4000 },
	{ "float_min_height",K_INT,    T(float_min_h),   1, 4000 },
	{ "float_scale",     K_DOUBLE, T(float_scale),   0, 0 },

	{ "anim_ms",            K_INT,    T(anim_ms),       0, 5000 },
	{ "fullscreen_anim_ms", K_INT,    T(anim_fs_ms),    0, 5000 },
	{ "focus_anim_ms",      K_INT,    T(anim_focus_ms), 0, 5000 },
	{ "drop_anim_ms",       K_INT,    T(drop_ms),       0, 5000 },
	{ "open_scale",         K_DOUBLE, T(open_scale),    0, 0 },

	{ "drag_threshold", K_INT,    T(drag_tear),   0, 500 },
	{ "resize_zone",    K_INT,    T(resize_zone), 0, 200 },
	{ "resize_step",    K_DOUBLE, T(resize_step), 0, 0 },

	{ "background",  K_COLOR, T(bg),          0, 0 },
	{ "frame",       K_COLOR, T(frame),       0, 0 },
	{ "frame_focus", K_COLOR, T(frame_on),    0, 0 },
	{ "border_color",K_COLOR, T(line),        0, 0 },
	{ "accent",      K_COLOR, T(accent),      0, 0 },
	{ "accent_soft", K_COLOR, T(accent_soft), 0, 0 },
	{ "text",        K_COLOR, T(ink),         0, 0 },
	{ "text_dim",    K_COLOR, T(dim),         0, 0 },
	{ "bar_color",   K_COLOR, T(bar_bg),      0, 0 },
	{ "urgent",      K_COLOR, T(urgent),      0, 0 },
	{ "drop_fill",   K_COLOR, T(drop_fill),   0, 0 },
	{ "drop_line",   K_COLOR, T(drop_line),   0, 0 },
	{ "notify_color",    K_COLOR, T(notify_bg),    0, 0 },
	{ "notify_ms",       K_INT,   T(notify_ms),    0, 120000 },
	{ "notify_max_width",K_INT,   T(notify_max_w), 100, 4000 },
	{ "scrim_color",     K_COLOR, T(scrim),        0, 0 },

	{ "font",       K_STR, T(font),       0, 0 },
	{ "font_small", K_STR, T(font_small), 0, 0 },
};
#undef T

/* Returns false when the key is not a theme key at all, so the caller can
 * carry on to the non-theme keys; *ok reports a bad VALUE for a known key. */
static bool theme_set(struct q_theme *t, const char *key, const char *value,
                      bool *ok)
{
	for (size_t i = 0; i < sizeof theme_keys / sizeof theme_keys[0]; i++) {
		const struct theme_key *k = &theme_keys[i];
		if (strcasecmp(key, k->name))
			continue;

		void *field = (char *)t + k->offset;
		switch (k->type) {
		case K_INT: {
			int n;
			*ok = parse_int(value, &n) && n >= k->min && n <= k->max;
			if (*ok)
				*(int *)field = n;
			break;
		}
		case K_COLOR:
			*ok = parse_color(value, (uint32_t *)field);
			break;
		case K_DOUBLE:
			*ok = parse_double(value, (double *)field);
			break;
		case K_STR: {
			char *dup = strdup(value);
			*ok = dup != NULL;
			if (dup) {
				free(*(char **)field);
				*(char **)field = dup;
			}
			break;
		}
		}
		return true;
	}
	return false;
}

/* The path we read, so the file watcher can watch the same one. NULL when
 * neither XDG_CONFIG_HOME nor HOME is set. Caller frees. */
char *config_path(void)
{
	char buf[512];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	if (xdg && *xdg)
		snprintf(buf, sizeof buf, "%s/aro/config", xdg);
	else if (home && *home)
		snprintf(buf, sizeof buf, "%s/.config/aro/config", home);
	else
		return NULL;

	return strdup(buf);
}

bool config_load(struct aro_config *c, const char *path)
{
	char *owned = NULL;

	if (!path) {
		owned = config_path();
		if (!owned)
			return true;            /* nowhere to look; defaults stand */
		path = owned;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "no config at %s; using defaults", path);
		free(owned);
		return true;
	}

	wlr_log(WLR_INFO, "reading config from %s", path);

	bool binds_replaced = false;
	char line[1024];
	int lineno = 0;

	/*
	 * Monitor blocks. `block` is the open one's index; `in_block` can be
	 * true with block == -1 when the opening line was bad — its body is
	 * then read into `scratch` and dropped, so one typo in a header is
	 * one toast rather than one per line inside it.
	 */
	bool in_block = false;
	int block = -1, block_line = 0;
	struct q_monitor_set scratch;

	while (fgets(line, sizeof line, f)) {
		lineno++;

		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';

		char *p = trim(line);
		if (!*p)
			continue;

		if (!strcmp(p, "}")) {
			if (!in_block)
				config_err(c, lineno, "'}' with no monitor block open");
			in_block = false;
			block = -1;
			continue;
		}
		if (!strncasecmp(p, "monitor", 7) &&
		    (isspace((unsigned char)p[7]) || p[7] == '{' || p[7] == '=')) {
			if (in_block)
				config_err(c, lineno, "monitor block from line %d "
				           "has no '}'", block_line);
			block = monitor_open(c, lineno, p + 7);
			in_block = block != -2;
			block_line = lineno;
			config_monitor_unset(&scratch);
			continue;
		}

		char *eq = strchr(p, '=');
		if (!eq) {
			config_err(c, lineno, "no '='");
			continue;
		}
		*eq = '\0';
		char *key = trim(p);
		char *value = trim(eq + 1);

		if (in_block) {
			monitor_key(c, lineno,
			            block >= 0 ? &c->monitors[block].set : &scratch,
			            key, value);
			continue;
		}

		bool ok = true;

		if (!strcasecmp(key, "mod")) {
			/*
			 * Remap what is already there. Bindings parsed above this
			 * line resolved `mod` to the old modifier, and so did the
			 * built-ins — leaving them on it would mean `mod` meaning
			 * two different things in one file depending on line order.
			 */
			if (!strcasecmp(value, "alt"))
				config_set_modkey(c, WLR_MODIFIER_ALT);
			else if (!strcasecmp(value, "super") || !strcasecmp(value, "logo"))
				config_set_modkey(c, WLR_MODIFIER_LOGO);
			else
				ok = false;
		} else if (theme_set(&c->theme, key, value, &ok)) {
			/* handled by the table; ok already set */
		} else if (!strcasecmp(key, "workspaces")) {
			ok = parse_int(value, &c->workspaces) &&
			     c->workspaces >= 1 && c->workspaces <= ARO_MAX_WS;
		} else if (!strcasecmp(key, "layout")) {
			if (!strcasecmp(value, "manual"))
				c->layout = Q_LAYOUT_MANUAL;
			else if (!strcasecmp(value, "dwindle"))
				c->layout = Q_LAYOUT_DWINDLE;
			else
				ok = false;
		} else if (!strcasecmp(key, "header")) {
			if (!strcasecmp(value, "always"))
				c->header = Q_HEADER_ALWAYS;
			else if (!strcasecmp(value, "auto"))
				c->header = Q_HEADER_AUTO;
			else
				ok = false;
		} else if (!strcasecmp(key, "keyboard_layout")) {
			ok = set_str(&c->xkb_layout, value);
		} else if (!strcasecmp(key, "keyboard_variant")) {
			ok = set_str(&c->xkb_variant, value);
		} else if (!strcasecmp(key, "keyboard_options")) {
			ok = set_str(&c->xkb_options, value);
		} else if (!strcasecmp(key, "keyboard_model")) {
			ok = set_str(&c->xkb_model, value);
		} else if (!strcasecmp(key, "keyboard_rules")) {
			ok = set_str(&c->xkb_rules, value);
		} else if (!strcasecmp(key, "focus_follows_mouse")) {
			ok = parse_bool(value, &c->focus_follows_mouse);
		} else if (!strcasecmp(key, "confirm_quit")) {
			ok = parse_bool(value, &c->confirm_quit);
		} else if (!strcasecmp(key, "exec")) {
			ok = list_add(&c->exec, &c->nexec, value);
		} else if (!strcasecmp(key, "exec_always")) {
			ok = list_add(&c->exec_always, &c->nexec_always, value);
		} else if (!strcasecmp(key, "rule")) {
			parse_rule(c, lineno, value);   /* reports its own errors */
			continue;
		} else if (!strcasecmp(key, "bind")) {
			/* the first bind line clears the built-ins, so a config that
			 * sets bindings starts clean instead of fighting them */
			if (!binds_replaced) {
				clear_binds(c);
				binds_replaced = true;
			}
			ok = parse_bind(c, value);
		} else {
			config_err(c, lineno, "unknown key '%s'", key);
			continue;
		}

		if (!ok)
			config_err(c, lineno, "bad value for '%s'", key);
	}

	/* keep what the unclosed block said; a missing brace is not a
	 * reason to ignore the settings above it */
	if (in_block)
		config_err(c, block_line, "monitor block has no '}'");

	fclose(f);
	free(owned);
	return true;
}

void config_finish(struct aro_config *c)
{
	free(c->theme.font);
	free(c->theme.font_small);
	free(c->xkb_layout);
	free(c->xkb_variant);
	free(c->xkb_options);
	free(c->xkb_model);
	free(c->xkb_rules);
	clear_binds(c);
	free(c->binds);
	for (int i = 0; i < c->nrules; i++)
		rule_free(&c->rules[i]);
	free(c->rules);
	for (int i = 0; i < c->nmonitors; i++)
		free(c->monitors[i].match);
	free(c->monitors);
	for (int i = 0; i < c->nexec; i++)
		free(c->exec[i]);
	free(c->exec);
	for (int i = 0; i < c->nexec_always; i++)
		free(c->exec_always[i]);
	free(c->exec_always);
	for (int i = 0; i < c->nerrors; i++)
		free(c->errors[i]);
	free(c->errors);
	memset(c, 0, sizeof *c);
}

/*
 * Double fork so the child is reparented to init and we never have to reap
 * it. A compositor that collects zombies is a compositor that stalls when a
 * spawned program exits at an awkward moment.
 */
void config_spawn(const char *cmd)
{
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed for '%s'", cmd);
		return;
	}
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);          /* the intermediate child, immediately */
}
