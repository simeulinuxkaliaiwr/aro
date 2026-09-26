/*
 * aro.h: shared compositor types.
 *
 * Wayland-side structures only; layout internals stay in layout.h.
 */
#ifndef ARO_H
#define ARO_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "anim.h"
#include "bar.h"
#include "config.h"
#include "ghost.h"
#include "idle.h"
#include "ime.h"
#include "lock.h"
#include "notify.h"
#include "overview.h"
#include "prompt.h"
#include "switcher.h"
#include "layout.h"
#include "text.h"
#include "wallpaper.h"

struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_fbox;
struct wlr_scene_layer_surface_v1;
struct wlr_layer_surface_v1;
struct wlr_surface;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct wlr_box;

/* layer-shell client: bar, wallpaper, launcher, lock screen */
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
 * Shell operations. xdg-shell and XWayland implement these differently.
 * Keep shell-specific details out of ui.c.
 */
struct aro_view;

struct view_impl {
	/* set position and size. xdg ignores x/y; X11 uses them */
	void (*configure)(struct aro_view *v, int x, int y, int w, int h);
	void (*close)(struct aro_view *v);
	void (*activate)(struct aro_view *v, bool activated);
	void (*set_fullscreen)(struct aro_view *v, bool fullscreen);
	const char *(*title)(struct aro_view *v);
	/* app_id for rules; WM_CLASS class on X11 */
	const char *(*app_id)(struct aro_view *v);
	/* window type for `type:` rules: "normal", "dialog", "splash", ... */
	const char *(*type)(struct aro_view *v);

	/* visible window geometry inside the surface (CSD shadow margins etc.) */
	void (*geometry)(struct aro_view *v, struct wlr_box *out);

	/* whether it should float on map, and the size it would like */
	bool (*wants_float)(struct aro_view *v);
	bool (*wants_fullscreen)(struct aro_view *v);   /* asked before map */
	void (*preferred_size)(struct aro_view *v, int *w, int *h);

	/* the surface that takes keyboard focus */
	struct wlr_surface *(*surface)(struct aro_view *v);
};

/* managed window */
struct aro_view {
	struct wl_list link;            /* aro_server.views */
	struct aro_server *server;
	uint32_t id;                    /* stable, never reused; aroctl's window id */

	/* shell vtable */
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

	/* floating windows are outside the tree. fullscreen is separate. */
	bool floating;
	bool fullscreen;

	/* float_follow: client controls floating size after initial configure */
	bool float_follow;

	/* last rule result; only changes are re-applied */
	struct q_rule_result rule_last;

	/* client-side decorations */
	bool csd;
	ly_box fbox;
	ly_box pre_fs;

	/* frame nodes: border, bg, ring, header, content, popups */
	struct wlr_scene_tree *frame_tree;
	struct wlr_scene_rect *frame;
	struct wlr_scene_rect *bg;
	struct wlr_scene_rect *ring;
	struct wlr_scene_rect *header;
	struct wlr_scene_tree *content;
	/* client surface tree; clipping must target this node */
	struct wlr_scene_tree *surface_tree;
	/* popups sit outside the clipped content */
	struct wlr_scene_tree *popups;
	struct qtext title;

	anim_box geo;                   /* current vs target geometry */
	bool mapped;
	struct wl_list mru_link;        /* aro_switcher.mru; self-linked when out */

	/* foreign toplevel handles (taskbars, window lists); mapped only */
	struct wlr_foreign_toplevel_handle_v1 *ftl;
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_ftl;
	struct wlr_output *ftl_output;  /* the output last reported */
	bool ftl_fullscreen;            /* the fullscreen state last reported */
	struct wl_listener ftl_activate;
	struct wl_listener ftl_close;
	struct wl_listener ftl_fullscreen_req;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener set_title;
	struct wl_listener set_app_id;          /* WM_CLASS on X11 */
	struct wl_listener request_fullscreen;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener destroy;
};

/* drag-and-drop slot preview */
struct aro_preview {
	struct aro_server *server;   /* for the theme; it draws itself */
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *edge;    /* hairline */
	struct wlr_scene_rect *fill;    /* accent wash */
	anim_box geo;
	bool active;
};

