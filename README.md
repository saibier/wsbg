# wsbg

wsbg is a wallpaper utility for [Sway](https://swaywm.org/) which supports
per-workspace configuration. It was forked from [swaybg](https://github.com/swaywm/swaybg).

See the man page, [`wsbg(1)`](wsbg.1.scd), for instructions on using wsbg.

## Installation

Install dependencies:

* meson \*
* wayland
* wayland-protocols \*
* pixman
* gdk-pixbuf2 (optional: image formats other than PNG)
* libpng (required if gdk-pixbuf2 is not available)
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional: man pages) \*
* git (optional: version information) \*

_\* Compile-time dep_

Run these commands:

    meson setup build/
    ninja -C build/
    sudo ninja -C build/ install
