/* ghost.h: a closed window's last frame, fading out */
#ifndef ARO_GHOST_H
#define ARO_GHOST_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

struct aro_server;
struct aro_output;
struct aro_view;

void ghost_init(struct aro_server *s);

/* from unmap, while the client's buffer is still there */
void ghost_spawn(struct aro_server *s, struct aro_view *v, ly_box from);

/* true while a ghost on o is still fading */
bool ghost_tick(struct aro_server *s, struct aro_output *o, uint32_t now);

/* every ghost on o now; NULL drops them all */
void ghost_drop(struct aro_server *s, struct aro_output *o);

#endif
