/* picker.c: the wallpaper picker, a strip of wallpapers to choose from */
#define _GNU_SOURCE /* memfd_create, strcasestr */

/* scene.h must come first */
#include "scene.h"

#include "picker.h"
#include "aro.h"
#include "logfile.h"
#include "theme.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <pango/pango.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#ifndef ARO_WALLPAPER_DIR
#define ARO_WALLPAPER_DIR "/usr/share/backgrounds/aro"
#endif

/*
 * The picker only lists and draws. Decoding a 6000px JPEG on the
 * compositor's thread would stall every screen, so aropaper makes the
 * thumbnails in its own process, nearest the selection first, and the
 * picker loads each small PNG once it lands, a couple per frame.
 * Thumbnails are cached by path, size and mtime, so a second opening
 * shows everything at once.
 */

#define PK_MAX_ITEMS 5000
#define PK_DEPTH     3          /* folders inside a wallpaper folder */
#define PK_LOADS     2          /* thumbnails decoded per frame */
#define PK_THUMB_VER "1"        /* bump when aropaper's thumbnails change */

static const anim_ease PK_EASE = {
	TH_EASE_FLAT_X1, TH_EASE_FLAT_Y1, TH_EASE_FLAT_X2, TH_EASE_FLAT_Y2,
};

/* ── small helpers ─────────────────────────────────────────────────────── */

static int max_i(int a, int b) { return a > b ? a : b; }

static double lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

static struct wlr_scene_rect *rect(struct wlr_scene_tree *parent, uint32_t rgba)
{
	float c[4];
	ui_color(rgba, c);
	return wlr_scene_rect_create(parent, 1, 1, c);
}

/* rgba, faded by a */
static void rect_color(struct wlr_scene_rect *r, uint32_t rgba, double a)
{
	float c[4];
	ui_color(rgba, c);
	for (int i = 0; i < 4; i++)
		c[i] *= (float)a;
	wlr_scene_rect_set_color(r, c);
}

static void rect_place(struct wlr_scene_rect *r, int x, int y, int w, int h)
{
	wlr_scene_node_set_position(&r->node, x, y);
	wlr_scene_rect_set_size(r, max_i(w, 1), max_i(h, 1));
}

static void set_radius(struct wlr_scene_rect *r, int radius)
{
#ifdef ARO_EFFECTS
	wlr_scene_rect_set_corner_radius(r, radius > 0 ? radius : 0);
#else
	(void)r;
	(void)radius;
#endif
}

static void schedule_all(struct aro_server *s)
{
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link)
		wlr_output_schedule_frame(o->wlr_output);
}

/* cards are rounder than frames: they are much bigger */
static int card_radius(const struct q_theme *th)
{
	return th->radius > 0 ? th->radius * 2 : 0;
}

/* the theme's font, half as big again and heavier, for the name */
static char *title_font(const char *font)
{
	PangoFontDescription *d = pango_font_description_from_string(font);
	int size = pango_font_description_get_size(d);
	if (size <= 0)
		size = 10 * PANGO_SCALE;
	if (pango_font_description_get_size_is_absolute(d))
		pango_font_description_set_absolute_size(d, size * 3.0 / 2);
	else
		pango_font_description_set_size(d, size * 3 / 2);
	pango_font_description_set_weight(d, PANGO_WEIGHT_SEMIBOLD);
	char *str = pango_font_description_to_string(d);
	char *out = strdup(str);
	g_free(str);
	pango_font_description_free(d);
	return out;
}

/* ── finding wallpapers ────────────────────────────────────────────────── */

static bool is_image(const char *name)
{
	static const char *exts[] = {
		"png", "jpg", "jpeg", "webp", "svg", "gif", "bmp",
		"tif", "tiff", "avif", "jxl", "heic",
	};
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name)
		return false;
	for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
		if (!strcasecmp(dot + 1, exts[i]))
			return true;
	return false;
}

struct found {
	char *path;
	char *key;              /* folder and name without extension or size */
	long score;             /* the best of one key wins */
	time_t mtime;
	long mtime_ns;
	off_t size;
};

struct scan {
	struct found *f;
	int n, cap;
};

/*
 * "aro-wallpaper-1920x1080.png", "…-3840x2160.png" and "aro-wallpaper.svg"
 * are one wallpaper: the name without the size is the key, and the SVG,
 * else the biggest, is the one listed.
 */
static void key_of(const char *path, struct found *f)
{
	f->key = strdup(path);
	if (!f->key)
		return;
	char *slash = strrchr(f->key, '/');
	char *dot = strrchr(f->key, '.');
	bool svg = dot && !strcasecmp(dot, ".svg");
	if (dot && dot > slash)
		*dot = '\0';

	f->score = svg ? LONG_MAX : 0;
	char *dash = strrchr(f->key, '-');
	if (!dash || dash < slash)
		dash = strrchr(f->key, '_');
	if (!dash || dash < slash)
		return;
	long w = 0, h = 0;
	int used = 0;
	if (sscanf(dash + 1, "%ldx%ld%n", &w, &h, &used) == 2 && !dash[1 + used] &&
	    w > 0 && h > 0) {
		*dash = '\0';
		if (!svg)
			f->score = w * h;
	}
}

