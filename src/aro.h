/*
 * aro — aro.h
 *
 * Types shared between the compositor core, the frame renderer and the
 * animator. Nothing here reaches into the layout tree's internals: the tree
 * owns structure, this file owns everything Wayland.
 */
#ifndef ARO_H
#define ARO_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "anim.h"
#include "bar.h"
#include "config.h"
#include "idle.h"
#include "lock.h"
#include "notify.h"
#include "prompt.h"
#include "layout.h"
#include "text.h"

struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_layer_surface_v1;
struct wlr_layer_surface_v1;
struct wlr_surface;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct wlr_box;

/* A layer-shell client: bars, wallpapers, launchers, lock screens. */
struct aro_layer {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/*
 * What a shell has to be able to do, so that nothing outside the shell's own
 * file needs to know which one a window came from.
 *
 * xdg-shell and XWayland answer these differently: xdg negotiates a size and
 * the client replies when it likes, X11 is told where it is and believes it;
 * xdg carries its title on the toplevel, X11 on the surface. Keeping the
 * difference behind these six calls is what stops ui.c — which owns
 * appearance and nothing else — from learning that X11 exists.
 */
struct aro_view;

struct view_impl {
	/*
	 * Position AND size, in layout coordinates. xdg-shell ignores x and y
	 * — a Wayland client does not know where it is — but X11 is told its
	 * absolute position and believes it, so the box has to be whole.
	 */
	void (*configure)(struct aro_view *v, int x, int y, int w, int h);
	void (*close)(struct aro_view *v);
	void (*activate)(struct aro_view *v, bool activated);
	void (*set_fullscreen)(struct aro_view *v, bool fullscreen);
	const char *(*title)(struct aro_view *v);
	/* what window rules match app_id: against — WM_CLASS class on X11 */
	const char *(*app_id)(struct aro_view *v);

	/*
	 * Where the real window sits inside its surface.
	 *
	 * A GTK client-side-decorated surface is BIGGER than the window you
	 * see: it carries invisible shadow margins, and xdg_surface.geometry
	 * says where the window proper begins inside it. Laying the surface
	 * out at its own origin puts the shadow inside our frame as a visible
	 * band and pushes the bottom of the window past the clip.
	 *
	 * X11 has no such thing, so xwayland returns the whole surface.
	 */
	void (*geometry)(struct aro_view *v, struct wlr_box *out);

	/* whether it should float on map, and the size it would like */
	bool (*wants_float)(struct aro_view *v);
	bool (*wants_fullscreen)(struct aro_view *v);   /* asked before map */
	void (*preferred_size)(struct aro_view *v, int *w, int *h);

	/* the surface that takes keyboard focus */
	struct wlr_surface *(*surface)(struct aro_view *v);
};

/* ── a managed window ──────────────────────────────────────────────────── */
struct aro_view {
	struct wl_list link;            /* aro_server.views */
	struct aro_server *server;

	/* Which shell this window came from. Every call that has to talk to
	 * the client goes through here, so nothing below aro.c needs to
	 * know whether it is a Wayland or an X11 window. */
	const struct view_impl *impl;
	struct wlr_xdg_toplevel *toplevel;      /* xdg-shell views only */
#ifdef ARO_XWAYLAND
	struct wlr_xwayland_surface *xsurface;  /* X11 views only */
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
#endif
	ly_node *node;                  /* our leaf; NULL while floating */
	struct aro_output *output;   /* which screen it lives on */
	int workspace;                  /* index into that output's ws[] */

	/*
	 * Floating windows are NOT in the layout tree. They are removed from it
	 * entirely, which is what keeps layout.c free of the concept: the tree
	 * only ever sees windows it actually tiles.
	 *
	 *   fbox     where the window sits while floating (layout coordinates)
	 *   pre_fs   fbox before fullscreen, restored on the way back out
	 *
	 * Fullscreen is orthogonal: a tiled window can be fullscreen too, and it
	 * keeps its leaf so leaving fullscreen just falls back into arrange.
	 */
	bool floating;
	bool fullscreen;

