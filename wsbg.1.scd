wsbg(1)

# NAME

wsbg - Background for *sway*(1) workspaces

# SYNOPSIS

*wsbg* [options...]

Displays a background image on all outputs of your Wayland session.

Without an output or workspace specified, appearance options apply to all
outputs and workspaces. Per-output and workspace appearance options can be set
by passing _-o, --output_ and/or  _-w, --workspace_ followed by these options.

# CONFIGURATION

It is recommended to configure wsbg in your sway config file (see
*sway*(5)). While wsbg can be used as a drop-in replacement for *swaybg*(1) by
setting _swaybg_command wsbg_, this will not allow for per-workspace or other
wsbg specific configuration. The recommended alternative is to start wsbg using
_exec_always_ and pass _-r, --exit-on-reload_ like so:

```
swaybg_command - # Disable swaybg
exec_always wsbg -r [options...]
```

# OPTIONS

*-c, --color* <[#]rrggbb>
	Set the background color.

*-h, --help*
	Show help message and quit.

*-i, --image* <path>
	Set the background image.

*-m, --mode* <mode>
	Scaling mode for images: _stretch_, _fill_, _fit_, _center_, or _tile_.
	Use the additional mode _solid\_color_ to display only the background
	color, even if a background image is specified.

*-o, --output* <name>
	Select an output to configure. Subsequent appearance options will only
	apply to this output. The special value _\*_ selects all outputs.
	Consecutive _-o, --output_ flags are appended to the selected output(s)
	rather than replacing them.

*-p, --position* <position>
	Position for images: _center_, _left_|_right_,
	_top_|_bottom_[/<_left_|_right_>].

*-r, --exit-on-reload*
	Exit when sway config is reloaded. Can be used in conjunction with sway's
	_exec_always_ config command to exit and restart when sway's config is
	reloaded.

*-v, --version*
	Show the version number and quit.

*-w, --workspace* <name>
	Select a workspace to configure. Subsequent appearance options will
	only apply to this workspace. The special value _\*_ selects all
	workspaces. Consecutive _-w, --workspace_ flags are appended to the
	selected workspace(s) rather than replacing them.

# AUTHORS

Maintained by Isaiah Bierbrauer <isaiah@isaiahbierbrauer.com>. For more
information about wsbg development, see https://isaiahbierbrauer.com/wsbg.

wsbg is a fork of swaybg. For more information about swaybg, see
https://github.com/swaywm/swaybg.