static void scan_dir(struct scan *sc, const char *dir, int depth)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) && sc->n < PK_MAX_ITEMS) {
		if (e->d_name[0] == '.')
			continue;
		char *path = NULL;
		if (asprintf(&path, "%s/%s", dir, e->d_name) < 0)
			continue;
		struct stat st;
		/* folders are not followed through links: no loops */
		if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			if (depth > 0)
				scan_dir(sc, path, depth - 1);
			free(path);
			continue;
		}
		if (!is_image(e->d_name) || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			free(path);
			continue;
		}
		if (sc->n == sc->cap) {
			int cap = sc->cap ? sc->cap * 2 : 64;
			struct found *f = realloc(sc->f, cap * sizeof *f);
			if (!f) {
				free(path);
				break;
			}
			sc->f = f;
			sc->cap = cap;
		}
		struct found *f = &sc->f[sc->n];
		*f = (struct found){
			.path = path, .mtime = st.st_mtim.tv_sec,
			.mtime_ns = st.st_mtim.tv_nsec, .size = st.st_size,
		};
		key_of(path, f);
		if (!f->key) {
			free(path);
			continue;
		}
		sc->n++;
	}
	closedir(d);
}

static int by_path(const void *a, const void *b)
{
	return strcmp(((const struct found *)a)->path, ((const struct found *)b)->path);
}

static int by_key(const void *a, const void *b)
{
	const struct found *x = a, *y = b;
	int c = strcmp(x->key, y->key);
	if (c)
		return c;
	return x->score > y->score ? -1 : x->score < y->score;
}

static int by_name(const void *a, const void *b)
{
	const struct pk_item *x = a, *y = b;
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->path, y->path);
}

/* the cache name: which file, and which version of it */
static char *thumb_path(const char *cache, const struct found *f)
{
	uint64_t h = 1469598103934665603ull;
	char tail[96];
	snprintf(tail, sizeof tail, "\n%lld.%ld\n%lld\n" PK_THUMB_VER,
	         (long long)f->mtime, f->mtime_ns, (long long)f->size);
	for (const char *p = f->path; *p; p++)
		h = (h ^ (unsigned char)*p) * 1099511628211ull;
	for (const char *p = tail; *p; p++)
		h = (h ^ (unsigned char)*p) * 1099511628211ull;
	char *out = NULL;
	if (asprintf(&out, "%s/%016llx.png", cache, (unsigned long long)h) < 0)
		return NULL;
	return out;
}

static void found_free(struct scan *sc)
{
	for (int i = 0; i < sc->n; i++) {
		free(sc->f[i].path);
		free(sc->f[i].key);
	}
	free(sc->f);
	*sc = (struct scan){ 0 };
}

static void scan_root(struct scan *sc, const char *dir)
{
	char *home = wallpaper_expand_home(dir);
	char *real = home ? realpath(home, NULL) : NULL;
	if (real)
		scan_dir(sc, real, PK_DEPTH);
	free(real);
	free(home);
}

static void items_free(struct aro_picker *k)
{
	for (int i = 0; i < k->n; i++) {
		struct pk_item *it = &k->items[i];
		if (it->buf)
			wlr_buffer_drop(it->buf);
		free(it->path);
		free(it->name);
		free(it->thumb);
	}
	free(k->items);
	free(k->view);
	k->items = NULL;
	k->view = NULL;
	k->n = k->nview = 0;
}

static bool items_load(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	const struct aro_config *c = &s->cfg;
	struct scan sc = { 0 };
	if (c->nwallpaper_dirs > 0) {
		for (int i = 0; i < c->nwallpaper_dirs; i++)
			scan_root(&sc, c->wallpaper_dirs[i]);
	} else {
		/* a wallpaper folder if there is one: ~/Pictures has screenshots too */
		static const char *dirs[] = {
			"~/Pictures/Wallpapers", "~/Pictures/wallpapers", "~/Pictures",
		};
		for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
			char *d = wallpaper_expand_home(dirs[i]);
			struct stat st;
			bool there = d && stat(d, &st) == 0 && S_ISDIR(st.st_mode);
			free(d);
			if (there) {
				scan_root(&sc, dirs[i]);
				break;
			}
		}
		scan_root(&sc, "~/.local/share/backgrounds");
	}
	scan_root(&sc, ARO_WALLPAPER_DIR);

	/* a folder listed twice, or inside another: once each */
	qsort(sc.f, sc.n, sizeof *sc.f, by_path);
	int m = 0;
	for (int i = 0; i < sc.n; i++) {
		if (m && !strcmp(sc.f[m - 1].path, sc.f[i].path)) {
			free(sc.f[i].path);
			free(sc.f[i].key);
			continue;
		}
		sc.f[m++] = sc.f[i];
	}
	sc.n = m;

	/* sizes of one wallpaper: the best only */
	qsort(sc.f, sc.n, sizeof *sc.f, by_key);
	k->items = calloc(max_i(sc.n, 1), sizeof *k->items);
	k->view = calloc(max_i(sc.n, 1), sizeof *k->view);
	if (!k->items || !k->view) {
		found_free(&sc);
		items_free(k);
		return false;
	}
	for (int i = 0; i < sc.n; i++) {
		if (i && !strcmp(sc.f[i - 1].key, sc.f[i].key))
			continue;
		struct pk_item *it = &k->items[k->n];
		const char *slash = strrchr(sc.f[i].key, '/');
		it->path = strdup(sc.f[i].path);
		it->name = strdup(slash ? slash + 1 : sc.f[i].key);
		it->thumb = thumb_path(k->cache, &sc.f[i]);
		if (!it->path || !it->name || !it->thumb) {
			free(it->path);
			free(it->name);
			free(it->thumb);
			continue;
		}
		it->state = access(it->thumb, R_OK) == 0 ? PK_READY : PK_WAIT;
		it->vi = -1;
		k->n++;
	}
	found_free(&sc);
	qsort(k->items, k->n, sizeof *k->items, by_name);
	return true;
}

