/*
 * ipc.h: the socket aroctl talks to.
 *
 * $XDG_RUNTIME_DIR/aro-$WAYLAND_DISPLAY.sock, mode 0600, exported to
 * children as ARO_SOCKET. One request per connection:
 *
 *     text|json <command> [args]\n
 *
 * The reply's first line is `ok` or `error <message>`; the body follows
 * and the server closes the connection.
 */
#ifndef ARO_IPC_H
#define ARO_IPC_H

#include <stdbool.h>

struct aro_server;

/* create the socket and set ARO_SOCKET; false (and s->ipc NULL) on failure,
 * which is logged and otherwise harmless: aro runs without aroctl */
bool ipc_init(struct aro_server *s, const char *wl_socket);

/* close every connection, remove the socket */
void ipc_finish(struct aro_server *s);

#endif
