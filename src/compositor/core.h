/*
 * core.h: what aro.c shares with the files split off from it.
 *
 * Not an API: aro.h is what the rest of aro uses. These are aro.c's own
 * helpers, exported only so the pieces of the compositor can call each other.
 */
#ifndef ARO_CORE_H
#define ARO_CORE_H

#include "aro.h"
#include "theme.h"

struct wlr_xdg_popup;

/* the window animation curves */
static const anim_ease SPRING = { TH_EASE_X1, TH_EASE_Y1, TH_EASE_X2, TH_EASE_Y2 };
static const anim_ease FLAT = { TH_EASE_FLAT_X1, TH_EASE_FLAT_Y1,
                                TH_EASE_FLAT_X2, TH_EASE_FLAT_Y2 };

/* aro.c */
void drag_icon_update(struct aro_server *s);
uint32_t edge_zone(ly_box b, double x, double y, int zone);
void grab_begin(struct aro_server *s, struct aro_view *v, enum aro_cursor_mode mode, uint32_t edges);
void grab_end(struct aro_server *s);
void grab_forget(struct aro_server *s, struct aro_view *v);
void grab_motion(struct aro_server *s);
void keyboard_focus_changed(struct aro_server *s);
uint32_t mask_from_ly_edge(ly_edge e);
ly_edge nearest_edge(ly_box b, double x, double y);
struct aro_view *output_pick_view(struct aro_server *s, struct aro_output *o);
struct aro_output *output_toward(struct aro_server *s, struct aro_output *from, ly_edge e);
void popup_track(struct aro_server *s, struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree);
void view_map(struct wl_listener *l, void *data);
void view_move_to_output(struct aro_server *s, struct aro_view *v, struct aro_output *dest);
void view_request_fullscreen(struct wl_listener *l, void *data);
void view_request_move(struct wl_listener *l, void *data);
void view_request_resize(struct wl_listener *l, void *data);
void view_set_app_id(struct wl_listener *l, void *data);
void view_set_floating(struct aro_server *s, struct aro_view *v, bool floating);
void view_set_fullscreen(struct aro_server *s, struct aro_view *v, bool fullscreen);
void view_set_title(struct wl_listener *l, void *data);
void view_swap(struct aro_view *a, struct aro_view *b);
void view_unmap(struct wl_listener *l, void *data);

/* actions.c */
bool handle_bind(struct aro_server *s, uint32_t mods, xkb_keysym_t sym);
bool handle_switch_bind(struct aro_server *s, uint32_t mods, xkb_keysym_t sym);
void prompt_after(struct aro_server *s, bool was_active);
void run_action(struct aro_server *s, const struct q_bind *b);

/* arrange.c */
bool box_eq(ly_box a, ly_box b);
void column_set_share(struct aro_view *v, double share, double prev);
double column_share(struct aro_server *s, struct aro_view *v);
ly_box drop_target_box(struct aro_server *s, struct aro_view *t, ly_edge e);
void monocle_sync(struct aro_server *s, struct aro_output *o);
ly_node *scroll_beside(struct aro_server *s, struct aro_output *o, int ws, ly_node *leaf);
void slide_finish(struct aro_output *o);
bool slides_active(struct aro_server *s);
void slides_reap(struct aro_server *s, uint32_t now);
ly_node *tree_insert(struct aro_server *s, struct aro_output *o, int ws, struct aro_view *v, ly_node *target, ly_dir dir, bool forced);
ly_box usable_area(struct aro_output *o);
ly_box view_draw_box(struct aro_view *v, uint32_t now);
bool view_on_screen(struct aro_view *v);
void view_send_to(struct aro_server *s, struct aro_view *v, int ws);
void view_set_visible(struct aro_view *v, bool visible);
ly_box view_target(struct aro_view *v);
bool view_visible(struct aro_view *v);
ly_node **view_ws_root(struct aro_view *v);
void workspace_show(struct aro_server *s, int ws);
void ws_arrange(struct aro_server *s, struct aro_output *o, int ws);
enum q_layout ws_layout(struct aro_server *s, struct aro_output *o, int ws);
bool ws_monocle(struct aro_server *s, struct aro_output *o, int ws);

/* input.c */
void apply_keymap(struct aro_server *s, struct wlr_keyboard *wlr_kb);
void constraint_sync(struct aro_server *s);
void cursor_axis(struct wl_listener *l, void *data);
void cursor_button(struct wl_listener *l, void *data);
void cursor_frame(struct wl_listener *l, void *data);
void cursor_motion(struct wl_listener *l, void *data);
void cursor_motion_abs(struct wl_listener *l, void *data);
void cursor_warp_to_view(struct aro_server *s, struct aro_view *v);
void new_constraint(struct wl_listener *l, void *data);
void new_input(struct wl_listener *l, void *data);
void new_virtual_keyboard(struct wl_listener *l, void *data);
void new_virtual_pointer(struct wl_listener *l, void *data);
void pointer_configure(struct aro_server *s, struct wlr_input_device *dev);
void pointer_motion_common(struct aro_server *s, uint32_t time);
void request_activate(struct wl_listener *l, void *data);
void request_cursor(struct wl_listener *l, void *data);
void request_set_shape(struct wl_listener *l, void *data);
struct aro_view *view_at(struct aro_server *s, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);

/* layers.c */
void arrange_layers(struct aro_server *s);
void arrange_layers_output(struct aro_output *o);
void bar_return_cancel(struct aro_output *o);
void layer_new_popup(struct wl_listener *listener, void *data);
void new_layer_surface(struct wl_listener *listener, void *data);

/* output.c */
void monitors_reapply(struct aro_server *s);
void new_output(struct wl_listener *l, void *data);
struct aro_output *output_at(struct aro_server *s, double x, double y);
void output_destroy(struct wl_listener *l, void *data);
struct aro_output *output_evacuate(struct aro_server *s, struct aro_output *o);
struct aro_output *output_from_wlr(struct aro_server *s, struct wlr_output *wo);
void output_mgr_apply(struct wl_listener *l, void *data);
void output_mgr_test(struct wl_listener *l, void *data);
void output_mgr_update(struct aro_server *s);
void output_refresh_box(struct aro_output *o);
void update_backdrop(struct aro_server *s);

/* toplevels.c */
int clock_tick(void *data);
void ftl_create(struct aro_view *v);
void ftl_destroy(struct aro_view *v);
void ftl_sync_activated(struct aro_server *s);
void ftl_sync_view(struct aro_view *v);
void ftl_update_ids(struct aro_view *v);

/* xwayland.c */
#ifdef ARO_XWAYLAND
void new_xwayland_surface(struct wl_listener *l, void *data);
void xwayland_ready(struct wl_listener *l, void *data);
#endif

#endif /* ARO_CORE_H */