/* the item on screen now, by path; -1 if it is not in the list */
static int find_current(struct aro_server *s)
{
	enum q_wallpaper mode = s->wallpaper_set ? s->wallpaper_mode : s->cfg.wallpaper;
	const char *file = s->wallpaper_set ? s->wallpaper_file : s->cfg.wallpaper_file;
	char *want = NULL;
	if (mode == Q_WALLPAPER_AUTO)
		want = strdup(ARO_WALLPAPER_DIR "/aro-wallpaper.svg");
	else if (mode == Q_WALLPAPER_FILE && file)
		want = wallpaper_expand_home(file);
	char *real = want ? realpath(want, NULL) : NULL;
	free(want);
	int at = -1;
	for (int i = 0; real && i < s->picker.n && at < 0; i++)
		if (!strcmp(s->picker.items[i].path, real))
			at = i;
	free(real);
	return at;
}

/* ── the thumbnailer ───────────────────────────────────────────────────── */

static int pidfd_open_(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}

static void thumbs_start(struct aro_server *s);
static void view_filter(struct aro_server *s);

static struct pk_item *item_by_thumb(struct aro_picker *k, const char *thumb)
{
	for (int i = 0; i < k->n; i++)
		if (!strcmp(k->items[i].thumb, thumb))
			return &k->items[i];
	return NULL;
}

static void thumbs_line(struct aro_server *s, char *line)
{
	struct aro_picker *k = &s->picker;
	bool failed = line[0] == '!';
	struct pk_item *it = item_by_thumb(k, line + failed);
	if (!it || it->state != PK_WAIT)
		return;
	it->state = failed ? PK_FAILED : PK_READY;
	if (failed && it->vi >= 0)
		view_filter(s);         /* one that cannot be read is left out */
	if (k->shown)
		schedule_all(s);
}

static void thumbs_close_out(struct pk_thumbnailer *t)
{
	if (t->out_src)
		wl_event_source_remove(t->out_src);
	if (t->out >= 0)
		close(t->out);
	t->out_src = NULL;
	t->out = -1;
	t->len = 0;
}

