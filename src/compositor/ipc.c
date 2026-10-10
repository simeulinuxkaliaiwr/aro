/* accept4, SOCK_CLOEXEC; before every include */
#define _GNU_SOURCE

/*
 * ipc.c: the server side of aroctl.
 *
 * A listening unix socket on the event loop. Each connection carries one
 * request line; the whole reply is built in memory, written (resuming on
 * EAGAIN if it does not fit the socket buffer), and the connection closed.
 * A subscriber's connection stays open instead: after each burst of
 * changes, aro compares what it shows with what it showed last time and
 * sends a line per difference.
 *
 * Text replies are for people: aligned columns, a header, the focused row
 * marked. JSON is for scripts: one line, every field, pipe it to jq.
 */

/* scene.h must come first */
#include "scene.h"

#include "ipc.h"
#include "aro.h"
#include "config.h"
#include "logfile.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>            /* strcasecmp */
#include <sys/socket.h>
#include <sys/stat.h>           /* umask */
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>   /* trap 14: we read wlr_output fields */
#include <wlr/util/log.h>

#ifndef ARO_VERSION
#define ARO_VERSION "unknown"
#endif

#define IPC_MAX_REQ     4096    /* one request line, newline included */
#define IPC_MAX_CONNS   64      /* beyond this a new connection is refused */
#define IPC_MAX_BACKLOG (1 << 20)       /* a subscriber this far behind is dropped */
#define IPC_TIMEOUT_MS  2000    /* a client that stalls is dropped */

/* ── a growable string ─────────────────────────────────────────────────── */

struct sbuf {
	char *d;
	size_t len, cap;
	bool oom;               /* sticky; the reply becomes an error */
};

static bool sb_grow(struct sbuf *b, size_t need)
{
	if (b->oom)
		return false;
	if (b->len + need + 1 <= b->cap)
		return true;
	size_t cap = b->cap ? b->cap : 1024;
	while (cap < b->len + need + 1)
		cap *= 2;
	char *d = realloc(b->d, cap);
	if (!d) {
		b->oom = true;
		return false;
	}
	b->d = d;
	b->cap = cap;
	return true;
}

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
	if (!sb_grow(b, n))
		return;
	memcpy(b->d + b->len, s, n);
	b->len += n;
	b->d[b->len] = '\0';
}

static void sb_puts(struct sbuf *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

__attribute__((format(printf, 2, 3)))
static void sb_printf(struct sbuf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0 || !sb_grow(b, (size_t)n))
		return;
	va_start(ap, fmt);
	vsnprintf(b->d + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

/* pad with spaces to a column width */
static void sb_pad(struct sbuf *b, const char *s, int width)
{
	sb_puts(b, s);
	for (int i = (int)strlen(s); i < width; i++)
		sb_put(b, " ", 1);
}

/* a JSON string, quotes included; NULL becomes null */
static void sb_json_str(struct sbuf *b, const char *s)
{
	if (!s) {
		sb_puts(b, "null");
		return;
	}
	sb_put(b, "\"", 1);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':  sb_puts(b, "\\\""); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '\n': sb_puts(b, "\\n");  break;
		case '\t': sb_puts(b, "\\t");  break;
		case '\r': sb_puts(b, "\\r");  break;
		default:
			if (*p < 0x20 || *p == 0x7f)
				sb_printf(b, "\\u%04x", *p);
			else
				sb_put(b, (const char *)p, 1);
		}
	}
	sb_put(b, "\"", 1);
}

/* for text output: a title with a newline in it must not break the table */
static void text_clean(char *dst, size_t size, const char *s)
{
	size_t i = 0;
	if (s)
		for (; *s && i + 1 < size; s++)
			dst[i++] = ((unsigned char)*s < 0x20 || *s == 0x7f) ? ' ' : *s;
	dst[i] = '\0';
}

/* ── the request ───────────────────────────────────────────────────────── */

struct req {
	struct aro_server *s;
	bool json;
	const char *args;       /* after the command word; "" when none */
	struct sbuf *body;
	bool subscribe;         /* keep the connection open for events */
	char err[256];          /* set = the reply is `error <err>` */
};

__attribute__((format(printf, 2, 3)))
static void req_fail(struct req *r, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->err, sizeof r->err, fmt, ap);
	va_end(ap);
}

static const char *transform_name(int t)
{
	static const char *names[] = {
		"normal", "90", "180", "270",
		"flipped", "flipped-90", "flipped-180", "flipped-270",
	};
	return t >= 0 && t < 8 ? names[t] : "normal";
}

/* mapped windows on this output's workspace ws, floating ones included */
static int ws_count(struct aro_server *s, struct aro_output *o, int ws)
{
	int n = 0;
	struct aro_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && !v->stashed && v->output == o && v->workspace == ws)
			n++;
	return n;
}

/* ── monitors ──────────────────────────────────────────────────────────── */

