#ifndef _WSBG_IMAGE_H
#define _WSBG_IMAGE_H
#include <stdint.h>
#include "state.h"

enum background_mode parse_background_mode(const char *mode);

void get_wsbg_image_transform(
		struct wsbg_image *image,
		enum background_mode mode,
		int32_t width, int32_t height,
		struct wsbg_image_transform *transform,
		bool *covered);

bool load_image(struct wsbg_image *image, struct wsbg_color background);
void unload_image(struct wsbg_image *image);

#endif