static int thumbs_readable(int fd, uint32_t mask, void *data)
{
	struct aro_server *s = data;
	struct pk_thumbnailer *t = &s->picker.thumbs;
	for (;;) {
		if (t->len == sizeof t->buf)
			t->len = 0;     /* a line longer than PATH_MAX: not ours */
		ssize_t n = read(fd, t->buf + t->len, sizeof t->buf - t->len);
		if (n > 0) {
			t->len += n;
			char *start = t->buf, *nl;
			while ((nl = memchr(start, '\n', t->buf + t->len - start))) {
				*nl = '\0';
				thumbs_line(s, start);
				start = nl + 1;
			}
			t->len -= start - t->buf;
			memmove(t->buf, start, t->len);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
			return 0;
		thumbs_close_out(t);    /* EOF: it is exiting */
		return 0;
	}
}

static int thumbs_exited(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct aro_server *s = data;
	struct aro_picker *k = &s->picker;
	struct pk_thumbnailer *t = &k->thumbs;

	/* the last lines, if it wrote any since the last read */
	if (t->out >= 0)
		thumbs_readable(t->out, WL_EVENT_HANGUP, s);
	int status = 0;
	bool clean = waitpid(t->pid, &status, WNOHANG) > 0 &&
	             WIFEXITED(status) && WEXITSTATUS(status) == 0;
	thumbs_close_out(t);
	wl_event_source_remove(t->exit_src);
	close(t->pidfd);
	t->exit_src = NULL;
	t->pidfd = -1;
	t->pid = 0;

	/*
	 * Still waiting on this run: a clean exit skipped them, so they
	 * fail; a crash blames the one it was on, and the rest go again.
	 */
	bool blamed = clean, refilter = false;
	for (int i = 0; i < k->n; i++) {
		struct pk_item *it = &k->items[i];
		if (it->state != PK_WAIT || it->run != t->run)
			continue;
		if (!blamed) {
			wlr_log(WLR_ERROR, "picker: aropaper stopped on %s", it->path);
			blamed = true;
			it->state = PK_FAILED;
			refilter |= it->vi >= 0;
		} else if (clean) {
			it->state = PK_FAILED;
			refilter |= it->vi >= 0;
		} else {
			it->run = 0;
		}
	}
	if (refilter)
		view_filter(s);
	if (k->open)
		thumbs_start(s);        /* what came in while it ran */
	return 0;
}

/* the visible ones first, outward from the selection, then the rest */
static int thumbs_order(struct aro_picker *k, int *order)
{
	int m = 0;
	for (int d = 0; d < k->nview; d++) {
		int at[2] = { k->sel + d, k->sel - d };
		for (int j = 0; j < (d ? 2 : 1); j++)
			if (at[j] >= 0 && at[j] < k->nview)
				order[m++] = k->view[at[j]];
	}
	for (int i = 0; i < k->n; i++)
		if (k->items[i].vi < 0)
			order[m++] = i;
	return m;
}

static void thumbs_start(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	struct pk_thumbnailer *t = &k->thumbs;
	if (t->pid || !k->cache || k->n == 0)
		return;

	int *order = calloc(k->n, sizeof *order);
	if (!order)
		return;
	int m = thumbs_order(k, order);

	/* the list goes in as a file: no pipe to keep fed */
	int list = memfd_create("aro-thumbnails", MFD_CLOEXEC);
	FILE *f = list >= 0 ? fdopen(dup(list), "w") : NULL;
	int want = 0, run = ++k->next_run;
	for (int i = 0; f && i < m; i++) {
		struct pk_item *it = &k->items[order[i]];
		if (it->state != PK_WAIT || it->run)
			continue;
		fprintf(f, "%s\t%s\n", it->path, it->thumb);
		it->run = run;
		want++;
	}
	free(order);
	bool listed = f && fclose(f) == 0;
	char *bin = want && listed ? wallpaper_find_aropaper() : NULL;
	int out[2] = { -1, -1 };
	if (!bin || lseek(list, 0, SEEK_SET) != 0 || pipe2(out, O_CLOEXEC) < 0) {
		if (want && !bin)
			wlr_log(WLR_ERROR, "picker: aropaper is not installed; no thumbnails");
		goto undo;
	}

	pid_t pid = fork();
	if (pid < 0)
		goto undo;
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		dup2(list, STDIN_FILENO);
		dup2(out[1], STDOUT_FILENO);
		setpriority(PRIO_PROCESS, 0, 10);       /* behind whatever the user is doing */
		execl(bin, "aropaper", "--thumbnails", (char *)NULL);
		_exit(127);
	}
	close(out[1]);
	close(list);
	free(bin);

	t->pid = pid;
	t->run = run;
	t->pidfd = pidfd_open_(pid);
	t->out = out[0];
	fcntl(t->out, F_SETFL, fcntl(t->out, F_GETFL) | O_NONBLOCK);
	t->out_src = wl_event_loop_add_fd(s->loop, t->out, WL_EVENT_READABLE,
	                                  thumbs_readable, s);
	t->exit_src = t->pidfd >= 0
		? wl_event_loop_add_fd(s->loop, t->pidfd, WL_EVENT_READABLE, thumbs_exited, s)
		: NULL;
	if (!t->exit_src) {
		/* nothing to reap it with: better no thumbnails than a zombie */
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		thumbs_close_out(t);
		if (t->pidfd >= 0)
			close(t->pidfd);
		t->pidfd = -1;
		t->pid = 0;
		for (int i = 0; i < k->n; i++)
			if (k->items[i].run == run)
				k->items[i].state = PK_FAILED;
		view_filter(s);
	}
	return;

undo:
	if (out[0] >= 0) {
		close(out[0]);
		close(out[1]);
	}
	if (list >= 0)
		close(list);
	free(bin);
	for (int i = 0; i < k->n; i++) {
		if (k->items[i].run == run) {
			k->items[i].run = 0;
			if (want)
				k->items[i].state = PK_FAILED;
		}
	}
	if (want)
		view_filter(s);
}

static void thumbs_stop(struct aro_picker *k)
{
	struct pk_thumbnailer *t = &k->thumbs;
	thumbs_close_out(t);
	if (t->exit_src)
		wl_event_source_remove(t->exit_src);
	if (t->pid) {
		kill(t->pid, SIGTERM);
		waitpid(t->pid, NULL, WNOHANG);
	}
	if (t->pidfd >= 0)
		close(t->pidfd);
	t->exit_src = NULL;
	t->pidfd = -1;
	t->pid = 0;
}

/* ── the view: what matches the query ──────────────────────────────────── */

static void texts_update(struct aro_server *s);

static void view_filter(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	int keep = k->sel >= 0 && k->sel < k->nview ? k->view[k->sel] : k->current;

	for (int i = 0; i < k->n; i++)
		k->items[i].vi = -1;
	k->nview = 0;
	for (int i = 0; i < k->n; i++) {
		struct pk_item *it = &k->items[i];
		if (it->state == PK_FAILED || (k->query[0] && !strcasestr(it->name, k->query)))
			continue;
		it->vi = k->nview;
		k->view[k->nview++] = i;
	}
	/* out of the view: no reason to hold its pixels */
	for (int i = 0; i < k->n; i++) {
		if (k->items[i].vi < 0 && k->items[i].buf) {
			wlr_buffer_drop(k->items[i].buf);
			k->items[i].buf = NULL;
		}
	}

	int sel = keep >= 0 && keep < k->n ? k->items[keep].vi : -1;
	k->sel = sel >= 0 ? sel : 0;
	k->pos = k->pos_from = k->pos_to = k->sel;
	if (k->tree)
		texts_update(s);
	schedule_all(s);
}

static void select_at(struct aro_server *s, int sel)
{
	struct aro_picker *k = &s->picker;
	if (k->nview == 0)
		return;
	sel = sel < 0 ? 0 : sel >= k->nview ? k->nview - 1 : sel;
	if (sel == k->sel)
		return;
	k->sel = sel;
	k->pos_from = k->pos;
	k->pos_to = sel;
	k->pos_start = aro_now_ms();

	/* far from the selection: dropped, loaded again on the way back */
	for (int i = 0; i < k->nview; i++) {
		struct pk_item *it = &k->items[k->view[i]];
		if (it->buf && abs(i - sel) > PK_SLOTS) {
			wlr_buffer_drop(it->buf);
			it->buf = NULL;
		}
	}
	texts_update(s);
	schedule_all(s);
}

