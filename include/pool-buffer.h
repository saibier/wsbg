#ifndef _WSBG_BUFFERS_H
#define _WSBG_BUFFERS_H
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "state.h"

bool mmap_buffer(struct wsbg_buffer *buffer, struct wsbg_state *state,
		int32_t width, int32_t height, cairo_surface_t **surface_ptr);
bool mmap_color_buffer(struct wsbg_buffer *buffer, struct wsbg_state *state,
		struct wsbg_color color);
void munmap_buffer(struct wsbg_buffer *buffer);

#endif