/* pointer mode */
enum aro_cursor_mode {
	ARO_CURSOR_PASSTHROUGH,
	ARO_CURSOR_MOVE,
	ARO_CURSOR_RESIZE,       /* a floating window's own box */
	ARO_CURSOR_RESIZE_TILE,  /* a boundary two tiled windows share */
};

/* ── an output ─────────────────────────────────────────────────────────── */
/* per-output state; each output owns its workspaces */
struct aro_output {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_output *wlr_output;

	ly_box box;                     /* this output in layout coordinates */
	ly_box usable;                  /* minus exclusive zones and our bar */
	float scale;                    /* for text rendered on this output */

	ly_node *ws[ARO_MAX_WS];     /* one tree per workspace, this output */
	int cur_ws;
	/* runtime layout per workspace; Q_LAYOUT_INHERIT = config */
	int ws_layout[ARO_MAX_WS];

	/*
	 * Workspace slide. Draw-time only: geo stays the real position and
	 * the tree never hears of it. The outgoing workspace is offset by
	 * from..to along the axis, the incoming one by that plus span, so the
	 * two move as one strip and a switch mid-slide continues from where
	 * things are.
	 */
	struct {
		bool active;
		int out_ws;             /* leaving; drawn until the slide ends */
		bool vertical;          /* axis, fixed when the slide starts */
		int span;               /* signed: incoming sits this far from outgoing */
		int from, to;           /* outgoing offset at start and end */
		uint32_t start_ms, dur_ms;
	} slide;

	struct aro_bar bar;          /* one bar per output */
	/* bar = auto: pending return timer */
	struct wl_event_source *bar_return;
	/* next arrange places instead of springing */
	bool bar_snap;

	/* layout-enabled flag; not the same as wlr_output DPMS state */
	bool enabled;