/* ── drawing ───────────────────────────────────────────────────────────── */

/* a card t places from the middle: its size and how solid it is */
static double card_scale(double t)
{
	return t < 1 ? 1 - 0.3 * t : fmax(0.45, 0.7 - 0.1 * (t - 1));
}

static double card_alpha(double t)
{
	return t < 1 ? 1 - 0.35 * t : fmax(0, 0.65 - 0.22 * (t - 1));
}

/* how far from the middle its centre is: each pair of neighbours a gap apart */
static double card_offset(double t, double w, int gap)
{
	double x = 0;
	int i = 0;
	for (; i + 1 <= t; i++)
		x += w * (card_scale(i) + card_scale(i + 1)) / 2 + gap;
	return x + (t - i) * (w * (card_scale(i) + card_scale(i + 1)) / 2 + gap);
}

/* the middle card: the screen's own shape, as the wallpaper will be cut */
static void card_size(struct aro_picker *k, double *w, double *h)
{
	ly_box ob = k->output->box;
	double aspect = ob.w > 0 && ob.h > 0 ? (double)ob.w / ob.h : 16.0 / 9;
	*w = ob.w * 0.42;
	*h = *w / aspect;
	if (*h > ob.h * 0.46) {
		*h = ob.h * 0.46;
		*w = *h * aspect;
	}
}

static const char *home_short(const char *path, char *buf, size_t len)
{
	const char *home = getenv("HOME");
	size_t n = home ? strlen(home) : 0;
	if (n > 1 && !strncmp(path, home, n) && path[n] == '/') {
		snprintf(buf, len, "~%s", path + n);
		return buf;
	}
	return path;
}

static void texts_update(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	const struct q_theme *th = &s->cfg.theme;
	const float scale = k->output->scale;
	const int max_w = k->output->box.w * 8 / 10;

	char info[PATH_MAX + 96], dir[PATH_MAX], shortdir[PATH_MAX];
	if (k->nview > 0) {
		const int at = k->view[k->sel];
		const struct pk_item *it = &k->items[at];
		snprintf(dir, sizeof dir, "%s", it->path);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
		snprintf(info, sizeof info, "%d of %d   ·   %s%s", k->sel + 1, k->nview,
		         home_short(dir, shortdir, sizeof shortdir),
		         at == k->current ? "   ·   current" : "");
		qtext_set(&k->name, it->name, th->ink, scale, max_w);
		qtext_set(&k->info, info, th->dim, scale, max_w);
	} else if (k->query[0]) {
		qtext_set(&k->name, "Nothing matches", th->ink, scale, max_w);
		qtext_set(&k->info, "backspace to widen the search", th->dim, scale, max_w);
	} else {
		qtext_set(&k->name, "No wallpapers found", th->ink, scale, max_w);
		qtext_set(&k->info, "add some to ~/Pictures/Wallpapers, or set wallpaper_dir",
		          th->dim, scale, max_w);
	}

	char q[PK_QUERY_MAX + 8];
	snprintf(q, sizeof q, "%s", k->query[0] ? k->query : "type to search");
	qtext_set(&k->search, q, k->query[0] ? th->accent : th->dim, scale, max_w);
	qtext_set(&k->hint,
	          "←  →  browse      enter  apply      ctrl+r  random      esc  close",
	          th->dim, scale, max_w);
}

static void text_fade(struct qtext *t, double a)
{
	if (t->node)
		wlr_scene_buffer_set_opacity(t->node, (float)a);
}

static void card_hide(struct pk_card *c)
{
	wlr_scene_node_set_enabled(&c->tree->node, false);
	c->vi = -1;
}

/* the picture, cut to the card's shape the way aropaper covers a screen */
static void card_image(struct pk_card *c, struct wlr_buffer *buf, ly_box b,
                       double a, int radius)
{
	if (!buf || b.w < 1 || b.h < 1) {
		wlr_scene_node_set_enabled(&c->img->node, false);
		return;
	}
	if (c->img->buffer != buf)
		wlr_scene_buffer_set_buffer(c->img, buf);
	double bw = buf->width, bh = buf->height;
	double want = (double)b.w / b.h;
	struct wlr_fbox src = { 0, 0, bw, bh };
	if (bw / bh > want) {
		src.width = bh * want;
		src.x = (bw - src.width) / 2;
	} else {
		src.height = bw / want;
		src.y = (bh - src.height) / 2;
	}
	wlr_scene_buffer_set_source_box(c->img, &src);
	wlr_scene_buffer_set_dest_size(c->img, b.w, b.h);
	wlr_scene_node_set_position(&c->img->node, b.x, b.y);
	wlr_scene_buffer_set_opacity(c->img, (float)a);
#ifdef ARO_EFFECTS
	wlr_scene_buffer_set_corner_radius(c->img, radius);
#else
	(void)radius;
#endif
	wlr_scene_node_set_enabled(&c->img->node, true);
}

