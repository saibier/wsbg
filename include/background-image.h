#ifndef _WSBG_BACKGROUND_IMAGE_H
#define _WSBG_BACKGROUND_IMAGE_H
#include <stdint.h>
#include "state.h"

enum background_mode parse_background_mode(const char *mode);

void unload_image(struct wsbg_image *image);

struct wsbg_buffer *get_wsbg_buffer(
		struct wsbg_config *config,
		struct wsbg_state *state,
		int32_t width, int32_t height);

void release_wsbg_buffer(struct wsbg_buffer *buffer);

#endif
