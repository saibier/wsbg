#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-client.h>
#include "background-image.h"
#include "cairo_util.h"
#include "json.h"
#include "log.h"
#include "pool-buffer.h"
#include "sway-ipc.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		wsbg_log(LOG_DEBUG, "Invalid color %s, defaulting to 0xFFFFFFFF",
				color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

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
};

struct wsbg_image {
	struct wl_list link;
	const char *path;
	bool load_required;
};

enum wsbg_option_type {
	WSBG_OUTPUT = 1,
	WSBG_WORKSPACE,
	WSBG_COLOR,
	WSBG_IMAGE,
	WSBG_MODE
};

struct wsbg_option {
	enum wsbg_option_type type;
	union {
		const char *name;
		uint32_t color;
		struct wsbg_image *image;
		enum background_mode mode;
	} value;
	struct wl_list link;
};

struct wsbg_config {
	const char *workspace;
	uint32_t color;
	struct wsbg_image *image;
	enum background_mode mode;
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
	bool configured, buffer_change;

	struct wl_list link;
};

struct wsbg_workspace {
	char *name;
	char *output;
	struct wl_list link;
};

bool is_valid_color(const char *color) {
	int len = strlen(color);
	if (len != 7 || color[0] != '#') {
		wsbg_log(LOG_ERROR, "%s is not a valid color for wsbg. "
				"Color should be specified as #rrggbb (no alpha).", color);
		return false;
	}

	int i;
	for (i = 1; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return false;
		}
	}

	return true;
}

static void render_buffer(struct wsbg_output *output, struct wl_buffer *buffer) {
	wl_surface_attach(output->surface, buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);

	struct wp_viewport *viewport = wp_viewporter_get_viewport(
			output->state->viewporter, output->surface);
	wp_viewport_set_destination(viewport, output->width, output->height);

	wl_surface_commit(output->surface);

	wp_viewport_destroy(viewport);
}

static void render_frame(struct wsbg_output *output, cairo_surface_t *surface) {
	output->buffer_change = false;

	if (output->config->mode == BACKGROUND_MODE_SOLID_COLOR &&
			output->state->single_pixel_buffer_manager) {
		uint8_t r8 = (output->config->color >> 24) & 0xFF;
		uint8_t g8 = (output->config->color >> 16) & 0xFF;
		uint8_t b8 = (output->config->color >> 8) & 0xFF;
		uint8_t a8 = (output->config->color >> 0) & 0xFF;
		uint32_t f = 0xFFFFFFFF / 0xFF; // division result is an integer
		uint32_t r32 = r8 * f;
		uint32_t g32 = g8 * f;
		uint32_t b32 = b8 * f;
		uint32_t a32 = a8 * f;
		struct wl_buffer *buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
			output->state->single_pixel_buffer_manager, r32, g32, b32, a32);
		render_buffer(output, buffer);
		wl_buffer_destroy(buffer);
		return;
	}

	int32_t width, height;
	// Rotate buffer to match output
	if ((output->buffer_width < output->buffer_height) ==
			(output->width < output->height)) {
		width = output->buffer_width;
		height = output->buffer_height;
	} else {
		width = output->buffer_height;
		height = output->buffer_width;
	}

	struct pool_buffer buffer;
	if (!create_buffer(&buffer, output->state->shm,
			width, height, WL_SHM_FORMAT_ARGB8888)) {
		return;
	}

	cairo_t *cairo = buffer.cairo;
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	if (output->config->mode == BACKGROUND_MODE_SOLID_COLOR) {
		cairo_set_source_u32(cairo, output->config->color);
		cairo_paint(cairo);
	} else {
		if (output->config->color) {
			cairo_set_source_u32(cairo, output->config->color);
			cairo_paint(cairo);
		}

		if (surface) {
			render_background_image(cairo, surface,
				output->config->mode, width, height);
		}
	}

	render_buffer(output, buffer.buffer);
	destroy_buffer(&buffer);
}

static void destroy_wsbg_image(struct wsbg_image *image) {
	if (!image) {
		return;
	}
	wl_list_remove(&image->link);
	free(image);
}

static void destroy_wsbg_option(struct wsbg_option *option) {
	if (!option) {
		return;
	}
	wl_list_remove(&option->link);
	free(option);
}

static void destroy_wsbg_config(struct wsbg_config *config) {
	if (!config) {
		return;
	}
	wl_list_remove(&config->link);
	free(config);
}

