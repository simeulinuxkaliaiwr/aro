/*
 * wallpaper.h: aro runs aropaper itself, as the `wallpaper` key says.
 *
 * Owning the process, rather than leaving it to an `exec` line, is what
 * makes the wallpaper a default: someone without a config gets it too. It
 * also means a reload can restart it when the setting changes and leave it
 * alone when it does not — `exec_always = aropaper` would stack a new one
 * on every save.
 */
#ifndef ARO_WALLPAPER_H
#define ARO_WALLPAPER_H

#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

#include "config.h"

struct aro_wallpaper {
	struct wl_event_loop *loop;
	struct wl_list children;        /* wp_child: the running one, and any
	                                 * still exiting after being stopped */
	struct wp_child *current;       /* NULL when none is running */

	/* what current was started with: a reload compares against this */
	enum q_wallpaper mode;
	char *file;                     /* owned; Q_WALLPAPER_FILE only */

	char *saved;                    /* the state file; NULL: nowhere to keep it */

	/* problems worth a toast; the log gets them either way */
	void (*report)(void *data, const char *msg);
	void *data;
};

void wallpaper_init(struct aro_wallpaper *w, struct wl_event_loop *loop, bool nested,
                    void (*report)(void *data, const char *msg), void *data);

/* aroctl's or aropaper's wallpaper, kept across sessions until the config's line changes */
void wallpaper_save(struct aro_wallpaper *w, enum q_wallpaper mode, const char *file,
                    enum q_wallpaper cfg_mode, const char *cfg_file);
/* true and *file owned when one applies; a stale one is removed */
bool wallpaper_saved(struct aro_wallpaper *w, enum q_wallpaper cfg_mode,
                     const char *cfg_file, enum q_wallpaper *mode, char **file);
void wallpaper_forget(struct aro_wallpaper *w);

/*
 * Start, restart or stop aropaper to match the config. Changes only: the
 * same setting as last time leaves a running aropaper alone. One that
 * exited on its own is started again, so saving the config is the retry.
 */
void wallpaper_apply(struct aro_wallpaper *w, enum q_wallpaper mode, const char *file);

/* aropaper next to aro, else on PATH; owned, NULL if neither */
char *wallpaper_find_aropaper(void);
/* `~/…` as home; owned */
char *wallpaper_expand_home(const char *p);

/* stop it and free everything; before the event loop goes */
void wallpaper_finish(struct aro_wallpaper *w);

#endif
