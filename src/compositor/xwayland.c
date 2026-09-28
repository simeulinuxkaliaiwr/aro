/* xwayland.c: X11 windows, through XWayland */
#ifdef ARO_XWAYLAND

/* scene.h must come first */
#include "scene.h"

#include "config.h"
#include "aro.h"
#include "core.h"
#include "lock.h"
#include "text.h"

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>

/* ── XWayland ──────────────────────────────────────────────────────────── */

/* XWayland views */

static void xwl_configure(struct aro_view *v, int x, int y, int w, int h)
{
	if (v->xsurface)
		wlr_xwayland_surface_configure(v->xsurface, x, y, w, h);
}

static void xwl_close(struct aro_view *v)
{
	if (v->xsurface)
		wlr_xwayland_surface_close(v->xsurface);
}

static void xwl_activate(struct aro_view *v, bool activated)
{
	if (!v->xsurface)
		return;
	wlr_xwayland_surface_activate(v->xsurface, activated);
	/* X11 clients read their own stacking; a buried game drops keys */
	if (activated)
		wlr_xwayland_surface_restack(v->xsurface, NULL, XCB_STACK_MODE_ABOVE);
}

static void xwl_set_fullscreen(struct aro_view *v, bool fullscreen)
{
	if (v->xsurface)
		wlr_xwayland_surface_set_fullscreen(v->xsurface, fullscreen);
}

static const char *xwl_title(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->title : NULL;
}

/* WM_CLASS class */
static const char *xwl_app_id(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->class : NULL;
}

/* _NET_WM_WINDOW_TYPE, most specific first; the names `type:` rules match */
static const struct {
	enum wlr_xwayland_net_wm_window_type type;
	const char *name;
} xwl_types[] = {
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,        "splash" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,        "dialog" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,       "utility" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,       "toolbar" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,          "menu" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, "dropdown-menu" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,    "popup-menu" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,       "tooltip" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,  "notification" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,         "combo" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,           "dnd" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DOCK,          "dock" },
	{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DESKTOP,       "desktop" },
};

static const char *xwl_type(struct aro_view *v)
{
	struct wlr_xwayland_surface *x = v->xsurface;
	if (!x)
		return "normal";
	for (size_t i = 0; i < sizeof xwl_types / sizeof xwl_types[0]; i++)
		if (wlr_xwayland_surface_has_window_type(x, xwl_types[i].type))
			return xwl_types[i].name;
	/* untyped but transient or modal: as good as a dialog */
	return x->modal || x->parent ? "dialog" : "normal";
}

/* X11 float heuristics */
static bool xwl_wants_float(struct aro_view *v)
{
	struct wlr_xwayland_surface *x = v->xsurface;
	if (!x)
		return false;
	if (x->modal || x->parent)
		return true;

	/* the types that are never a main window */
	if (wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
	    wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
	    wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
	    wlr_xwayland_surface_has_window_type(x, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH))
		return true;

	if (x->size_hints) {
		int minw = x->size_hints->min_width, maxw = x->size_hints->max_width;
		int minh = x->size_hints->min_height, maxh = x->size_hints->max_height;
		if (minw > 0 && maxw > 0 && minh > 0 && maxh > 0 &&
		    minw == maxw && minh == maxh)
			return true;
	}
	return false;
}

static bool xwl_wants_fullscreen(struct aro_view *v)
{
	return v->xsurface && v->xsurface->fullscreen;
}

static void xwl_preferred_size(struct aro_view *v, int *w, int *h)
{
	*w = v->xsurface ? v->xsurface->width : 0;
	*h = v->xsurface ? v->xsurface->height : 0;
}

static void xwl_geometry(struct aro_view *v, struct wlr_box *out)
{
	/* X11 geometry is the surface */
	*out = (struct wlr_box){ 0, 0,
		v->xsurface ? v->xsurface->width : 0,
		v->xsurface ? v->xsurface->height : 0 };
}

static struct wlr_surface *xwl_surface(struct aro_view *v)
{
	return v->xsurface ? v->xsurface->surface : NULL;
}

