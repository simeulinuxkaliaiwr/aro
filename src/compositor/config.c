/* config.c: config parsing */
/* strsep needs _GNU_SOURCE */
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

/* helpers */

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

/* parse 0xRRGGBBAA / #rrggbbaa / rrggbbaa */
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
	if (end == v)
		return false;
	/* trailing blanks only; read, never write — v may be const */
	while (isspace((unsigned char)*end))
		end++;
	if (*end != '\0')
		return false;
	*out = (int)n;
	return true;
}

/* key bindings */

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

/* parse "mod+shift+h" into mods + level-0 keysym */
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

/*
 * The action half of a bind line: "focus" + "left", "workspace" + "3",
 * "spawn" + "foot". arg may be NULL. Shared by bind lines and by IPC's
 * dispatch, so a command typed at aroctl means exactly what it would mean
 * in the config. For Q_SPAWN the command stays in arg; the caller copies it.
 */
bool config_parse_action(const char *name, const char *arg,
                         enum q_action *action, int *num)
{
	*action = Q_NONE;
	*num = 0;
	if (!name)
		return false;

	if (!strcasecmp(name, "spawn") || !strcasecmp(name, "exec")) {
		if (!arg || !*arg)
			return false;
		*action = Q_SPAWN;
		return true;
	}
	if (!strcasecmp(name, "close"))      { *action = Q_CLOSE;      return true; }
	if (!strcasecmp(name, "quit"))       { *action = Q_QUIT;       return true; }
	if (!strcasecmp(name, "float"))      { *action = Q_FLOAT;      return true; }
	if (!strcasecmp(name, "fullscreen")) { *action = Q_FULLSCREEN; return true; }

	if (!strcasecmp(name, "focus") || !strcasecmp(name, "move") ||
	    !strcasecmp(name, "resize")) {
		if (!arg || !parse_edge(arg, num))
			return false;
		*action = !strcasecmp(name, "focus") ? Q_FOCUS
		        : !strcasecmp(name, "move")  ? Q_MOVE
		                                     : Q_RESIZE;
		return true;
	}

	if (!strcasecmp(name, "workspace") || !strcasecmp(name, "sendto")) {
		int n;
		if (!arg || !parse_int(arg, &n) || n < 1 || n > ARO_MAX_WS)
			return false;
		*num = n - 1;
		*action = !strcasecmp(name, "workspace") ? Q_WORKSPACE : Q_SENDTO;
		return true;
	}

	if (!strcasecmp(name, "switch")) {
		if (!arg || !*arg || !strcasecmp(arg, "next"))
			*num = 1;
		else if (!strcasecmp(arg, "prev") || !strcasecmp(arg, "previous"))
			*num = -1;
		else
			return false;
		*action = Q_SWITCH;
		return true;
	}

	if (!strcasecmp(name, "split")) {
		if (!arg)
			return false;
		if (!strcasecmp(arg, "right") || !strcasecmp(arg, "horizontal"))
			*num = LY_ROW;
		else if (!strcasecmp(arg, "down") || !strcasecmp(arg, "vertical"))
			*num = LY_COL;
		else
			return false;
		*action = Q_SPLIT;
		return true;
	}

	if (!strcasecmp(name, "layout")) {
		if (!arg || !*arg || !strcasecmp(arg, "toggle"))
			*num = Q_LAYOUT_TOGGLE;
		else if (!strcasecmp(arg, "manual"))
			*num = Q_LAYOUT_MANUAL;
		else if (!strcasecmp(arg, "dwindle"))
			*num = Q_LAYOUT_DWINDLE;
		else
			return false;
		*action = Q_LAYOUT;
		return true;
	}

	return false;
}

static bool parse_layout(const char *s, enum q_layout *out)
{
	if (!strcasecmp(s, "manual"))  { *out = Q_LAYOUT_MANUAL;  return true; }
	if (!strcasecmp(s, "dwindle")) { *out = Q_LAYOUT_DWINDLE; return true; }
	return false;
}

const char *config_layout_name(enum q_layout l)
{
	return l == Q_LAYOUT_DWINDLE ? "dwindle" : "manual";
}

enum q_layout config_ws_layout(const struct aro_config *c, int ws)
{
	if (ws >= 0 && ws < ARO_MAX_WS && c->ws_layout[ws] != Q_LAYOUT_INHERIT)
		return (enum q_layout)c->ws_layout[ws];
	return c->layout;
}

/* bind line: combo, action, arg */
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

	enum q_action a;
	int num;
	if (!config_parse_action(action, arg, &a, &num))
		return false;
	return bind_add(c, mods, sym, a, num, a == Q_SPAWN ? arg : NULL);
}