/* true while thumbnails near the selection are still to be decoded */
static bool cards_place(struct aro_server *s, double p)
{
	struct aro_picker *k = &s->picker;
	const struct q_theme *th = &s->cfg.theme;
	const ly_box ob = k->output->box;
	const int gap = th->gap * 3, bw = th->border;
	const int r = card_radius(th), ir = max_i(r - bw, 0);
	int loads = PK_LOADS;
	bool pending = false, refilter = false;

	double cw, ch;
	card_size(k, &cw, &ch);
	const double cx = ob.x + ob.w / 2.0;
	const double cy = ob.y + ob.h * 0.46 + (1 - p) * ob.h * 0.05;

	const int base = (int)floor(k->pos) - PK_SLOTS / 2;
	struct pk_card *front = NULL;
	double front_t = 1e9;
	for (int j = 0; j < PK_SLOTS; j++) {
		struct pk_card *c = &k->cards[j];
		const int vi = base + j;
		if (vi < 0 || vi >= k->nview) {
			card_hide(c);
			continue;
		}
		const double d = vi - k->pos, t = fabs(d);
		const double sc = card_scale(t), a = card_alpha(t) * p;
		const double w = cw * sc, h = ch * sc;
		const double x = cx + (d < 0 ? -1 : 1) * card_offset(t, cw, gap);
		const ly_box b = { (int)lround(x - w / 2), (int)lround(cy - h / 2),
		                   (int)lround(w), (int)lround(h) };
		if (a < 0.01 || b.x + b.w <= ob.x || b.x >= ob.x + ob.w) {
			card_hide(c);
			continue;
		}
		c->vi = vi;
		c->box = b;
		wlr_scene_node_set_enabled(&c->tree->node, true);

		struct pk_item *it = &k->items[k->view[vi]];
		if (!it->buf && it->state == PK_READY) {
			if (loads > 0) {
				loads--;
				it->buf = ui_png_load(it->thumb);
				if (!it->buf) {
					it->state = PK_FAILED;
					refilter = true;
				}
			} else {
				pending = true;
			}
		}

		const bool sel = vi == k->sel;
		rect_color(c->edge, sel ? th->accent : th->line, a);
		rect_color(c->bg, th->frame, a);
		rect_place(c->edge, b.x, b.y, b.w, b.h);
		const ly_box in = { b.x + bw, b.y + bw, b.w - bw * 2, b.h - bw * 2 };
		rect_place(c->bg, in.x, in.y, in.w, in.h);
		set_radius(c->edge, r);
		set_radius(c->bg, ir);
		card_image(c, it->buf, in, a, ir);

		if (t < front_t) {
			front_t = t;
			front = c;
		}
	}
	/* in the middle of a slide, the nearer card is the one on top */
	if (front)
		wlr_scene_node_raise_to_top(&front->tree->node);

	if (refilter)
		view_filter(s);         /* a thumbnail that would not load */

	/* the words, around the middle card */
	/* nothing to show: the message takes the middle */
	const int ny = k->nview ? (int)(cy + ch / 2) + gap : (int)cy - k->name.h;
	qtext_move(&k->name, (int)(cx - k->name.w / 2.0), ny);
	qtext_move(&k->info, (int)(cx - k->info.w / 2.0), ny + k->name.h + th->text_pad / 2);
	qtext_move(&k->search, (int)(cx - k->search.w / 2.0),
	           (int)(cy - ch / 2) - gap - k->search.h);
	qtext_move(&k->hint, (int)(cx - k->hint.w / 2.0),
	           ob.y + ob.h - gap - k->hint.h);
	text_fade(&k->name, p);
	text_fade(&k->info, p);
	text_fade(&k->search, p);
	text_fade(&k->hint, p * 0.8);
	return pending;
}

static void backdrop_place(struct aro_server *s, double p)
{
	struct aro_picker *k = &s->picker;
	const struct q_theme *th = &s->cfg.theme;

	/* the scrim dims every screen; the picker is on one */
	int x0 = k->output->box.x, y0 = k->output->box.y;
	int x1 = x0 + k->output->box.w, y1 = y0 + k->output->box.h;
	struct aro_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->box.x < x0) x0 = o->box.x;
		if (o->box.y < y0) y0 = o->box.y;
		if (o->box.x + o->box.w > x1) x1 = o->box.x + o->box.w;
		if (o->box.y + o->box.h > y1) y1 = o->box.y + o->box.h;
	}
	rect_place(k->scrim, x0, y0, x1 - x0, y1 - y0);
	rect_color(k->scrim, th->scrim, p);
#ifdef ARO_EFFECTS
	if (k->blur) {
		const ly_box ob = k->output->box;
		wlr_scene_node_set_position(&k->blur->node, ob.x, ob.y);
		wlr_scene_blur_set_size(k->blur, ob.w, ob.h);
		wlr_scene_blur_set_strength(k->blur, (float)p);
		wlr_scene_blur_set_alpha(k->blur, (float)p);
	}
#endif
}

static double advance(double from, double to, uint32_t start, uint32_t now,
                      int ms, bool *moving)
{
	if (from == to)
		return to;
	uint32_t el = now - start;
	if (ms <= 0 || el >= (uint32_t)ms)
		return to;
	*moving = true;
	return lerp(from, to, anim_ease_eval(&PK_EASE, (double)el / ms));
}

/* ── building and tearing down ─────────────────────────────────────────── */