static void monitor_json(struct req *r, struct aro_output *o, bool first)
{
	struct wlr_output *wo = o->wlr_output;
	struct sbuf *b = r->body;
	bool focused = o == aro_focused_output(r->s) && o->enabled;

	sb_puts(b, first ? "{" : ",{");
	sb_puts(b, "\"name\":");
	sb_json_str(b, wo->name);
	sb_puts(b, ",\"description\":");
	sb_json_str(b, wo->description);
	sb_puts(b, ",\"make\":");
	sb_json_str(b, wo->make);
	sb_puts(b, ",\"model\":");
	sb_json_str(b, wo->model);
	sb_puts(b, ",\"serial\":");
	sb_json_str(b, wo->serial);
	sb_printf(b, ",\"enabled\":%s,\"power\":%s,\"focused\":%s",
	          o->enabled ? "true" : "false",
	          wo->enabled ? "true" : "false",
	          focused ? "true" : "false");
	if (o->enabled) {
		sb_printf(b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
		          o->box.x, o->box.y, o->box.w, o->box.h);
		sb_printf(b, ",\"mode\":{\"width\":%d,\"height\":%d,\"refresh\":%d}",
		          wo->width, wo->height, wo->refresh);
		sb_printf(b, ",\"scale\":%g", (double)wo->scale);
		sb_puts(b, ",\"transform\":");
		sb_json_str(b, transform_name((int)wo->transform));
		sb_printf(b, ",\"adaptive_sync\":%s,\"workspace\":%d",
		          wo->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED
		                  ? "true" : "false",
		          o->cur_ws + 1);
	}
	sb_puts(b, "}");
}

static void monitor_text(struct req *r, struct aro_output *o)
{
	struct wlr_output *wo = o->wlr_output;
	struct sbuf *b = r->body;
	bool focused = o == aro_focused_output(r->s) && o->enabled;

	sb_printf(b, "%s%s\n", wo->name,
	          !o->enabled ? " (disabled)" : focused ? " (focused)" : "");
	if (wo->description)
		sb_printf(b, "  description    %s\n", wo->description);
	if (!o->enabled)
		return;
	sb_printf(b, "  mode           %dx%d @ %.3f Hz\n",
	          wo->width, wo->height, wo->refresh / 1000.0);
	sb_printf(b, "  position       %d,%d\n", o->box.x, o->box.y);
	sb_printf(b, "  size           %dx%d (logical)\n", o->box.w, o->box.h);
	sb_printf(b, "  scale          %g\n", (double)wo->scale);
	sb_printf(b, "  transform      %s\n", transform_name((int)wo->transform));
	sb_printf(b, "  adaptive sync  %s\n",
	          wo->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED
	                  ? "on" : "off");
	sb_printf(b, "  power          %s\n", wo->enabled ? "on" : "off");
	sb_printf(b, "  workspace      %d\n", o->cur_ws + 1);
}

static void cmd_monitors(struct req *r)
{
	struct aro_server *s = r->s;
	struct wl_list *lists[2] = { &s->outputs, &s->outputs_off };
	bool first = true;

	if (r->json)
		sb_puts(r->body, "[");
	for (int i = 0; i < 2; i++) {
		struct aro_output *o;
		wl_list_for_each(o, lists[i], link) {
			if (r->json)
				monitor_json(r, o, first);
			else {
				if (!first)
					sb_puts(r->body, "\n");
				monitor_text(r, o);
			}
			first = false;
		}
	}
	if (r->json)
		sb_puts(r->body, "]\n");
}

/* ── workspaces ────────────────────────────────────────────────────────── */

/* the pills the bar shows: the first `workspaces`, the current, any in use */
static bool ws_listed(struct aro_server *s, struct aro_output *o, int ws,
                      int count)
{
	return ws < s->cfg.workspaces || ws == o->cur_ws || count > 0;
}

static void cmd_workspaces(struct req *r)
{
	struct aro_server *s = r->s;
	struct aro_output *fo = aro_focused_output(s);
	struct sbuf *b = r->body;
	struct aro_output *o;

	if (r->json) {
		bool first = true;
		sb_puts(b, "[");
		wl_list_for_each(o, &s->outputs, link) {
			for (int i = 0; i < ARO_MAX_WS; i++) {
				int n = ws_count(s, o, i);
				if (!ws_listed(s, o, i, n))
					continue;
				sb_puts(b, first ? "{" : ",{");
				first = false;
				sb_puts(b, "\"output\":");
				sb_json_str(b, o->wlr_output->name);
				sb_printf(b, ",\"index\":%d,\"windows\":%d,"
				          "\"layout\":\"%s\","
				          "\"visible\":%s,\"focused\":%s}",
				          i + 1, n,
				          config_layout_name(aro_ws_layout(o, i)),
				          i == o->cur_ws ? "true" : "false",
				          o == fo && i == o->cur_ws ? "true" : "false");
			}
		}
		sb_puts(b, "]\n");
		return;
	}

	int w = (int)strlen("OUTPUT");
	wl_list_for_each(o, &s->outputs, link) {
		int n = (int)strlen(o->wlr_output->name);
		if (n > w)
			w = n;
	}
	w += 2;

	sb_pad(b, "OUTPUT", w);
	sb_puts(b, "WS  WINDOWS  LAYOUT   STATE\n");
	wl_list_for_each(o, &s->outputs, link) {
		for (int i = 0; i < ARO_MAX_WS; i++) {
			int n = ws_count(s, o, i);
			if (!ws_listed(s, o, i, n))
				continue;
			const char *state = i != o->cur_ws ? ""
			                  : o == fo ? "focused" : "visible";
			const char *lay = config_layout_name(aro_ws_layout(o, i));
			sb_pad(b, o->wlr_output->name, w);
			if (*state)
				sb_printf(b, "%-4d%-9d%-9s%s\n", i + 1, n, lay, state);
			else
				sb_printf(b, "%-4d%-9d%s\n", i + 1, n, lay);
		}
	}
}