static void destroy_wsbg_output(struct wsbg_output *output) {
	if (!output) {
		return;
	}
	wl_list_remove(&output->link);
	if (output->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(output->layer_surface);
	}
	if (output->surface != NULL) {
		wl_surface_destroy(output->surface);
	}
	wl_output_destroy(output->wl_output);
	struct wsbg_config *config, *tmp_config;
	wl_list_for_each_safe(config, tmp_config, &output->configs, link) {
		destroy_wsbg_config(config);
	}
	free(output->name);
	free(output->identifier);
	free(output);
}

static void destroy_wsbg_workspace(struct wsbg_workspace *workspace) {
	if (!workspace) {
		return;
	}
	wl_list_remove(&workspace->link);
	free(workspace->name);
	free(workspace->output);
	free(workspace);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct wsbg_output *output = data;
	if (output->width != width || output->height != height) {
		// Detect when output rotation changes
		if ((width < height) != (output->width < output->height)) {
			output->buffer_change = true;
		}

		output->width = width;
		output->height = height;

		if (width < 1 || height < 1) {
			return;
		}

		struct wp_viewport *viewport = wp_viewporter_get_viewport(
				output->state->viewporter, output->surface);

		zwlr_layer_surface_v1_ack_configure(surface, serial);
		wp_viewport_set_destination(viewport, width, height);
		wl_surface_commit(output->surface);
		output->configured = true;

		wp_viewport_destroy(viewport);
	}
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct wsbg_output *output = data;
	wsbg_log(LOG_DEBUG, "Destroying output %s (%s)",
			output->name, output->identifier);
	destroy_wsbg_output(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	struct wsbg_output *output = data;
	if (output->buffer_width != width || output->buffer_height != height) {
		output->buffer_width = width;
		output->buffer_height = height;
		output->buffer_change = true;
	}
}

static void create_layer_surface(struct wsbg_output *output) {
	output->surface = wl_compositor_create_surface(output->state->compositor);
	assert(output->surface);

	// Empty input region
	struct wl_region *input_region =
		wl_compositor_create_region(output->state->compositor);
	assert(input_region);
	wl_surface_set_input_region(output->surface, input_region);
	wl_region_destroy(input_region);

	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			output->state->layer_shell, output->surface, output->wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	assert(output->layer_surface);

	zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(output->layer_surface,
			&layer_surface_listener, output);
	wl_surface_commit(output->surface);
}

static void output_done(void *data, struct wl_output *wl_output) {
	struct wsbg_output *output = data;
	if (!output->config) {
		wsbg_log(LOG_DEBUG, "Could not find config for output %s (%s)",
				output->name, output->identifier);
		destroy_wsbg_output(output);
	} else if (!output->layer_surface) {
		wsbg_log(LOG_DEBUG, "Found config for output %s (%s)",
				output->name, output->identifier);
		create_layer_surface(output);
	}
}

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	// Who cares
}

