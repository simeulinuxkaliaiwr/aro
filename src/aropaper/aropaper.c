/*
 * aropaper — aro's wallpaper.
 *
 * One background layer surface per output, each drawn once at that
 * output's exact pixel size and then left alone. It is a plain Wayland
 * client: no wlroots, and it works on any compositor with wlr-layer-shell
 * (sway, Hyprland, labwc…), not only on aro.
 *
 *   aropaper [FILE]
 *
 * FILE is anything gdk-pixbuf can load: PNG, JPEG, WebP, and SVG through
 * librsvg's loader. With no FILE, aro's own wallpaper: the SVG if an SVG
 * loader is installed, else the 4K PNG next to it.
 *
 * The image covers the screen (like swaybg's "fill"): scaled to the
 * smaller size that leaves no gap, centred, the overflow cropped. An SVG
 * is rendered straight at that size rather than scaled after the fact, so
 * it is sharp on any screen.
 *
 * Nothing is kept once a frame is on screen. The buffer is destroyed right
 * after the commit that shows it — allowed by the protocol as long as its
 * storage is never touched again, which it is not — and the compositor
 * keeps the pixels. Idle, aropaper holds no image memory at all; a new
 * size or scale decodes the file again.
 */
#define _GNU_SOURCE /* memfd_create */

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef ARO_WALLPAPER_DIR
#define ARO_WALLPAPER_DIR "/usr/share/backgrounds/aro"
#endif
#ifndef ARO_VERSION
#define ARO_VERSION "unknown"
#endif

/* tried in order when no file is given */
static const char *default_files[] = {
	ARO_WALLPAPER_DIR "/aro-wallpaper.svg",
	ARO_WALLPAPER_DIR "/aro-wallpaper-3840x2160.png",
};

#define LOG(...) fprintf(stderr, "aropaper: " __VA_ARGS__)

struct paper {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_viewporter *viewporter;
	struct wp_fractional_scale_manager_v1 *frac_mgr;
	struct wl_list outputs;
	bool ready; /* initial globals are in; new outputs get a surface at once */

	const char *path;
	int img_w, img_h; /* intrinsic size: an SVG's own width and height */
};

struct output {
	struct paper *p;
	struct wl_list link;
	uint32_t global;         /* registry name, for global_remove */
	struct wl_output *wl;
	char *name;              /* "HDMI-A-1", for the log only */
	int32_t scale;           /* wl_output.scale: an integer, rounded up */

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *frac;

	uint32_t w, h;           /* logical size, from the last configure */
	uint32_t scale120;       /* preferred fractional scale; 0 = not told yet */
	bool configured;
	bool dirty;              /* draw after this batch of events */

	/* what is on screen, so an identical configure draws nothing */
	uint32_t drawn_w, drawn_h;
	int32_t drawn_scale;
};

/* === pixels === */

/*
 * Decode the file at a size that covers pw x ph and copy the centre into
 * a fresh shm buffer. Returns NULL (having said why) on failure.
 */
static struct wl_buffer *paint(struct paper *p, uint32_t pw, uint32_t ph)
{
	uint64_t bytes = (uint64_t)pw * ph * 4;
	if (bytes > INT32_MAX) { /* wl_shm pools are sized in int32 */
		LOG("%ux%u is too large for a shm buffer\n", pw, ph);
		return NULL;
	}

	double sx = (double)pw / p->img_w, sy = (double)ph / p->img_h;
	double s = sx > sy ? sx : sy;
	int tw = (int)ceil(p->img_w * s), th = (int)ceil(p->img_h * s);
	if (tw < (int)pw)
		tw = (int)pw;
	if (th < (int)ph)
		th = (int)ph;

	/*
	 * preserve_aspect is FALSE on purpose: with TRUE gdk-pixbuf fits the
	 * image INSIDE the box and may round one side a pixel short of the
	 * screen. tw x th is already the right aspect to within that pixel.
	 */
	GError *err = NULL;
	GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(p->path, tw, th,
		FALSE, &err);
	if (!pb) {
		LOG("%s: %s\n", p->path, err ? err->message : "cannot load");
		g_clear_error(&err);
		return NULL;
	}