/* ── windows ───────────────────────────────────────────────────────────── */

static const char *view_state(struct aro_view *v)
{
	return v->stashed ? "scratchpad" : v->fullscreen ? "fullscreen" : v->sticky ? "sticky"
	     : v->group ? "tabbed"
	     : v->floating ? "floating" : "tiled";
}

static const char *view_shell(struct aro_view *v)
{
	return v->toplevel ? "xdg" : "xwayland";
}

static void view_json(struct req *r, struct aro_view *v)
{
	struct sbuf *b = r->body;
	ly_box box = aro_view_box(v);

	sb_printf(b, "{\"id\":%u,\"app_id\":", v->id);
	sb_json_str(b, view_app_id(v));
	sb_puts(b, ",\"title\":");
	sb_json_str(b, view_title(v));
	sb_puts(b, ",\"type\":");
	sb_json_str(b, aro_view_type(v));
	sb_puts(b, ",\"shell\":");
	sb_json_str(b, view_shell(v));
	sb_puts(b, ",\"output\":");
	sb_json_str(b, v->output ? v->output->wlr_output->name : NULL);
	sb_printf(b, ",\"workspace\":%d,\"state\":\"%s\","
	          "\"floating\":%s,\"sticky\":%s,\"fullscreen\":%s,\"focused\":%s,"
	          "\"visible\":%s,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
	          v->workspace + 1, view_state(v),
	          v->floating ? "true" : "false",
	          v->sticky ? "true" : "false",
	          v->fullscreen ? "true" : "false",
	          v == r->s->focused ? "true" : "false",
	          v->output && !v->stashed && !view_tab_hidden(v) && v->workspace == v->output->cur_ws
	              ? "true" : "false",
	          box.x, box.y, box.w, box.h);
}

/* the fields a text row shows, cleaned and formatted */
struct row {
	char id[16], out[32], ws[16], geo[64], app[128], title[512];
	const char *state;
	bool focused;
};

static void row_fill(struct aro_server *s, struct aro_view *v, struct row *w)
{
	ly_box box = aro_view_box(v);
	snprintf(w->id, sizeof w->id, "%u", v->id);
	text_clean(w->out, sizeof w->out,
	           v->output ? v->output->wlr_output->name : "-");
	snprintf(w->ws, sizeof w->ws, "%d", v->workspace + 1);
	snprintf(w->geo, sizeof w->geo, "%dx%d%+d%+d", box.w, box.h, box.x, box.y);
	text_clean(w->app, sizeof w->app, view_app_id(v));
	if (!w->app[0])
		snprintf(w->app, sizeof w->app, "-");
	text_clean(w->title, sizeof w->title, view_title(v));
	if (!w->title[0])
		snprintf(w->title, sizeof w->title, "-");
	w->state = view_state(v);
	w->focused = v == s->focused;
}

static void cmd_windows(struct req *r)
{
	struct aro_server *s = r->s;
	struct sbuf *b = r->body;
	struct aro_view *v;

	/* s->views has the newest first; reversed is ascending id */
	if (r->json) {
		bool first = true;
		sb_puts(b, "[");
		wl_list_for_each_reverse(v, &s->views, link) {
			if (!v->mapped)
				continue;
			if (!first)
				sb_puts(b, ",");
			first = false;
			view_json(r, v);
		}
		sb_puts(b, "]\n");
		return;
	}

	/* two passes: widths, then rows */
	int wid = 2, wout = 6, wws = 2, wstate = 5, wgeo = 8, wapp = 6;
	struct row row;
	wl_list_for_each_reverse(v, &s->views, link) {
		if (!v->mapped)
			continue;
		row_fill(s, v, &row);
#define WIDEN(w, str) do { int n_ = (int)strlen(str); if (n_ > (w)) (w) = n_; } while (0)
		WIDEN(wid, row.id);
		WIDEN(wout, row.out);
		WIDEN(wws, row.ws);
		WIDEN(wstate, row.state);
		WIDEN(wgeo, row.geo);
		WIDEN(wapp, row.app);
#undef WIDEN
	}

	sb_puts(b, "  ");
	sb_pad(b, "ID", wid + 2);
	sb_pad(b, "OUTPUT", wout + 2);
	sb_pad(b, "WS", wws + 2);
	sb_pad(b, "STATE", wstate + 2);
	sb_pad(b, "GEOMETRY", wgeo + 2);
	sb_pad(b, "APP_ID", wapp + 2);
	sb_puts(b, "TITLE\n");

	wl_list_for_each_reverse(v, &s->views, link) {
		if (!v->mapped)
			continue;
		row_fill(s, v, &row);
		sb_puts(b, row.focused ? "* " : "  ");
		sb_pad(b, row.id, wid + 2);
		sb_pad(b, row.out, wout + 2);
		sb_pad(b, row.ws, wws + 2);
		sb_pad(b, row.state, wstate + 2);
		sb_pad(b, row.geo, wgeo + 2);
		sb_pad(b, row.app, wapp + 2);
		sb_puts(b, row.title);
		sb_puts(b, "\n");
	}
}

