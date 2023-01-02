#ifndef _WSBG_BUFFER_H
#define _WSBG_BUFFER_H
#include <stdint.h>
#include "state.h"

struct wsbg_buffer *get_wsbg_buffer(
		struct wsbg_config *config,
		struct wsbg_state *state,
		int32_t width, int32_t height);

void release_wsbg_buffer(struct wsbg_buffer *buffer);

#endif
