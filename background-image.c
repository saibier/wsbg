#include <stdint.h>
#include <stdlib.h>
#include <wayland-client.h>
#include "background-image.h"
#include "cairo_util.h"
#include "log.h"
#include "pool-buffer.h"

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

cairo_surface_t *load_background_image(const char *path) {
	cairo_surface_t *image;
#if HAVE_GDK_PIXBUF
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (!pixbuf) {
		wsbg_log(LOG_ERROR, "Failed to load background image (%s).",
				err->message);
		return NULL;
	}
	image = gdk_cairo_image_surface_create_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);
#else
	image = cairo_image_surface_create_from_png(path);
#endif // HAVE_GDK_PIXBUF
	if (!image) {
		wsbg_log(LOG_ERROR, "Failed to read background image.");
		return NULL;
	}
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		wsbg_log(LOG_ERROR, "Failed to read background image: %s."
#if !HAVE_GDK_PIXBUF
				"\nwsbg was compiled without gdk_pixbuf support, so only"
				"\nPNG images can be loaded. This is the likely cause."
#endif // !HAVE_GDK_PIXBUF
				, cairo_status_to_string(cairo_surface_status(image)));
		return NULL;
	}
	return image;
}

static bool load_image(struct wsbg_image *image) {
	if (image->surface) {
		return true;
	} else if (image->width == -1) {
		return false;
	}

	image->surface = load_background_image(image->path);
	if (!image->surface) {
		image->width = -1;
		return false;
	}

	image->width = cairo_image_surface_get_width(image->surface);
	image->height = cairo_image_surface_get_height(image->surface);

#if IMAGE_SIZE_MAX < INT_MAX
	if (IMAGE_SIZE_MAX < image->width || IMAGE_SIZE_MAX < image->height) {
		wsbg_log(LOG_ERROR, "Image too large: %s", image->path);
		unload_image(image);
		image->width = -1;
		return false;
	}
#endif

	return true;
}

void unload_image(struct wsbg_image *image) {
	if (image->surface) {
		cairo_surface_destroy(image->surface);
		image->surface = NULL;
	}
}

void release_wsbg_buffer(struct wsbg_buffer *buffer) {
	if (!buffer || --buffer->ref_count != 0) {
		return;
	}

	wl_list_remove(&buffer->link);
	munmap_buffer(buffer);
	free(buffer);
}

static void get_wsbg_image_dest_q16(
		struct wsbg_image *image,
		enum background_mode mode,
		int32_t width, int32_t height,
		struct wsbg_image_dest *dest_q16) {
	int64_t width_q16 = width * Q16;
	int64_t height_q16 = height * Q16;

	switch (mode) {
	case BACKGROUND_MODE_CENTER:
	case BACKGROUND_MODE_TILE:
		dest_q16->width = image->width * Q16;
		dest_q16->height = image->height * Q16;
		break;
	case BACKGROUND_MODE_STRETCH:
		dest_q16->width = width_q16;
		dest_q16->height = height_q16;
		break;
	case BACKGROUND_MODE_FILL:
	case BACKGROUND_MODE_FIT:
	default:
		dest_q16->width = image->width * height_q16 / image->height;
		if (mode == BACKGROUND_MODE_FIT ?
				width_q16 < dest_q16->width : dest_q16->width < width_q16) {
			dest_q16->width = width_q16;
			dest_q16->height = image->height * width_q16 / image->width;
		} else {
			dest_q16->height = height_q16;
		}
	}

	switch (mode) {
	case BACKGROUND_MODE_FILL:
	case BACKGROUND_MODE_FIT:
	case BACKGROUND_MODE_CENTER:
		dest_q16->x = (width_q16 - dest_q16->width) / 2;
		dest_q16->y = (height_q16 - dest_q16->height) / 2;
		break;
	default:
		dest_q16->x = 0;
		dest_q16->y = 0;
	}
}

static struct wsbg_buffer *get_wsbg_color_buffer(
		struct wsbg_state *state,
		struct wsbg_color color) {
	struct wsbg_buffer *buffer;
	wl_list_for_each(buffer, &state->colors, link) {
		if (color_eql(buffer->color, color)) {
			++buffer->ref_count;
			return buffer;
		}
	}

	if (!(buffer = calloc(1, sizeof *buffer))) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
		return NULL;
	} else if (!mmap_color_buffer(buffer, state, color)) {
		free(buffer);
		return NULL;
	}

	buffer->color = color;
	buffer->ref_count = 1;
	wl_list_insert(&state->colors, &buffer->link);
	return buffer;
}

struct wsbg_buffer *get_wsbg_buffer(
		struct wsbg_config *config,
		struct wsbg_state *state,
		int32_t width, int32_t height) {
	struct wsbg_image *image = config->image;

	if (!image) {
		return get_wsbg_color_buffer(state, config->color);
	}

	if (image->width <= 0 && !load_image(image)) {
		return NULL;
	}

	struct wsbg_image_dest dest_q16;
	get_wsbg_image_dest_q16(image, config->mode, width, height, &dest_q16);

	struct wsbg_buffer *buffer;
	wl_list_for_each(buffer, &image->buffers, link) {
		if (color_eql(buffer->color, config->color) &&
				buffer->dest_q16.x == dest_q16.x &&
				buffer->dest_q16.y == dest_q16.y &&
				buffer->dest_q16.width == dest_q16.width &&
				buffer->dest_q16.height == dest_q16.height) {
			++buffer->ref_count;
			return buffer;
		}
	}

	if (!load_image(image)) {
		return NULL;
	} else if (!(buffer = calloc(1, sizeof *buffer))) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
		return NULL;
	}

	cairo_surface_t *surface;
	if (!mmap_buffer(buffer, state, width, height, &surface)) {
		free(buffer);
		return NULL;
	}

	buffer->color = config->color;
	buffer->dest_q16 = dest_q16;

	cairo_t *cairo = cairo_create(surface);

	cairo_set_source_rgb(cairo,
		config->color.r / (double)0xFF,
		config->color.g / (double)0xFF,
		config->color.b / (double)0xFF);
	cairo_paint(cairo);

	cairo_translate(cairo,
		(double)dest_q16.x / Q16,
		(double)dest_q16.y / Q16);
	cairo_scale(cairo,
		(double)dest_q16.width / Q16 / image->width,
		(double)dest_q16.height / Q16 / image->height);

	cairo_pattern_t *pattern =
		cairo_pattern_create_for_surface(image->surface);
	if (config->mode == BACKGROUND_MODE_TILE) {
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	}
	cairo_set_source(cairo, pattern);
	cairo_paint(cairo);

	cairo_pattern_destroy(pattern);
	cairo_destroy(cairo);
	cairo_surface_destroy(surface);

	buffer->ref_count = 1;
	wl_list_insert(&image->buffers, &buffer->link);
	return buffer;
}