static void cmd_focused(struct req *r)
{
	struct aro_view *v = r->s->focused;
	if (!v || !v->mapped) {
		req_fail(r, "no window is focused");
		return;
	}
	if (r->json) {
		view_json(r, v);
		sb_puts(r->body, "\n");
		return;
	}

	struct row row;
	row_fill(r->s, v, &row);
	struct sbuf *b = r->body;
	sb_printf(b, "id         %s\n", row.id);
	sb_printf(b, "app_id     %s\n", row.app);
	sb_printf(b, "title      %s\n", row.title);
	sb_printf(b, "type       %s\n", aro_view_type(v));
	sb_printf(b, "shell      %s\n", view_shell(v));
	sb_printf(b, "output     %s\n", row.out);
	sb_printf(b, "workspace  %s\n", row.ws);
	sb_printf(b, "state      %s\n", row.state);
	sb_printf(b, "geometry   %s\n", row.geo);
}

/* ── actions ───────────────────────────────────────────────────────────── */

static void cmd_reload(struct req *r)
{
	struct aro_server *s = r->s;
	aro_config_reload(s);

	/* a reload with bad lines still happened; the problems are the body */
	if (r->json) {
		sb_puts(r->body, "{\"errors\":[");
		for (int i = 0; i < s->cfg.nerrors; i++) {
			if (i)
				sb_puts(r->body, ",");
			sb_json_str(r->body, s->cfg.errors[i]);
		}
		sb_puts(r->body, "]}\n");
		return;
	}
	for (int i = 0; i < s->cfg.nerrors; i++)
		sb_printf(r->body, "%s\n", s->cfg.errors[i]);
  if (s->cfg.nerrors == 0)
    sb_puts(r->body, "ok\n");
}

/* "wallpaper": what is shown; "wallpaper auto|none|/abs/file": show that */
static void cmd_wallpaper(struct req *r)
{
	struct aro_server *s = r->s;
	const char *a = r->args;
	while (isspace((unsigned char)*a))
		a++;
	if (*a) {
		enum q_wallpaper mode = !strcasecmp(a, "auto") ? Q_WALLPAPER_AUTO
		                      : !strcasecmp(a, "none") ? Q_WALLPAPER_NONE
		                      : Q_WALLPAPER_FILE;
		if (mode == Q_WALLPAPER_FILE && *a != '/') {
			req_fail(r, "wallpaper needs an absolute path, auto or none");
			return;
		}
		if (!aro_wallpaper_set(s, mode, a)) {
			req_fail(r, "out of memory");
			return;
		}
	}
	enum q_wallpaper mode = s->wallpaper_set ? s->wallpaper_mode : s->cfg.wallpaper;
	const char *file = s->wallpaper_set ? s->wallpaper_file : s->cfg.wallpaper_file;
	const char *shown = mode == Q_WALLPAPER_AUTO ? "auto"
	                  : mode == Q_WALLPAPER_NONE ? "none" : file ? file : "";
	if (r->json) {
		sb_puts(r->body, "{\"wallpaper\":");
		sb_json_str(r->body, shown);
		sb_puts(r->body, "}\n");
	} else {
		sb_printf(r->body, "%s\n", shown);
	}
}

static void cmd_dispatch(struct req *r)
{
	/* "focus left", "spawn foot --server", "workspace 3" */
	char buf[IPC_MAX_REQ];
	snprintf(buf, sizeof buf, "%s", r->args);

	char *name = buf;
	char *arg = name;
	while (*arg && !isspace((unsigned char)*arg))
		arg++;
	if (*arg)
		*arg++ = '\0';
	while (isspace((unsigned char)*arg))
		arg++;

	if (!*name) {
		req_fail(r, "dispatch needs an action, as in a bind line: "
		         "dispatch focus left");
		return;
	}

	enum q_action action;
	int num;
	if (!config_parse_action(name, *arg ? arg : NULL, &action, &num)) {
		req_fail(r, "bad action '%s%s%s'", name, *arg ? " " : "", arg);
		return;
	}
	/* the switcher commits when mod is released: a release that a
	 * socket will never send, so it would stay open */
	if (action == Q_SWITCH) {
		req_fail(r, "switch needs a held modifier; it cannot be dispatched");
		return;
	}
	if (aro_locked(r->s)) {
		req_fail(r, "the session is locked");
		return;
	}

	struct q_bind b = {
		.action = action,
		.num = num,
		.arg = (action == Q_SPAWN || action == Q_SCRATCH_SHOW) && *arg ? arg : NULL,
	};
	aro_run_action(r->s, &b);
	if (r->json)
		sb_puts(r->body, "{}\n");
}