/* defaults */

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
	bind_add(c, M, XKB_KEY_Tab, Q_SWITCH, 1, NULL);
	bind_add(c, M | S, XKB_KEY_Tab, Q_SWITCH, -1, NULL);
	bind_add(c, M, XKB_KEY_t, Q_LAYOUT, Q_LAYOUT_TOGGLE, NULL);

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
	c->bar = TH_BAR;
	c->wallpaper = Q_WALLPAPER_AUTO;
	c->layout = Q_LAYOUT_MANUAL;
	for (int i = 0; i < ARO_MAX_WS; i++)
		c->ws_layout[i] = Q_LAYOUT_INHERIT;
	c->header = Q_HEADER_ALWAYS;
	c->ws_slide = Q_SLIDE_HORIZONTAL;

	c->theme = (struct q_theme){
		.gap = TH_GAP, .outer_gap = TH_OUTER_GAP, .border = TH_BORDER,
		.radius = TH_RADIUS, .min = TH_MIN,
		.header_h = TH_HEADER_H, .bar_h = TH_BAR_H,
		.bar_pad = TH_BAR_PAD, .text_pad = TH_TEXT_PAD,
		.float_min_w = TH_FLOAT_MIN_W, .float_min_h = TH_FLOAT_MIN_H,
		.float_scale = TH_FLOAT_SCALE,

		.anim_ms = TH_ANIM_MS, .anim_fs_ms = TH_ANIM_FS_MS,
		.anim_focus_ms = TH_ANIM_FOCUS_MS, .drop_ms = TH_DROP_MS,
		.ws_slide_ms = TH_WS_SLIDE_MS,
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
		.switch_delay_ms = TH_SWITCH_DELAY_MS,
		.switch_debounce_ms = TH_SWITCH_DEBOUNCE_MS,
		.switch_preview_h = TH_SWITCH_PREVIEW_H,
	};
	c->theme.font = strdup(TH_FONT);
	c->theme.font_small = strdup(TH_FONT_SMALL);

	install_default_binds(c);
}

/* config loading */

static void clear_binds(struct aro_config *c)
{
	for (int i = 0; i < c->nbinds; i++)
		free(c->binds[i].arg);
	c->nbinds = 0;
}

/* record and log a parse error; never fatal */
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

/* window rules */

/* case-insensitive glob: * and ?, with \\ escaping */
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

/* next word; double quotes group spaces */
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
	free(r->type);
	r->app_id = r->title = r->type = NULL;
}

/* workspace = N layout X */
static void parse_workspace(struct aro_config *c, int lineno, char *value)
{
	bool bad_quote = false;
	char *cur = value;
	char *n = next_word(&cur, &bad_quote);
	int ws;

	if (!n || !parse_int(n, &ws) || ws < 1 || ws > ARO_MAX_WS) {
		config_err(c, lineno, "workspace: needs a number from 1 to %d",
		           ARO_MAX_WS);
		return;
	}

	int layout = Q_LAYOUT_INHERIT;
	bool any = false;
	char *w;
	while ((w = next_word(&cur, &bad_quote))) {
		if (!strcasecmp(w, "layout")) {
			char *l = next_word(&cur, &bad_quote);
			enum q_layout ql;
			if (!l || !parse_layout(l, &ql)) {
				config_err(c, lineno, "workspace %d: layout is manual "
				           "or dwindle", ws);
				return;
			}
			layout = ql;
			any = true;
		} else {
			config_err(c, lineno, "workspace %d: unknown word '%s'", ws, w);
			return;
		}
	}
	if (bad_quote) {
		config_err(c, lineno, "workspace %d: unterminated quote", ws);
		return;
	}
	if (!any) {
		config_err(c, lineno, "workspace %d: nothing to set (try "
		           "'layout dwindle')", ws);
		return;
	}
	c->ws_layout[ws - 1] = layout;
}