	/*
	 * Hybrid float sizing: who decides how big a floating window is.
	 *
	 * We tell it once, when it starts floating, and then get out of the
	 * way — float_follow means "the client's own size wins now, stop
	 * configuring it". Its commits move fbox instead (view_float_follow).
	 *
	 * Configuring a float on every frame is what made file pickers come up
	 * at half the screen and stay there: the first box we computed was
	 * wrong, and we then held the client to it forever, so it could never
	 * settle on what it actually wanted.
	 *
	 * Dragging an edge clears it for good — the user asking for a size
	 * outranks both of us — and it is never set for X11, which is TOLD
	 * where it is and believes it, so it has to keep being told.
	 */
	bool float_follow;

	/*
	 * What the window rules said about this window the last time they
	 * were asked — at map, on a title change, on a config reload.
	 *
	 * Re-checks apply only what CHANGED since then. A terminal retitles
	 * itself on every command, and re-applying a float rule each time
	 * would undo mod+space the moment you pressed enter; comparing
	 * against the last answer lets a rule fire when it newly matches
	 * (Firefox renaming a window "Picture-in-Picture" after it maps) and
	 * otherwise leaves your manual changes alone.
	 */
	struct q_rule_result rule_last;

	/*
	 * The client draws its own title bar.
	 *
	 * True by default, because a client that never creates an
	 * xdg-decoration object — every GTK app — is telling us nothing and
	 * drawing its own. Cleared only when a decoration actually settles on
	 * server-side mode. Drawing our header on top of the client's gives
	 * you two title bars stacked, which is unmistakable.
	 */
	bool csd;
	ly_box fbox;
	ly_box pre_fs;

	/* scene graph, outside in:
	 *   frame  — border colour, full size
	 *   bg     — frame background, inset by TH_BORDER
	 *   ring   — 1px accent hairline just inside the border, focused only
	 *   header — title strip (a plain rect until the cairo buffer lands)
	 *   content— the client's own surface tree
	 */
	struct wlr_scene_tree *frame_tree;
	struct wlr_scene_rect *frame;
	struct wlr_scene_rect *bg;
	struct wlr_scene_rect *ring;
	struct wlr_scene_rect *header;
	struct wlr_scene_tree *content;
	/*
	 * The client's own surface tree, nested inside content.
	 *
	 * Kept because wlr_scene_subsurface_tree_set_clip() wants THIS node,
	 * not the plain tree around it — clipping the wrapper silently does
	 * nothing, which is how an unclipped GTK shadow ends up looking like
	 * a fat border.
	 */
	struct wlr_scene_tree *surface_tree;
	/*
	 * Popups — menus, dropdowns, tooltips — hang here rather than under
	 * content, because content is clipped to the frame and a context menu
	 * is meant to overflow it. Created after content, so it draws above.
	 */
	struct wlr_scene_tree *popups;
	struct qtext title;

	anim_box geo;                   /* current vs target geometry */
	bool mapped;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener set_title;
	struct wl_listener request_fullscreen;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener destroy;
};

/* The drop indicator shown while a window is being dragged. Lives in its own
 * scene tree above the tiled windows and below the floating ones, so the
 * window you are dragging stays over the slot it would land in. */
struct aro_preview {
	struct aro_server *server;   /* for the theme; it draws itself */
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *edge;    /* hairline */
	struct wlr_scene_rect *fill;    /* accent wash */
	anim_box geo;
	bool active;
};

/*
 * What the pointer is currently doing. PASSTHROUGH is the normal case: events
 * go to the client under the cursor. The others mean we have taken the
 * pointer for ourselves and the client sees nothing until the button is let
 * go.
 */
enum aro_cursor_mode {
	ARO_CURSOR_PASSTHROUGH,
	ARO_CURSOR_MOVE,
	ARO_CURSOR_RESIZE,       /* a floating window's own box */
	ARO_CURSOR_RESIZE_TILE,  /* a boundary two tiled windows share */
};

/* ── an output ─────────────────────────────────────────────────────────── */
/*
 * Each output owns its own workspaces, sway-style: mod+1..4 switches the
 * focused output and leaves the others alone. The alternative — workspaces
 * that migrate between outputs, i3-style — needs a workspace-to-output map
 * and a rule for what happens when an output vanishes; this needs neither,
 * because a workspace cannot outlive the screen it belongs to.
 *
 * The trees live here rather than on the server. layout.c is untouched by
 * any of this: it still takes an area and returns rectangles, which is
 * exactly why multi-output is contained to the compositor.
 */
struct aro_output {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_output *wlr_output;