static void cmd_version(struct req *r)
{
	if (r->json)
		sb_puts(r->body, "{\"version\":\"" ARO_VERSION "\"}\n");
	else
		sb_puts(r->body, "aro " ARO_VERSION "\n");
}

/* log path only; aroctl reads the file */
static void cmd_log(struct req *r)
{
	const char *path = logfile_path();
	if (!path) {
		req_fail(r, "aro is writing no log file (started with -l none, "
		         "or it could not open one; see its stderr)");
		return;
	}
	if (r->json) {
		sb_puts(r->body, "{\"path\":");
		sb_json_str(r->body, path);
		sb_puts(r->body, "}\n");
	} else {
		sb_printf(r->body, "%s\n", path);
	}
}

/* the reply is only `ok`; the connection then carries events */
static void cmd_subscribe(struct req *r)
{
	r->subscribe = true;
}

static const struct {
	const char *name;
	void (*fn)(struct req *r);
	bool args;              /* takes arguments */
} commands[] = {
	{ "monitors",   cmd_monitors,   false },
	{ "outputs",    cmd_monitors,   false },
	{ "workspaces", cmd_workspaces, false },
	{ "windows",    cmd_windows,    false },
	{ "focused",    cmd_focused,    false },
	{ "reload",     cmd_reload,     false },
	{ "dispatch",   cmd_dispatch,   true },
	{ "wallpaper",  cmd_wallpaper,  true },
	{ "log",        cmd_log,        false },
	{ "subscribe",  cmd_subscribe,  false },
	{ "version",    cmd_version,    false },
};

/* one request line in, the whole reply out: status line, then body;
 * *json and *subscribe say how a subscription should be fed */
static void ipc_handle(struct aro_server *s, char *line, struct sbuf *out,
                       bool *json, bool *subscribe)
{
	struct sbuf body = { 0 };
	struct req r = { .s = s, .body = &body, .args = "" };
	char *cmd = NULL, *end = NULL;

	/* first word: the format */
	char *p = line;
	while (isspace((unsigned char)*p))
		p++;
	char *fmt = p;
	while (*p && !isspace((unsigned char)*p))
		p++;
	if (*p)
		*p++ = '\0';

	if (!strcmp(fmt, "json"))
		r.json = true;
	else if (strcmp(fmt, "text")) {
		req_fail(&r, "bad request: expected 'text' or 'json' first");
		goto reply;
	}

	/* second word: the command; the rest is its arguments */
	while (isspace((unsigned char)*p))
		p++;
	cmd = p;
	while (*p && !isspace((unsigned char)*p))
		p++;
	if (*p)
		*p++ = '\0';
	while (isspace((unsigned char)*p))
		p++;
	end = p + strlen(p);
	while (end > p && isspace((unsigned char)end[-1]))
		*--end = '\0';
	r.args = p;

	for (size_t i = 0; i < sizeof commands / sizeof *commands; i++) {
		if (strcasecmp(cmd, commands[i].name))
			continue;
		if (!commands[i].args && *r.args) {
			req_fail(&r, "'%s' takes no arguments", commands[i].name);
			goto reply;
		}
		commands[i].fn(&r);
		goto reply;
	}
	if (!*cmd)
		req_fail(&r, "no command");
	else
		req_fail(&r, "unknown command '%s'", cmd);

reply:
	if (body.oom && !r.err[0])
		req_fail(&r, "out of memory");
	if (r.err[0]) {
		wlr_log(WLR_INFO, "ipc: error %s", r.err);
		sb_printf(out, "error %s\n", r.err);
	} else {
		sb_puts(out, "ok\n");
		if (body.len)
			sb_put(out, body.d, body.len);
	}
	free(body.d);
	*json = r.json;
	*subscribe = r.subscribe && !r.err[0];
}

/* ── connections ───────────────────────────────────────────────────────── */

/* what subscribers were last told about */
struct snap_view {
	uint32_t id;
	char *app_id, *title, *output;
	int ws;
};

struct snap_output {
	char *name;
	int ws, layout;
};

struct snap {
	struct snap_view *views;
	int nviews;
	struct snap_output *outputs;
	int noutputs;
	uint32_t focus;                 /* 0: nothing focused */
};

struct aro_ipc {
	struct aro_server *s;
	int fd;
	struct wl_event_source *src;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct wl_list conns;           /* ipc_conn.link */
	int nconns;

	int nsubs;
	struct wl_event_source *idle;   /* a report is due */
	struct snap last;               /* valid while nsubs > 0 */
};

struct ipc_conn {
	struct wl_list link;
	struct aro_ipc *ipc;
	int fd;
	struct wl_event_source *src;
	struct wl_event_source *timer;

	char in[IPC_MAX_REQ + 1];
	size_t in_len;

	bool replying;
	struct sbuf out;
	size_t out_off;

	bool sub, sub_json;             /* subscribed: stays open for events */
};

static void snap_free(struct snap *sn);
static void snap_take(struct aro_server *s, struct snap *sn);