/* parse one rule line */
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
		if (!strncasecmp(w, "app_id:", 7) || !strncasecmp(w, "title:", 6) ||
		    !strncasecmp(w, "type:", 5)) {
			char **field = (w[0] == 'a' || w[0] == 'A') ? &r.app_id
			             : (w[1] == 'i' || w[1] == 'I') ? &r.title
			             : &r.type;
			const char *name = field == &r.app_id ? "app_id"
			                 : field == &r.title ? "title" : "type";
			char *pat = strchr(w, ':') + 1;
			if (!*pat) {
				config_err(c, lineno, "rule: empty %s pattern", name);
				goto fail;
			}
			if (!set_str(field, pat))
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
	if (!r.app_id && !r.title && !r.type) {
		/* would match every window — much likelier a typo than a wish */
		config_err(c, lineno, "rule: needs app_id:, title: or type:");
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
                       const char *title, const char *type,
                       struct q_rule_result *out)
{
	*out = (struct q_rule_result){
		Q_RULE_UNSET, Q_RULE_UNSET, Q_RULE_UNSET,
	};
	const char *a = app_id ? app_id : "";
	const char *t = title ? title : "";
	const char *y = type ? type : "normal";

	for (int i = 0; i < c->nrules; i++) {
		const struct q_rule *r = &c->rules[i];
		if (r->app_id && !glob_match(r->app_id, a))
			continue;
		if (r->title && !glob_match(r->title, t))
			continue;
		if (r->type && !glob_match(r->type, y))
			continue;
		if (r->floating != Q_RULE_UNSET)
			out->floating = r->floating;
		if (r->workspace != Q_RULE_UNSET)
			out->workspace = r->workspace;
		if (r->fullscreen != Q_RULE_UNSET)
			out->fullscreen = r->fullscreen;
	}
}

/* monitor blocks */

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

/* parse mode: WxH[@Hz], preferred/auto */
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

/* parse position: x,y or auto */
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

/* one key inside a monitor block */
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

/* open a monitor block */
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

/* theme keys table */
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
	{ "workspace_slide_ms", K_INT,    T(ws_slide_ms),   0, 5000 },
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
	{ "switcher_delay_ms",      K_INT, T(switch_delay_ms),    0, 2000 },
	{ "switcher_debounce_ms",   K_INT, T(switch_debounce_ms), 0, 5000 },
	{ "switcher_preview_height",K_INT, T(switch_preview_h),   40, 1000 },
	{ "scrim_color",     K_COLOR, T(scrim),        0, 0 },

	{ "font",       K_STR, T(font),       0, 0 },
	{ "font_small", K_STR, T(font_small), 0, 0 },
};
#undef T

/* set a theme key; false means not a theme key */
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

/* config file path */
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

	/* monitor block state while parsing */
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
			/* changing mod remaps existing bindings */
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
			ok = parse_layout(value, &c->layout);
		} else if (!strcasecmp(key, "workspace")) {
			parse_workspace(c, lineno, value);
			continue;
		} else if (!strcasecmp(key, "header")) {
			if (!strcasecmp(value, "always"))
				c->header = Q_HEADER_ALWAYS;
			else if (!strcasecmp(value, "auto"))
				c->header = Q_HEADER_AUTO;
			else
				ok = false;
		} else if (!strcasecmp(key, "workspace_slide")) {
			if (!strcasecmp(value, "horizontal"))
				c->ws_slide = Q_SLIDE_HORIZONTAL;
			else if (!strcasecmp(value, "vertical"))
				c->ws_slide = Q_SLIDE_VERTICAL;
			else if (!strcasecmp(value, "off"))
				c->ws_slide = Q_SLIDE_OFF;
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
		} else if (!strcasecmp(key, "bar")) {
			ok = parse_bool(value, &c->bar);
		} else if (!strcasecmp(key, "wallpaper")) {
			/* the keywords are words, not paths: `none` never means a
			 * file called none. ./none does, if anyone needs it. */
			if (!strcasecmp(value, "auto")) {
				c->wallpaper = Q_WALLPAPER_AUTO;
			} else if (!strcasecmp(value, "none")) {
				c->wallpaper = Q_WALLPAPER_NONE;
			} else if (!*value) {
				ok = false;
			} else {
				ok = set_str(&c->wallpaper_file, value);
				if (ok)
					c->wallpaper = Q_WALLPAPER_FILE;
			}
		} else if (!strcasecmp(key, "exec")) {
			ok = list_add(&c->exec, &c->nexec, value);
		} else if (!strcasecmp(key, "exec_always")) {
			ok = list_add(&c->exec_always, &c->nexec_always, value);
		} else if (!strcasecmp(key, "rule")) {
			parse_rule(c, lineno, value);   /* reports its own errors */
			continue;
		} else if (!strcasecmp(key, "bind")) {
			/* first bind line replaces defaults */
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

	/* keep settings from an unclosed block */
	if (in_block)
		config_err(c, block_line, "monitor block has no '}'");

	/* no bar takes no room; see aro_config.bar */
	if (!c->bar)
		c->theme.bar_h = 0;

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
	free(c->wallpaper_file);
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

/* double-fork so we don't reap zombies */
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