	int bw = gdk_pixbuf_get_width(pb), bh = gdk_pixbuf_get_height(pb);
	int nch = gdk_pixbuf_get_n_channels(pb);
	int rs = gdk_pixbuf_get_rowstride(pb);
	bool alpha = gdk_pixbuf_get_has_alpha(pb);
	const guchar *src = gdk_pixbuf_read_pixels(pb);
	if (gdk_pixbuf_get_bits_per_sample(pb) != 8 || nch < 3) {
		LOG("%s: unsupported pixel format\n", p->path);
		g_object_unref(pb);
		return NULL;
	}
	/* a loader is free to hand back another size; crop what we got */
	int ox = bw > (int)pw ? (bw - (int)pw) / 2 : 0;
	int oy = bh > (int)ph ? (bh - (int)ph) / 2 : 0;

	int fd = memfd_create("aropaper", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)bytes) < 0) {
		LOG("shm: %s\n", strerror(errno));
		if (fd >= 0)
			close(fd);
		g_object_unref(pb);
		return NULL;
	}
	uint32_t *dst = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if (dst == MAP_FAILED) {
		LOG("mmap: %s\n", strerror(errno));
		close(fd);
		g_object_unref(pb);
		return NULL;
	}

	/*
	 * XRGB8888: every compositor must support it. A translucent image is
	 * flattened onto black — there is nothing behind a wallpaper to show.
	 * Pixels outside the decoded image (a short loader) stay black too;
	 * ftruncate zero-filled them.
	 */
	for (uint32_t y = 0; y < ph && (int)y + oy < bh; y++) {
		const guchar *row = src + (size_t)(y + oy) * rs + (size_t)ox * nch;
		uint32_t *out = dst + (size_t)y * pw;
		for (uint32_t x = 0; x < pw && (int)x + ox < bw; x++) {
			const guchar *px = row + (size_t)x * nch;
			uint32_t r = px[0], g = px[1], b = px[2];
			if (alpha) {
				uint32_t a = px[3];
				r = (r * a + 127) / 255;
				g = (g * a + 127) / 255;
				b = (b * a + 127) / 255;
			}
			out[x] = r << 16 | g << 8 | b;
		}
	}
	g_object_unref(pb);
	munmap(dst, bytes);

	struct wl_shm_pool *pool = wl_shm_create_pool(p->shm, fd, (int32_t)bytes);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)pw,
		(int32_t)ph, (int32_t)pw * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd); /* the compositor has its own reference now */
	return buf;
}

/*
 * Put a frame on screen for the output's current size and scale. Draws
 * only when the pixel size or buffer scale changed; always commits, so an
 * acked configure is applied either way.
 */
static void draw(struct output *o)
{
	struct paper *p = o->p;
	if (!o->configured || !o->w || !o->h)
		return;

	int32_t iscale = o->scale > 0 ? o->scale : 1;
	uint32_t pw, ph;
	int32_t bscale;
	if (o->viewport) {
		/*
		 * Fractional: a buffer of exactly the output's pixels, scaled to
		 * the surface by the viewport. Until the compositor says which
		 * fraction, the integer scale stands in (one redraw at startup).
		 * Rounding is half away from zero, as the protocol specifies.
		 */
		uint32_t s120 = o->scale120 ? o->scale120 : (uint32_t)iscale * 120;
		pw = (o->w * s120 + 60) / 120;
		ph = (o->h * s120 + 60) / 120;
		bscale = 1;
		wp_viewport_set_destination(o->viewport, (int32_t)o->w,
			(int32_t)o->h);
	} else {
		pw = o->w * (uint32_t)iscale;
		ph = o->h * (uint32_t)iscale;
		bscale = iscale;
	}

	if (pw != o->drawn_w || ph != o->drawn_h || bscale != o->drawn_scale) {
		struct wl_buffer *buf = paint(p, pw, ph);
		if (buf) {
			wl_surface_set_buffer_scale(o->surface, bscale);
			wl_surface_attach(o->surface, buf, 0, 0);
			wl_surface_damage_buffer(o->surface, 0, 0, INT32_MAX,
				INT32_MAX);
			wl_surface_commit(o->surface);
			/* see the top of the file: the pixels outlive the object */
			wl_buffer_destroy(buf);
			o->drawn_w = pw;
			o->drawn_h = ph;
			o->drawn_scale = bscale;
			LOG("%s: %ux%u px\n", o->name ? o->name : "output", pw, ph);
			return;
		}
	}
	wl_surface_commit(o->surface);
}

/* === surfaces === */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
	uint32_t serial, uint32_t w, uint32_t h)
{
	struct output *o = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	o->w = w;
	o->h = h;
	o->configured = true;
	o->dirty = true;
}

