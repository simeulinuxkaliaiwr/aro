/*
 * aroctl: ask a running aro something, or tell it to do something.
 *
 *     aroctl windows
 *     aroctl -j monitors | jq '.[].name'
 *     aroctl dispatch workspace 3
 *     aroctl dispatch spawn foot --server
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

	char pathbuf[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (!sock)
		sock = default_socket(pathbuf, sizeof pathbuf);
	if (!sock) {
		fprintf(stderr, "aroctl: ARO_SOCKET is not set; is aro running? "
		        "(or pass -s PATH)\n");
		free(req);
		return 1;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(sock) >= sizeof addr.sun_path) {
		fprintf(stderr, "aroctl: socket path too long: %s\n", sock);
		free(req);
		return 1;
	}
	strcpy(addr.sun_path, sock);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "aroctl: cannot reach aro at %s: %s\n",
		        sock, strerror(errno));
		if (fd >= 0)
			close(fd);
		free(req);
		return 1;
	}

	bool sent = send_all(fd, req, strlen(req));
	free(req);
	if (!sent) {
		fprintf(stderr, "aroctl: could not send the request: %s\n",
		        strerror(errno));
		close(fd);
		return 1;
	}
	shutdown(fd, SHUT_WR);

	size_t n = 0;
	char *reply = recv_all(fd, &n);
	close(fd);
	if (!reply) {
		fprintf(stderr, "aroctl: could not read the reply\n");
		return 1;
	}

	char *body = strchr(reply, '\n');
	if (body)
		*body++ = '\0';
	else
		body = reply + n;       /* a status line with no newline */

	int status;
	if (!strcmp(reply, "ok")) {
		fputs(body, stdout);
		status = 0;
	} else if (!strncmp(reply, "error", 5) &&
	           (reply[5] == ' ' || reply[5] == '\0')) {
		fprintf(stderr, "aroctl: %s\n",
		        reply[5] ? reply + 6 : "aro reported an error");
		status = 1;
	} else if (n == 0) {
		fprintf(stderr, "aroctl: aro closed the connection without a reply\n");
		status = 1;
	} else {
		fprintf(stderr, "aroctl: unexpected reply: %s\n", reply);
		status = 1;
	}
	free(reply);

	if (fflush(stdout) != 0)
		status = 1;
	return status;
}
