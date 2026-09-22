#!/bin/sh
# aro — one-shot move from a flat src/ into src/layout, src/compositor,
# src/ui, plus data/ and docs/. Run from the repository root, once, then
# delete it.
#
# No #include line changes: meson.build puts every source directory on the
# include path, so headers are still included by their own name. The only
# file that had to know about the new paths is meson.build, and the copy in
# this commit already does.
#
# It ends by checking the result rather than trusting it: every source
# meson.build names must exist, nothing may be left loose in src/, and the
# tree must build.

set -e

[ -f meson.build ] && [ -d src ] || {
	echo "run this from the repository root" >&2
	exit 1
}

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	echo "uncommitted changes — commit or stash first, so this is one step" >&2
	exit 1
fi

mkdir -p src/layout src/compositor src/ui data docs

mv_to() {
	dir=$1
	shift
	for f in "$@"; do
		[ -e "src/$f" ] || { echo "missing: src/$f" >&2; exit 1; }
		git mv "src/$f" "$dir/$f"
	done
}

# The part with no wlroots in it. Already its own static library; now its
# own directory, which is the only split here that a build rule enforces.
mv_to src/layout layout.c layout.h anim.c anim.h layout_harness.c

# Wayland, outputs, input, state.
mv_to src/compositor aro.c aro.h config.c config.h idle.c idle.h lock.c lock.h

# Everything that knows what anything looks like.
mv_to src/ui ui.c theme.h text.c text.h bar.c bar.h notify.c notify.h \
      prompt.c prompt.h

# src/scene.h stays where it is: it is the project-wide switch between
# wlr_scene and scenefx, and belongs to no one layer.

for f in aro.desktop config.example portals.conf; do
	[ -e "$f" ] && git mv "$f" "data/$f"
done
[ -e screen.jpg ] && git mv screen.jpg docs/screen.jpg

# The video is too big for git and GitHub hosts it for you when you drop it
# into the README editor. Out of the index, still on disk, never committed.
if [ -e demo.mp4 ]; then
	git rm --cached -q demo.mp4 2>/dev/null || true
	mkdir -p docs
	mv demo.mp4 docs/demo.mp4
	grep -qx 'docs/demo.mp4' .git/info/exclude 2>/dev/null ||
		echo 'docs/demo.mp4' >> .git/info/exclude
fi

echo
echo "── checking ──"

fail=0

# every source meson.build names
for f in $(grep -o "'src/[a-z_/]*\.c'" meson.build | tr -d "'" | sort -u); do
	[ -e "$f" ] || { echo "meson.build names a missing file: $f"; fail=1; }
done

# nothing left loose in src/ but the scene switch
for f in src/*.c src/*.h; do
	[ -e "$f" ] || continue
	[ "$f" = src/scene.h ] && continue
	echo "still loose in src/: $f"
	fail=1
done

for f in data/aro.desktop data/config.example data/portals.conf; do
	[ -e "$f" ] || { echo "missing: $f"; fail=1; }
done

[ "$fail" = 0 ] || { echo "not building: fix the above first" >&2; exit 1; }

# meson caches absolute paths, so a moved tree needs a fresh build dir
rm -rf build
meson setup build
ninja -C build

echo
echo "moved and built. Now:"
echo "  git add -A && git commit --amend --no-edit"
echo "  rm restructure.sh"
