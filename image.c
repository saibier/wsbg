#define _POSIX_C_SOURCE 200809
#include <limits.h>
#include <pixman.h>
#include <stdint.h>
#include <stdlib.h>
#include "image.h"
#include "log.h"

#define Q16 INT64_C(0x10000)
#define IMAGE_SIZE_MAX (INT64_MAX / (INT32_MAX * Q16))

enum background_mode parse_background_mode(const char *mode) {
	if (strcmp(mode, "stretch") == 0) {
		return BACKGROUND_MODE_STRETCH;
	} else if (strcmp(mode, "fill") == 0) {
		return BACKGROUND_MODE_FILL;
	} else if (strcmp(mode, "fit") == 0) {
		return BACKGROUND_MODE_FIT;
	} else if (strcmp(mode, "center") == 0) {
		return BACKGROUND_MODE_CENTER;
	} else if (strcmp(mode, "tile") == 0) {
		return BACKGROUND_MODE_TILE;
	} else if (strcmp(mode, "solid_color") == 0) {
		return BACKGROUND_MODE_SOLID_COLOR;
	}
	wsbg_log(LOG_ERROR, "Unsupported background mode: %s", mode);
	return BACKGROUND_MODE_INVALID;
}

void get_wsbg_image_transform(
		struct wsbg_image *image,
		enum background_mode mode,
		int32_t width, int32_t height,
		struct wsbg_image_transform *transform,
		bool *covered) {
	int64_t width_q16 = width * Q16;
	int64_t height_q16 = height * Q16;

	int64_t dest_width, dest_height;

	switch (mode) {
	case BACKGROUND_MODE_CENTER:
	case BACKGROUND_MODE_TILE:
		dest_width = image->width * Q16;
		dest_height = image->height * Q16;
		transform->scale_x = Q16;
		transform->scale_y = Q16;
		break;
	case BACKGROUND_MODE_STRETCH:
		dest_width = width_q16;
		dest_height = height_q16;
		transform->scale_x = image->width * Q16 / width;
		transform->scale_y = image->height * Q16 / height;
		break;
	case BACKGROUND_MODE_FILL:
	case BACKGROUND_MODE_FIT:
	default:
		dest_width = image->width * height_q16 / image->height;
		if (mode == BACKGROUND_MODE_FIT ?
				width_q16 < dest_width : dest_width < width_q16) {
			dest_width = width_q16;
			dest_height = image->height * width_q16 / image->width;
			transform->scale_x = transform->scale_y =
				image->width * Q16 / width;
		} else {
			dest_height = height_q16;
			transform->scale_x = transform->scale_y =
				image->height * Q16 / height;
		}
	}

	switch (mode) {
	case BACKGROUND_MODE_FILL:
	case BACKGROUND_MODE_FIT:
	case BACKGROUND_MODE_CENTER:
		transform->x = (dest_width - width_q16) / 2;
		transform->y = (dest_height - height_q16) / 2;
		// If scale is 1:1, align pixels for sharper look
		if (transform->scale_x == Q16) {
			transform->x &= ~(Q16 - 1);
		}
		if (transform->scale_y == Q16) {
			transform->y &= ~(Q16 - 1);
		}
		break;
	default:
		transform->x = 0;
		transform->y = 0;
	}

	*covered =
		transform->x <= 0 &&
		transform->y <= 0 &&
		width_q16 <= transform->x + dest_width &&
		height_q16 <= transform->y + dest_height;
}

#if HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>

static void unref_image_pixbuf(pixman_image_t *image, void *pixbuf) {
	g_object_unref(pixbuf);
}

static void load_gdk_pixbuf(struct wsbg_image *image) {
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

#else // !HAVE_GDK_PIXBUF
#include <png.h>

static void free_image_data(pixman_image_t *image, void *data) {
	free(data);
}

static void load_png(struct wsbg_image *image) {
	png_image reader = {};
	reader.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file(&reader, image->path)) {
		wsbg_log(LOG_ERROR, "Failed to load %s: %s", image->path, reader.message);
		return;
	}

	if (!(reader.format & PNG_FORMAT_FLAG_ALPHA)) {
		image->background = (struct wsbg_color){};
	}

	image->width = reader.width;
	image->height = reader.height;

	int stride = PNG_IMAGE_ROW_STRIDE(reader);
	void *buffer = malloc(PNG_IMAGE_BUFFER_SIZE(reader, stride));
	if (!buffer) {
		png_image_free(&reader);
		return;
	}

	png_color background = {
		.red   = image->background.r,
		.green = image->background.g,
		.blue  = image->background.b
	};

	reader.format = PNG_FORMAT_RGB;
	if (!png_image_finish_read(&reader, &background, buffer, stride, NULL)) {
		free(buffer);
		return;
	}

	image->surface = pixman_image_create_bits_no_clear(
			PIXMAN_b8g8r8, image->width, image->height, buffer, stride);
	if (!image->surface) {
		free(buffer);
		return;
	}

	pixman_image_set_destroy_function(
			image->surface, &free_image_data, buffer);
}
#endif // HAVE_GDK_PIXBUF

bool load_image(struct wsbg_image *image, struct wsbg_color background) {
	if (image->surface) {
		if (!image->background.a || color_eql(background, image->background)) {
			return true;
		}
		unload_image(image);
	} else if (image->width == -1) {
		return false;
	}

	image->background = background;

#if HAVE_GDK_PIXBUF
	load_gdk_pixbuf(image);
#else
	load_png(image);
#endif

	if (!image->surface) {
		image->width = -1;
		return false;
	}

#if IMAGE_SIZE_MAX < INT_MAX
	if (IMAGE_SIZE_MAX < image->width || IMAGE_SIZE_MAX < image->height) {
		wsbg_log(LOG_ERROR, "Failed to load %s: Image too large", image->path);
		unload_image(image);
		image->width = -1;
		return false;
	}
#endif

	return true;
}

void unload_image(struct wsbg_image *image) {
	if (image->surface) {
		pixman_image_unref(image->surface);
		image->surface = NULL;
	}
}
