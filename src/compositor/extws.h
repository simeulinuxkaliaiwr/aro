/* extws.h: aro's workspaces over ext-workspace-v1, for waybar and other bars */
#ifndef ARO_EXTWS_H
#define ARO_EXTWS_H

struct aro_server;
struct aro_output;

void extws_init(struct aro_server *s);

/* publish every output's workspaces; after anything that changes them */
void extws_sync(struct aro_server *s);

/* an output is going away or being disabled */
void extws_output_gone(struct aro_output *o);

void extws_finish(struct aro_server *s);

#endif
