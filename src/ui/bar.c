/* bar.c: status bar */
/* localtime_r */
#define _POSIX_C_SOURCE 200809L

/* scene.h must come first */
#include "scene.h"

#include "bar.h"
#include "aro.h"
#include "theme.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* ── battery ───────────────────────────────────────────────────────────── */

#define POWER_SUPPLY "/sys/class/power_supply"

struct battery {
	bool present, charging;
	int percent;
};

/* one line of a sysfs file; false if it is not there */
static bool read_line(const char *dir, const char *name, char *buf, size_t size)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

static long read_long(const char *dir, const char *name)
{
	char buf[32];
	return read_line(dir, name, buf, sizeof buf) ? strtol(buf, NULL, 10) : -1;
}

/* every system battery together; a mouse's or a phone's does not count */
static struct battery battery_read(void)
{
	struct battery bat = { 0 };
	DIR *d = opendir(POWER_SUPPLY);
	if (!d)
		return bat;
	long now = 0, full = 0, pct_sum = 0, npct = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char dir[300], buf[32];
		snprintf(dir, sizeof dir, POWER_SUPPLY "/%s", e->d_name);
		if (!read_line(dir, "type", buf, sizeof buf) || strcmp(buf, "Battery"))
			continue;
		if (read_line(dir, "scope", buf, sizeof buf) && !strcmp(buf, "Device"))
			continue;
		bat.present = true;
		if (read_line(dir, "status", buf, sizeof buf) && !strcmp(buf, "Charging"))
			bat.charging = true;
		/* energy or charge counters weigh two batteries properly; capacity is the fallback */
		long n = read_long(dir, "energy_now"), f = read_long(dir, "energy_full");
		if (n < 0 || f <= 0) {
			n = read_long(dir, "charge_now");
			f = read_long(dir, "charge_full");
		}
		if (n >= 0 && f > 0) {
			now += n;
			full += f;
		} else {
			long c = read_long(dir, "capacity");
			if (c >= 0) {
				pct_sum += c;
				npct++;
			}
		}
	}
	closedir(d);
	if (full > 0)
		bat.percent = (int)((now * 100 + full / 2) / full);
	else if (npct > 0)
		bat.percent = (int)(pct_sum / npct);
	else
		bat.present = false;
	if (bat.percent > 100)
		bat.percent = 100;
	return bat;
}

/* read at most every 30 s: the bar redraws far more often than batteries change */
static struct battery battery_get(void)
{
	static struct battery cached;
	static time_t at;
	time_t now = time(NULL);
	if (!at || now - at >= 30 || now < at) {
		cached = battery_read();
		at = now;
	}
	return cached;
}

/* ── the bar ───────────────────────────────────────────────────────────── */

#define PILL_H 18
#define PILL_PAD 9
#define PILL_GAP 4

bool bar_create(struct aro_bar *b, struct aro_output *o)
{
	struct wlr_scene_tree *parent = o->server->l_bar;
	const struct q_theme *th = &o->server->cfg.theme;

	memset(b, 0, sizeof *b);
	b->output = o;

	/* create all workspace pills up front */
	b->nws = ARO_MAX_WS;
	b->scale = 1.0f;

	b->tree = wlr_scene_tree_create(parent);
	if (!b->tree)
		return false;

	float c[4];
	ui_color(th->bar_bg, c);
	b->bg = wlr_scene_rect_create(b->tree, 1, th->bar_h, c);

	for (int i = 0; i < b->nws; i++) {
		ui_color(th->accent, c);
		b->ws[i].pill = wlr_scene_rect_create(b->tree, 1, PILL_H, c);
		if (!qtext_init(&b->ws[i].label, b->tree, th->font_small))
			return false;
	}
	if (!qtext_init(&b->title, b->tree, th->font))
		return false;
	if (!qtext_init(&b->clock, b->tree, th->font_small))
		return false;
	if (!qtext_init(&b->battery, b->tree, th->font_small))
		return false;

	return b->bg != NULL;
}

int bar_height(const struct aro_bar *b)
{
	if (!b->tree || b->yielded)
		return 0;
	int h = b->output->server->cfg.theme.bar_h;
	return h > 0 ? h : 0;
}

void bar_place(struct aro_bar *b, int x, int y, int w, float scale)
{
	const struct q_theme *th = &b->output->server->cfg.theme;
	const int h = bar_height(b);
	b->x = x;
	b->y = y;
	b->w = w;
	b->scale = scale > 0 ? scale : 1.0f;

	/* hidden, not destroyed */
	wlr_scene_node_set_enabled(&b->tree->node, h > 0);
	wlr_scene_node_set_position(&b->tree->node, x, y);
	if (h > 0)
		wlr_scene_rect_set_size(b->bg, w, th->bar_h);
}

