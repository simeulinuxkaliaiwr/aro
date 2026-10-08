/* bgeffect.c: ext-background-effect-v1, so apps that ask for blur behind them learn they have it
 *
 * aro already blurs behind every see-through window (ui.c, with blur = true),
 * so the region an app asks for is only kept here, double-buffered as the
 * protocol says. What this changes is that apps such as foot see the blur
 * capability instead of warning that the compositor has none. */
#include "scene.h"

#include "protocols.h"
#include "aro.h"

#include "ext-background-effect-v1-protocol.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

#include <pixman.h>
#include <stdlib.h>

/* one per wl_surface that ever asked, living as long as the surface does */
struct bg_surface {
	struct wlr_addon addon;
	struct wl_resource *resource;   /* NULL once the app destroys its object */
	pixman_region32_t pending, current;
	struct wl_listener commit;
};

static uint32_t caps_for(struct aro_server *s)
{
#ifdef ARO_EFFECTS
	return s->cfg.blur ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR : 0;
#else
	(void)s;
	return 0;
#endif
}

static void addon_destroy(struct wlr_addon *addon)
{
	struct bg_surface *b = wl_container_of(addon, b, addon);
	if (b->resource)
		wl_resource_set_user_data(b->resource, NULL);   /* inert from now on */
	wl_list_remove(&b->commit.link);
	pixman_region32_fini(&b->pending);
	pixman_region32_fini(&b->current);
	wlr_addon_finish(&b->addon);
	free(b);
}

static const struct wlr_addon_interface addon_impl = {
	.name = "ext_background_effect_surface_v1",
	.destroy = addon_destroy,
};

static void surface_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct bg_surface *b = wl_container_of(l, b, commit);
	pixman_region32_copy(&b->current, &b->pending);
}

static void set_blur_region(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *region)
{
	(void)client;
	struct bg_surface *b = wl_resource_get_user_data(resource);
	if (!b) {
		wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"the surface has been destroyed");
		return;
	}
	if (region)
		pixman_region32_copy(&b->pending, wlr_region_from_resource(region));
	else
		pixman_region32_clear(&b->pending);
}

static void surface_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
	.destroy = surface_destroy_req,
	.set_blur_region = set_blur_region,
};

/* the app let go of its object: the region goes on the next commit */
static void surface_resource_destroy(struct wl_resource *resource)
{
	struct bg_surface *b = wl_resource_get_user_data(resource);
	if (!b)
		return;
	b->resource = NULL;
	pixman_region32_clear(&b->pending);
}

static void get_background_effect(struct wl_client *client, struct wl_resource *manager,
	uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct wlr_addon *found = wlr_addon_find(&surface->addons, NULL, &addon_impl);
	struct bg_surface *b = found ? wl_container_of(found, b, addon) : NULL;
	if (b && b->resource) {
		wl_resource_post_error(manager, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"the surface already has a background effect object");
		return;
	}

	struct wl_resource *r = wl_resource_create(client, &ext_background_effect_surface_v1_interface,
		wl_resource_get_version(manager), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	if (!b) {
		b = calloc(1, sizeof(*b));
		if (!b) {
			wl_resource_destroy(r);
			wl_client_post_no_memory(client);
			return;
		}
		pixman_region32_init(&b->pending);
		pixman_region32_init(&b->current);
		b->commit.notify = surface_commit;
		wl_signal_add(&surface->events.commit, &b->commit);
		wlr_addon_init(&b->addon, &surface->addons, NULL, &addon_impl);
	}
	b->resource = r;
	wl_resource_set_implementation(r, &surface_impl, b, surface_resource_destroy);
}

static void manager_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
	.destroy = manager_destroy_req,
	.get_background_effect = get_background_effect,
};

static void manager_resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct aro_server *s = data;
	struct wl_resource *r = wl_resource_create(client,
		&ext_background_effect_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, s, manager_resource_destroy);
	wl_list_insert(&s->bg_managers, wl_resource_get_link(r));
	ext_background_effect_manager_v1_send_capabilities(r, caps_for(s));
}

void bgeffect_sync(struct aro_server *s)
{
	uint32_t caps = caps_for(s);
	if (caps == s->bg_caps)
		return;
	s->bg_caps = caps;
	struct wl_resource *r;
	wl_resource_for_each(r, &s->bg_managers)
		ext_background_effect_manager_v1_send_capabilities(r, caps);
}

void bgeffect_init(struct aro_server *s)
{
	wl_list_init(&s->bg_managers);
	s->bg_caps = caps_for(s);
	if (!wl_global_create(s->display, &ext_background_effect_manager_v1_interface, 1, s, manager_bind))
		wlr_log(WLR_ERROR, "background effect: could not start");
}
