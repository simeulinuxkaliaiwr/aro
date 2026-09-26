/* theme.h: visual defaults */
#ifndef ARO_THEME_H
#define ARO_THEME_H

#define ARO_NAME "aro"

/* spacing */
#define TH_GAP          9       /* between sibling frames */
#define TH_OUTER_GAP    9       /* tree to screen edge */
#define TH_BORDER       2       /* frame border, drawn inside the frame */
#define TH_RADIUS       5       /* corner radius (needs scenefx) */
#define TH_MIN          80      /* a frame never arranges smaller than this */

#define TH_HEADER_H     26      /* per-frame title strip; 0 disables it */
#define TH_BAR_H        30      /* status bar along the bottom */
#define TH_BAR_PAD      12

/* floating windows need separate minimums */
#define TH_FLOAT_MIN_W  240     /* a floating frame is never narrower */
#define TH_FLOAT_MIN_H  120
#define TH_FLOAT_SCALE  0.5     /* of the usable area, when the client
                                   has not told us a size yet */

/* fonts */
/* pango font descriptions */
#define TH_FONT         "Sans 10"
#define TH_FONT_SMALL   "Sans 9"
#define TH_TEXT_PAD     10      /* inset of header text from the frame edge */

/* colors */
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

/* shadows off */
#define TH_SHADOW_ENABLE 0
#define TH_SHADOW_COLOR  0x00000066
#define TH_SHADOW_BLUR   18
#define TH_SHADOW_Y      6

/* animation */
/* animation timings */
#define TH_ANIM_MS       340    /* open, retile */
#define TH_ANIM_FOCUS_MS 160    /* border colour crossfade */
#define TH_OPEN_SCALE    0.88   /* new frames grow from this, overshooting */
#define TH_CLOSE_MS      160    /* closing shrinks back to open scale, flat */

/* spring easing */
#define TH_EASE_X1 0.30
#define TH_EASE_Y1 1.50
#define TH_EASE_X2 0.50
#define TH_EASE_Y2 1.00

/* flat easing for fullscreen */
#define TH_EASE_FLAT_X1 0.22
#define TH_EASE_FLAT_Y1 1.00
#define TH_EASE_FLAT_X2 0.36
#define TH_EASE_FLAT_Y2 1.00
#define TH_ANIM_FS_MS   220

/* workspace switch: flat slide, the old workspace out, the new one in.
 * Flat because the move is a whole screen wide (see fullscreen). */
#define TH_WS_SLIDE_MS  220

/* toast styling */
/* internal toasts only */
#define TH_NOTIFY_BG    0x12161deb      /* slightly translucent frame colour */
#define TH_NOTIFY_MS    6000            /* how long a message stays up */
#define TH_NOTIFY_MAX_W 520

/* prompt scrim */
#define TH_SCRIM        0x0c0f13b3

/* drag/resize */
/* drag tear-out threshold */
#define TH_DRAG_TEAR    12

/* resize grab zone */
#define TH_RESIZE_ZONE  8

/* drop preview colors */
#define TH_DROP_FILL    0xe6a54b1f
#define TH_DROP_LINE    0xe6a54bcc
#define TH_DROP_MS      160     /* it chases the cursor, so it moves faster */

/* behavior defaults */
#define TH_WORKSPACES   4

/* focus follows mouse default */
#define TH_FOCUS_FOLLOWS_MOUSE 0

/* window switcher (mod+tab) */
#define TH_SWITCH_DELAY_MS    150       /* a quick tap swaps without the card */
#define TH_SWITCH_DEBOUNCE_MS 750       /* focus this long before it counts */
#define TH_SWITCH_PREVIEW_H   260       /* preview height; half the screen
                                           at most */

/* overview */
#define TH_OVERVIEW_ZOOM 0.5    /* workspace size, of the screen */
#define TH_OVERVIEW_MS   250
#define TH_OVERVIEW_TINT 0x0c0f1366     /* over the blurred wallpaper */

/* confirm quit default */
#define TH_CONFIRM_QUIT 1

/* bar: 0 off, 1 always, 2 auto */
#define TH_BAR          2
/* delay before our bar comes back */
#define TH_BAR_RETURN_MS 500
#define TH_RESIZE_STEP  0.04    /* one press of the resize bind */

#endif
