/*
 * aro — text.c
 */
/* strdup */
#define _POSIX_C_SOURCE 200809L

/* scene.h first: it decides which scene implementation the build uses,
 * and that only works if it is included before any wlroots header. */
#include "scene.h"

#include "text.h"

#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <wayland-server-core.h>
/* Implementing a buffer needs the interfaces header; the types header only
 * covers using one. wlr_buffer_impl and wlr_buffer_init live here. */
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>

/* ── a wlr_buffer backed by a cairo image surface ──────────────────────── */
/*
 * wlroots reads this through the data-pointer path, so the surface format
 * has to match what we claim: cairo's ARGB32 is premultiplied, native-endian
 * 32-bit, which is DRM_FORMAT_ARGB8888 on little-endian. Do not hand this
 * out for writing — cairo owns the pixels.
 */
struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cb_destroy(struct wlr_buffer *buffer)
{
	struct cairo_buffer *cb = wl_container_of(buffer, cb, base);
	cairo_surface_destroy(cb->surface);
	free(cb);
}

static bool cb_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags,
                                     void **data, uint32_t *format, size_t *stride)
{
	struct cairo_buffer *cb = wl_container_of(buffer, cb, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = cairo_image_surface_get_data(cb->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)cairo_image_surface_get_stride(cb->surface);
	return true;
}

static void cb_end_data_ptr_access(struct wlr_buffer *buffer)
{
	(void)buffer;
}

static const struct wlr_buffer_impl cb_impl = {
	.destroy = cb_destroy,
	.begin_data_ptr_access = cb_begin_data_ptr_access,
	.end_data_ptr_access = cb_end_data_ptr_access,
};

/* ── rendering ─────────────────────────────────────────────────────────── */

static PangoLayout *layout_for(cairo_t *cr, const char *font, const char *text,
                               int max_w)
{
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	pango_layout_set_text(layout, text, -1);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	if (max_w > 0) {
		pango_layout_set_width(layout, max_w * PANGO_SCALE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	}
	return layout;
}

static struct wlr_buffer *render(const char *text, const char *font,
                                 uint32_t color, float scale, int max_w,
                                 int *out_w, int *out_h)
{
	if (scale <= 0.0f)
		scale = 1.0f;

	/* measure first: the surface has to be the right size before we draw */
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *pcr = cairo_create(probe);
	PangoLayout *probe_layout = layout_for(pcr, font, text, max_w);
	int lw = 0, lh = 0;
	pango_layout_get_pixel_size(probe_layout, &lw, &lh);
	g_object_unref(probe_layout);
	cairo_destroy(pcr);
	cairo_surface_destroy(probe);

	if (lw < 1)
		lw = 1;
	if (lh < 1)
		lh = 1;

	int pw = (int)(lw * scale + 0.5f);
	int ph = (int)(lh * scale + 0.5f);

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* cairo takes straight alpha and premultiplies on its own */
	cairo_set_source_rgba(cr,
	                      ((color >> 24) & 0xff) / 255.0,
	                      ((color >> 16) & 0xff) / 255.0,
	                      ((color >> 8) & 0xff) / 255.0,
	                      (color & 0xff) / 255.0);

	PangoLayout *layout = layout_for(cr, font, text, max_w);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	cairo_destroy(cr);
	cairo_surface_flush(surface);

	struct cairo_buffer *cb = calloc(1, sizeof *cb);
	if (!cb) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	cb->surface = surface;
	wlr_buffer_init(&cb->base, &cb_impl, pw, ph);

	*out_w = lw;
	*out_h = lh;
	return &cb->base;
}

/* ── qtext ─────────────────────────────────────────────────────────────── */

bool qtext_init(struct qtext *t, struct wlr_scene_tree *parent, const char *font)
{
	memset(t, 0, sizeof *t);
	t->font = font;
	t->scale = 1.0f;
	t->node = wlr_scene_buffer_create(parent, NULL);
	return t->node != NULL;
}

/*
 * Change the font and force the next qtext_set() to re-render.
 *
 * The cache below compares text, colour, scale and width — not the font,
 * because until the config file existed the font could not change. Clearing
 * the cached string is what makes the comparison miss.
 */
void qtext_set_font(struct qtext *t, const char *font)
{
	if (!font || (t->font && strcmp(t->font, font) == 0))
		return;
	t->font = font;
	free(t->text);
	t->text = NULL;
}

void qtext_set(struct qtext *t, const char *text, uint32_t color,
               float scale, int max_w)
{
	if (!t->node)
		return;
	if (!text)
		text = "";

	if (t->text && strcmp(t->text, text) == 0 &&
	    t->color == color && t->scale == scale && t->max_w == max_w)
		return;                 /* nothing that affects pixels has changed */

	/* `text` may BE t->text — callers legitimately re-render with the
	 * current string to change its colour. Copy before freeing, or the
	 * strdup reads memory we just handed back to the allocator. */
	char *copy = strdup(text);
	if (!copy)
		return;
	free(t->text);
	t->text = copy;
	t->color = color;
	t->scale = scale;
	t->max_w = max_w;

	if (*copy == '\0') {
		wlr_scene_buffer_set_buffer(t->node, NULL);
		t->w = t->h = 0;
		return;
	}

	int w = 0, h = 0;
	struct wlr_buffer *buf = render(copy, t->font, color, scale, max_w, &w, &h);
	if (!buf)
		return;

	wlr_scene_buffer_set_buffer(t->node, buf);
	wlr_scene_buffer_set_dest_size(t->node, w, h);
	wlr_buffer_drop(buf);           /* the scene node holds its own reference */

	t->w = w;
	t->h = h;
}

void qtext_move(struct qtext *t, int x, int y)
{
	if (t->node)
		wlr_scene_node_set_position(&t->node->node, x, y);
}

void qtext_show(struct qtext *t, bool visible)
{
	if (t->node)
		wlr_scene_node_set_enabled(&t->node->node, visible);
}

void qtext_finish(struct qtext *t)
{
	if (t->node)
		wlr_scene_node_destroy(&t->node->node);
	free(t->text);
	t->node = NULL;
	t->text = NULL;
}
