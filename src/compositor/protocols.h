/* protocols.h: smaller Wayland protocols that need little of aro's own state */
#ifndef ARO_PROTOCOLS_H
#define ARO_PROTOCOLS_H

#include <stdbool.h>

struct aro_server;

/* after the renderer is bound to the display, before clients connect */
void protocols_init(struct aro_server *s);

void protocols_finish(struct aro_server *s);

/* after the scene: colour management, when the renderer can convert */
void colour_init(struct aro_server *s);

/* keyboard focus moved: an app's shortcut inhibitor holds only while it has focus */
void shortcuts_inhibit_sync(struct aro_server *s);

/* the focused app has taken aro's shortcuts, e.g. a VM or remote desktop */
bool shortcuts_inhibited(struct aro_server *s);

#endif
