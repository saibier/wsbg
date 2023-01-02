#if HAVE_GDK_PIXBUF
#include <stdint.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "cairo_util.h"
#include "log.h"

static void unref_image_pixbuf(pixman_image_t *image, void *pixbuf) {
	g_object_unref(pixbuf);
}

void load_gdk_pixbuf(struct wsbg_image *image) {
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(image->path, &err);
	if (!pixbuf) {
		wsbg_log(LOG_ERROR, "Failed to load %s: %s", image->path, err->message);
		return;
	}

	GdkPixbuf *rotated_pixbuf = gdk_pixbuf_apply_embedded_orientation(pixbuf);
	if (rotated_pixbuf) {
		g_object_unref(pixbuf);
		pixbuf = rotated_pixbuf;
	}

	gint width = gdk_pixbuf_get_width(pixbuf);
	gint height = gdk_pixbuf_get_height(pixbuf);

	image->width = width;
	image->height = height;

	if (gdk_pixbuf_get_has_alpha(pixbuf)) {
		guint32 background =
			UINT32_C(0xFF000000) +
			UINT32_C(0x00010000) * image->background.r +
			UINT32_C(0x00000100) * image->background.g +
			UINT32_C(0x00000001) * image->background.b;
		GdkPixbuf *pixbuf_no_alpha = gdk_pixbuf_composite_color_simple(
				pixbuf, width, height, GDK_INTERP_NEAREST,
				0xFF, 8, background, background);
		g_object_unref(pixbuf);
		if (!pixbuf_no_alpha) {
			return;
		}
		pixbuf = pixbuf_no_alpha;
	} else {
		image->background = (struct wsbg_color){};
	}

	int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
	if (n_channels < 3 || 4 < n_channels) {
		g_object_unref(pixbuf);
		return;
	}

	void *pixels = (void *)gdk_pixbuf_read_pixels(pixbuf);
	if (!pixels) {
		g_object_unref(pixels);
		return;
	}
	int stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixman_format_code_t format = n_channels == 3 ?
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
		PIXMAN_b8g8r8 : PIXMAN_x8b8g8r8;
#else
		PIXMAN_b8g8r8 : PIXMAN_r8g8b8x8;
#endif
	image->surface = pixman_image_create_bits_no_clear(
			format, width, height, pixels, stride);
	if (!image->surface) {
		g_object_unref(pixbuf);
		return;
	}

	pixman_image_set_destroy_function(
			image->surface, &unref_image_pixbuf, pixbuf);
}
#endif // HAVE_GDK_PIXBUF