static void surface_teardown(struct output *o)
{
	if (o->frac)
		wp_fractional_scale_v1_destroy(o->frac);
	if (o->viewport)
		wp_viewport_destroy(o->viewport);
	if (o->layer)
		zwlr_layer_surface_v1_destroy(o->layer);
	if (o->surface)
		wl_surface_destroy(o->surface);
	o->frac = NULL;
	o->viewport = NULL;
	o->layer = NULL;
	o->surface = NULL;
	o->configured = false;
	o->drawn_w = o->drawn_h = 0;
	o->drawn_scale = 0;
}

/*
 * The compositor withdrew the surface. When the screen itself is going,
 * the output global is removed as well and the rest is freed there; a
 * surface closed on a screen that stays is simply not replaced.
 */
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	(void)layer;
	surface_teardown(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void frac_preferred(void *data, struct wp_fractional_scale_v1 *frac,
	uint32_t scale120)
{
	(void)frac;
	struct output *o = data;
	if (o->scale120 == scale120)
		return;
	o->scale120 = scale120;
	o->dirty = true;
}

static const struct wp_fractional_scale_v1_listener frac_listener = {
	.preferred_scale = frac_preferred,
};

static void surface_setup(struct output *o)
{
	struct paper *p = o->p;
	if (o->surface)
		return;

	o->surface = wl_compositor_create_surface(p->compositor);

	/* clicks on the desktop are the compositor's business, not ours */
	struct wl_region *none = wl_compositor_create_region(p->compositor);
	wl_surface_set_input_region(o->surface, none);
	wl_region_destroy(none);

	/* fractional scale is only usable with a viewport to map it back */
	if (p->viewporter) {
		o->viewport = wp_viewporter_get_viewport(p->viewporter, o->surface);
		if (p->frac_mgr) {
			o->frac = wp_fractional_scale_manager_v1_get_fractional_scale(
				p->frac_mgr, o->surface);
			wp_fractional_scale_v1_add_listener(o->frac, &frac_listener, o);
		}
	}

	o->layer = zwlr_layer_shell_v1_get_layer_surface(p->layer_shell,
		o->surface, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		"wallpaper");
	zwlr_layer_surface_v1_set_anchor(o->layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	/* -1: the whole screen, ignoring every panel's exclusive zone */
	zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
	zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
	wl_surface_commit(o->surface); /* asks for the first configure */
}

/* === outputs === */

static void output_geometry(void *data, struct wl_output *wl, int32_t x,
	int32_t y, int32_t pw, int32_t ph, int32_t subpixel, const char *make,
	const char *model, int32_t transform)
{
	(void)data; (void)wl; (void)x; (void)y; (void)pw; (void)ph;
	(void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *wl, uint32_t flags,
	int32_t w, int32_t h, int32_t refresh)
{
	(void)data; (void)wl; (void)flags; (void)w; (void)h; (void)refresh;
}

static void output_done(void *data, struct wl_output *wl)
{
	(void)wl;
	struct output *o = data;
	o->dirty = true; /* an integer scale change lands here */
}

static void output_scale(void *data, struct wl_output *wl, int32_t factor)
{
	(void)wl;
	struct output *o = data;
	o->scale = factor;
}

static void output_name(void *data, struct wl_output *wl, const char *name)
{
	(void)wl;
	struct output *o = data;
	free(o->name);
	o->name = strdup(name);
}

static void output_description(void *data, struct wl_output *wl,
	const char *desc)
{
	(void)data; (void)wl; (void)desc;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void output_destroy(struct output *o)
{
	surface_teardown(o);
	if (wl_output_get_version(o->wl) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
		wl_output_release(o->wl);
	else
		wl_output_destroy(o->wl);
	wl_list_remove(&o->link);
	free(o->name);
	free(o);
}

/* === registry === */

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static void registry_global(void *data, struct wl_registry *reg,
	uint32_t name, const char *iface, uint32_t version)
{
	struct paper *p = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		/* 4: damage_buffer */
		if (version < 4)
			return;
		p->compositor = wl_registry_bind(reg, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		p->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		p->layer_shell = wl_registry_bind(reg, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		p->viewporter = wl_registry_bind(reg, name,
			&wp_viewporter_interface, 1);
	} else if (strcmp(iface,
			wp_fractional_scale_manager_v1_interface.name) == 0) {
		p->frac_mgr = wl_registry_bind(reg, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		struct output *o = calloc(1, sizeof(*o));
		if (!o)
			return;
		o->p = p;
		o->global = name;
		o->scale = 1;
		o->wl = wl_registry_bind(reg, name, &wl_output_interface,
			min_u32(version, 4));
		wl_output_add_listener(o->wl, &output_listener, o);
		wl_list_insert(p->outputs.prev, &o->link);
		if (p->ready)
			surface_setup(o);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg,
	uint32_t name)
{
	(void)reg;
	struct paper *p = data;
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &p->outputs, link) {
		if (o->global == name) {
			output_destroy(o);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* === main === */

static void usage(FILE *f)
{
	fprintf(f,
		"usage: aropaper [FILE]\n"
		"\n"
		"Draws FILE (PNG, JPEG, SVG, …) as the wallpaper on every screen,\n"
		"covering it and cropping the overflow. With no FILE, aro's own\n"
		"wallpaper from %s.\n", ARO_WALLPAPER_DIR);
}

/* true when gdk-pixbuf can read the file; stores its intrinsic size */
static bool probe(struct paper *p, const char *path)
{
	int w = 0, h = 0;
	if (!gdk_pixbuf_get_file_info(path, &w, &h) || w <= 0 || h <= 0)
		return false;
	p->path = path;
	p->img_w = w;
	p->img_h = h;
	return true;
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'v'},
		{0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "hv", longopts, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage(stdout);
			return 0;
		case 'v':
			printf("aropaper %s\n", ARO_VERSION);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}
	if (argc - optind > 1) {
		usage(stderr);
		return 1;
	}

	struct paper p = {0};
	wl_list_init(&p.outputs);

	if (optind < argc) {
		/* gdk-pixbuf says "can't read" for a missing file too; say what is wrong */
		if (access(argv[optind], R_OK) != 0) {
			LOG("%s: %s\n", argv[optind], strerror(errno));
			return 1;
		}
		if (!probe(&p, argv[optind])) {
			LOG("%s: not an image gdk-pixbuf can read\n", argv[optind]);
			return 1;
		}
	} else {
		size_t n = sizeof(default_files) / sizeof(default_files[0]);
		for (size_t i = 0; i < n && !p.path; i++)
			probe(&p, default_files[i]);
		if (!p.path) {
			LOG("no default wallpaper in %s; give a file instead\n",
				ARO_WALLPAPER_DIR);
			return 1;
		}
	}
	LOG("%s (%dx%d)\n", p.path, p.img_w, p.img_h);

	p.display = wl_display_connect(NULL);
	if (!p.display) {
		LOG("cannot connect to a Wayland compositor\n");
		return 1;
	}
	p.registry = wl_display_get_registry(p.display);
	wl_registry_add_listener(p.registry, &registry_listener, &p);
	wl_display_roundtrip(p.display);

	const char *missing = !p.compositor ? "wl_compositor v4"
		: !p.shm ? "wl_shm"
		: !p.layer_shell ? "zwlr_layer_shell_v1 (layer shell)"
		: NULL;
	if (missing) {
		LOG("the compositor does not offer %s\n", missing);
		return 1;
	}

	/* outputs can be announced before layer shell, so surfaces wait until
	 * every initial global is in */
	p.ready = true;
	struct output *o, *tmp;
	wl_list_for_each(o, &p.outputs, link)
		surface_setup(o);

	/*
	 * Handlers only mark an output dirty; drawing waits until the whole
	 * batch is read. A scale change arrives as preferred_scale and THEN a
	 * configure with the new logical size — drawing on the first would
	 * render a full frame at a size that is already stale.
	 *
	 * Runs until the compositor goes away. With no outputs (a VT switch
	 * takes them all on aro) it just waits for them to come back.
	 */
	while (wl_display_dispatch(p.display) != -1) {
		wl_list_for_each(o, &p.outputs, link) {
			if (o->dirty) {
				o->dirty = false;
				draw(o);
			}
		}
	}

	wl_list_for_each_safe(o, tmp, &p.outputs, link)
		output_destroy(o);
	if (p.frac_mgr)
		wp_fractional_scale_manager_v1_destroy(p.frac_mgr);
	if (p.viewporter)
		wp_viewporter_destroy(p.viewporter);
	zwlr_layer_shell_v1_destroy(p.layer_shell);
	wl_shm_destroy(p.shm);
	wl_compositor_destroy(p.compositor);
	wl_registry_destroy(p.registry);
	wl_display_disconnect(p.display);
	return 0;
}