static void configure_output(struct wsbg_output *output) {
	while (output->configs.next != &output->configs) {
		struct wsbg_config *config =
				wl_container_of(output->configs.next, config, link);
		destroy_wsbg_config(config);
	}

	struct wl_list configs;
	wl_list_init(&configs);

	struct wsbg_config *default_config;
	if (!(default_config = calloc(1, sizeof *default_config))) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
		return;
	}
	wl_list_insert(&configs, &default_config->link);
	output->config = default_config;

	const char *workspace = NULL;
	struct wsbg_workspace *ws;
	wl_list_for_each(ws, &output->state->workspaces, link) {
		if (strcmp(output->name, ws->output) == 0) {
			workspace = ws->name;
			break;
		}
	}

	struct wsbg_option *option = NULL;
	enum wsbg_option_type prev_type = 0;
	bool selected = true;
	wl_list_for_each(option, &output->state->options, link) {
		if (option->type == WSBG_OUTPUT) {
			selected = (selected && prev_type == WSBG_OUTPUT) ||
				!option->value.name ||
				0 == strcmp(output->name, option->value.name) ||
				0 == strcmp(output->identifier, option->value.name);
		} else if (option->type == WSBG_WORKSPACE) {
			if (!option->value.name) {
				wl_list_insert_list(&configs, &output->configs);
				wl_list_init(&output->configs);
			} else {
				struct wsbg_config *config = NULL;
				if (prev_type == WSBG_WORKSPACE) {
					struct wsbg_config *needle;
					wl_list_for_each(needle, &configs, link) {
						if (0 == strcmp(option->value.name, needle->workspace)) {
							config = needle;
							break;
						}
					}
				} else {
					wl_list_insert_list(&output->configs, &configs);
					wl_list_init(&configs);
				}
				if (!config) {
					struct wsbg_config *needle, *tmp;
					wl_list_for_each_safe(needle, tmp, &output->configs, link) {
						if (needle->workspace && strcmp(option->value.name, needle->workspace) == 0) {
							wl_list_remove(&needle->link);
							config = needle;
							break;
						}
					}
					if (!config) {
						if (!(config = malloc(sizeof *config))) {
							wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
						} else {
							memcpy(config, default_config, sizeof *config);
							config->workspace = option->value.name;
							if (workspace && strcmp(config->workspace, workspace) == 0) {
								output->config = config;
							}
						}
					}
					if (config) {
						wl_list_insert(&configs, &config->link);
					}
				}
			}
		} else if (selected) {
			struct wsbg_config *config;
			if (option->type == WSBG_COLOR) {
				wl_list_for_each(config, &configs, link) {
					config->color = option->value.color;
				}
			} else if (option->type == WSBG_IMAGE) {
				wl_list_for_each(config, &configs, link) {
					config->image = option->value.image;
				}
			} else if (option->type == WSBG_MODE) {
				wl_list_for_each(config, &configs, link) {
					config->mode = option->value.mode;
				}
			}
		}
		prev_type = option->type;
	}
	wl_list_insert_list(&output->configs, &configs);
}

static void output_name(void *data, struct wl_output *wl_output,
		const char *name) {
	struct wsbg_output *output = data;
	if (output->name) {
		if (strcmp(output->name, name) == 0) {
			return;
		}
		free(output->name);
	}
	if (!(output->name = strdup(name))) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
	}
	if (output->name && output->identifier) {
		configure_output(output);
	}
}

