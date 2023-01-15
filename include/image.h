#ifndef _WSBG_IMAGE_H
#define _WSBG_IMAGE_H
#include <stdint.h>
#include "state.h"

bool parse_mode(
		const char *str,
		enum background_mode *mode,
		struct wsbg_size *position);

bool parse_position(const char *str, struct wsbg_size *position);

void get_wsbg_image_transform(
		struct wsbg_image *image,
		enum background_mode mode,
		struct wsbg_size position,
		int32_t width, int32_t height,
		struct wsbg_image_transform *transform,
		bool *covered);

bool load_image(struct wsbg_image *image, struct wsbg_color background);
void unload_image(struct wsbg_image *image);

#endif