static void conn_close(struct ipc_conn *c)
{
	if (c->sub && --c->ipc->nsubs == 0)
		snap_free(&c->ipc->last);
	if (c->src)
		wl_event_source_remove(c->src);
	if (c->timer)
		wl_event_source_remove(c->timer);
	close(c->fd);
	free(c->out.d);
	wl_list_remove(&c->link);
	c->ipc->nconns--;
	free(c);
}

/* write what is left of the reply; closes when done or when it cannot */
static void conn_flush(struct ipc_conn *c)
{
	while (c->out_off < c->out.len) {
		/* MSG_NOSIGNAL: a client gone mid-reply is EPIPE, not SIGPIPE,
		 * which would kill the whole compositor */
		ssize_t n = send(c->fd, c->out.d + c->out_off,
		                 c->out.len - c->out_off, MSG_NOSIGNAL);
		if (n > 0) {
			c->out_off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wl_event_source_fd_update(c->src, WL_EVENT_WRITABLE);
			return;
		}
		break;                  /* EPIPE and friends: nobody to tell */
	}
	if (c->sub && c->out_off == c->out.len) {
		/* all sent: wait for the next event, hearing only a hang-up */
		c->out.len = c->out_off = 0;
		wl_event_source_fd_update(c->src, 0);
		return;
	}
	conn_close(c);
}

static void conn_reply(struct ipc_conn *c)
{
	c->in[c->in_len] = '\0';
	char *nl = strchr(c->in, '\n');
	if (nl)
		*nl = '\0';             /* one request; anything after is ignored */
	size_t n = strlen(c->in);
	if (n && c->in[n - 1] == '\r')
		c->in[n - 1] = '\0';

	c->replying = true;
	bool json = false, subscribe = false;
	ipc_handle(c->ipc->s, c->in, &c->out, &json, &subscribe);
	if (c->out.oom) {
		/* not even room for the error: say something short, once */
		static const char msg[] = "error out of memory\n";
		send(c->fd, msg, sizeof msg - 1, MSG_NOSIGNAL);
		conn_close(c);
		return;
	}
	if (subscribe) {
		struct aro_ipc *ipc = c->ipc;
		if (ipc->nsubs++ == 0)
			snap_take(ipc->s, &ipc->last);  /* changes from here on */
		c->sub = true;
		c->sub_json = json;
		wl_event_source_remove(c->timer);       /* may wait forever */
		c->timer = NULL;
	}
	conn_flush(c);
}

static int conn_event(int fd, uint32_t mask, void *data)
{
	struct ipc_conn *c = data;

	if (c->replying) {
		if (mask & WL_EVENT_WRITABLE)
			conn_flush(c);
		else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
			conn_close(c);
		return 0;
	}

	bool eof = false;
	for (;;) {
		if (c->in_len == IPC_MAX_REQ) {
			c->replying = true;
			sb_puts(&c->out, "error request too long\n");
			conn_flush(c);
			return 0;
		}
		ssize_t n = read(fd, c->in + c->in_len, IPC_MAX_REQ - c->in_len);
		if (n > 0) {
			bool line = memchr(c->in + c->in_len, '\n', (size_t)n) != NULL;
			c->in_len += (size_t)n;
			if (line)
				break;
			continue;
		}
		if (n == 0) {
			eof = true;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
				eof = true;
			else
				return 0;       /* the rest of the line is still coming */
			break;
		}
		conn_close(c);
		return 0;
	}

	/* a line, or everything the client sent before closing its end */
	if (memchr(c->in, '\n', c->in_len) || (eof && c->in_len > 0))
		conn_reply(c);
	else
		conn_close(c);
	return 0;
}

static int conn_timeout(void *data)
{
	struct ipc_conn *c = data;
	wlr_log(WLR_INFO, "ipc: dropping a client that stalled");
	conn_close(c);
	return 0;
}

static int ipc_accept(int fd, uint32_t mask, void *data)
{
	struct aro_ipc *ipc = data;
	(void)mask;

	for (;;) {
		int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				wlr_log_errno(WLR_ERROR, "ipc: accept");
			return 0;
		}
		if (ipc->nconns >= IPC_MAX_CONNS) {
			close(cfd);
			continue;
		}

		struct ipc_conn *c = calloc(1, sizeof *c);
		if (!c) {
			close(cfd);
			continue;
		}
		c->ipc = ipc;
		c->fd = cfd;
		wl_list_insert(&ipc->conns, &c->link);
		ipc->nconns++;

		c->src = wl_event_loop_add_fd(ipc->s->loop, cfd, WL_EVENT_READABLE,
		                              conn_event, c);
		c->timer = wl_event_loop_add_timer(ipc->s->loop, conn_timeout, c);
		if (!c->src || !c->timer) {
			conn_close(c);
			continue;
		}
		wl_event_source_timer_update(c->timer, IPC_TIMEOUT_MS);
	}
}

/* ── events ────────────────────────────────────────────────────────────── */

static char *dup_or_empty(const char *s)
{
	return strdup(s ? s : "");
}

static void snap_free(struct snap *sn)
{
	for (int i = 0; i < sn->nviews; i++) {
		free(sn->views[i].app_id);
		free(sn->views[i].title);
		free(sn->views[i].output);
	}
	for (int i = 0; i < sn->noutputs; i++)
		free(sn->outputs[i].name);
	free(sn->views);
	free(sn->outputs);
	*sn = (struct snap){ 0 };
}