	/* last applied monitor block values; reload applies only changes */
	struct q_monitor_set mon_last;
	bool mon_applied;


	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct aro_keyboard {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual;        /* virtual-keyboard: brings its own keymap */

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* kept so a reload can reconfigure it */
struct aro_pointer {
	struct wl_list link;
	struct aro_server *server;
	struct wlr_input_device *dev;
	struct wl_listener destroy;
};

/* one per pointer constraint, so its destroy can be seen */
struct aro_constraint {
	struct aro_server *server;
	struct wlr_pointer_constraint_v1 *constraint;
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

	/* stacking layers, bottom to top */
	struct wlr_scene_tree *l_background;
	struct wlr_scene_tree *l_bottom;
	struct wlr_scene_tree *l_tiled;         /* our windows */
	struct wlr_scene_tree *l_preview;       /* drop indicator, over the tiles */
	struct wlr_scene_tree *l_float;         /* dialogs, pickers, toggled */
	struct wlr_scene_tree *l_overview;      /* under the bar */
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
	struct wlr_output_manager_v1 *output_mgr;   /* wlr-randr, kanshi */
	struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wlr_gamma_control_manager_v1 *gamma_mgr;
	struct aro_ime *ime;                    /* ime.c; NULL if unavailable */
	struct wlr_foreign_toplevel_manager_v1 *ftl_mgr;
	struct wlr_ext_foreign_toplevel_list_v1 *ext_ftl_list;
	struct aro_view *ftl_activated;         /* last view reported focused */
	struct wlr_virtual_keyboard_manager_v1 *virtual_kbd_mgr;
	struct wlr_virtual_pointer_manager_v1 *virtual_ptr_mgr;
	struct aro_lock *lock;               /* non-NULL while locked */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_mgr;
	char *xcursor_theme;            /* what xcursor_mgr loaded; NULL = default */
	int xcursor_size;
	/* the session's XCURSOR_THEME / XCURSOR_SIZE */
	char *env_cursor_theme;
	int env_cursor_size;

	struct wl_list outputs;         /* enabled ones only; see aro_output */
	struct wl_list outputs_off;     /* disabled by config or wlr-randr */
	struct wl_list views;
	struct wl_list keyboards;
	struct wl_list pointers;
	struct wl_list layers;
	struct wl_list notifications;
	struct aro_prompt prompt;            /* the modal yes/no card */
	struct aro_switcher switcher;        /* mod+tab */
	struct aro_overview overview;
	struct wl_list ghosts;               /* closed windows fading out */
	struct wl_list inhibitors;

	/* per-output state lives in aro_output */
	struct aro_output *focused_output;

  	/* parked workspaces: kept when the last output disappears */
	ly_node *orphan_ws[ARO_MAX_WS];
	int orphan_ws_layout[ARO_MAX_WS];
	int orphan_cur_ws;
	bool parked;

	struct aro_config cfg;

	struct wl_event_source *clock_timer;

	/* aropaper, run as the `wallpaper` key says */
	struct aro_wallpaper wallpaper;

	/* aroctl: ipc.c; NULL if the socket could not be made */
	struct aro_ipc *ipc;
	uint32_t next_view_id;          /* last id handed out; 0 is never one */

	/* config reload: watch the directory, not the file */
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

	/* tiled resize drives the split ratio */
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
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;
	struct wl_listener new_constraint;
	struct wl_listener request_set_shape;
	struct wl_listener request_activate;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_abs;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;

	/* pointer focus origin for grabbed motion */
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
/* switch to the window's workspace and focus it */
void view_raise_and_focus(struct aro_server *s, struct aro_view *v);
struct aro_output *aro_focused_output(struct aro_server *s);

/* for ipc.c: the same paths the keyboard and the config watch take */
void aro_run_action(struct aro_server *s, const struct q_bind *b);
void aro_config_reload(struct aro_server *s);
/* tiling area: usable minus our bar */
ly_box aro_output_usable(struct aro_output *o);
/* where the frame is going (view_target): output box, fbox or leaf box */
ly_box aro_view_box(struct aro_view *v);
const char *aro_view_type(struct aro_view *v);
/* effective workspace layout */
enum q_layout aro_ws_layout(struct aro_output *o, int ws);

/* for overview.c: dropping a dragged window, and the main drag's geometry */
void aro_view_drop(struct aro_server *s, struct aro_view *v,
                   struct aro_output *o, int ws, struct aro_view *target,
                   ly_edge e, const ly_box *fbox);
ly_box aro_drop_slot(ly_box t, ly_edge e);
ly_edge aro_nearest_edge(ly_box b, double x, double y);

/* shell wrappers */
void view_configure(struct aro_view *v, int x, int y, int w, int h);
void view_geometry(struct aro_view *v, struct wlr_box *out);
void view_close(struct aro_view *v);
void view_activate(struct aro_view *v, bool activated);
const char *view_title(struct aro_view *v);
const char *view_app_id(struct aro_view *v);
struct wlr_surface *view_surface(struct aro_view *v);
uint32_t aro_now_ms(void);

/* ui.c frame helpers */
bool ui_frame_create(struct aro_view *v, struct wlr_scene_tree *parent);
void ui_frame_geometry(struct aro_view *v, ly_box b);
/* content box inside a frame */
void ui_frame_content_box(struct aro_view *v, ly_box b, ly_box *out);
void ui_frame_focus(struct aro_view *v, bool focused);
void ui_frame_title(struct aro_view *v, int frame_w, float scale);
void ui_frame_clip_content(struct aro_view *v);
void ui_frame_fullscreen(struct aro_view *v, bool fullscreen);
void ui_frame_retheme(struct aro_view *v);
void ui_color(uint32_t rgba, float out[4]);

/* a window's buffer as a scene node; shared by switcher and overview */
bool ui_snapshot_src(struct aro_view *v, struct wlr_fbox *out);
struct wlr_scene_buffer *ui_snapshot_create(struct wlr_scene_tree *parent,
                                            struct aro_view *v, int w, int h,
                                            int radius);
void ui_snapshot_update(struct wlr_scene_buffer *b, struct aro_view *v);

/* drop preview */
bool ui_preview_create(struct aro_preview *p, struct aro_server *s);
void ui_preview_show(struct aro_preview *p, bool visible);
void ui_preview_geometry(struct aro_preview *p, ly_box b);
void ui_preview_finish(struct aro_preview *p);
void ui_preview_retheme(struct aro_preview *p);

#endif