/* occupied = tree exists or mapped view exists */
static bool ws_occupied(struct aro_output *o, int ws)
{
	if (o->ws[ws])
		return true;

	struct aro_view *v;
	wl_list_for_each(v, &o->server->views, link) {
		if (v->mapped && v->output == o && v->workspace == ws)
			return true;
	}
	return false;
}

/* bar contents are positioned relative to the bar tree */
void bar_update(struct aro_bar *b, struct aro_output *o)
{
	struct aro_server *s = o->server;
	const struct q_theme *th = &s->cfg.theme;

	/* hidden: skip the text passes, the clock ticks here every minute */
	if (!b->tree || b->w <= 0 || bar_height(b) <= 0)
		return;

	const int mid = th->bar_h / 2;
	int cursor = th->bar_pad;

	/* workspace pills */
	int last = s->cfg.workspaces - 1;
	for (int i = 0; i < b->nws; i++)
		if (ws_occupied(o, i) || i == o->cur_ws)
			last = i > last ? i : last;
	if (last >= b->nws)
		last = b->nws - 1;

	for (int i = 0; i < b->nws; i++) {
		bool shown = i <= last;

		wlr_scene_node_set_enabled(&b->ws[i].pill->node, false);
		qtext_show(&b->ws[i].label, shown);
		if (!shown)
			continue;

		bool active = (i == o->cur_ws);
		bool occupied = ws_occupied(o, i);

		char label[12];
		snprintf(label, sizeof label, "%d", i + 1);

		uint32_t fg = active ? th->bar_bg : (occupied ? th->ink : th->dim);
		qtext_set(&b->ws[i].label, label, fg, b->scale, 0);

		int pw = b->ws[i].label.w + PILL_PAD * 2;

		wlr_scene_node_set_enabled(&b->ws[i].pill->node, active);
		wlr_scene_rect_set_size(b->ws[i].pill, pw, PILL_H);
		wlr_scene_node_set_position(&b->ws[i].pill->node, cursor, mid - PILL_H / 2);

		qtext_move(&b->ws[i].label,
		           cursor + PILL_PAD,
		           mid - b->ws[i].label.h / 2);

		cursor += pw + PILL_GAP;
	}
	b->shown = last + 1;

	/* clock */
	char when[16] = "--:--";
	time_t now = time(NULL);
	struct tm tm;
	if (localtime_r(&now, &tm))
		strftime(when, sizeof when, "%H:%M", &tm);
	qtext_set(&b->clock, when, th->dim, b->scale, 0);
	qtext_move(&b->clock,
	           b->w - th->bar_pad - b->clock.w,
	           mid - b->clock.h / 2);

	/* battery, left of the clock; red when low and draining */
	int right = b->w - th->bar_pad - b->clock.w;
	const struct battery bat = s->cfg.bar_battery ? battery_get() : (struct battery){ 0 };
	qtext_show(&b->battery, bat.present);
	if (bat.present) {
		char text[32];
		snprintf(text, sizeof text, bat.charging ? "%d%% charging" : "%d%%", bat.percent);
		bool low = !bat.charging && bat.percent <= 15;
		qtext_set(&b->battery, text, low ? th->urgent : th->dim, b->scale, 0);
		right -= th->bar_pad + b->battery.w;
		qtext_move(&b->battery, right, mid - b->battery.h / 2);
	}

	/* focused title */
	/* only show title on its own output */
	const char *title = NULL;
	if (s->focused && s->focused->output == o)
		title = view_title(s->focused);

	int title_x = cursor + th->bar_pad;
	int avail = right - th->bar_pad * 2 - title_x;
	if (avail < 0)
		avail = 0;

	qtext_set(&b->title, title ? title : "", th->ink, b->scale, avail);
	qtext_move(&b->title, title_x, mid - b->title.h / 2);
}

void bar_finish(struct aro_bar *b)
{
	for (int i = 0; i < b->nws; i++)
		qtext_finish(&b->ws[i].label);
	qtext_finish(&b->title);
	qtext_finish(&b->clock);
	qtext_finish(&b->battery);
	if (b->tree)
		wlr_scene_node_destroy(&b->tree->node);
	b->tree = NULL;
}

/* retheme bar */
void bar_retheme(struct aro_bar *b)
{
	const struct q_theme *th = &b->output->server->cfg.theme;
	float c[4];

	if (!b->tree)
		return;

	ui_color(th->bar_bg, c);
	wlr_scene_rect_set_color(b->bg, c);
	if (th->bar_h > 0)
		wlr_scene_rect_set_size(b->bg, b->w, th->bar_h);

	for (int i = 0; i < b->nws; i++) {
		ui_color(th->accent, c);
		wlr_scene_rect_set_color(b->ws[i].pill, c);
		qtext_set_font(&b->ws[i].label, th->font_small);
	}
	qtext_set_font(&b->title, th->font);
	qtext_set_font(&b->clock, th->font_small);
	qtext_set_font(&b->battery, th->font_small);
}