	ly_box box;                     /* this output in layout coordinates */
	ly_box usable;                  /* minus exclusive zones and our bar */
	float scale;                    /* for text rendered on this output */

	ly_node *ws[ARO_MAX_WS];     /* one tree per workspace, this output */
	int cur_ws;

	struct aro_bar bar;          /* one bar per output */

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct aro_keyboard {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ── the server ────────────────────────────────────────────────────────── */
struct aro_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;    /* NULL unless we are on DRM/TTY */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_rect *root_bg;         /* what shows through the gaps */

	/* Stacking, bottom to top. Scene siblings draw in creation order, so
	 * these are created in exactly this sequence and never reordered. */
	struct wlr_scene_tree *l_background;
	struct wlr_scene_tree *l_bottom;
	struct wlr_scene_tree *l_tiled;         /* our windows */
	struct wlr_scene_tree *l_preview;       /* drop indicator, over the tiles */
	struct wlr_scene_tree *l_float;         /* dialogs, pickers, toggled */
	struct wlr_scene_tree *l_unmanaged;     /* X11 override-redirect */
	struct wlr_scene_tree *l_bar;           /* our bar */
	struct wlr_scene_tree *l_top;
	struct wlr_scene_tree *l_fullscreen;    /* covers the bar and panels */
	struct wlr_scene_tree *l_notify;        /* our own messages, over all of it */
	struct wlr_scene_tree *l_overlay;       /* lock screens stay above */
	struct wlr_output_layout *output_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_session_lock_manager_v1 *lock_mgr;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct aro_lock *lock;               /* non-NULL while locked */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_mgr;

	struct wl_list outputs;
	struct wl_list views;
	struct wl_list keyboards;
	struct wl_list layers;
	struct wl_list notifications;
	struct aro_prompt prompt;            /* the modal yes/no card */
	struct wl_list inhibitors;

	/* Workspaces, trees, usable area, scale and the bar all moved to
	 * aro_output. What is left here is how many workspaces each output
	 * gets, and which output the keyboard is on. */
	struct aro_output *focused_output;

  	/*
	 * Parked workspaces: where an output's trees go when the LAST output
	 * disappears, instead of being freed.
	 *
	 * A VT switch takes the outputs with it — wlroots treats it as an
	 * unplug — and a laptop has one. Freeing the trees there leaves the
	 * windows running with nowhere to be, and the output that comes back
	 * comes back empty. Parking keeps the tree intact, split ratios and
	 * all, so switching back restores the layout rather than re-inserting
	 * the survivors in a row.
	 *
	 * A view is parked exactly when it is mapped and v->output is NULL.
	 * While parked its fbox and pre_fs are relative to the output it lost,
	 * since the next one need not be at the same origin.
	 */
	ly_node *orphan_ws[ARO_MAX_WS];
	int orphan_cur_ws;
	bool parked;

	struct aro_config cfg;

	struct wl_event_source *clock_timer;

	/* live config reload: inotify on the config's DIRECTORY, because
	 * editors replace the file rather than rewriting it in place */
	char *cfg_path;
	int cfg_fd, cfg_wd;
	struct wl_event_source *cfg_source;

	struct aro_view *focused;
	struct aro_layer *focused_layer;     /* steals the keyboard while up */
	ly_dir pending_split;           /* orientation the next window will use */
	bool split_forced;              /* ...because mod+v/mod+s asked, not dwindle */

	/* ── pointer grab ──────────────────────────────────────────────── */
	enum aro_cursor_mode cursor_mode;
	struct aro_view *grabbed;
	double grab_x, grab_y;          /* cursor position when the grab began */
	ly_box grab_box;                /* the view's box when the grab began */
	uint32_t grab_edges;            /* WLR_EDGE_*, resize only */

	/* Tiled resize does not move a window, it moves a boundary. This is
	 * the split node that owns it; its ratio is what the cursor drives. */
	ly_node *grab_split;
	double grab_ratio;              /* that boundary's ratio when grabbed */
	bool grab_from_tree;            /* it was tiled when the drag started */
	bool grab_tore_out;             /* ...and has since been pulled loose */

	/* where the dragged window would land if dropped now */
	struct aro_preview preview;
	struct aro_view *drop_target;
	ly_edge drop_edge;

	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
#ifdef ARO_XWAYLAND
	struct wlr_xwayland *xwayland;
	struct wl_list unmanaged;               /* override-redirect surfaces */
	struct wl_listener new_xwayland_surface;
	struct wl_listener xwayland_ready;
#endif
	struct wl_listener new_decoration;
	struct wl_listener new_layer_surface;
	struct wl_listener new_lock;
	struct wl_listener new_inhibitor;
	struct wl_listener output_power_set_mode;
	struct wl_listener new_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_abs;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;

	/*
	 * Layout position of whatever surface currently has pointer focus,
	 * kept so motion can stay in that surface's coordinate space while a
	 * button is held and the cursor has wandered off it.
	 */
	double ptr_lx, ptr_ly;

	/* selections (clipboard and middle-click paste) and drag-and-drop */
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_icon_destroy;
	struct wlr_scene_tree *drag_icon;
};

/* aro.c */
void aro_arrange(struct aro_server *s);
void aro_focus(struct aro_server *s, struct aro_view *v);
struct aro_output *aro_focused_output(struct aro_server *s);

/* shell-agnostic wrappers — safe on a view whose shell is gone */
void view_configure(struct aro_view *v, int x, int y, int w, int h);
void view_geometry(struct aro_view *v, struct wlr_box *out);
void view_close(struct aro_view *v);
void view_activate(struct aro_view *v, bool activated);
const char *view_title(struct aro_view *v);
const char *view_app_id(struct aro_view *v);
struct wlr_surface *view_surface(struct aro_view *v);
uint32_t aro_now_ms(void);

/* ui.c — everything that knows what a frame looks like */
bool ui_frame_create(struct aro_view *v, struct wlr_scene_tree *parent);
void ui_frame_geometry(struct aro_view *v, ly_box b);
/* the client's box inside a frame box — the one place that knows how much of
 * a frame is chrome, so nothing else has to subtract borders and headers */
void ui_frame_content_box(struct aro_view *v, ly_box b, ly_box *out);
void ui_frame_focus(struct aro_view *v, bool focused);
void ui_frame_title(struct aro_view *v, int frame_w, float scale);
void ui_frame_clip_content(struct aro_view *v);
void ui_frame_fullscreen(struct aro_view *v, bool fullscreen);
void ui_frame_retheme(struct aro_view *v);
void ui_color(uint32_t rgba, float out[4]);

/* the drop indicator — appearance lives in ui.c like everything else */
bool ui_preview_create(struct aro_preview *p, struct aro_server *s);
void ui_preview_show(struct aro_preview *p, bool visible);
void ui_preview_geometry(struct aro_preview *p, ly_box b);
void ui_preview_finish(struct aro_preview *p);
void ui_preview_retheme(struct aro_preview *p);

#endif