static void scene_free(struct aro_picker *k)
{
	if (k->tree) {
		qtext_finish(&k->name);
		qtext_finish(&k->info);
		qtext_finish(&k->search);
		qtext_finish(&k->hint);
		wlr_scene_node_destroy(&k->tree->node);
	}
	k->tree = k->strip = NULL;
	k->blur = NULL;
	k->scrim = NULL;
	memset(k->cards, 0, sizeof k->cards);
	memset(&k->name, 0, sizeof k->name);
	memset(&k->info, 0, sizeof k->info);
	memset(&k->search, 0, sizeof k->search);
	memset(&k->hint, 0, sizeof k->hint);
	free(k->title_font);
	k->title_font = NULL;
}

static bool scene_build(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	const struct q_theme *th = &s->cfg.theme;

	k->tree = wlr_scene_tree_create(s->l_notify);
	if (!k->tree)
		return false;
	wlr_scene_node_raise_to_top(&k->tree->node);
#ifdef ARO_EFFECTS
	k->blur = wlr_scene_blur_create(k->tree, 1, 1);
#endif
	k->scrim = rect(k->tree, th->scrim);
	k->strip = wlr_scene_tree_create(k->tree);
	k->title_font = title_font(th->font);
	if (!k->scrim || !k->strip || !k->title_font)
		goto fail;

	for (int j = 0; j < PK_SLOTS; j++) {
		struct pk_card *c = &k->cards[j];
		c->tree = wlr_scene_tree_create(k->strip);
		if (!c->tree)
			goto fail;
		c->edge = rect(c->tree, th->line);
		c->bg = rect(c->tree, th->frame);
		c->img = wlr_scene_buffer_create(c->tree, NULL);
		if (!c->edge || !c->bg || !c->img)
			goto fail;
		c->vi = -1;
	}
	if (!qtext_init(&k->name, k->tree, k->title_font) ||
	    !qtext_init(&k->info, k->tree, th->font_small) ||
	    !qtext_init(&k->search, k->tree, th->font) ||
	    !qtext_init(&k->hint, k->tree, th->font_small))
		goto fail;
	texts_update(s);
	return true;

fail:
	wlr_log(WLR_ERROR, "could not build the wallpaper picker");
	scene_free(k);
	return false;
}

/* gone for good: after the closing fade, or at once */
static void picker_teardown(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	scene_free(k);
	items_free(k);
	k->open = k->shown = false;
	k->output = NULL;
	k->p = k->p_from = k->p_to = 0;
	schedule_all(s);
}

static void picker_close(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	if (!k->open)
		return;
	k->open = false;
	k->p_from = k->p;
	k->p_to = 0;
	k->p_start = aro_now_ms();
	if (s->cfg.theme.overview_ms <= 0)
		picker_teardown(s);
	else
		schedule_all(s);
}

static void picker_open(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	if (k->shown)
		picker_teardown(s);     /* still fading out: start over */
	k->output = aro_focused_output(s);
	if (!k->output || !s->l_notify)
		return;
	if (!k->cache)
		k->cache = cache_dir("thumbnails");
	if (!items_load(s))
		return;
	k->query[0] = '\0';
	k->current = find_current(s);
	k->sel = -1;
	k->nview = 0;
	k->pressed = -1;
	k->scroll = 0;
	if (!scene_build(s)) {
		items_free(k);
		return;
	}
	view_filter(s);
	k->open = k->shown = true;
	k->p = k->p_from = 0;
	k->p_to = 1;
	k->p_start = aro_now_ms();
	thumbs_start(s);
	schedule_all(s);
}

