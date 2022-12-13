#define _POSIX_C_SOURCE 200809
#include <assert.h>
#include <cairo.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "log.h"
#include "pool-buffer.h"

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

bool mmap_buffer(struct wsbg_buffer *buffer, struct wsbg_state *state,
		int32_t width, int32_t height, cairo_surface_t **surface_ptr) {
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
		*surface_ptr = cairo_image_surface_create_for_data(
				buffer->data, CAIRO_FORMAT_RGB24, width, height, stride);
	}
	return true;
}

bool mmap_color_buffer(struct wsbg_buffer *buffer, struct wsbg_state *state,
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

void munmap_buffer(struct wsbg_buffer *buffer) {
	if (buffer->buffer) {
		wl_buffer_destroy(buffer->buffer);
	}
	if (buffer->data) {
		munmap(buffer->data, buffer->size);
	}
}