static const struct view_impl xwl_impl = {
	.configure        = xwl_configure,
	.close            = xwl_close,
	.activate         = xwl_activate,
	.set_fullscreen   = xwl_set_fullscreen,
	.title            = xwl_title,
	.app_id           = xwl_app_id,
	.type             = xwl_type,
	.geometry         = xwl_geometry,
	.wants_float      = xwl_wants_float,
	.wants_fullscreen = xwl_wants_fullscreen,
	.preferred_size   = xwl_preferred_size,
	.surface          = xwl_surface,
};

/* ── override-redirect ─────────────────────────────────────────────────── */

/* override-redirect X11 surfaces */
struct aro_unmanaged {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *tree;

	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener request_configure;
	struct wl_listener destroy;
};

/* the keyboard goes to a surface without moving aro's own focus */
static void unmanaged_keyboard(struct aro_server *s, struct wlr_surface *surface)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (surface && kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes,
		                               kb->num_keycodes, &kb->modifiers);
	else if (surface)
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
	else
		wlr_seat_keyboard_notify_clear_focus(s->seat);
	keyboard_focus_changed(s);
}

static void unmanaged_map(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, map);
	struct aro_server *s = u->server;
	(void)data;

	u->tree = wlr_scene_subsurface_tree_create(s->l_unmanaged,
	                                           u->xsurface->surface);
	if (u->tree)
		wlr_scene_node_set_position(&u->tree->node,
		                            u->xsurface->x, u->xsurface->y);

	/* wine's fullscreen games are often override-redirect */
	if (wlr_xwayland_surface_override_redirect_wants_focus(u->xsurface) &&
	    !aro_locked(s) && !s->focused_layer)
		unmanaged_keyboard(s, u->xsurface->surface);
}

static void unmanaged_unmap(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, unmap);
	struct aro_server *s = u->server;
	(void)data;
	if (u->tree)
		wlr_scene_node_destroy(&u->tree->node);
	u->tree = NULL;

	/* hand the keyboard back to the focused window */
	if (s->seat->keyboard_state.focused_surface == u->xsurface->surface &&
	    !aro_locked(s) && !s->focused_layer)
		unmanaged_keyboard(s, s->focused ? view_surface(s->focused) : NULL);
}

static void unmanaged_request_configure(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	/* honor override-redirect configure */
	wlr_xwayland_surface_configure(u->xsurface, ev->x, ev->y,
	                               ev->width, ev->height);
	if (u->tree)
		wlr_scene_node_set_position(&u->tree->node, ev->x, ev->y);
}

static void unmanaged_associate(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, associate);
	(void)data;
	u->map.notify = unmanaged_map;
	wl_signal_add(&u->xsurface->surface->events.map, &u->map);
	u->unmap.notify = unmanaged_unmap;
	wl_signal_add(&u->xsurface->surface->events.unmap, &u->unmap);
}

static void unmanaged_dissociate(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, dissociate);
	(void)data;
	wl_list_remove(&u->map.link);
	wl_list_remove(&u->unmap.link);
	wl_list_init(&u->map.link);     /* destroy removes them again */
	wl_list_init(&u->unmap.link);
}

static void unmanaged_destroy(struct wl_listener *l, void *data)
{
	struct aro_unmanaged *u = wl_container_of(l, u, destroy);
	(void)data;
	wl_list_remove(&u->associate.link);
	wl_list_remove(&u->dissociate.link);
	wl_list_remove(&u->request_configure.link);
	wl_list_remove(&u->destroy.link);
	wl_list_remove(&u->link);
	free(u);
}

static void new_unmanaged(struct aro_server *s,
                          struct wlr_xwayland_surface *xsurface)
{
	struct aro_unmanaged *u = calloc(1, sizeof *u);
	if (!u)
		return;
	u->server = s;
	u->xsurface = xsurface;
	wl_list_init(&u->map.link);
	wl_list_init(&u->unmap.link);

	u->associate.notify = unmanaged_associate;
	wl_signal_add(&xsurface->events.associate, &u->associate);
	u->dissociate.notify = unmanaged_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &u->dissociate);
	u->request_configure.notify = unmanaged_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &u->request_configure);
	u->destroy.notify = unmanaged_destroy;
	wl_signal_add(&xsurface->events.destroy, &u->destroy);

	wl_list_insert(&s->unmanaged, &u->link);
}

/* ── managed X11 windows ───────────────────────────────────────────────── */

