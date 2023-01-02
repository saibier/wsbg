#ifndef _WSBG_CAIRO_UTIL_H
#define _WSBG_CAIRO_UTIL_H

#include "state.h"

#if HAVE_GDK_PIXBUF
void load_gdk_pixbuf(struct wsbg_image *image);
#endif

#endif
