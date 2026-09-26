# Contributing to aro

Pull requests are welcome, no issue needed first. For a bug, a clear
description in the PR is enough; for a behaviour change, say what it was
like before and why the new way is better.

Not sure whether an idea fits, or stuck on something? Ask in
[Discussions](https://github.com/simeulinuxkaliaiwr/aro/discussions)
first; bugs go in issues.

## Scope

aro is meant to stay small: minimal, flat, one accent colour. A feature
that adds a new look, a second way of doing something aro already does,
or a pile of options is likely to be turned down, however well it is
written. Fixes, protocol support that makes real apps work, and polish on
what is already there are always in scope. If you are unsure, open the PR
anyway and say so.

## Building

Dependencies are listed in the [README](README.md#building). For
development, a plain build is enough:

```sh
meson setup build
ninja -C build
```

No SceneFX? Build without it:

```sh
meson setup build -Deffects=false
```

## Running your build

Nested inside your current session, with Alt as the modifier so it does
not fight your main compositor over Super:

```sh
./build/aro -m alt -s foot
```

A nested aro logs to `~/.local/state/aro/aro-nested.log`, so it never
touches your real session's log. `./build/aro -c` checks a config file
without starting anything.

## Testing

CI builds with warnings as errors and with sanitizers on, once with
XWayland and once without. To catch what it would before you push:

```sh
meson setup build-ci --werror -Db_sanitize=address,undefined -Deffects=false
ninja -C build-ci
```

The layout tree has a test harness that needs no compositor. It is
interactive: `v` / `s` split, `hjkl` move focus, `HJKL` resize, `q` close,
`x` quit. CI feeds it keys, and so can you:

```sh
printf 'vsvshjklHJKLqq' | ./build-ci/aro-layout > /dev/null
```

If you touch code behind `ARO_XWAYLAND`, also build with
`-Dxwayland=disabled`. CI cannot build with SceneFX, so if you touch code
behind `ARO_EFFECTS`, build with it locally too.

## Where things are

```
src/layout/      the tiling tree (layout.c) and spring animation (anim.c)
src/compositor/  aro.c is the compositor proper: views, focus, workspaces,
                 outputs, input, grabs, config reload, main(). Also
                 config, ipc (aroctl's server side), lock, idle, ime,
                 logfile, wallpaper.
src/ui/          everything aro draws: frames (ui.c), bar, overview,
                 switcher, exit prompt, notifications, drop preview, text
src/aroctl/      the command-line client; talks to aro over a unix socket
src/aropaper/    the wallpaper client; plain Wayland, no wlroots
```

`aro.c` is large; searching for the function you want is faster than
reading it top to bottom.

## Rules the compiler will not catch

- `src/layout/` stays free of wlroots and Wayland. That is what lets
  CI test it, and what keeps the tree logic readable.
- `scene.h` is the first include in any file that uses the scene graph.
  It picks SceneFX or plain `wlr_scene` depending on `-Deffects`.
- Files that need `_GNU_SOURCE` define it before every include.
- xdg-shell and XWayland differences go behind `struct view_impl` in
  `aro.h`. `src/ui/` should not know which kind of window it is drawing.
- Code for optional features stays inside its `#ifdef ARO_XWAYLAND` or
  `#ifdef ARO_EFFECTS`.

## Style

Match the code around you: C11, tabs, lines kept near 80 columns.
Comments are one line, short and lowercase, and say why rather than
what. Names carry their module's prefix (`aro_`, `ly_`, `q_`, and so on).

Commit messages are one short line saying what changed, like
`Add drag+drop in overview`. One logical change per commit.

## Credit

Your commits are your credit; there is no AUTHORS file to update.
Contributions are accepted under the project's [MIT license](LICENSE).
