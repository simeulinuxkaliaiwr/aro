/*
 * aroctl: ask a running aro something, or tell it to do something.
 *
 *     aroctl windows
 *     aroctl -j monitors | jq '.[].name'
 *     aroctl dispatch workspace 3
 *     aroctl dispatch spawn foot --server
 *     aroctl log -f
 *
 * One request per connection: `text|json <command> [args]\n`. The reply's
 * first line is `ok` or `error <message>`; the body follows. No wlroots,
 * no libwayland: this is a socket and a line of text.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef ARO_VERSION
#define ARO_VERSION "unknown"
#endif

static void usage(FILE *f)
{
	fprintf(f,
	        "usage: aroctl [-j] [-s socket] <command> [args]\n"
	        "\n"
	        "commands:\n"
	        "  monitors            outputs, their modes and workspaces (alias: outputs)\n"
	        "  workspaces          workspaces in use or shown on the bar\n"
	        "  windows             every window, * marks the focused one\n"
	        "  focused             the focused window; fails if there is none\n"
	        "  reload              re-read the config; prints any problems\n"
	        "  dispatch ACTION     run an action, written as in a bind line:\n"
	        "                        dispatch focus left\n"
	        "                        dispatch workspace 3\n"
	        "                        dispatch spawn foot\n"
	        "  version             aro's version\n"
	        "  log [-f] [--old] [--path]\n"
	        "                      print aro's log; -f follows it, --old is the\n"
	        "                      previous session's (the one that crashed),\n"
	        "                      --path only says where it is. With aro not\n"
	        "                      running, reads ~/.local/state/aro/aro.log\n"
	        "\n"
	        "options:\n"
	        "  -j, --json          JSON instead of text\n"
	        "  -s, --socket PATH   default: $ARO_SOCKET\n"
	        "  -h, --help          this\n"
	        "      --version       aroctl's own version\n");
}

/* $ARO_SOCKET, or the path aro would have made from our WAYLAND_DISPLAY */
static const char *default_socket(char *buf, size_t size)
{
	const char *env = getenv("ARO_SOCKET");
	if (env && *env)
		return env;

	const char *rt = getenv("XDG_RUNTIME_DIR");
	const char *wl = getenv("WAYLAND_DISPLAY");
	if (!rt || !*rt || !wl || !*wl)
		return NULL;
	int n = snprintf(buf, size, "%s/aro-%s.sock", rt, wl);
	return n > 0 && (size_t)n < size ? buf : NULL;
}

static bool send_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		n -= (size_t)w;
	}
	return true;
}

/* read until the server closes; NUL-terminated, length in *len */
static char *recv_all(int fd, size_t *len)
{
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	for (;;) {
		if (n + 1 >= cap) {
			char *nb = realloc(buf, cap * 2);
			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
			cap *= 2;
		}
		ssize_t r = read(fd, buf + n, cap - n - 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			free(buf);
			return NULL;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	buf[n] = '\0';
	*len = n;
	return buf;
}

/* a connected socket, or -1 with errno set */
static int open_socket(const char *sock)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(sock) >= sizeof addr.sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(addr.sun_path, sock);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		int err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	return fd;
}

/* send req, read reply; 0 on ok, body points into reply */
static int exchange(int fd, const char *req, char **reply, const char **body)
{
	*reply = NULL;
	*body = NULL;

	if (!send_all(fd, req, strlen(req))) {
		fprintf(stderr, "aroctl: could not send the request: %s\n",
		        strerror(errno));
		close(fd);
		return 1;
	}
	shutdown(fd, SHUT_WR);

	size_t n = 0;
	char *r = recv_all(fd, &n);
	close(fd);
	if (!r) {
		fprintf(stderr, "aroctl: could not read the reply\n");
		return 1;
	}
	*reply = r;

	char *b = strchr(r, '\n');
	if (b)
		*b++ = '\0';
	else
		b = r + n;      /* a status line with no newline */

	if (!strcmp(r, "ok")) {
		*body = b;
		return 0;
	}
	if (!strncmp(r, "error", 5) && (r[5] == ' ' || r[5] == '\0'))
		fprintf(stderr, "aroctl: %s\n",
		        r[5] ? r + 6 : "aro reported an error");
	else if (n == 0)
		fprintf(stderr, "aroctl: aro closed the connection without a reply\n");
	else
		fprintf(stderr, "aroctl: unexpected reply: %s\n", r);
	return 1;
}

/* ── aroctl log ────────────────────────────────────────────────────────── */

/* same rule as logfile.c */
static bool log_default(char *buf, size_t size)
{
	const char *state = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	int n;
	if (state && state[0] == '/')
		n = snprintf(buf, size, "%s/aro/aro.log", state);
	else if (home && home[0] == '/')
		n = snprintf(buf, size, "%s/.local/state/aro/aro.log", home);
	else
		return false;
	return n > 0 && (size_t)n < size;
}

static int log_print(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "aroctl: cannot read %s: %s\n", path,
		        strerror(errno));
		return 1;
	}
	char buf[65536];
	size_t r;
	int status = 0;
	while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
		if (fwrite(buf, 1, r, stdout) != r) {
			status = 1;     /* closed pipe */
			break;
		}
	}
	if (ferror(f)) {
		fprintf(stderr, "aroctl: error reading %s\n", path);
		status = 1;
	}
	fclose(f);
	if (fflush(stdout) != 0)
		status = 1;
	return status;
}

