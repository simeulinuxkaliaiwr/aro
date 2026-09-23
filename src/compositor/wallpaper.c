/*
 * wallpaper.c: aro runs aropaper itself (see wallpaper.h for why).
 *
 * aropaper is a direct child, unlike everything config_spawn() starts: aro
 * has to be able to stop it, so it cannot be double-forked away. It is
 * watched through a pidfd on the event loop, which does three jobs at
 * once — tells us when it exits, lets us reap it without a SIGCHLD
 * handler, and signals the process itself rather than a pid, so a pid
 * reused after an unnoticed exit can never be killed by mistake.
 *
 * No automatic respawn. A wallpaper that exits on its own (a file it
 * cannot decode, a crash) posts a toast instead of looping; saving the
 * config starts it again.
 */
#define _GNU_SOURCE /* asprintf */

#include "wallpaper.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/util/log.h>

struct wp_child {
	struct wl_list link;            /* aro_wallpaper.children */
	struct aro_wallpaper *w;
	pid_t pid;
	int pidfd;
	struct wl_event_source *source;
	bool stopping;                  /* we sent SIGTERM; its exit is expected */
};

/* glibc gained wrappers late (2.36); the syscalls are older and enough */
static int pidfd_open_(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}

static int pidfd_kill(int pidfd, int sig)
{
	return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
}

static void report(struct aro_wallpaper *w, const char *fmt, ...)
{
	char msg[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	wlr_log(WLR_ERROR, "%s", msg);
	if (w->report)
		w->report(w->data, msg);
}

static void child_free(struct wp_child *c)
{
	if (c->w->current == c)
		c->w->current = NULL;
	if (c->source)
		wl_event_source_remove(c->source);
	if (c->pidfd >= 0)
		close(c->pidfd);
	wl_list_remove(&c->link);
	free(c);
}

/* the pidfd turns readable once the process has exited */
static int child_exited(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct wp_child *c = data;

	int status = 0;
	pid_t r = waitpid(c->pid, &status, WNOHANG);
	if (r == 0)
		return 0;       /* not yet; cannot happen with a readable pidfd */

	if (!c->stopping) {
		/* r < 0: someone else reaped it and the status is lost */
		if (r > 0 && WIFEXITED(status))
			report(c->w, "wallpaper: aropaper exited with status %d; "
			       "the log says why", WEXITSTATUS(status));
		else if (r > 0 && WIFSIGNALED(status))
			report(c->w, "wallpaper: aropaper was killed by signal %d (%s)",
			       WTERMSIG(status), strsignal(WTERMSIG(status)));
		else
			report(c->w, "wallpaper: aropaper exited");
	} else {
		wlr_log(WLR_INFO, "wallpaper: aropaper (pid %d) stopped", c->pid);
	}
	child_free(c);
	return 0;
}

/*
 * Next to aro's own binary first, then PATH. The first is what makes a
 * development build work: ./build/aro runs ./build/aropaper without
 * anything installed. Installed, the two are the same place anyway.
 */
static char *find_aropaper(void)
{
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n > 0) {
		self[n] = '\0';
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = '\0';
			char *p = NULL;
			if (asprintf(&p, "%s/aropaper", self) >= 0) {
				if (access(p, X_OK) == 0)
					return p;
				free(p);
			}
		}
	}

	const char *env = getenv("PATH");
	if (!env)
		return NULL;
	char *path = strdup(env);
	if (!path)
		return NULL;
	char *found = NULL, *save = NULL;
	for (char *dir = strtok_r(path, ":", &save); dir && !found;
	     dir = strtok_r(NULL, ":", &save)) {
		char *p = NULL;
		if (asprintf(&p, "%s/aropaper", *dir ? dir : ".") < 0)
			continue;
		if (access(p, X_OK) == 0)
			found = p;
		else
			free(p);
	}
	free(path);
	return found;
}

/* `~/…` means home; no shell runs, so nothing else would expand it */
static char *expand_home(const char *p)
{
	const char *home = getenv("HOME");
	if (p[0] == '~' && (p[1] == '/' || p[1] == '\0') && home && *home) {
		char *out = NULL;
		if (asprintf(&out, "%s%s", home, p + 1) < 0)
			return NULL;
		return out;
	}
	return strdup(p);
}

