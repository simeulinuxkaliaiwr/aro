/* protocols.h: smaller Wayland protocols that need little of aro's own state */
#ifndef ARO_PROTOCOLS_H
#define ARO_PROTOCOLS_H

#include <stdbool.h>

struct aro_server;
struct aro_view;
struct wlr_buffer;
struct wlr_swapchain;

/* after the renderer is bound to the display, before clients connect */
void protocols_init(struct aro_server *s);

void protocols_finish(struct aro_server *s);

/* after the scene: colour management, when the renderer can convert */
void colour_init(struct aro_server *s);

/* keyboard focus moved: an app's shortcut inhibitor holds only while it has focus */
void shortcuts_inhibit_sync(struct aro_server *s);

/* the focused app has taken aro's shortcuts, e.g. a VM or remote desktop */
bool shortcuts_inhibited(struct aro_server *s);

/* a window unmaps or goes: its capture, if anyone made one, ends */
void capture_view_gone(struct aro_view *v);

/* a window's size in buffer pixels, buffers that size, and the window drawn into one */
void capture_size(struct aro_view *v, int *w, int *h, float *scale);
struct wlr_swapchain *capture_swapchain(struct aro_view *v, int w, int h);
bool capture_draw(struct aro_view *v, struct wlr_buffer *buf);

/* export.c: Hyprland's window export, which Quickshell's window previews use */
void export_init(struct aro_server *s);
void export_view_gone(struct aro_view *v);

/* bgeffect.c: ext-background-effect-v1, blur behind windows that ask for it */
void bgeffect_init(struct aro_server *s);

/* after a config reload: tell apps if blur turned on or off */
void bgeffect_sync(struct aro_server *s);

#endif