/* what aro shows right now; on failure, an empty snapshot */
static void snap_take(struct aro_server *s, struct snap *sn)
{
	*sn = (struct snap){ 0 };
	int nv = 0, no = 0;
	struct aro_view *v;
	struct aro_output *o;
	wl_list_for_each(v, &s->views, link)
		nv += v->mapped;
	wl_list_for_each(o, &s->outputs, link)
		no++;
	sn->views = calloc(nv ? nv : 1, sizeof *sn->views);
	sn->outputs = calloc(no ? no : 1, sizeof *sn->outputs);
	if (!sn->views || !sn->outputs) {
		snap_free(sn);
		return;
	}
	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped)
			continue;
		struct snap_view *sv = &sn->views[sn->nviews++];
		sv->id = v->id;
		sv->app_id = dup_or_empty(view_app_id(v));
		sv->title = dup_or_empty(view_title(v));
		sv->output = dup_or_empty(v->output ? v->output->wlr_output->name : NULL);
		sv->ws = v->workspace;
	}
	wl_list_for_each(o, &s->outputs, link) {
		struct snap_output *so = &sn->outputs[sn->noutputs++];
		so->name = dup_or_empty(o->wlr_output->name);
		so->ws = o->cur_ws;
		so->layout = aro_ws_layout(o, o->cur_ws);
	}
	sn->focus = s->focused && s->focused->mapped ? s->focused->id : 0;
}

static struct snap_view *snap_view(struct snap *sn, uint32_t id)
{
	for (int i = 0; i < sn->nviews; i++)
		if (sn->views[i].id == id)
			return &sn->views[i];
	return NULL;
}

static struct snap_output *snap_output(struct snap *sn, const char *name)
{
	for (int i = 0; i < sn->noutputs; i++)
		if (!strcmp(sn->outputs[i].name, name))
			return &sn->outputs[i];
	return NULL;
}

/* one event, as a text line and a JSON line, to every subscriber */
struct event {
	struct sbuf text, json;
};

static void ev_begin(struct event *e, const char *name)
{
	sb_puts(&e->text, name);
	sb_puts(&e->json, "{\"event\":");
	sb_json_str(&e->json, name);
}

/* a word in the text line; the last one may hold spaces */
static void ev_str(struct event *e, const char *key, const char *val)
{
	char clean[512];
	text_clean(clean, sizeof clean, val && *val ? val : "-");
	sb_printf(&e->text, " %s", clean);
	sb_printf(&e->json, ",\"%s\":", key);
	sb_json_str(&e->json, val);
}

static void ev_int(struct event *e, const char *key, long val)
{
	sb_printf(&e->text, " %ld", val);
	sb_printf(&e->json, ",\"%s\":%ld", key, val);
}

static void ev_send(struct aro_ipc *ipc, struct event *e)
{
	sb_puts(&e->text, "\n");
	sb_puts(&e->json, "}\n");
	struct ipc_conn *c, *tmp;
	wl_list_for_each_safe(c, tmp, &ipc->conns, link) {
		if (!c->sub)
			continue;
		struct sbuf *line = c->sub_json ? &e->json : &e->text;
		if (line->oom || c->out.len - c->out_off + line->len > IPC_MAX_BACKLOG) {
			wlr_log(WLR_INFO, "ipc: dropping a subscriber that stopped reading");
			conn_close(c);
			continue;
		}
		sb_put(&c->out, line->d, line->len);
		conn_flush(c);
	}
	free(e->text.d);
	free(e->json.d);
	*e = (struct event){ 0 };
}

static void ev_view(struct aro_ipc *ipc, const char *name, const struct snap_view *v)
{
	struct event e = { 0 };
	ev_begin(&e, name);
	ev_int(&e, "id", v->id);
	ev_str(&e, "app_id", v->app_id);
	ev_str(&e, "title", v->title);
	ev_send(ipc, &e);
}

