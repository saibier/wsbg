# wsbg

wsbg is a wallpaper utility for [Sway](https://swaywm.org/) which supports
per-workspace configuration. It was forked from [swaybg](https://github.com/swaywm/swaybg).

See the man page, `wsbg(1)`, for instructions on using wsbg.

## Installation

Install dependencies:

* meson \*
* wayland
* wayland-protocols \*
* cairo
* gdk-pixbuf2 (optional: image formats other than PNG)
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional: man pages) \*
* git (optional: version information) \*

_\* Compile-time dep_

Run these commands:

    meson build/
    ninja -C build/
    sudo ninja -C build/ install
