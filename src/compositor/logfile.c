/* logfile.c: aro's log, to stderr and to a file */
/* realpath needs XSI; 700 also gives strdup, fdopen, setenv */
#define _XOPEN_SOURCE 700

#include "logfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *log_file;
static char *log_path;
static struct timespec log_start;
static bool log_color;          /* stderr is a terminal */

static const char *const level_tag[WLR_LOG_IMPORTANCE_LAST] = {
	[WLR_SILENT] = "",
	[WLR_ERROR] = "[ERROR]",
	[WLR_INFO] = "[INFO]",
	[WLR_DEBUG] = "[DEBUG]",
};

static const char *const level_color[WLR_LOG_IMPORTANCE_LAST] = {
	[WLR_SILENT] = "",
	[WLR_ERROR] = "\x1b[1;31m",
	[WLR_INFO] = "\x1b[1;34m",
	[WLR_DEBUG] = "\x1b[1;90m",
};

/* colour on stderr only */
static void log_cb(enum wlr_log_importance importance, const char *fmt,
                   va_list args)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (long)(now.tv_sec - log_start.tv_sec) * 1000 +
	          (now.tv_nsec - log_start.tv_nsec) / 1000000;
	if (ms < 0)
		ms = 0;

	char stamp[32];
	snprintf(stamp, sizeof stamp, "%02ld:%02ld:%02ld.%03ld",
	         ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);

	unsigned i = (unsigned)importance < WLR_LOG_IMPORTANCE_LAST
	             ? (unsigned)importance : WLR_DEBUG;

	/* one va_list per sink */
	va_list copy;
	va_copy(copy, args);

	if (log_color)
		fprintf(stderr, "%s %s%s ", stamp, level_color[i], level_tag[i]);
	else
		fprintf(stderr, "%s %s ", stamp, level_tag[i]);
	vfprintf(stderr, fmt, args);
	fputs(log_color ? "\x1b[0m\n" : "\n", stderr);

	if (log_file) {
		fprintf(log_file, "%s %s ", stamp, level_tag[i]);
		vfprintf(log_file, fmt, copy);
		fputc('\n', log_file);
	}
	va_end(copy);
}

/* mkdir -p, 0700 */
static bool mkdir_p(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0700) < 0 && errno != EEXIST) {
			*p = '/';
			return false;
		}
		*p = '/';
	}
	return mkdir(path, 0700) == 0 || errno == EEXIST;
}

/* $XDG_STATE_HOME/aro, else ~/.local/state/aro; NULL if neither */
static char *default_path(bool nested)
{
	const char *state = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	const char *file = nested ? "aro-nested.log" : "aro.log";
	char dir[4096];
	int n;

	/* relative XDG_* is ignored */
	if (state && state[0] == '/')
		n = snprintf(dir, sizeof dir, "%s/aro", state);
	else if (home && home[0] == '/')
		n = snprintf(dir, sizeof dir, "%s/.local/state/aro", home);
	else
		return NULL;
	if (n < 0 || (size_t)n >= sizeof dir || !mkdir_p(dir))
		return NULL;

	char *path = malloc(strlen(dir) + strlen(file) + 2);
	if (path)
		sprintf(path, "%s/%s", dir, file);
	return path;
}

/* keep the previous log as .old */
static void rotate(const char *path)
{
	char *old = malloc(strlen(path) + 5);
	if (!old)
		return;
	sprintf(old, "%s.old", path);
	rename(path, old);
	free(old);
}

void logfile_init(const char *path, bool nested,
                  enum wlr_log_importance verbosity)
{
	clock_gettime(CLOCK_MONOTONIC, &log_start);
	log_color = isatty(STDERR_FILENO);

	const char *problem = NULL;
	int err = 0;

	if (path && !strcmp(path, "none")) {
		log_path = NULL;
	} else {
		log_path = path ? strdup(path) : default_path(nested);
		if (!log_path) {
			problem = "no log directory: neither XDG_STATE_HOME nor HOME "
			          "gave one aro could create";
		} else {
			rotate(log_path);
			/* no symlinks */
			int fd = open(log_path,
			              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
			              O_NOFOLLOW, 0600);
			if (fd >= 0) {
				log_file = fdopen(fd, "w");
				if (!log_file)
					close(fd);
			}
			if (!log_file) {
				err = errno;
				problem = "cannot write the log";
			} else {
				/* line-buffered for crashes */
				setvbuf(log_file, NULL, _IOLBF, 0);
				/* absolute, for aroctl */
				char *abs = realpath(log_path, NULL);
				if (abs) {
					free(log_path);
					log_path = abs;
				}
			}
		}
	}

	wlr_log_init(verbosity, log_cb);

	if (log_file) {
		setenv("ARO_LOG", log_path, 1);
		time_t t = time(NULL);
		struct tm tm;
		char when[32] = "?";
		if (localtime_r(&t, &tm))
			strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
		wlr_log(WLR_INFO, "log: %s, started %s%s", log_path, when,
		        nested ? " (nested)" : "");
	} else {
		unsetenv("ARO_LOG");
		if (problem)
			wlr_log(WLR_ERROR, "log: %s%s%s%s%s", problem,
			        log_path ? " " : "", log_path ? log_path : "",
			        err ? ": " : "", err ? strerror(err) : "");
		free(log_path);
		log_path = NULL;
	}
}

const char *logfile_path(void)
{
	return log_path;
}

void logfile_finish(void)
{
	if (log_file)
		fclose(log_file);
	log_file = NULL;
	free(log_path);
	log_path = NULL;
}
