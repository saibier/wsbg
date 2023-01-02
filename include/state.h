#ifndef _WSBG_STATE_H
#define _WSBG_STATE_H
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct wsbg_state {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_viewporter *viewporter;
	struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
	struct wl_list options;     // struct wsbg_option::link
	struct wl_list outputs;     // struct wsbg_output::link
	struct wl_list workspaces;  // struct wsbg_workspace::link
	struct wl_list images;      // struct wsbg_image::link
	struct wl_list colors;      // struct wsbg_buffer::link
};

struct wsbg_color {
	uint8_t b, g, r, a;
};

#define color_eql(x, y) ( \
		(x).b == (y).b && \
		(x).g == (y).g && \
		(x).r == (y).r && \
		(x).a == (y).a)

struct wsbg_image_transform {
	pixman_fixed_t x, y, scale_x, scale_y;
};

#define transform_eql(a, b) ( \
		(a).x == (b).x && \
		(a).y == (b).y && \
		(a).scale_x == (b).scale_x && \
		(a).scale_y == (b).scale_y)

struct wsbg_image {
	const char *path;
	struct wsbg_color background;
	pixman_image_t *surface;
	int width, height;
	struct wl_list buffers;  // struct wsbg_buffer::link
	struct wl_list link;
};

struct wsbg_buffer {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	size_t ref_count;
	int32_t width, height;
	struct wsbg_image_transform transform;
	struct wsbg_color background;
	bool repeat;
	struct wl_list link;
};

enum wsbg_option_type {
	WSBG_OUTPUT = 1,
	WSBG_WORKSPACE,
	WSBG_COLOR,
	WSBG_IMAGE,
	WSBG_MODE
};

enum background_mode {
	BACKGROUND_MODE_STRETCH,
	BACKGROUND_MODE_FILL,
	BACKGROUND_MODE_FIT,
	BACKGROUND_MODE_CENTER,
	BACKGROUND_MODE_TILE,
	BACKGROUND_MODE_SOLID_COLOR,
	BACKGROUND_MODE_INVALID,
};

struct wsbg_option {
	enum wsbg_option_type type;
	union {
		const char *name;
		struct wsbg_color color;
		struct wsbg_image *image;
		enum background_mode mode;
	} value;
	struct wl_list link;
};

struct wsbg_config {
	const char *workspace;
	enum background_mode mode;
	struct wsbg_color color;
	struct wsbg_image *image;
	struct wsbg_buffer *buffer;
	struct wl_list link;
};

struct wsbg_output {
	uint32_t wl_name;
	struct wl_output *wl_output;
	char *name;
	char *identifier;

	struct wsbg_state *state;
	struct wsbg_config *config;

	struct wl_list configs;  // struct wsbg_config::link

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t width, height;
	int32_t buffer_width, buffer_height;
	bool configured, buffer_change, config_change;

	struct wl_list link;
};

struct wsbg_workspace {
	char *name;
	char *output;
	struct wl_list link;
};

#endif
