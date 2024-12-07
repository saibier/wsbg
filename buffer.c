#define _POSIX_C_SOURCE 200809
#include <fcntl.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "buffer.h"
#include "image.h"
#include "log.h"

static struct wl_shm_pool *mmap_pool(
		struct wsbg_buffer *buffer,
		struct wl_shm *shm,
		size_t size) {
	static const char *template = "wsbg-XXXXXX";
	const char *path = getenv("XDG_RUNTIME_DIR");
	if (path == NULL) {
		wsbg_log(LOG_ERROR, "XDG_RUNTIME_DIR is not set");
		return NULL;
	}

	size_t name_size = strlen(template) + 1 + strlen(path) + 1;
	char *name = malloc(name_size);
	if (name == NULL) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
		return NULL;
	}
	snprintf(name, name_size, "%s/%s", path, template);

	struct wl_shm_pool *pool = NULL;
	int fd = mkstemp(name);
	if (fd == -1) {
		wsbg_log_errno(LOG_ERROR, "Temp file creation failed");
		free(name);
		return NULL;
	}

	while (ftruncate(fd, size) == -1) {
		if (errno == EINTR) {
			continue;
		}
		wsbg_log_errno(LOG_ERROR, "Temp file creation failed");
		goto cleanup_and_return;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		wsbg_log_errno(LOG_ERROR, "Shared memory map failed");
		goto cleanup_and_return;
	}

	pool = wl_shm_create_pool(shm, fd, size);
	buffer->size = size;
	buffer->data = data;

cleanup_and_return:
	close(fd);
	unlink(name);
	free(name);
	return pool;
}

static void munmap_buffer(struct wsbg_buffer *buffer) {
	if (buffer->buffer) {
		wl_buffer_destroy(buffer->buffer);
	}
	if (buffer->data) {
		munmap(buffer->data, buffer->size);
	}
}

static bool mmap_buffer(
		struct wsbg_buffer *buffer, struct wsbg_state *state,
		int32_t width, int32_t height, pixman_image_t **surface_ptr) {
	uint32_t stride = width * 4;
	size_t size = stride * height;

	struct wl_shm_pool *pool = mmap_pool(buffer, state->shm, size);
	if (!pool) {
		return false;
	}
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	if (surface_ptr) {
		pixman_format_code_t format =
			(*(char *)(int[]){1}) ? PIXMAN_x8r8g8b8 : PIXMAN_b8g8r8x8;

		*surface_ptr = pixman_image_create_bits_no_clear(
				format, width, height, buffer->data, stride);

		if (!*surface_ptr) {
			wsbg_log(LOG_ERROR, "Creation of pixman image failed");
			munmap_buffer(buffer);
			return false;
		}
	}
	return true;
}

static bool mmap_color_buffer(
		struct wsbg_buffer *buffer, struct wsbg_state *state,
		struct wsbg_color color) {
	if (state->single_pixel_buffer_manager) {
		buffer->buffer =
			wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
				state->single_pixel_buffer_manager,
				color.r * UINT32_C(0x01010101),
				color.g * UINT32_C(0x01010101),
				color.b * UINT32_C(0x01010101),
				color.a * UINT32_C(0x01010101));

		if (buffer->buffer) {
			return true;
		}
	}

	uint8_t data[] = { color.b, color.g, color.r, color.a };
	struct wl_shm_pool *pool = mmap_pool(buffer, state->shm, sizeof data);
	if (!pool) {
		return false;
	}
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, sizeof data,
	        color.a == 0xFF ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	memcpy(buffer->data, &data, sizeof data);
	return true;
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

	if (!image || config->mode == BACKGROUND_MODE_SOLID_COLOR) {
		return get_wsbg_color_buffer(state, config->color);
	}

	if (image->width <= 0 && !load_image(image, config->color, 0, 0)) {
		return NULL;
	}

	struct wsbg_image_transform transform;
	bool covered;
	get_wsbg_image_transform(
			image, config->mode, config->position, width, height,
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

	int scaled_width = 0, scaled_height = 0;
	if (image->is_scalable) {
		scaled_width = rounded_div(image->width * Q16, transform.scale_x);
		scaled_height = rounded_div(image->height * Q16, transform.scale_y);
	}

	if (!load_image(image, config->color, scaled_width, scaled_height)) {
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
	if (!image->is_scalable) {
		pixman_transform_scale(
				&matrix, NULL, transform.scale_x, transform.scale_y);
	}

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

void release_wsbg_buffer(struct wsbg_buffer *buffer) {
	if (!buffer || --buffer->ref_count != 0) {
		return;
	}

	wl_list_remove(&buffer->link);
	munmap_buffer(buffer);
	free(buffer);
}