static void output_description(void *data, struct wl_output *wl_output,
		const char *description) {
	struct wsbg_output *output = data;
	char *identifier;

	// wlroots currently sets the description to `make model serial (name)`
	// If this changes in the future, this will need to be modified.
	char *paren = strrchr(description, '(');
	if (paren) {
		size_t length = paren - description;
		if ((identifier = malloc(length))) {
			strncpy(identifier, description, length);
			identifier[length - 1] = '\0';
		}
	} else {
		identifier = strdup(description);
	}
	if (!identifier) {
		wsbg_log(LOG_ERROR, "Memory allocation failed");
		return;
	}
	if (output->identifier) {
		if (strcmp(output->identifier, identifier) == 0) {
			free(identifier);
			return;
		}
		free(output->identifier);
	}
	output->identifier = identifier;
	if (output->name && output->identifier) {
		configure_output(output);
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wsbg_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor =
			wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wsbg_output *output = calloc(1, sizeof(struct wsbg_output));
		output->state = state;
		wl_list_init(&output->configs);
		output->wl_name = name;
		output->wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		wl_list_insert(&state->outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell =
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		state->viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, 1);
	} else if (strcmp(interface,
			wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		state->single_pixel_buffer_manager = wl_registry_bind(registry, name,
			&wp_single_pixel_buffer_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct wsbg_state *state = data;
	struct wsbg_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output->wl_name == name) {
			wsbg_log(LOG_DEBUG, "Destroying output %s (%s)",
					output->name, output->identifier);
			destroy_wsbg_output(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static bool wsbg_option_select(struct wsbg_state *state,
		enum wsbg_option_type type, const char *name) {
	struct wsbg_option *option = calloc(1, sizeof *option);
	if (!option) {
		wsbg_log(LOG_ERROR, "Memory allocation failed");
		return false;
	}
	option->type = type;
	if (strcmp("*", name) != 0) {
		option->value.name = name;
	}
	wl_list_insert(state->options.prev, &option->link);
	return true;
}

static struct wsbg_option *wsbg_option_new(struct wsbg_state *state,
		enum wsbg_option_type type) {
	struct wsbg_option *option;
	wl_list_for_each_reverse(option, &state->options, link) {
		if (option->type == WSBG_OUTPUT || option->type == WSBG_WORKSPACE) {
			break;
		}
		if (option->type == type) {
			return option;
		}
	}
	if (!(option = calloc(1, sizeof *option))) {
		wsbg_log(LOG_ERROR, "Memory allocation failed");
		static struct wsbg_option empty = {};
		return &empty;
	}
	option->type = type;
	wl_list_insert(state->options.prev, &option->link);
	return option;
}

static void parse_command_line(int argc, char **argv,
		struct wsbg_state *state) {
	static struct option long_options[] = {
		{"color", required_argument, NULL, 'c'},
		{"help", no_argument, NULL, 'h'},
		{"image", required_argument, NULL, 'i'},
		{"mode", required_argument, NULL, 'm'},
		{"output", required_argument, NULL, 'o'},
		{"version", no_argument, NULL, 'v'},
		{"workspace", required_argument, NULL, 'w'},
		{0, 0, 0, 0}
	};

	const char *usage =
		"Usage: wsbg <options...>\n"
		"\n"
		"  -c, --color            Set the background color.\n"
		"  -h, --help             Show help message and quit.\n"
		"  -i, --image            Set the image to display.\n"
		"  -m, --mode             Set the mode to use for the image.\n"
		"  -o, --output           Set the output to operate on or * for all.\n"
		"  -v, --version          Show the version number and quit.\n"
		"  -w, --workspace        Set the workspace to operate on or * for all.\n"
		"\n"
		"Background Modes:\n"
		"  stretch, fit, fill, center, tile, or solid_color\n";

	int c;
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "c:hi:m:o:vw:", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':  // color
			if (!is_valid_color(optarg)) {
				wsbg_log(LOG_ERROR, "Invalid color: %s", optarg);
				continue;
			}
			wsbg_option_new(state, WSBG_COLOR)
				->value.color = parse_color(optarg);
			break;
		case 'i': { // image
			struct wsbg_image *im, *image = NULL;
			wl_list_for_each(im, &state->images, link) {
				if (strcmp(optarg, im->path) == 0) {
					image = im;
					break;
				}
			}
			if (!image) {
				image = calloc(1, sizeof *image);
				image->path = optarg;
				wl_list_insert(&state->images, &image->link);
			}
			wsbg_option_new(state, WSBG_IMAGE)->value.image = image;
			break;
		}
		case 'm': { // mode
			enum background_mode mode = parse_background_mode(optarg);
			if (mode == BACKGROUND_MODE_INVALID) {
				wsbg_log(LOG_ERROR, "Invalid mode: %s", optarg);
			}
			wsbg_option_new(state, WSBG_MODE)->value.mode = mode;
			break;
		}
		case 'o':  // output
			wsbg_option_select(state, WSBG_OUTPUT, optarg);
			break;
		case 'v':  // version
			fprintf(stdout, "wsbg version " WSBG_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		case 'w':  // workspace
			wsbg_option_select(state, WSBG_WORKSPACE, optarg);
			break;
		default:
			fprintf(c == 'h' ? stdout : stderr, "%s", usage);
			exit(c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}

	// Check for invalid options
	if (optind < argc) {
		fprintf(stderr, "%s", usage);
		exit(EXIT_FAILURE);
	}
}

struct wsbg_workspace *update_workspace(
		struct wsbg_state *state, struct wl_list *last,
		char *name, char *output) {
	struct wsbg_workspace *workspace = NULL;
	struct wsbg_workspace *needle = wl_container_of(last->next, needle, link);
	for (; &needle->link != &state->workspaces;
			needle = wl_container_of(needle->link.next, needle, link)) {
		bool name_matches = strcmp(needle->name, name) == 0;
		bool output_matches = strcmp(needle->output, output) == 0;
		if (!name_matches && !output_matches) {
			continue;
		}
		wl_list_remove(&needle->link);
		workspace = needle;
		if (!name_matches) {
			free(workspace->name);
			workspace->name = NULL;
		}
		if (!output_matches) {
			free(workspace->output);
			workspace->output = NULL;
		}
		break;
	}
	if (!workspace && !(workspace = calloc(1, sizeof *workspace))) {
		goto err;
	}
	if (!workspace->name || !workspace->output) {
		if (!workspace->name && !(workspace->name = strdup(name))) {
			goto err;
		}
		if (!workspace->output && !(workspace->output = strdup(output))) {
			goto err;
		}
		struct wsbg_output *output;
		wl_list_for_each(output, &state->outputs, link) {
			if (!output->name || strcmp(output->name, workspace->output) != 0) {
				continue;
			}
			if (output->config && output->config->workspace &&
					strcmp(output->config->workspace, workspace->name) == 0) {
				break;
			}
			struct wsbg_config *config;
			wl_list_for_each(config, &output->configs, link) {
				if (!config->workspace) {
					output->buffer_change = output->buffer_change || (output->config != config);
					output->config = config;
				} else if (strcmp(config->workspace, workspace->name) == 0) {
					output->buffer_change = true;
					output->config = config;
					break;
				}
			}
			break;
		}
	}
	wl_list_insert(last, &workspace->link);
	return workspace;
err:
	wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
	if (workspace) {
		if (workspace->name) {
			free(workspace->name);
		}
		if (workspace->output) {
			free(workspace->output);
		}
		free(workspace);
	}
	return NULL;
}

const char *handle_sway_workspaces(struct wsbg_state *state, char *json_buffer, size_t json_size) {
	struct json_state s;
	struct wl_list *last = &state->workspaces;
	json_init(&s, json_buffer, json_size);
	if (!json_list(&s)) {
		return s.err ? s.err : "Root is not a list";
	}
	while (!json_end_list(&s)) {
		char *name = NULL, *output = NULL;
		bool visible = false;
		if (!json_object(&s)) {
			return s.err ? s.err : "Element is not an object";
		}
		while (!json_end_object(&s)) {
			if (!name && json_key(&s, "name")) {
				if (!json_get_string(&s, json_buffer, &json_size, true)) {
					return s.err ? s.err : "'name' is not a string";
				}
				name = json_buffer;
				json_buffer += json_size;
			} else if (!output && json_key(&s, "output")) {
				if (!json_get_string(&s, json_buffer, &json_size, true)) {
					return s.err ? s.err : "'output' is not a string";
				}
				output = json_buffer;
				json_buffer += json_size;
			} else if (!visible && json_key(&s, "visible")) {
				if (json_true(&s)) {
					visible = true;
				} else {
					json_skip_value(&s);
				}
			} else {
				json_skip_key_value_pair(&s);
			}
		}
		if (name && output && visible) {
			struct wsbg_workspace *workspace = update_workspace(state, last, name, output);
			if (!workspace) {
				return NULL;
			}
			last = &workspace->link;
		}
	}
	while (last->next != &state->workspaces) {
		struct wsbg_workspace *workspace = wl_container_of(last->next, workspace, link);
		wl_list_remove(&workspace->link);
		free(workspace->name);
		free(workspace->output);
		free(workspace);
	}
	return s.err;
}

const char *handle_sway_workspace_event(struct wsbg_state *state, char *buffer, size_t size) {
	struct json_state s;
	json_init(&s, buffer, size);
	if (!json_object(&s)) {
		return s.err ? s.err : "Root is not an object";
	}
	char *name = NULL, *output = NULL;
	bool update = false;
	while (!json_end_object(&s)) {
		if (json_key(&s, "change")) {
			if (!(json_string(&s, "init") ||
					json_string(&s, "focus") ||
					json_string(&s, "move") ||
					json_string(&s, "rename"))) {
				return s.err;
			}
			update = true;
		} else if (json_key(&s, "current")) {
			if (!json_object(&s)) {
				return s.err ? s.err : "'current' is not an object";
			}
			while (!json_end_object(&s)) {
				if (json_key(&s, "name")) {
					if (!json_get_string(&s, buffer, &size, true)) {
						return s.err ? s.err : "'current.name' is not a string";
					}
					name = buffer;
					buffer += size;
				} else if (json_key(&s, "output")) {
					if (!json_get_string(&s, buffer, &size, true)) {
						return s.err ? s.err : "'current.output' is not a string";
					}
					output = buffer;
					buffer += size;
				} else {
					json_skip_key_value_pair(&s);
				}
			}
		} else {
			json_skip_key_value_pair(&s);
		}
	}
	if (update && name && output) {
		update_workspace(state, &state->workspaces, name, output);
	}
	return s.err;
}

int main(int argc, char **argv) {
	wsbg_log_init(LOG_DEBUG);

	struct wsbg_state state = {0};
	wl_list_init(&state.options);
	wl_list_init(&state.outputs);
	wl_list_init(&state.workspaces);
	wl_list_init(&state.images);

	parse_command_line(argc, argv, &state);


	state.display = wl_display_connect(NULL);
	if (!state.display) {
		wsbg_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	if (wl_display_roundtrip(state.display) == -1) {
		wsbg_log(LOG_ERROR, "wl_display_roundtrip failed");
		return 1;
	}
	if (state.compositor == NULL || state.shm == NULL ||
			state.layer_shell == NULL || state.viewporter == NULL) {
		wsbg_log(LOG_ERROR, "Missing a required Wayland interface");
		return 1;
	}


	struct sway_ipc_state sway_ipc_state;
	sway_ipc_open(&sway_ipc_state);
	sway_ipc_send(&sway_ipc_state, SWAY_IPC_SUBSCRIBE, "[\"workspace\"]");
	sway_ipc_send(&sway_ipc_state, SWAY_IPC_GET_WORKSPACES, NULL);

	struct pollfd display_pfd = {
		.fd = wl_display_get_fd(state.display),
		.events = POLLOUT
	};

	struct pollfd pfd[] = {
		{ .fd = display_pfd.fd, .events = POLLIN },
		{ .fd = sway_ipc_state.fd, .events = POLLIN }
	};

	while (true) {
		while (wl_display_prepare_read(state.display) == -1) {
			if (wl_display_dispatch_pending(state.display) == -1) {
				goto exit;
			}
		}

		while (wl_display_flush(state.display) == -1) {
			if (errno == EAGAIN) {
				while (poll(&display_pfd, 1, -1) == -1) {
					if (errno == EINTR) {
						continue;
					}
					goto cancel_read_and_exit;
				}
				continue;
			} else if (errno == EPIPE) {
				break;
			}
			goto cancel_read_and_exit;
		}

		while (poll(pfd, sizeof pfd / sizeof *pfd, -1) == -1) {
			if (errno == EINTR) {
				continue;
			}
			goto cancel_read_and_exit;
		}

		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(state.display) == -1 ||
					wl_display_dispatch_pending(state.display) == -1) {
				goto exit;
			}
		} else {
			wl_display_cancel_read(state.display);
		}

		if (pfd[1].revents & POLLIN) {
			struct sway_ipc_message response;
			while (sway_ipc_recv(&sway_ipc_state, &response)) {
				const char *error = NULL;
				if (response.type == SWAY_IPC_GET_WORKSPACES) {
					error = handle_sway_workspaces(
							&state, response.payload, response.size);
				} else if (response.type == SWAY_IPC_EVENT_WORKSPACE) {
					error = handle_sway_workspace_event(
							&state, response.payload, response.size);
				}
				if (error) {
					wsbg_log(LOG_ERROR, "Sway IPC error: %s", error);
				}
			}
		}

		// Determine which images need to be loaded
		struct wsbg_output *output;
		wl_list_for_each(output, &state.outputs, link) {
			if (output->configured && output->config->image && output->buffer_change) {
				output->config->image->load_required = true;
			}
		}

		// Load images, render associated frames, and unload
		struct wsbg_image *image;
		wl_list_for_each(image, &state.images, link) {
			if (!image->load_required) {
				continue;
			}

			cairo_surface_t *surface = load_background_image(image->path);
			if (!surface) {
				wsbg_log(LOG_ERROR, "Failed to load image: %s", image->path);
				continue;
			}

			wl_list_for_each(output, &state.outputs, link) {
				if (output->configured && output->buffer_change &&
						output->config->image == image) {
					render_frame(output, surface);
				}
			}

			image->load_required = false;
			cairo_surface_destroy(surface);
		}

		// Redraw outputs without associated image
		wl_list_for_each(output, &state.outputs, link) {
			if (output->configured && output->buffer_change) {
				render_frame(output, NULL);
			}
		}
	}

cancel_read_and_exit:
	wl_display_cancel_read(state.display);
exit:
	sway_ipc_close(&sway_ipc_state);

	struct wsbg_output *output, *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &state.outputs, link) {
		destroy_wsbg_output(output);
	}

	struct wsbg_option *option, *tmp_option;
	wl_list_for_each_safe(option, tmp_option, &state.options, link) {
		destroy_wsbg_option(option);
	}

	struct wsbg_workspace *workspace, *tmp_workspace;
	wl_list_for_each_safe(workspace, tmp_workspace, &state.workspaces, link) {
		destroy_wsbg_workspace(workspace);
	}

	struct wsbg_image *image, *tmp_image;
	wl_list_for_each_safe(image, tmp_image, &state.images, link) {
		destroy_wsbg_image(image);
	}

	return 0;
}
