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
#include "buffer.h"
#include "image.h"
#include "json.h"
#include "log.h"
#include "state.h"
#include "sway-ipc.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"

const struct wsbg_color default_color = {
	.r = 0x00, .b = 0x00, .g = 0x00, .a = 0xFF };

static bool parse_color(const char *str, struct wsbg_color *color) {
	int len = strlen(str);
	if (len == 7 && str[0] == '#') {
		++str;
	} else if (len != 6) {
		return false;
	}
	uint8_t rgb[3] = {};
	for (unsigned i = 0; i < 6; ++i) {
		if ((i & 1)) {
			rgb[i >> 1] <<= 4;
		}
		if ('0' <= str[i] && str[i] <= '9') {
			rgb[i >> 1] += str[i] - '0';
		} else if ('a' <= str[i] && str[i] <= 'f') {
			rgb[i >> 1] += str[i] - 'a' + 0xA;
		} else if ('A' <= str[i] && str[i] <= 'F') {
			rgb[i >> 1] += str[i] - 'A' + 0xA;
		} else {
			return false;
		}
	}
	color->r = rgb[0];
	color->g = rgb[1];
	color->b = rgb[2];
	color->a = 0xFF;
	return true;
}

static void render_buffer(struct wsbg_output *output) {
	if (!output->config->buffer) {
		return;
	}

	wl_surface_attach(output->surface, output->config->buffer->buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);

	struct wp_viewport *viewport = wp_viewporter_get_viewport(
			output->state->viewporter, output->surface);
	wp_viewport_set_destination(viewport, output->width, output->height);

	wl_surface_commit(output->surface);

	wp_viewport_destroy(viewport);
}

static void render_frame(struct wsbg_output *output,
		struct wsbg_config *config) {
	int32_t width, height;
	if (output->fractional_scale) {
		width = (output->width * output->scale_120 + 60) / 120;
		height = (output->height * output->scale_120 + 60) / 120;
	} else {
		// Rotate buffer to match output
		if ((output->mode_width < output->mode_height) ==
				(output->width < output->height)) {
			width = output->mode_width;
			height = output->mode_height;
		} else {
			width = output->mode_height;
			height = output->mode_width;
		}
	}

	struct wsbg_buffer *buffer =
		get_wsbg_buffer(config, output->state, width, height);

	release_wsbg_buffer(config->buffer);
	config->buffer = buffer;
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
	release_wsbg_buffer(config->buffer);
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
	if (output->fractional_scale != NULL) {
		wp_fractional_scale_v1_destroy(output->fractional_scale);
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
		if (output->fractional_scale) {
			if (output->width != width || output->height != height) {
				output->buffer_change = true;
			}
		} else {
			// Flag when output rotation changes
			if ((width < height) != (output->width < output->height)) {
				output->buffer_change = true;
			}
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

static void output_preferred_scale(void *data,
	struct wp_fractional_scale_v1 *fractional_scale, uint32_t scale_120)
{
	struct wsbg_output *output = data;
	if (output->scale_120 != scale_120) {
		output->scale_120 = scale_120;
		output->buffer_change = true;
	}
}

static const struct wp_fractional_scale_v1_listener
	fractional_scale_listener = {
	.preferred_scale = output_preferred_scale,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	struct wsbg_output *output = data;
	if (output->fractional_scale) {
		return;
	}
	if (output->mode_width != width || output->mode_height != height) {
		output->mode_width = width;
		output->mode_height = height;
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

	if (output->state->fractional_scale_manager) {
		output->scale_120 = 120;
		output->fractional_scale =
			wp_fractional_scale_manager_v1_get_fractional_scale(
				output->state->fractional_scale_manager, output->surface);
		assert(output->fractional_scale);
		wp_fractional_scale_v1_add_listener(output->fractional_scale,
			&fractional_scale_listener, output);
	}

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

	output->buffer_change = true;

	struct wl_list configs;
	wl_list_init(&configs);

	struct wsbg_config *default_config = malloc(sizeof *default_config);
	if (!default_config) {
		wsbg_log_errno(LOG_ERROR, "Memory allocation failed");
		return;
	}
	*default_config = (struct wsbg_config){
		.color = default_color,
		.mode = BACKGROUND_MODE_FILL,
		.position = { .x = Q16 / 2, .y = Q16 / 2 },
	};
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
							*config = *default_config;
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
			} else if (option->type == WSBG_POSITION) {
				wl_list_for_each(config, &configs, link) {
					config->position = option->value.size;
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
			wp_fractional_scale_manager_v1_interface.name) == 0) {
		state->fractional_scale_manager = wl_registry_bind(registry, name,
			&wp_fractional_scale_manager_v1_interface, 1);
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
		{"position", required_argument, NULL, 'p'},
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
		"  -p, --position         Set the position of the image."
		"  -v, --version          Show the version number and quit.\n"
		"  -w, --workspace        Set the workspace to operate on or * for all.\n"
		"\n"
		"Background Modes:\n"
		"  stretch, fit, fill, center, tile, or solid_color\n"
		"\n"
		"Background Positions:\n"
		"  center, left, right, top, bottom, or (top|bottom)/(left|right)\n";

	int c;
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "c:hi:m:o:p:vw:", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c': { // color
			struct wsbg_color color;
			if (!parse_color(optarg, &color)) {
				wsbg_log(LOG_ERROR, "Invalid color: %s "
					"(color should be specified as rrggbb or #rrggbb)", optarg);
				continue;
			}
			wsbg_option_new(state, WSBG_COLOR)->value.color = color;
			break;
		}
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
				wl_list_init(&image->buffers);
				wl_list_insert(&state->images, &image->link);
			}
			wsbg_option_new(state, WSBG_IMAGE)->value.image = image;
			break;
		}
		case 'm':  // mode
			{
				enum background_mode mode;
				struct wsbg_size position;
				if (!parse_mode(optarg, &mode, &position)) {
					wsbg_log(LOG_ERROR, "Invalid mode: %s", optarg);
					break;
				}
				wsbg_option_new(state, WSBG_MODE)
					->value.mode = mode;
				wsbg_option_new(state, WSBG_POSITION)
					->value.size = position;
			}
			break;
		case 'o':  // output
			wsbg_option_select(state, WSBG_OUTPUT, optarg);
			break;
		case 'p':  // position
			{
				struct wsbg_size position;
				if (!parse_position(optarg, &position)) {
					wsbg_log(LOG_ERROR, "Invalid position: %s", optarg);
					break;
				}
				wsbg_option_new(state, WSBG_POSITION)
					->value.size = position;
			}
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
					output->config_change = output->config_change || (output->config != config);
					output->config = config;
				} else if (strcmp(config->workspace, workspace->name) == 0) {
					output->config_change = true;
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
	wl_list_init(&state.colors);

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

		struct wsbg_output *output;
		wl_list_for_each(output, &state.outputs, link) {
			if (!output->configured) {
				continue;
			}
			if (output->buffer_change) {
				struct wsbg_config *config;
				wl_list_for_each(config, &output->configs, link) {
					render_frame(output, config);
				}
			}
			if (output->buffer_change || output->config_change) {
				render_buffer(output);
				output->buffer_change = false;
				output->config_change = false;
			}
		}

		struct wsbg_image *image;
		wl_list_for_each(image, &state.images, link) {
			unload_image(image);
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