static void start(struct aro_wallpaper *w)
{
	char *bin = find_aropaper();
	if (!bin) {
		report(w, "wallpaper: aropaper is not installed; install it, "
		       "or set wallpaper = none");
		return;
	}

	/*
	 * Check the file here too, although aropaper would say the same:
	 * a toast naming the path beats "exited with status 1".
	 */
	char *file = NULL;
	if (w->mode == Q_WALLPAPER_FILE) {
		file = expand_home(w->file);
		if (!file || access(file, R_OK) != 0) {
			report(w, "wallpaper: cannot read %s: %s",
			       file ? file : w->file, strerror(errno));
			free(file);
			free(bin);
			return;
		}
	}

	struct wp_child *c = calloc(1, sizeof *c);
	if (!c) {
		free(file);
		free(bin);
		return;
	}
	c->w = w;
	c->pidfd = -1;

	pid_t pid = fork();
	if (pid < 0) {
		report(w, "wallpaper: fork failed: %s", strerror(errno));
		free(c);
		free(file);
		free(bin);
		return;
	}
	if (pid == 0) {
		/*
		 * A signal blocked or ignored here would survive the exec, and
		 * an aropaper that ignores SIGTERM cannot be stopped on reload.
		 */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		/* auto: no argument, aropaper picks aro's own art */
		char *argv[] = { "aropaper", file, NULL };
		execv(bin, argv);
		_exit(127);
	}
	free(file);
	free(bin);

	c->pid = pid;
	c->pidfd = pidfd_open_(pid);
	if (c->pidfd < 0) {
		/* nothing to watch it with: better no wallpaper than an orphan */
		report(w, "wallpaper: cannot watch aropaper (pidfd_open: %s)",
		       strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		free(c);
		return;
	}
	c->source = wl_event_loop_add_fd(w->loop, c->pidfd, WL_EVENT_READABLE,
	                                 child_exited, c);
	if (!c->source) {
		pidfd_kill(c->pidfd, SIGKILL);
		waitpid(pid, NULL, 0);
		close(c->pidfd);
		free(c);
		return;
	}
	wl_list_insert(&w->children, &c->link);
	w->current = c;
	wlr_log(WLR_INFO, "wallpaper: started aropaper (pid %d)", pid);
}

/* asks the running one to go; it is reaped when it has */
static void stop(struct aro_wallpaper *w)
{
	struct wp_child *c = w->current;
	if (!c)
		return;
	c->stopping = true;
	pidfd_kill(c->pidfd, SIGTERM);
	w->current = NULL;
}

void wallpaper_init(struct aro_wallpaper *w, struct wl_event_loop *loop,
                    void (*report_fn)(void *data, const char *msg), void *data)
{
	memset(w, 0, sizeof *w);
	w->loop = loop;
	w->report = report_fn;
	w->data = data;
	w->mode = Q_WALLPAPER_NONE;     /* nothing running is "none" */
	wl_list_init(&w->children);
}

void wallpaper_apply(struct aro_wallpaper *w, const struct aro_config *c)
{
	enum q_wallpaper mode = c->wallpaper;
	const char *file = mode == Q_WALLPAPER_FILE ? c->wallpaper_file : NULL;

	bool same = mode == w->mode &&
	            (mode != Q_WALLPAPER_FILE ||
	             (w->file && file && !strcmp(w->file, file)));
	if (same && (w->current || mode == Q_WALLPAPER_NONE))
		return;

	stop(w);
	free(w->file);
	w->file = NULL;
	w->mode = mode;
	if (file) {
		w->file = strdup(file);
		if (!w->file) {
			w->mode = Q_WALLPAPER_NONE;
			return;
		}
	}
	if (mode != Q_WALLPAPER_NONE)
		start(w);
}

void wallpaper_finish(struct aro_wallpaper *w)
{
	/*
	 * Not waited for: aro is on its way out, and an aropaper that
	 * missed the signal exits anyway when its connection closes.
	 */
	struct wp_child *c, *tmp;
	wl_list_for_each_safe(c, tmp, &w->children, link) {
		if (!c->stopping)
			pidfd_kill(c->pidfd, SIGTERM);
		child_free(c);
	}
	free(w->file);
	w->file = NULL;
	w->current = NULL;
}