void picker_init(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	k->thumbs.pidfd = k->thumbs.out = -1;
	k->current = -1;
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

void picker_toggle(struct aro_server *s)
{
	if (s->picker.open)
		picker_close(s);
	else if (!aro_locked(s) && !prompt_active(s) && !overview_shown(s) &&
	         !switcher_active(s))
		picker_open(s);
}

bool picker_active(struct aro_server *s)
{
	return s->picker.open;
}

bool picker_tick(struct aro_server *s, uint32_t now)
{
	struct aro_picker *k = &s->picker;
	if (!k->shown)
		return false;
	if (k->open && aro_locked(s))
		picker_close(s);

	const int ms = s->cfg.theme.overview_ms;
	bool moving = false;
	k->p = advance(k->p_from, k->p_to, k->p_start, now, ms, &moving);
	k->pos = advance(k->pos_from, k->pos_to, k->pos_start, now, ms, &moving);
	if (!k->open && k->p <= 0) {
		picker_teardown(s);
		return false;
	}
	backdrop_place(s, k->p);
	if (cards_place(s, k->p))
		moving = true;
	return moving;
}

/* ── choosing ──────────────────────────────────────────────────────────── */

static void apply(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	if (k->nview == 0)
		return;
	const int at = k->view[k->sel];
	if (at != k->current && !aro_wallpaper_set(s, Q_WALLPAPER_FILE, k->items[at].path))
		return;
	k->current = at;
	picker_close(s);
}

static void random_pick(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	if (k->nview < 2)
		return;
	int to = rand() % (k->nview - 1);
	select_at(s, to >= k->sel ? to + 1 : to);
}

static void query_changed(struct aro_server *s)
{
	view_filter(s);
	/* the new first few are the ones to make next */
	struct aro_picker *k = &s->picker;
	if (!k->thumbs.pid)
		thumbs_start(s);
}

void picker_key(struct aro_server *s, uint32_t mods, xkb_keysym_t raw,
                xkb_keysym_t typed)
{
	struct aro_picker *k = &s->picker;
	if (!k->open)
		return;

	const uint32_t care = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
	                      WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	mods &= care;
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct q_bind *b = &s->cfg.binds[i];
		if (b->sym != raw || b->mods != mods || !mods)
			continue;
		if (b->action == Q_WALLPAPERS) {
			picker_close(s);
			return;
		}
		if (b->action == Q_FOCUS && (b->num == LY_LEFT || b->num == LY_RIGHT)) {
			select_at(s, k->sel + (b->num == LY_LEFT ? -1 : 1));
			return;
		}
	}

	const bool ctrl = mods & WLR_MODIFIER_CTRL;
	size_t len = strlen(k->query);
	switch (raw) {
	case XKB_KEY_Escape:
		if (len) {
			k->query[0] = '\0';
			query_changed(s);
		} else {
			picker_close(s);
		}
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		apply(s);
		return;
	case XKB_KEY_Left:
	case XKB_KEY_Up:
		select_at(s, k->sel - 1);
		return;
	case XKB_KEY_Right:
	case XKB_KEY_Down:
		select_at(s, k->sel + 1);
		return;
	case XKB_KEY_Tab:
		select_at(s, k->sel + (mods & WLR_MODIFIER_SHIFT ? -1 : 1));
		return;
	case XKB_KEY_ISO_Left_Tab:
		select_at(s, k->sel - 1);
		return;
	case XKB_KEY_Page_Up:
		select_at(s, k->sel - 5);
		return;
	case XKB_KEY_Page_Down:
		select_at(s, k->sel + 5);
		return;
	case XKB_KEY_Home:
		select_at(s, 0);
		return;
	case XKB_KEY_End:
		select_at(s, k->nview - 1);
		return;
	case XKB_KEY_BackSpace:
		if (!len)
			return;
		if (ctrl) {
			k->query[0] = '\0';
		} else {
			/* one character, not one byte */
			while (len > 0 && ((unsigned char)k->query[len - 1] & 0xc0) == 0x80)
				len--;
			k->query[len > 0 ? len - 1 : 0] = '\0';
		}
		query_changed(s);
		return;
	default:
		break;
	}
	if (ctrl && (raw == XKB_KEY_r || raw == XKB_KEY_R)) {
		random_pick(s);
		return;
	}
	if (ctrl && raw == XKB_KEY_u) {
		k->query[0] = '\0';
		query_changed(s);
		return;
	}
	if (mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))
		return;

	char utf8[8];
	uint32_t cp = xkb_keysym_to_utf32(typed);
	if (cp < 0x20 || cp == 0x7f || xkb_keysym_to_utf8(typed, utf8, sizeof utf8) <= 1)
		return;
	if (len + strlen(utf8) < sizeof k->query) {
		strcat(k->query, utf8);
		query_changed(s);
	}
}

static int card_at(struct aro_picker *k, double lx, double ly)
{
	/* the middle one is on top, so it wins where they touch */
	int hit = -1;
	double best = 1e9;
	for (int j = 0; j < PK_SLOTS; j++) {
		const struct pk_card *c = &k->cards[j];
		if (c->vi < 0 || lx < c->box.x || lx >= c->box.x + c->box.w ||
		    ly < c->box.y || ly >= c->box.y + c->box.h)
			continue;
		double t = fabs(c->vi - k->pos);
		if (t < best) {
			best = t;
			hit = c->vi;
		}
	}
	return hit;
}

void picker_pointer_motion(struct aro_server *s, double lx, double ly)
{
	(void)s;
	(void)lx;
	(void)ly;
}

/* a click picks a card, a click on the picked one applies it; a click away closes */
void picker_pointer_button(struct aro_server *s, double lx, double ly, bool pressed)
{
	struct aro_picker *k = &s->picker;
	if (!k->open)
		return;
	const int at = card_at(k, lx, ly);
	if (pressed) {
		k->pressed = at >= 0 ? at : -2;
		return;
	}
	const int was = k->pressed;
	k->pressed = -1;
	if (was >= 0 && was == at) {
		if (at == k->sel)
			apply(s);
		else
			select_at(s, at);
	} else if (was == -2 && at < 0) {
		picker_close(s);
	}
}

void picker_pointer_axis(struct aro_server *s, double delta, int discrete)
{
	struct aro_picker *k = &s->picker;
	if (!k->open)
		return;
	if (discrete) {
		select_at(s, k->sel + (discrete > 0 ? 1 : -1));
		return;
	}
	/* a touchpad: a step per so much travel */
	k->scroll += delta;
	while (fabs(k->scroll) >= 30) {
		select_at(s, k->sel + (k->scroll > 0 ? 1 : -1));
		k->scroll -= k->scroll > 0 ? 30 : -30;
	}
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

/* fonts are borrowed from the config a reload just freed: rebuild */
void picker_retheme(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	if (!k->shown)
		return;
	scene_free(k);
	if (!scene_build(s)) {
		items_free(k);
		k->open = k->shown = false;
		k->output = NULL;
	}
	schedule_all(s);
}

void picker_output_gone(struct aro_server *s, struct aro_output *o)
{
	if (s->picker.shown && s->picker.output == o)
		picker_teardown(s);
}

void picker_finish(struct aro_server *s)
{
	struct aro_picker *k = &s->picker;
	if (k->shown)
		picker_teardown(s);
	thumbs_stop(k);
	free(k->cache);
	k->cache = NULL;
}