/* tell subscribers what changed since they were last told */
static void report(void *data)
{
	struct aro_ipc *ipc = data;
	ipc->idle = NULL;
	if (!ipc->nsubs)
		return;
	/* ours now: a subscriber dropped below must not free it under us */
	struct snap old = ipc->last, now, *was = &old;
	ipc->last = (struct snap){ 0 };
	snap_take(ipc->s, &now);
	struct event e = { 0 };

	for (int i = 0; i < now.noutputs; i++)
		if (!snap_output(was, now.outputs[i].name)) {
			ev_begin(&e, "monitor");
			ev_str(&e, "change", "add");
			ev_str(&e, "name", now.outputs[i].name);
			ev_send(ipc, &e);
		}
	for (int i = 0; i < was->noutputs; i++)
		if (!snap_output(&now, was->outputs[i].name)) {
			ev_begin(&e, "monitor");
			ev_str(&e, "change", "remove");
			ev_str(&e, "name", was->outputs[i].name);
			ev_send(ipc, &e);
		}

	for (int i = 0; i < now.nviews; i++) {
		struct snap_view *n = &now.views[i], *w = snap_view(was, n->id);
		if (!w) {
			ev_view(ipc, "open", n);
		} else if (strcmp(n->title, w->title) || strcmp(n->app_id, w->app_id)) {
			ev_view(ipc, "title", n);
		}
		if (!w || strcmp(n->output, w->output) || n->ws != w->ws) {
			ev_begin(&e, "moved");
			ev_int(&e, "id", n->id);
			ev_str(&e, "output", n->output);
			ev_int(&e, "workspace", n->ws + 1);
			ev_send(ipc, &e);
		}
	}
	for (int i = 0; i < was->nviews; i++)
		if (!snap_view(&now, was->views[i].id)) {
			ev_begin(&e, "close");
			ev_int(&e, "id", was->views[i].id);
			ev_send(ipc, &e);
		}

	for (int i = 0; i < now.noutputs; i++) {
		struct snap_output *n = &now.outputs[i], *w = snap_output(was, n->name);
		if (!w || n->ws != w->ws) {
			ev_begin(&e, "workspace");
			ev_str(&e, "output", n->name);
			ev_int(&e, "workspace", n->ws + 1);
			ev_send(ipc, &e);
		}
		if (!w || n->ws != w->ws || n->layout != w->layout) {
			ev_begin(&e, "layout");
			ev_str(&e, "output", n->name);
			ev_int(&e, "workspace", n->ws + 1);
			ev_str(&e, "layout", config_layout_name(n->layout));
			ev_send(ipc, &e);
		}
	}

	if (now.focus != was->focus) {
		struct snap_view *f = snap_view(&now, now.focus);
		if (f) {
			ev_view(ipc, "focus", f);
		} else {
			ev_begin(&e, "focus");  /* nothing: "focus -", "id":null */
			sb_puts(&e.text, " -");
			sb_puts(&e.json, ",\"id\":null");
			ev_send(ipc, &e);
		}
	}

	snap_free(&old);
	if (ipc->nsubs)
		ipc->last = now;
	else
		snap_free(&now);        /* the last subscriber went while we sent */
}

void ipc_notify(struct aro_server *s)
{
	struct aro_ipc *ipc = s->ipc;
	if (!ipc || !ipc->nsubs || ipc->idle)
		return;
	ipc->idle = wl_event_loop_add_idle(s->loop, report, ipc);
}

/* ── setup ─────────────────────────────────────────────────────────────── */

bool ipc_init(struct aro_server *s, const char *wl_socket)
{
	s->ipc = NULL;

	/* never let children reach a parent aro's socket by mistake: a
	 * nested aro inherits ARO_SOCKET, and if ours fails it must go */
	unsetenv("ARO_SOCKET");

	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt || !wl_socket) {
		wlr_log(WLR_ERROR, "ipc: XDG_RUNTIME_DIR is not set; aroctl will "
		        "not work");
		return false;
	}

	struct aro_ipc *ipc = calloc(1, sizeof *ipc);
	if (!ipc)
		return false;
	ipc->s = s;
	ipc->fd = -1;
	wl_list_init(&ipc->conns);

	int n = snprintf(ipc->path, sizeof ipc->path, "%s/aro-%s.sock",
	                 rt, wl_socket);
	if (n < 0 || (size_t)n >= sizeof ipc->path) {
		wlr_log(WLR_ERROR, "ipc: socket path too long under %s", rt);
		free(ipc);
		return false;
	}

	ipc->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (ipc->fd < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: socket");
		free(ipc);
		return false;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	memcpy(addr.sun_path, ipc->path, (size_t)n + 1);

	/* stale from a crashed aro: the Wayland socket name we were given is
	 * free, so nothing live can own this one */
	unlink(ipc->path);

	/* 0600 from birth, not chmod'ed after: no window where it is open */
	mode_t old = umask(0177);
	int rc = bind(ipc->fd, (struct sockaddr *)&addr, sizeof addr);
	umask(old);
	if (rc < 0 || listen(ipc->fd, 16) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: could not listen on %s", ipc->path);
		close(ipc->fd);
		if (rc == 0)
			unlink(ipc->path);
		free(ipc);
		return false;
	}

	ipc->src = wl_event_loop_add_fd(s->loop, ipc->fd, WL_EVENT_READABLE,
	                                ipc_accept, ipc);
	if (!ipc->src) {
		close(ipc->fd);
		unlink(ipc->path);
		free(ipc);
		return false;
	}

	setenv("ARO_SOCKET", ipc->path, true);
	s->ipc = ipc;
	wlr_log(WLR_INFO, "ipc: listening on %s", ipc->path);
	return true;
}

void ipc_finish(struct aro_server *s)
{
	struct aro_ipc *ipc = s->ipc;
	if (!ipc)
		return;

	struct ipc_conn *c, *tmp;
	wl_list_for_each_safe(c, tmp, &ipc->conns, link)
		conn_close(c);
	if (ipc->idle)
		wl_event_source_remove(ipc->idle);

	wl_event_source_remove(ipc->src);
	close(ipc->fd);
	unlink(ipc->path);
	free(ipc);
	s->ipc = NULL;
}
