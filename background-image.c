#include <limits.h>
#include <pixman.h>
#if !HAVE_GDK_PIXBUF
#include <png.h>
#endif
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

#if !HAVE_GDK_PIXBUF
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
#endif

static bool load_image(struct wsbg_image *image, struct wsbg_color background) {
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

void release_wsbg_buffer(struct wsbg_buffer *buffer) {
	if (!buffer || --buffer->ref_count != 0) {
		return;
	}

	wl_list_remove(&buffer->link);
	munmap_buffer(buffer);
	free(buffer);
}

static void get_wsbg_image_transform(
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

static struct wsbg_buffer *get_wsbg_color_buffer(
		struct wsbg_state *state,
		struct wsbg_color color) {
	struct wsbg_buffer *buffer;
	wl_list_for_each(buffer, &state->colors, link) {
		if (color_eql(buffer->background, color)) {
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

	buffer->background = color;
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

	if (image->width <= 0 && !load_image(image, config->color)) {
		return NULL;
	}

	struct wsbg_image_transform transform;
	bool covered;
	get_wsbg_image_transform(
			image, config->mode, width, height,
			&transform, &covered);

	struct wsbg_color background = (!covered || image->background.a) ?
			config->color : (struct wsbg_color){};
	bool repeat = (config->mode == BACKGROUND_MODE_TILE) && !covered;

	struct wsbg_buffer *buffer;
	wl_list_for_each(buffer, &image->buffers, link) {
		if (transform_eql(buffer->transform, transform) &&
				color_eql(buffer->background, background) &&
				buffer->repeat == repeat) {
			++buffer->ref_count;
			return buffer;
		}
	}

	if (!load_image(image, config->color)) {
		return NULL;
	} else if (!(buffer = calloc(1, sizeof *buffer))) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
		return NULL;
	}

	pixman_image_t *surface;
	if (!mmap_buffer(buffer, state, width, height, &surface)) {
		free(buffer);
		return NULL;
	}

	if (background.a) {
		pixman_color_t fill = {
			.red   = background.r * UINT16_C(0x0101),
			.green = background.g * UINT16_C(0x0101),
			.blue  = background.b * UINT16_C(0x0101),
			.alpha = background.a * UINT16_C(0x0101)
		};

		pixman_box32_t box = { .x2 = width, .y2 = height };
		pixman_image_fill_boxes(PIXMAN_OP_SRC, surface, &fill, 1, &box);
	}

	pixman_transform_t matrix;
	pixman_transform_init_translate(
			&matrix, transform.x, transform.y);
	pixman_transform_scale(
			&matrix, NULL, transform.scale_x, transform.scale_y);

	pixman_image_set_filter(image->surface, PIXMAN_FILTER_BEST, NULL, 0);
	pixman_image_set_transform(image->surface, &matrix);
	pixman_image_set_repeat(image->surface,
			repeat ? PIXMAN_REPEAT_NORMAL : PIXMAN_REPEAT_NONE);

	pixman_image_composite32(
		PIXMAN_OP_OVER, image->surface, NULL, surface,
		0, 0, 0, 0, 0, 0, width, height);

	pixman_image_unref(surface);

	buffer->background = background;
	buffer->transform = transform;
	buffer->repeat = repeat;

	buffer->ref_count = 1;
	wl_list_insert(&image->buffers, &buffer->link);
	return buffer;
}
