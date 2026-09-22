/*
 * aro — theme.h
 *
 * Every visual constant, lifted from the splits start page. One accent, one
 * radius, one border width. If a value appears twice in this file, one of
 * them is wrong.
 *
 * Colours are 0xRRGGBBAA. The renderer converts to premultiplied float.
 */
#ifndef ARO_THEME_H
#define ARO_THEME_H

#define ARO_NAME "aro"

/* ── spacing ───────────────────────────────────────────────────────────── */
#define TH_GAP          9       /* between sibling frames */
#define TH_OUTER_GAP    9       /* tree to screen edge */
#define TH_BORDER       2       /* frame border, drawn inside the frame */
#define TH_RADIUS       5       /* corner radius (needs scenefx) */
#define TH_MIN          80      /* a frame never arranges smaller than this */

#define TH_HEADER_H     26      /* per-frame title strip; 0 disables it */
#define TH_BAR_H        30      /* status bar along the bottom */
#define TH_BAR_PAD      12

/* Floating windows are sized by the client, not by the tree, so they need
 * their own floor and their own fallback. TH_MIN is a tiling constraint —
 * what a frame shrinks to when its siblings crowd it — and reusing it here
 * would let a dialog come up at 80x80. */
#define TH_FLOAT_MIN_W  240     /* a floating frame is never narrower */
#define TH_FLOAT_MIN_H  120
#define TH_FLOAT_SCALE  0.5     /* of the usable area, when the client
                                   has not told us a size yet */

/* ── type ──────────────────────────────────────────────────────────────── */
/* Pango descriptions. One family, two sizes: titles and labels are the same
 * voice at different volumes. */
#define TH_FONT         "Sans 10"
#define TH_FONT_SMALL   "Sans 9"
#define TH_TEXT_PAD     10      /* inset of header text from the frame edge */

/* ── colour ────────────────────────────────────────────────────────────── */
#define TH_BG           0x0c0f13ff      /* root surface, seen through the gaps */
#define TH_FRAME        0x12161dff      /* frame background, unfocused */
#define TH_FRAME_ON     0x161b23ff      /* frame background, focused */
#define TH_LINE         0x222a35ff      /* border, unfocused */
#define TH_ACCENT       0xe6a54bff      /* border + header text, focused. The
                                           ONLY saturated colour on screen. */
#define TH_ACCENT_SOFT  0xe6a54b4d      /* the 1px ring just inside the border */
#define TH_INK          0xd7dfe9ff      /* header text, focused frame */
#define TH_DIM          0x6b7788ff      /* header text, everything else */
#define TH_BAR_BG       0x0a0d11ff
#define TH_URGENT       0xc0566bff

/* Shadows: off. splits has none, and a shadow under every frame is the
 * fastest way to stop looking minimal. Left here so the choice is visible. */
#define TH_SHADOW_ENABLE 0
#define TH_SHADOW_COLOR  0x00000066
#define TH_SHADOW_BLUR   18
#define TH_SHADOW_Y      6

/* ── motion ────────────────────────────────────────────────────────────── */
/* Geometry changes are animated by the compositor, not by the layout tree.
 * The tree only ever states where a frame should end up. */
#define TH_ANIM_MS       340    /* open, close, retile */
#define TH_ANIM_FOCUS_MS 160    /* border colour crossfade */
#define TH_OPEN_SCALE    0.88   /* new frames grow from this, overshooting */

/* cubic-bezier(.3, 1.5, .5, 1) — the spring from the start page */
#define TH_EASE_X1 0.30
#define TH_EASE_Y1 1.50
#define TH_EASE_X2 0.50
#define TH_EASE_Y2 1.00

/* Fullscreen does NOT spring. An 8% overshoot on a window that is already
 * the size of the screen has nowhere to go: x and y spring past zero, w and h
 * past the screen, and the window lurches off every edge before settling.
 * That reads as a shake, not as a spring. Same curve family, no overshoot. */
#define TH_EASE_FLAT_X1 0.22
#define TH_EASE_FLAT_Y1 1.00
#define TH_EASE_FLAT_X2 0.36
#define TH_EASE_FLAT_Y2 1.00
#define TH_ANIM_FS_MS   220

/* ── the compositor's own messages ─────────────────────────────────────── */
/* Not a notification daemon — this is aro telling you about aro, such
 * as a config line that did not parse. Errors use TH_URGENT. */
#define TH_NOTIFY_BG    0x12161deb      /* slightly translucent frame colour */
#define TH_NOTIFY_MS    6000            /* how long a message stays up */
#define TH_NOTIFY_MAX_W 520

/* The scrim behind a prompt ("Exit aro?"): the root background at 70%,
 * so the desktop dims towards its own colour rather than towards black. */
#define TH_SCRIM        0x0c0f13b3

/* ── dragging ──────────────────────────────────────────────────────────── */
/* How far a tiled window must be dragged before it tears out of the tree.
 * Too small and every click-to-focus with a twitchy hand detaches a window. */
#define TH_DRAG_TEAR    12

/* How far in from a frame's edge still counts as grabbing that edge. The
 * border itself is 2px, which is not a pointer target. */
#define TH_RESIZE_ZONE  8

/* The drop indicator: a slot-sized rectangle, accent at low alpha with a
 * solid hairline, following the same one-accent rule as everything else. */
#define TH_DROP_FILL    0xe6a54b1f
#define TH_DROP_LINE    0xe6a54bcc
#define TH_DROP_MS      160     /* it chases the cursor, so it moves faster */

/* ── behaviour ─────────────────────────────────────────────────────────── */
#define TH_WORKSPACES   4

/*
 * The DEFAULT for focus_follows_mouse; the config file decides at runtime.
 * Off, and not only as taste: in a tiling WM windows open, close and resize
 * under a stationary cursor all the time, so with this on, focus moves when
 * you have not touched the mouse. Click-to-focus never surprises you.
 */
#define TH_FOCUS_FOLLOWS_MOUSE 0

/* The DEFAULT for confirm_quit. On: quitting ends every client at once, and
 * the bind sits one key away from ones you press all day. */
#define TH_CONFIRM_QUIT 1
#define TH_RESIZE_STEP  0.04    /* one press of the resize bind */

#endif
