/* notify.c: compositor toasts */
/* scene.h must come first */
#include "scene.h"

#include "notify.h"
#include "aro.h"
#include "theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <wlr/types/wlr_output.h>

/* stack toasts from top-right */
static void notify_layout(struct aro_server *s)
{
	const struct q_theme *th = &s->cfg.theme;
	struct aro_output *o = aro_focused_output(s);
	if (!o)
		return;

	uint32_t now = aro_now_ms();
	int y = o->usable.y + th->outer_gap;

	struct aro_notification *n;
	wl_list_for_each(n, &s->notifications, link) {
		int w = n->text.w + th->text_pad * 2 + th->border * 2;
		int h = n->text.h + th->text_pad * 2 + th->border * 2;

		if (w > th->notify_max_w)
			w = th->notify_max_w;

		ly_box target = {
			.x = o->usable.x + o->usable.w - th->outer_gap - w,
			.y = y,
			.w = w,
			.h = h,
		};
		anim_box_to(&n->geo, target, now, th->anim_ms, &(anim_ease){
			TH_EASE_X1, TH_EASE_Y1, TH_EASE_X2, TH_EASE_Y2 });

		y += h + th->gap;
	}

	struct aro_output *out;
	wl_list_for_each(out, &s->outputs, link)
		wlr_output_schedule_frame(out->wlr_output);
}

static void notification_destroy(struct aro_notification *n)
{
	if (n->timer)
		wl_event_source_remove(n->timer);
	qtext_finish(&n->text);
	if (n->tree)
		wlr_scene_node_destroy(&n->tree->node);
	wl_list_remove(&n->link);

	struct aro_server *s = n->server;
	free(n);
	notify_layout(s);
}

static int notification_expire(void *data)
{
	notification_destroy(data);
	return 0;
}

void notify(struct aro_server *s, enum notify_level level,
            const char *fmt, ...)
{
	const struct q_theme *th = &s->cfg.theme;

	/* drop toast if no output exists */
	if (!s->l_notify || !aro_focused_output(s))
		return;

	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	struct aro_notification *n = calloc(1, sizeof *n);
	if (!n)
		return;
	n->server = s;
	n->level = level;

	n->tree = wlr_scene_tree_create(s->l_notify);
	if (!n->tree) {
		free(n);
		return;
	}

	float col[4];
	ui_color(level == NOTIFY_ERROR ? th->urgent : th->accent, col);
	n->edge = wlr_scene_rect_create(n->tree, 1, 1, col);

	ui_color(th->notify_bg, col);
	n->bg = wlr_scene_rect_create(n->tree, 1, 1, col);

	if (!n->edge || !n->bg || !qtext_init(&n->text, n->tree, th->font)) {
		wlr_scene_node_destroy(&n->tree->node);
		free(n);
		return;
	}

	qtext_set(&n->text, msg,
	          level == NOTIFY_ERROR ? th->urgent : th->ink,
	          aro_focused_output(s)->scale,
	          th->notify_max_w - th->text_pad * 2 - th->border * 2);

	wl_list_insert(s->notifications.prev, &n->link);

	/* animate toast in */
	anim_box_set(&n->geo, (ly_box){ 0, 0, 1, 1 });

	/* errors stay until cleared */
	if (level != NOTIFY_ERROR) {
		n->timer = wl_event_loop_add_timer(s->loop, notification_expire, n);
		if (n->timer)
			wl_event_source_timer_update(n->timer, th->notify_ms);
	}

	notify_layout(s);
}

void notify_clear_errors(struct aro_server *s)
{
	struct aro_notification *n, *tmp;
	wl_list_for_each_safe(n, tmp, &s->notifications, link) {
		if (n->level != NOTIFY_ERROR)
			continue;
		/* restack after removing errors */
		notification_destroy(n);
	}
}

bool notify_tick(struct aro_server *s, uint32_t now)
{
	const struct q_theme *th = &s->cfg.theme;
	const int bw = th->border;
	bool moving = false;

	struct aro_notification *n;
	wl_list_for_each(n, &s->notifications, link) {
		if (anim_box_tick(&n->geo, now))
			moving = true;

		ly_box b = n->geo.cur;
		int iw = b.w - bw * 2, ih = b.h - bw * 2;
		if (iw < 1)
			iw = 1;
		if (ih < 1)
			ih = 1;

		wlr_scene_node_set_position(&n->tree->node, b.x, b.y);
		wlr_scene_rect_set_size(n->edge, b.w, b.h);
		wlr_scene_node_set_position(&n->bg->node, bw, bw);
		wlr_scene_rect_set_size(n->bg, iw, ih);
		qtext_move(&n->text, bw + th->text_pad, bw + th->text_pad);
	}
	return moving;
}

void notify_retheme(struct aro_server *s)
{
	const struct q_theme *th = &s->cfg.theme;
	float col[4];

	struct aro_notification *n;
	wl_list_for_each(n, &s->notifications, link) {
		ui_color(th->notify_bg, col);
		wlr_scene_rect_set_color(n->bg, col);
		qtext_set_font(&n->text, th->font);
	}
	notify_layout(s);
}

void notify_finish(struct aro_server *s)
{
	struct aro_notification *n, *tmp;
	wl_list_for_each_safe(n, tmp, &s->notifications, link) {
		if (n->timer)
			wl_event_source_remove(n->timer);
		qtext_finish(&n->text);
		if (n->tree)
			wlr_scene_node_destroy(&n->tree->node);
		wl_list_remove(&n->link);
		free(n);
	}
}