static void xwl_commit(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, commit);
	(void)data;
	/* new buffers arrive square; round them as they come */
	ui_frame_clip_content(v);
	overview_view_commit(v->server, v);
}

/* answer unmapped X11 configure */
static void xwl_request_configure(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	if (!v->mapped) {
		wlr_xwayland_surface_configure(v->xsurface, ev->x, ev->y,
		                               ev->width, ev->height);
		return;
	}

	/* floating X11 resize */
	ly_box b = view_target(v), c;
	ui_frame_content_box(v, b, &c);
	if (v->floating && !v->fullscreen) {
		v->fbox.w = ev->width + b.w - c.w;
		v->fbox.h = ev->height + b.h - c.h;
		aro_arrange(v->server);
	} else {
		/* tiled X11 configure */
		wlr_xwayland_surface_configure(v->xsurface, c.x, c.y, c.w, c.h);
	}
}

static void xwl_associate(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, associate);
	(void)data;

	v->surface_tree = wlr_scene_subsurface_tree_create(v->content,
	                                                   v->xsurface->surface);

	v->map.notify = view_map;
	wl_signal_add(&v->xsurface->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&v->xsurface->surface->events.unmap, &v->unmap);
	v->commit.notify = xwl_commit;
	wl_signal_add(&v->xsurface->surface->events.commit, &v->commit);
}

static void xwl_dissociate(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, dissociate);
	(void)data;
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_init(&v->map.link);
	wl_list_init(&v->unmap.link);
	wl_list_init(&v->commit.link);
}

static void xwl_destroy(struct wl_listener *l, void *data)
{
	struct aro_view *v = wl_container_of(l, v, destroy);
	(void)data;

	if (v->server->focused == v)
		v->server->focused = NULL;
	grab_forget(v->server, v);

	ftl_destroy(v);
	mru_remove(v->server, v);
	qtext_finish(&v->title);
	if (v->frame_tree)
		wlr_scene_node_destroy(&v->frame_tree->node);

	/* listeners are safe to remove */
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->associate.link);
	wl_list_remove(&v->dissociate.link);
	wl_list_remove(&v->request_configure.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->link);
	free(v);
}

void new_xwayland_surface(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	if (xsurface->override_redirect) {
		new_unmanaged(s, xsurface);
		return;
	}

	struct aro_view *v = calloc(1, sizeof *v);
	if (!v)
		return;
	v->server = s;
	v->impl = &xwl_impl;
	v->csd = false;     /* X11 clients expect us to decorate them */
	v->xsurface = xsurface;
	wl_list_init(&v->mru_link);

	if (!ui_frame_create(v, s->l_tiled)) {
		wlr_log(WLR_ERROR, "could not build a frame for an X11 window");
		free(v);
		return;
	}
	v->frame_tree->node.data = v;   /* view_at() walks up looking for this */
	wlr_scene_node_set_enabled(&v->frame_tree->node, false);

	/* surface arrives on associate */
	wl_list_init(&v->map.link);
	wl_list_init(&v->unmap.link);
	wl_list_init(&v->commit.link);

	v->associate.notify = xwl_associate;
	wl_signal_add(&xsurface->events.associate, &v->associate);
	v->dissociate.notify = xwl_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &v->dissociate);
	v->set_title.notify = view_set_title;
	wl_signal_add(&xsurface->events.set_title, &v->set_title);
	v->set_app_id.notify = view_set_app_id;
	wl_signal_add(&xsurface->events.set_class, &v->set_app_id);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen, &v->request_fullscreen);
	v->request_move.notify = view_request_move;
	wl_signal_add(&xsurface->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_resize;
	wl_signal_add(&xsurface->events.request_resize, &v->request_resize);
	v->request_configure.notify = xwl_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &v->request_configure);
	v->destroy.notify = xwl_destroy;
	wl_signal_add(&xsurface->events.destroy, &v->destroy);

	v->id = ++s->next_view_id;
	wl_list_insert(&s->views, &v->link);
}

void xwayland_ready(struct wl_listener *l, void *data)
{
	struct aro_server *s = wl_container_of(l, s, xwayland_ready);
	(void)data;
	wlr_xwayland_set_seat(s->xwayland, s->seat);
	wlr_log(WLR_INFO, "xwayland ready on %s", s->xwayland->display_name);
}

#endif /* ARO_XWAYLAND */
