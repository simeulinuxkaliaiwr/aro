/* idle.c: idle tracking, inhibit, output power */
/* scene.h must come first */
#include "scene.h"

#include "idle.h"
#include "aro.h"

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/util/log.h>

void idle_activity(struct aro_server *s)
{
	if (s->idle_notifier)
		wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
}

/* cache last inhibited state for logging */
static int last_inhibited = -1;

/* one visible inhibitor is enough */
void idle_update(struct aro_server *s)
{
	if (!s->idle_notifier)
		return;

	int total = 0;
	bool inhibited = false;
	struct aro_inhibitor *qi;
	wl_list_for_each(qi, &s->inhibitors, link) {
		total++;
		if (!inhibited && aro_surface_visible(s, qi->inhibitor->surface))
			inhibited = true;
	}

	if ((int)inhibited == last_inhibited)
		return;
	last_inhibited = inhibited;
	wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, inhibited);
	wlr_log(WLR_INFO, "idle: %s (%d inhibitor%s)",
	        inhibited ? "inhibited" : "not inhibited",
	        total, total == 1 ? "" : "s");
}

static void inhibitor_surface_map(struct wl_listener *l, void *data)
{
	struct aro_inhibitor *qi = wl_container_of(l, qi, surface_map);
	(void)data;
	idle_update(qi->server);
}

static void inhibitor_surface_unmap(struct wl_listener *l, void *data)
{
	struct aro_inhibitor *qi = wl_container_of(l, qi, surface_unmap);
	(void)data;
	idle_update(qi->server);
}

static void inhibitor_free(struct aro_inhibitor *qi)
{
	wl_list_remove(&qi->destroy.link);
	wl_list_remove(&qi->surface_map.link);
	wl_list_remove(&qi->surface_unmap.link);
	wl_list_remove(&qi->link);
	free(qi);
}

/* inhibitor destroy */
static void inhibitor_destroy(struct wl_listener *l, void *data)
{
	struct aro_inhibitor *qi = wl_container_of(l, qi, destroy);
	struct aro_server *s = qi->server;
	(void)data;

	inhibitor_free(qi);
	idle_update(s);
}

static void new_inhibitor(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *inhibitor = data;

	struct aro_inhibitor *qi = calloc(1, sizeof *qi);
	if (!qi)
		return;
	qi->server = s;
	qi->inhibitor = inhibitor;

	qi->destroy.notify = inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &qi->destroy);
	qi->surface_map.notify = inhibitor_surface_map;
	wl_signal_add(&inhibitor->surface->events.map, &qi->surface_map);
	qi->surface_unmap.notify = inhibitor_surface_unmap;
	wl_signal_add(&inhibitor->surface->events.unmap, &qi->surface_unmap);

	wl_list_insert(&s->inhibitors, &qi->link);
	idle_update(s);
}

/* output power switch */
static void output_set_power(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *ev = data;
	(void)s;

	bool on = ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
	if (ev->output->enabled == on)
		return;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	wlr_output_commit_state(ev->output, &state);
	wlr_output_state_finish(&state);

	wlr_log(WLR_INFO, "output %s powered %s", ev->output->name, on ? "on" : "off");
}

void idle_init(struct aro_server *s)
{
	wl_list_init(&s->inhibitors);
	last_inhibited = -1;

	s->idle_notifier = wlr_idle_notifier_v1_create(s->display);
	if (!s->idle_notifier)
		wlr_log(WLR_ERROR, "idle notification unavailable");

	s->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(s->display);
	if (s->idle_inhibit_mgr) {
		s->new_inhibitor.notify = new_inhibitor;
		wl_signal_add(&s->idle_inhibit_mgr->events.new_inhibitor,
		              &s->new_inhibitor);
	} else {
		wlr_log(WLR_ERROR, "idle inhibit unavailable");
	}

	s->output_power_mgr = wlr_output_power_manager_v1_create(s->display);
	if (s->output_power_mgr) {
		s->output_power_set_mode.notify = output_set_power;
		wl_signal_add(&s->output_power_mgr->events.set_mode,
		              &s->output_power_set_mode);
	} else {
		wlr_log(WLR_ERROR, "output power management unavailable");
	}
}

void idle_finish(struct aro_server *s)
{
	struct aro_inhibitor *qi, *tmp;
	wl_list_for_each_safe(qi, tmp, &s->inhibitors, link)
		inhibitor_free(qi);

	if (s->idle_inhibit_mgr)
		wl_list_remove(&s->new_inhibitor.link);
	if (s->output_power_mgr)
		wl_list_remove(&s->output_power_set_mode.link);

	s->idle_notifier = NULL;
	s->idle_inhibit_mgr = NULL;
	s->output_power_mgr = NULL;
}
