#define _POSIX_C_SOURCE 200809
#include <limits.h>
#include <pixman.h>
#include <stdint.h>
#include <stdlib.h>
#include "image.h"
#include "log.h"

#define IMAGE_SIZE_MAX (INT64_MAX / (INT32_MAX * Q16))

bool parse_mode(
		const char *str,
		enum background_mode *mode,
		struct wsbg_size *position) {
	if (strcmp(str, "stretch") == 0) {
		*mode = BACKGROUND_MODE_STRETCH;
		*position = (struct wsbg_size){ .x = 0, .y = 0 };
	} else if (strcmp(str, "fill") == 0) {
		*mode = BACKGROUND_MODE_FILL;
		*position = (struct wsbg_size){ .x = Q16 / 2, .y = Q16 / 2 };
	} else if (strcmp(str, "fit") == 0) {
		*mode = BACKGROUND_MODE_FIT;
		*position = (struct wsbg_size){ .x = Q16 / 2, .y = Q16 / 2 };
	} else if (strcmp(str, "center") == 0) {
		*mode = BACKGROUND_MODE_CENTER;
		*position = (struct wsbg_size){ .x = Q16 / 2, .y = Q16 / 2 };
	} else if (strcmp(str, "tile") == 0) {
		*mode = BACKGROUND_MODE_TILE;
		*position = (struct wsbg_size){ .x = 0, .y = 0 };
	} else if (strcmp(str, "solid_color") == 0) {
		*mode = BACKGROUND_MODE_SOLID_COLOR;
		*position = (struct wsbg_size){ .x = 0, .y = 0 };
	} else {
		return false;
	}
	return true;
}

bool parse_position(const char *str, struct wsbg_size *position) {
	*position = (struct wsbg_size){ .x = Q16 / 2, .y = Q16 / 2 };
	if (strcmp(str, "center") == 0) {
		return true;
	}
	if (strncmp(str, "top", 3) == 0) {
		position->y = 0;
		str += 3;
	} else if (strncmp(str, "bottom", 6) == 0) {
		position->y = Q16;
		str += 6;
	} else {
		goto left_right;
	}
	if (*str == '\0') {
		return true;
	} else if (*str != '/') {
		return false;
	}
	++str;
left_right:
	if (strcmp(str, "left") == 0) {
		position->x = 0;
	} else if (strcmp(str, "right") == 0) {
		position->x = Q16;
	} else {
		return false;
	}
	return true;
}

int64_t rounded_div(int64_t dividend, int64_t divisor)
{
	int64_t x = (dividend * 2) / divisor;
	return (x / 2) + (x & 1);
}

void get_wsbg_image_transform(
		struct wsbg_image *image,
		enum background_mode mode,
		struct wsbg_size position,
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
		transform->scale_x = rounded_div(image->width * Q16, width);
		transform->scale_y = rounded_div(image->height * Q16, height);
		break;
	case BACKGROUND_MODE_FILL:
	case BACKGROUND_MODE_FIT:
	default:
		dest_width = rounded_div(image->width * height_q16, image->height);
		if (mode == BACKGROUND_MODE_FIT ?
				width_q16 < dest_width : dest_width < width_q16) {
			dest_width = width_q16;
			dest_height = rounded_div(image->height * width_q16, image->width);
			transform->scale_x = transform->scale_y =
				rounded_div(image->width * Q16, width);
		} else {
			dest_height = height_q16;
			transform->scale_x = transform->scale_y =
				rounded_div(image->height * Q16, height);
		}
	}

	transform->x = rounded_div((dest_width - width_q16) * position.x, Q16);
	transform->y = rounded_div((dest_height - height_q16) * position.y, Q16);
	// If scale is 1:1, align pixels for sharper look
	if (transform->scale_x == Q16) {
		transform->x += (Q16 / 2);
		transform->x &= ~(Q16 - 1);
	}
	if (transform->scale_y == Q16) {
		transform->y += (Q16 / 2);
		transform->y &= ~(Q16 - 1);
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

static void load_gdk_pixbuf(struct wsbg_image *image,
		int scaled_width, int scaled_height) {
	gint width, height;
	if (image->width <= 0) {
		GdkPixbufFormat *format =
				gdk_pixbuf_get_file_info(image->path, &width, &height);

		if (format != NULL && gdk_pixbuf_format_is_scalable(format)) {
			image->width = width;
			image->height = height;
			image->is_scalable = true;
			if (scaled_width == 0) {
				return;
			}
		}
	}

	GError *err = NULL;
	GdkPixbuf *pixbuf;
	if (scaled_width == 0) {
		pixbuf = gdk_pixbuf_new_from_file(image->path, &err);
		if (pixbuf) {
			GdkPixbuf *rotated_pixbuf =
					gdk_pixbuf_apply_embedded_orientation(pixbuf);
			if (rotated_pixbuf) {
				g_object_unref(pixbuf);
				pixbuf = rotated_pixbuf;
			}

			image->width = width = gdk_pixbuf_get_width(pixbuf);
			image->height = height = gdk_pixbuf_get_height(pixbuf);
		}
	} else {
		pixbuf = gdk_pixbuf_new_from_file_at_scale(
				image->path, scaled_width, scaled_height, false, &err);
		width = scaled_width;
		height = scaled_height;
	}

	if (!pixbuf) {
		wsbg_log(LOG_ERROR, "Failed to load %s: %s", image->path, err->message);
		return;
	}

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
		g_object_unref(pixbuf);
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

bool load_image(struct wsbg_image *image,
		struct wsbg_color background, int scaled_width, int scaled_height) {
	if (image->surface) {
		if ((!image->background.a || color_eql(background, image->background))
			&& (scaled_width == 0 || (
					scaled_width == pixman_image_get_width(image->surface) &&
					scaled_height == pixman_image_get_height(image->surface)))) {
			return true;
		}
		unload_image(image);
	} else if (image->width == -1) {
		return false;
	}

	image->background = background;

#if HAVE_GDK_PIXBUF
	load_gdk_pixbuf(image, scaled_width, scaled_height);
#else
	load_png(image);
#endif

	if (!image->surface && !(image->is_scalable && scaled_width == 0)) {
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