/* -F follows the name, across restarts */
static int log_follow(const char *path)
{
	fflush(stdout);
	execlp("tail", "tail", "-n", "20", "-F", path, (char *)NULL);
	fprintf(stderr, "aroctl: cannot run tail: %s\n", strerror(errno));
	return 1;
}

/* JSON string */
static void json_str(const char *s)
{
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 0x20 || *p == 0x7f)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

/* ask aro for the path; if it is not running, fall back to aro.log */
static int do_log(const char *sock, bool sock_given, bool json,
                  int argc, char *argv[], int i)
{
	bool follow = false, old = false, path_only = false;
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-f") || !strcmp(a, "--follow"))
			follow = true;
		else if (!strcmp(a, "--old"))
			old = true;
		else if (!strcmp(a, "--path"))
			path_only = true;
		else {
			fprintf(stderr, "aroctl: log: unknown option %s\n", a);
			return 2;
		}
	}
	if (follow && path_only) {
		fprintf(stderr, "aroctl: log: -f and --path do not go together\n");
		return 2;
	}

	char path[4096];
	int fd = sock ? open_socket(sock) : -1;
	if (!sock)
		errno = ENOENT;

	if (fd >= 0) {
		char *reply;
		const char *body;
		if (exchange(fd, "text log\n", &reply, &body)) {
			free(reply);
			return 1;
		}
		size_t n = strcspn(body, "\n");
		if (n == 0 || n >= sizeof path) {
			fprintf(stderr, "aroctl: aro sent no usable log path\n");
			free(reply);
			return 1;
		}
		memcpy(path, body, n);
		path[n] = '\0';
		free(reply);
	} else if (!sock_given && (errno == ENOENT || errno == ECONNREFUSED)) {
		if (!log_default(path, sizeof path)) {
			fprintf(stderr, "aroctl: aro is not running, and neither "
			        "XDG_STATE_HOME nor HOME says where its log is\n");
			return 1;
		}
	} else {
		fprintf(stderr, "aroctl: cannot reach aro at %s: %s\n",
		        sock, strerror(errno));
		return 1;
	}

	if (old) {
		if (strlen(path) + 4 >= sizeof path) {
			fprintf(stderr, "aroctl: log path too long\n");
			return 1;
		}
		strcat(path, ".old");
	}
	if (fd < 0)
		fprintf(stderr, "aroctl: aro is not running here; reading %s\n",
		        path);

	if (path_only) {
		if (json) {
			fputs("{\"path\":", stdout);
			json_str(path);
			fputs("}\n", stdout);
		} else {
			puts(path);
		}
		return fflush(stdout) != 0;
	}
	return follow ? log_follow(path) : log_print(path);
}

int main(int argc, char *argv[])
{
	bool json = false;
	const char *sock = NULL;
	int i = 1;

	/* options come first; everything from the command on is the request,
	 * so `dispatch spawn foot -e htop` keeps its -e */
	for (; i < argc && argv[i][0] == '-'; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-j") || !strcmp(a, "--json")) {
			json = true;
		} else if (!strcmp(a, "-s") || !strcmp(a, "--socket")) {
			if (++i >= argc) {
				fprintf(stderr, "aroctl: %s needs a path\n", a);
				return 2;
			}
			sock = argv[i];
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "--version")) {
			printf("aroctl %s\n", ARO_VERSION);
			return 0;
		} else if (!strcmp(a, "--")) {
			i++;
			break;
		} else {
			fprintf(stderr, "aroctl: unknown option %s\n", a);
			usage(stderr);
			return 2;
		}
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}

	char pathbuf[sizeof(((struct sockaddr_un *)0)->sun_path)];
	const bool sock_given = sock != NULL;
	if (!sock)
		sock = default_socket(pathbuf, sizeof pathbuf);

	/* handled here, not by aro */
	if (!strcmp(argv[i], "log"))
		return do_log(sock, sock_given, json, argc, argv, i + 1);

	/* the request line */
	size_t len = strlen(json ? "json" : "text") + 1;
	for (int j = i; j < argc; j++)
		len += strlen(argv[j]) + 1;
	char *req = malloc(len + 1);
	if (!req) {
		fprintf(stderr, "aroctl: out of memory\n");
		return 1;
	}
	strcpy(req, json ? "json" : "text");
	for (int j = i; j < argc; j++) {
		if (strchr(argv[j], '\n')) {
			fprintf(stderr, "aroctl: arguments cannot contain newlines\n");
			free(req);
			return 2;
		}
		strcat(req, " ");
		strcat(req, argv[j]);
	}
	strcat(req, "\n");

	if (!sock) {
		fprintf(stderr, "aroctl: ARO_SOCKET is not set; is aro running? "
		        "(or pass -s PATH)\n");
		free(req);
		return 1;
	}

	int fd = open_socket(sock);
	if (fd < 0) {
		fprintf(stderr, "aroctl: cannot reach aro at %s: %s\n",
		        sock, strerror(errno));
		free(req);
		return 1;
	}

	char *reply = NULL;
	const char *body = NULL;
	int status = exchange(fd, req, &reply, &body);
	free(req);
	if (status == 0)
		fputs(body, stdout);
	free(reply);

	if (fflush(stdout) != 0)
		status = 1;
	return status;
}
