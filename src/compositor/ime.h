/*
 * ime.h: input methods (fcitx5, ibus) via text-input-v3 and input-method-v2.
 *
 * aro relays between the two: an application's text field (text-input) and
 * the one input method client (input-method). Keys go to the input method
 * while it holds a keyboard grab; it sends composed text back, which aro
 * forwards to the focused text field. Candidate windows are the input
 * method's popups, placed under the text cursor.
 */
#ifndef ARO_IME_H
#define ARO_IME_H

#include <stdbool.h>
#include <stdint.h>

struct aro_server;
struct aro_keyboard;
struct wlr_surface;
struct wlr_keyboard_key_event;

void ime_init(struct aro_server *s);
void ime_finish(struct aro_server *s);

/* the surface with keyboard focus changed (NULL: nothing focused) */
void ime_set_focus(struct aro_server *s, struct wlr_surface *surface);

/*
 * Route a key or a modifier change to the input method's grab. Return true
 * when the grab took it and the seat must not see it.
 */
bool ime_key(struct aro_server *s, struct aro_keyboard *kb,
             const struct wlr_keyboard_key_event *ev);
bool ime_modifiers(struct aro_server *s, struct aro_keyboard *kb);

#endif
