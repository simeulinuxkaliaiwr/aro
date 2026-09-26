<p align="center">
  <img src="docs/aro-banner.svg" alt="aro" width="100%">
</p>

<p align="center">
  A tiling window manager for Wayland, built on wlroots.<br>
  Minimal, flat, one accent colour, spring-animated.
</p>

<p align="center">
  <a href="https://github.com/simeulinuxkaliaiwr/aro/actions/workflows/build.yml"><img alt="build" src="https://img.shields.io/github/actions/workflow/status/simeulinuxkaliaiwr/aro/build.yml?branch=main&label=build&labelColor=12161d"></a>
  <a href="LICENSE"><img alt="license: MIT" src="https://img.shields.io/badge/license-MIT-222a35?labelColor=12161d"></a>
  <img alt="wlroots 0.20" src="https://img.shields.io/badge/wlroots-0.20-222a35?labelColor=12161d">
  <img alt="written in C" src="https://img.shields.io/badge/written_in-C-222a35?labelColor=12161d">
</p>

*aro* is Portuguese for the rim of a pair of glasses. It names the 1px
accent ring just inside every focused border — the one thing on screen that
says "this is the window you are in".

## What it does

- **Tiling, split on demand.** New windows open beside the focused one, or
  wherever `mod+v` / `mod+s` said; `dwindle` splits along the longer axis
  instead, so the layout spirals on its own.
- **Spatial focus.** `mod+hjkl` moves to the window that is actually up,
  left or right on screen, not to the next one in a tree, and across
  monitors by the same rule.
- **Drag to rearrange.** Pull a tiled window loose and a preview shows the
  slot it would drop into. Drag a border to move the boundary two windows
  share — both sides follow the cursor at once.
- **Floating when it matters.** Dialogs and fixed-size windows float on
  their own, at the size they asked for; `mod+space` for anything else.
- **One workspace set per monitor**, sway-style, created as you use them.
  A VT switch, or unplugging the only screen, keeps layouts and split
  ratios intact.
- **A window switcher.** `mod+tab` walks every window in recently-used
  order; a quick tap swaps back without the card ever appearing.
- **Live config.** Saving the file applies immediately; a line that does
  not parse says so on screen, with its line number.
- **Its own wallpaper**, drawn by `aropaper` at each screen's exact
  resolution, and a status bar you can turn off.
- **Window rules**, monitor configuration, an exit prompt, and rounded
  corners through [SceneFX](https://github.com/wlrfx/scenefx). Layer shell,
  XWayland, session lock, idle inhibit, clipboard, drag and drop,
  screencopy and xdg-decoration all work.

## Building

Needs **wlroots 0.20**, **SceneFX 0.5**, wayland-protocols, libxkbcommon,
pixman and pangocairo; `aropaper` also needs gdk-pixbuf. On Arch:

```sh
pacman -S --needed base-devel meson ninja wayland wayland-protocols \
    wlroots0.20 libxkbcommon pixman pango cairo gdk-pixbuf2 librsvg
# scenefx is in the AUR: scenefx or scenefx-git
```

`librsvg` is only what lets `aropaper` read SVG. Without it the default
wallpaper falls back to the default wallpaper.

```sh
# installing it system-wide
meson setup build --prefix=/usr --buildtype=release
ninja -C build
sudo ninja -C build install

# just building it
meson setup build
ninja -C build
```

Options:

- `-Deffects=false` builds against plain `wlr_scene`: no rounded corners,
  no SceneFX needed.
- `-Dxwayland=disabled` drops X11 support.
- `-Dwallpaper=disabled` skips `aropaper`.
- `-Dcompositor=false` builds only what needs no wlroots: `aro-layout`, the
  layout tree's test harness, and `aropaper`.

## Running

From a TTY:

```sh
aro # if installed, if not: ./build/aro
```

Nested inside another compositor, to try it out:

```sh
aro -m alt -s foot # If not installed, ./build/aro
```

`-m alt` because your main compositor is already using the SUPER key.

## Configuration

`~/.config/aro/config`, `key = value`, watched: saving applies it. Copy
[`config.example`](data/config.example) — installed to
`/usr/share/doc/aro/config.example` — and delete what you do not want.

```
mod = super
gaps = 9
accent = 0xe6a54bff
wallpaper = ~/pictures/wallpaper.jpg
bar = false
monitor eDP-1 {
    scale = auto
    position = auto
}
rule = app_id:mpv  workspace 3
rule = app_id:firefox  title:Picture-in-Picture  float
bind = mod+Return, spawn, foot
```

Every window logs `map: app_id="…" title="…"` and every screen logs
`output: name="…" desc="…"` as they appear, which is how you find what to
write a rule or a monitor block against.

### Wallpaper

aro runs `aropaper` itself, so there is a wallpaper with no config at all.
`wallpaper = auto` is aro's own, a path shows that image (PNG, JPEG or
SVG, covering the screen and cropping the overflow), and `wallpaper = none`
leaves the background to you — `exec = swaybg -i …`, for instance.

`aropaper` is an ordinary layer-shell client, so it also works on any compositor that has layer shell — sway, Hyprland, niri and river among them, though not GNOME

```sh
aropaper ~/pictures/wallpaper.jpg
```

### Default bindings

Mod is Super, or Alt with `mod = alt`. The first `bind` line in your config
replaces this table entirely.

| bind | action |
| --- | --- |
| `mod+Return` / `mod+d` | foot / fuzzel |
| `mod+v` / `mod+s` | next window opens right / below (one-shot) |
| `mod+hjkl` | move focus |
| `mod+shift+hjkl` | move the window |
| `mod+ctrl+hjkl` | resize |
| `mod+tab` / `mod+shift+tab` | window switcher, recently used order |
| `mod+space` | toggle floating |
| `mod+f` | toggle fullscreen |
| `mod+1..4` | workspace |
| `mod+shift+1..4` | send window to workspace |
| `mod+q` | close |
| `mod+shift+e` | quit (asks first) |
| `mod+drag` | move; a tiled window tears out of the tree |
| `mod+right-drag` | resize |
| drag a header / a border | move / resize, no modifier |

## License

MIT — see [LICENSE](LICENSE).
