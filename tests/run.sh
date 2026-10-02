#!/usr/bin/env bash
# run.sh ARO AROCTL CLIENTDIR [multi]: start aro headless and run the test clients; multi uses two monitors

set -u
ARO=$1 CTL=$2 CLIENTS=$3 MODE=${4:-main}
OUTPUTS=1
[ "$MODE" = multi ] && OUTPUTS=2

# short on purpose: a unix socket path must fit in 108 bytes
T=$(mktemp -d /tmp/aro-test.XXXXXX)
PID=
FAILED=0

cleanup() {
	[ -n "$PID" ] && kill "$PID" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$T/config/aro" "$T/run" "$T/state"
chmod 700 "$T/run"

config() {
	printf '%s\n' "confirm_quit = false" "wallpaper = none" "$@" > "$T/config/aro/config"
}

ok() { echo "ok    $1"; }
bad() {
	echo "FAIL  $1"
	FAILED=1
}

alive() {
	kill -0 "$PID" 2>/dev/null && return 0
	echo "aro is gone; the end of its output:"
	tail -n 30 "$T/aro.err"
	exit 1
}

sock() { ls "$T"/run/aro-*.sock 2>/dev/null | head -n 1; }

client() {
	env -i PATH="$PATH" HOME="$T" XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 \
		ASAN_OPTIONS=detect_leaks=0 timeout 30 "$CLIENTS/$@"
}

ctl() {
	env -i PATH="$PATH" XDG_RUNTIME_DIR="$T/run" ARO_SOCKET="$(sock)" timeout 5 "$CTL" "$@"
}

# a bogus DISPLAY makes aro act nested, so it leaves the session's portals alone
start_aro() {
	env -i PATH="$PATH" HOME="$T" DISPLAY=:aro-test \
		XDG_CONFIG_HOME="$T/config" XDG_RUNTIME_DIR="$T/run" XDG_STATE_HOME="$T/state" \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=$OUTPUTS ${1:+WLR_RENDERER=$1} \
		ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$ARO" -l "$T/aro.log" > "$T/aro.err" 2>&1 &
	PID=$!
	for _ in $(seq 100); do
		[ -S "$T/run/wayland-0" ] && [ -n "$(sock)" ] && return 0
		kill -0 "$PID" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

# wait until a background client prints a line
wait_for() {
	for _ in $(seq 100); do
		grep -q "$2" "$1" 2>/dev/null && return 0
		sleep 0.1
	done
	return 1
}

config
# pixman needs no GPU; a SceneFX build needs GLES, so it falls back to the default
if start_aro pixman; then
	ok "aro starts headless (pixman)"
elif grep -q SceneFX "$T/aro.err" && start_aro ""; then
	ok "aro starts headless (GLES, for SceneFX)"
else
	echo "FAIL  aro did not start:"
	tail -n 20 "$T/aro.err"
	exit 1
fi
ctl version > /dev/null && ok "aroctl answers" || bad "aroctl answers"

if [ "$MODE" = main ]; then

# protocols apps and tools rely on; explicit sync only exists on GPUs with timelines
client globals > "$T/globals.out"
for p in zwp_keyboard_shortcuts_inhibit_manager_v1 ext_workspace_manager_v1 zwp_pointer_gestures_v1 zwp_tablet_manager_v2 wp_tearing_control_manager_v1 ext_image_copy_capture_manager_v1 \
	zwlr_layer_shell_v1 zwlr_screencopy_manager_v1; do
	grep -q "^$p " "$T/globals.out" && ok "offers $p" || bad "offers $p"
done
grep -q "^xwayland_shell_v1 " "$T/globals.out" && bad "xwayland_shell hidden from other clients" \
	|| ok "xwayland_shell hidden from other clients"

# a sandboxed app (Flatpak) keeps the basics but none of the privileged protocols
client sandbox > "$T/sandbox.out" 2>&1 || bad "sandboxed client connects"
for p in wl_compositor xdg_wm_base wl_seat; do
	grep -qx "$p" "$T/sandbox.out" || bad "sandbox keeps $p"
done
for p in zwlr_screencopy_manager_v1 zwlr_layer_shell_v1 zwp_virtual_keyboard_manager_v1 \
	zwlr_data_control_manager_v1 ext_session_lock_manager_v1; do
	grep -qx "$p" "$T/sandbox.out" && bad "sandbox cannot see $p"
done
ok "sandboxed clients see only safe protocols"
alive

# an input method lets go of the keyboard, then types into a field: both
# crashed aro, whose handlers read the data wlroots sends as NULL (#1).
# a field only gets the input method with keyboard focus, so a virtual
# keyboard stays around; this runs early, before other tests move focus
if command -v wtype > /dev/null; then
	env -i PATH="$PATH" XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 wtype -s 4000 -k Shift_L &
	KB=$!
	sleep 0.5
	if client ime > "$T/ime.out" 2>&1 && grep -q "^committed xin chào" "$T/ime.out" \
		&& grep -q "^grab released" "$T/ime.out"; then
		ok "input method text reaches the field"
	else
		bad "input method text reaches the field: $(tail -n 1 "$T/ime.out")"
	fi
	kill "$KB" 2> /dev/null
	wait "$KB" 2> /dev/null
	alive
else
	echo "skip  input methods (no wtype)"
fi

# 1. popups with no parent: this crashed aro on every waybar tooltip
client popup-orphan > /dev/null && ok "parentless popups" || bad "parentless popups"
alive

# 2. a layer-shell tooltip is drawn, not just survived
client layer-popup 3 > "$T/popup.out" 2>&1 &
CP=$!
if wait_for "$T/popup.out" "tooltip drawn"; then
	ok "layer-shell tooltip configured"
	if command -v grim > /dev/null; then
		read -r X Y < <(sed -n 's/^tooltip placed at \([0-9-]*\),\([0-9-]*\),.*/\1 \2/p' "$T/popup.out")
		sleep 0.3
		px=$(env -i XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 \
			grim -g "$((X + 100)),$((Y + 60)) 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tx1 | tr -d ' \n')
		[ "$px" = "ff0000" ] && ok "layer-shell tooltip drawn (red at $((X + 100)),$((Y + 60)))" \
			|| bad "layer-shell tooltip drawn: pixel is '$px', expected ff0000"
	else
		echo "skip  layer-shell tooltip drawn (no grim)"
	fi
else
	bad "layer-shell tooltip configured"
	cat "$T/popup.out"
fi
wait "$CP" || bad "layer-popup client exited with an error"
alive

# 3. workspaces for bars, with the same rule as aro's own bar
ws() { client extws expect "$1" > /dev/null || bad "workspaces: $2"; }
ws "[1] 2 3 4" "four at start, first active"
client extws activate 3 && ws "1 2 [3] 4" "a bar switches to 3"
ctl dispatch workspace 7 > /dev/null && ws "1 2 3 4 5 6 [7]" "the list grows to 7"
ctl dispatch workspace 2 > /dev/null && ws "1 [2] 3 4" "and shrinks back"
ctl dispatch workspace 1 > /dev/null
ok "ext-workspace checks done"
alive

# 4. windows through every layout, then closing
client window 3 4 > "$T/win.out" 2>&1 &
CW=$!
if wait_for "$T/win.out" "3 windows open"; then
	n=$(ctl -j windows | grep -o '"id":' | wc -l)
	[ "$n" -eq 3 ] && ok "three windows mapped" || bad "three windows mapped: aroctl sees $n"
	grep -q "3 tiled" "$T/win.out" && ok "tiled windows are told they are tiled" \
		|| bad "tiled windows are told they are tiled: $(cat "$T/win.out")"
	for step in "layout monocle" "focus right" "focus left" "layout dwindle" \
		"layout manual" "layout monocle" "workspace 2" "workspace 1" "layout manual"; do
		ctl dispatch $step > /dev/null || bad "dispatch $step"
		sleep 0.15
		alive
	done
	ok "layouts, focus and workspace switches with windows open"

	# screen sharing: the captured frame shows the windows' colour, 203040
	sleep 0.5                       # let the last layout change settle
	c=$(client capture output 2>&1)
	case $c in *"pixel 203040"*) ok "screen capture shows the windows" ;;
		*) bad "screen capture shows the windows: $c" ;; esac
	if grep -q "^ext_foreign_toplevel_image_capture_source_manager_v1 " "$T/globals.out"; then
		c=$(client capture window aro-test 2>&1)
		case $c in *"pixel 203040"*) ok "single-window capture" ;;
			*) bad "single-window capture: $c" ;; esac
	else
		echo "skip  single-window capture (not offered with SceneFX)"
	fi
else
	bad "three windows mapped"
	cat "$T/win.out"
fi
wait "$CW" || bad "window client exited with an error"
n=$(ctl -j windows | grep -o '"id":' | wc -l)
[ "$n" -eq 0 ] && ok "windows closed" || bad "windows closed: aroctl still sees $n"
alive

# 4a. a modal dialog floats even without a parent window; a plain window tiles
client window 1 2 modal > "$T/modal.out" 2>&1 &
CM=$!
if wait_for "$T/modal.out" "1 windows open"; then
	ctl windows | grep -q "floating.*aro-test" && ok "modal dialog floats" || bad "modal dialog floats"
else
	bad "modal dialog floats: it never opened"
fi
wait "$CM" || bad "modal client exited with an error"
client window 1 2 > "$T/plain.out" 2>&1 &
CM=$!
if wait_for "$T/plain.out" "1 windows open"; then
	ctl windows | grep -q "tiled.*aro-test" && ok "plain window tiles" || bad "plain window tiles"
fi
wait "$CM"
alive

# 4e. a sticky window follows workspace switches until it is tiled again
client window 1 4 modal > "$T/sticky.out" 2>&1 &
CS=$!
if wait_for "$T/sticky.out" "1 windows open"; then
	wsof() { ctl -j windows | grep -o '"workspace":[0-9]*,"state":"[a-z]*"' | head -n 1; }
	ctl dispatch sticky > /dev/null
	ctl dispatch workspace 3 > /dev/null; sleep 0.3
	[ "$(wsof)" = '"workspace":3,"state":"sticky"' ] && ok "sticky window follows to workspace 3" \
		|| bad "sticky window follows to workspace 3: $(wsof)"
	ctl dispatch float > /dev/null
	ctl dispatch workspace 1 > /dev/null; sleep 0.3
	[ "$(wsof)" = '"workspace":3,"state":"tiled"' ] && ok "tiling it ends stickiness" \
		|| bad "tiling it ends stickiness: $(wsof)"
fi
wait "$CS"
alive

# 4e2. the float key frees a tiled window at float_scale, centred, and the app's
# next commit at its old tile size doesn't undo that (#2)
client window 1 4 > "$T/floatkey.out" 2>&1 &
CF=$!
if wait_for "$T/floatkey.out" "1 windows open"; then
	ctl dispatch float > /dev/null; sleep 0.6
	read -r fw fx < <(ctl -j windows | grep -o '"x":-\?[0-9]*,"y":-\?[0-9]*,"width":[0-9]*' | head -n 1 \
		| sed -E 's/"x":(-?[0-9]*),"y":-?[0-9]*,"width":([0-9]*)/\2 \1/')
	ow=$(ctl -j outputs | grep -o '"width":[0-9]*' | head -n 1 | cut -d: -f2)
	if [ -n "$fw" ] && [ "$fw" -lt $((ow * 6 / 10)) ] && [ "$fw" -gt $((ow * 4 / 10)) ] &&
	   [ $((2 * fx + fw - ow)) -le 2 ] && [ $((2 * fx + fw - ow)) -ge -2 ]; then
		ok "the float key gives float_scale, centred"
	else
		bad "the float key gives float_scale, centred: width $fw at x $fx on $ow"
	fi
fi
wait "$CF"
alive

# 4f. the scratchpad hides a window and brings it to whatever workspace is showing
client window 2 5 > "$T/scratch.out" 2>&1 &
CX=$!
if wait_for "$T/scratch.out" "2 windows open"; then
	# "state visible focused" of the window with id $1
	sx() { ctl -j windows | grep -o "\"id\":$1,[^}]*" | sed -E 's/.*"state":"([a-z]*)".*"focused":([a-z]*),"visible":([a-z]*).*/\1 \3 \2/'; }
	id=$(ctl windows | awk '$1 == "*" { print $2 }')
	ctl dispatch scratchpad > /dev/null; sleep 0.3
	[ "$(sx "$id")" = "scratchpad false false" ] && ok "scratchpad hides the window" \
		|| bad "scratchpad hides the window: $(sx "$id")"
	ctl dispatch workspace 2 > /dev/null
	ctl dispatch scratchpad_show "nothing-*" > /dev/null; sleep 0.3
	[ "$(sx "$id")" = "scratchpad false false" ] && ok "scratchpad_show with no match does nothing" \
		|| bad "scratchpad_show with no match does nothing: $(sx "$id")"
	ctl dispatch scratchpad_show "AR?-test" > /dev/null; sleep 0.3
	[ "$(sx "$id")" = "floating true true" ] && ok "scratchpad_show brings it to workspace 2" \
		|| bad "scratchpad_show brings it to workspace 2: $(sx "$id")"
	ctl dispatch scratchpad_show > /dev/null; sleep 0.3
	[ "$(sx "$id")" = "scratchpad false false" ] && ok "scratchpad_show again hides it" \
		|| bad "scratchpad_show again hides it: $(sx "$id")"
	ctl dispatch scratchpad_show > /dev/null
	ctl dispatch scratchpad > /dev/null
	ctl dispatch scratchpad_show > /dev/null; sleep 0.3
	[ "$(sx "$id")" = "floating true true" ] && ok "scratchpad on a shown one takes it out" \
		|| bad "scratchpad on a shown one takes it out: $(sx "$id")"
	ctl dispatch workspace 1 > /dev/null
fi
wait "$CX"
alive

# 4f2. rules size and place a floating window, and follow a reload
config "rule = app_id:aro-test float size 400 300 position 10 20 sticky no_border"
sleep 0.3
client window 1 4 > "$T/rules.out" 2>&1 &
CR=$!
if wait_for "$T/rules.out" "1 windows open"; then
	rg() { ctl -j windows | grep -o '"state":"[a-z]*"\|"width":[0-9]*,"height":[0-9]*' | tr '\n' ' '; }
	ctl -j windows | grep -q '"state":"sticky".*"x":10,"y":20,"width":400,"height":300' \
		&& ok "rule: size, position and sticky" || bad "rule: size, position and sticky: $(rg)"
	config "rule = app_id:aro-test float size 50% 50% center"
	sleep 0.5
	ctl -j windows | grep -q '"width":640,' && ok "rule: a reload resizes it to 50%" \
		|| bad "rule: a reload resizes it to 50%: $(rg)"
fi
wait "$CR"
alive
config

# 4f3. opacity holds on a window that starts floating (a commit resets its buffer)
config "rule = app_id:aro-test float size 400 300 position 100 100 opacity 0.5"
sleep 0.3
client window 1 3 > "$T/fade.out" 2>&1 &
CF=$!
if wait_for "$T/fade.out" "1 windows open" && command -v grim > /dev/null; then
	sleep 0.6
	px=$(env -i XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 \
		grim -g "300,250 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tx1 | tr -d ' \n')
	[ -n "$px" ] && [ "$px" != "203040" ] && ok "rule: opacity fades a floating window ($px)" \
		|| bad "rule: opacity fades a floating window: pixel is '$px'"
fi
wait "$CF"
alive
config

# 4f4. the overview shows a window's subsurfaces too, not just its main surface (Firefox)
client window 2 4 sub > "$T/sub.out" 2>&1 &
CU=$!
if wait_for "$T/sub.out" "2 windows open" && command -v grim > /dev/null; then
	ctl dispatch overview > /dev/null
	sleep 0.8
	red=$(env -i XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 grim -t ppm - 2>/dev/null \
		| tail -c +17 | od -An -tx1 -v -w3 | grep -c ' ff 00 00')
	[ "$red" -gt 1000 ] && ok "overview shows subsurfaces ($red red pixels)" \
		|| bad "overview shows subsurfaces: $red red pixels"
	ctl dispatch overview > /dev/null
fi
wait "$CU"
alive

# 4f5. tabbed groups: group, switch tabs by key, click and wheel, drag onto a title, ungroup
config "layout = manual" "focus_follows_mouse = false" "switcher_debounce_ms = 0"
sleep 0.3
client window 3 8 > "$T/grp.out" 2>&1 &
CG=$!
if wait_for "$T/grp.out" "3 windows open"; then
	# "id:state:visible" for each window, focused one marked with *
	gs() { ctl -j windows | grep -o '"id":[0-9]*\|"state":"[a-z]*"\|"focused":[a-z]*\|"visible":[a-z]*' \
		| paste -d, - - - - | sed -E 's/"id":([0-9]+),"state":"([a-z]+)","focused":([a-z]+),"visible":([a-z]+)/\2:\4:\3/;s/:true$/*/;s/:false$//' | tr '\n' ' '; }
	ctl dispatch group left > /dev/null; sleep 0.3
	case "$(gs)" in *"tabbed:false tabbed:true*"*) ok "group: joins the tile to the left as a tab" ;;
		*) bad "group: joins the tile to the left as a tab: $(gs)" ;; esac
	ctl dispatch tab next > /dev/null; sleep 0.3
	case "$(gs)" in *"tabbed:true* tabbed:false"*) ok "group: tab next shows the other tab" ;;
		*) bad "group: tab next shows the other tab: $(gs)" ;; esac
	ctl dispatch ungroup > /dev/null; sleep 0.3
	case "$(gs)" in *"tiled:true tiled:true* tiled:true"*) ok "group: ungroup gives it its own tile again" ;;
		*) bad "group: ungroup gives it its own tile again: $(gs)" ;; esac
	# the right-hand window's title dragged onto its neighbour's title
	gxy() { ctl windows | awk -v n="$1" 'NR > 1 && ++k == n { i = ($1 == "*") ? 2 : 1; split($(i + 4), a, "[x+]"); print a[3], a[4], a[1] }'; }
	read -r x y w < <(gxy 3)
	read -r x2 y2 _ < <(gxy 2)
	client click left $((x + w / 2)) $((y + 12)) $((x2 + 40)) $((y2 + 12))
	sleep 0.5
	case "$(gs)" in *"tabbed"*"tabbed"*) ok "group: dropping a window on a title makes a tab" ;;
		*) bad "group: dropping a window on a title makes a tab: $(gs)" ;; esac
	# the group's tile, wherever the drop left it: its first tab is at the left
	read -r x2 y2 < <(ctl windows | awk '/tabbed/ { i = ($1 == "*") ? 2 : 1; split($(i + 4), a, "[x+]"); print a[3], a[4]; exit }')
	before=$(gs)
	client click left $((x2 + 40)) $((y2 + 12)); sleep 0.3
	[ "$(gs)" != "$before" ] && ok "group: clicking the other tab shows it" \
		|| bad "group: clicking the other tab shows it: $(gs)"
	before=$(gs)
	client click down $((x2 + 40)) $((y2 + 12)); sleep 0.3
	[ "$(gs)" != "$before" ] && ok "group: the wheel over the tabs steps through them" \
		|| bad "group: the wheel over the tabs steps through them: $(gs)"
fi
wait "$CG"
alive
config

# 4g. layout = scroll: columns on a strip, the screen scrolls to focus, neighbours peek in
# focus follows mouse too: aro moving the pointer after a key press must not steal focus back
config "layout = scroll" "switcher_debounce_ms = 0" "focus_follows_mouse = true"
sleep 0.3
# geometry of window N as "x w", from aroctl's WxH+X+Y column
geo() {
	ctl windows | awk -v id="$1" '{
		i = ($1 == "*") ? 2 : 1
		if ($i != id) next
		split($(i + 4), a, "x"); r = a[2]; r = substr(r, match(r, /[+-]/))
		print substr(r, 1, match(substr(r, 2), /[+-]/)) + 0, a[1]
	}'
}
focus_id() { ctl windows | awk '$1 == "*" { print $2 }'; }
client window 4 8 > "$T/scroll.out" 2>&1 &
CW=$!
if wait_for "$T/scroll.out" "4 windows open"; then
	sleep 0.3
	ids=$(ctl windows | awk 'NR > 1 { print ($1 == "*") ? $2 : $1 }' | sort -n | tr '\n' ' ')
	set -- $ids
	ctl dispatch focus left > /dev/null; ctl dispatch focus left > /dev/null
	sleep 0.3
	f=$(focus_id)
	read fx fw <<< "$(geo "$f")"
	read lx lw <<< "$(geo "$1")"
	[ "$f" = "$2" ] && [ "$fx" -ge 0 ] && [ $((fx + fw)) -le 1280 ] \
		&& ok "scroll: the strip follows focus" || bad "scroll: the strip follows focus ($f at $fx+$fw)"
	edge=$((lx + lw))
	[ "$edge" -gt 0 ] && [ "$edge" -lt "$fx" ] && ok "scroll: the previous column peeks in ($edge px)" \
		|| bad "scroll: the previous column peeks in ($lx+$lw)"
	ctl dispatch resize right > /dev/null
	sleep 0.2
	read fx2 fw2 <<< "$(geo "$f")"
	[ "$fw2" -gt "$fw" ] && ok "scroll: resize widens the column" || bad "scroll: resize widens the column"
	ctl dispatch maximize > /dev/null; sleep 0.2
	read mx mw <<< "$(geo "$f")"
	ctl dispatch maximize > /dev/null; sleep 0.2
	read bx bw <<< "$(geo "$f")"
	[ "$mw" -ge 1200 ] && [ "$mx" -ge 0 ] && [ $((mx + mw)) -le 1280 ] && [ "$bw" = "$fw2" ] && ok "scroll: maximize fills the screen and goes back" \
		|| bad "scroll: maximize fills the screen and goes back ($fw2 -> $mx+$mw -> $bw)"
	# presets 1/3, 1/2, 2/3 of 1262: from the resized width, next is 2/3, then round to 1/3
	ctl dispatch width > /dev/null; sleep 0.2
	read px pw1 <<< "$(geo "$f")"
	ctl dispatch width > /dev/null; sleep 0.2
	read px pw2 <<< "$(geo "$f")"
	[ "$pw1" = 841 ] && [ "$pw2" = 421 ] && ok "scroll: width steps through the presets" \
		|| bad "scroll: width steps through the presets ($fw2 -> $pw1 -> $pw2)"
	ctl dispatch workspace 2 > /dev/null; sleep 0.2
	ctl dispatch workspace 1 > /dev/null; sleep 0.3
	[ "$(focus_id)" = "$f" ] && ok "scroll: coming back keeps the column" || bad "scroll: coming back keeps the column"
	ctl dispatch layout manual > /dev/null
	sleep 0.3
	neg=$(ctl windows | awk 'NR > 1 { i = ($1 == "*") ? 2 : 1; if ($(i + 4) ~ /x[0-9]+-/) print "off" }')
	[ -z "$neg" ] && ok "scroll: leaving it restores the splits" || bad "scroll: leaving it restores the splits"
	c=$(client capture output 2>&1)
	case $c in *captured*) ok "scroll: screen capture still works" ;; *) bad "scroll: screen capture: $c" ;; esac
	ctl dispatch layout scroll > /dev/null
	# split down stacks the next window in the focused column, like tiling
	ctl dispatch split down > /dev/null
	client window 1 3 > "$T/stack.out" 2>&1 &
	CK=$!
	if wait_for "$T/stack.out" "1 windows open"; then
		sleep 0.3
		new=$(focus_id)
		ctl dispatch focus up > /dev/null
		above=$(focus_id)
		read nx nw <<< "$(geo "$new")"
		read ax aw <<< "$(geo "$above")"
		[ "$above" != "$new" ] && [ "$nx" = "$ax" ] && ok "scroll: split down stacks in the column" \
			|| bad "scroll: split down stacks in the column ($new at $nx, $above at $ax)"
	fi
	wait "$CK"
	ctl dispatch overview > /dev/null; sleep 0.6
	ctl dispatch overview > /dev/null; sleep 0.4
	alive
	ok "scroll: the overview shows the strip"
fi
wait "$CW" || bad "scroll client exited with an error"
config
sleep 0.3
alive

# 4j. an app's icon is drawn in its header, left of the title
client window 1 3 icon > "$T/icon.out" 2>&1 &
CI=$!
if wait_for "$T/icon.out" "1 windows open"; then
	if command -v grim > /dev/null; then
		sleep 0.5
		# the frame's corner, then border 2, text padding 10, half the 16 px icon; half the 26 px header
		read -r FX FY <<< "$(ctl windows | awk 'NR == 2 { g = ($1 == "*") ? $6 : $5; sub(/^[0-9]+x[0-9]+/, "", g); split(g, a, "+"); print a[2], a[3] }')"
		px=$(env -i XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 \
			grim -g "$((FX + 20)),$((FY + 15)) 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tx1 | tr -d ' \n')
		[ "$px" = "ff0000" ] && ok "app icon drawn in the header" || bad "app icon drawn in the header: pixel is '$px'"
	else
		echo "skip  app icon drawn in the header (no grim)"
	fi
else
	bad "icon window did not open: $(cat "$T/icon.out")"
fi
wait "$CI"

# 4i. aroctl subscribe: a line per change, and a subscriber that vanishes costs aro nothing
ctl subscribe > "$T/sub.txt" 2>&1 &
SUB=$!
ctl -j subscribe > "$T/sub.json" 2>&1 &
SUBJ=$!
ctl subscribe > /dev/null 2>&1 &
GONE=$!
sleep 0.3
kill "$GONE" 2> /dev/null
client window 1 2 > "$T/subwin.out" 2>&1 &
CS=$!
if wait_for "$T/subwin.out" "1 windows open"; then
	ctl dispatch layout monocle > /dev/null
	ctl dispatch workspace 2 > /dev/null
	ctl dispatch workspace 1 > /dev/null
	ctl dispatch layout manual > /dev/null
fi
wait "$CS"
sleep 0.3
alive
for ev in "^open [0-9]* aro-test" "^focus [0-9]* aro-test" "^layout [^ ]* 1 monocle" "^workspace [^ ]* 2" "^close [0-9]*" "^focus -"; do
	grep -q "$ev" "$T/sub.txt" && ok "subscribe: $ev" || bad "subscribe: $ev (got: $(tr '\n' '|' < "$T/sub.txt"))"
done
! grep -qv '^{"event":"[a-z]*".*}$' "$T/sub.json" && grep -q '"event":"open","id":[0-9]*,"app_id":"aro-test"' "$T/sub.json" \
	&& ok "subscribe: JSON lines" || bad "subscribe: JSON lines"
kill "$SUB" "$SUBJ" 2> /dev/null
wait "$SUB" "$SUBJ" 2> /dev/null

# 4h. aroctl wallpaper: a file, relative to aroctl, kept over a reload until the config's line changes
printf 'x' > "$T/wp.png"
[ "$(ctl wallpaper)" = none ] && ok "wallpaper: shows the config's" || bad "wallpaper: shows the config's"
w=$(cd "$T" && ctl wallpaper wp.png)
[ "$w" = "$(realpath "$T/wp.png")" ] && ok "wallpaper: a relative file is set" || bad "wallpaper: a relative file is set ($w)"
ctl reload > /dev/null
[ "$(ctl wallpaper)" = "$w" ] && ok "wallpaper: a reload keeps it" || bad "wallpaper: a reload keeps it"
config "wallpaper = auto"
sleep 0.3
[ "$(ctl wallpaper)" = auto ] && ok "wallpaper: a changed config line wins" || bad "wallpaper: a changed config line wins"
ctl wallpaper "$T/missing.png" > /dev/null 2>&1 && bad "wallpaper: a missing file is refused" \
	|| ok "wallpaper: a missing file is refused"
config
sleep 0.3
alive

# 4f. a bound mouse button runs its action: the back button goes to workspace 3
config "bind = mouse_back, workspace, 3" "bind = mod+Return, spawn, foot"
sleep 0.3
client click back
sleep 0.3
[ "$(ctl workspaces | awk '$NF == "focused" { print $2 }')" = 3 ] && ok "mouse_back binding switches workspace" \
	|| bad "mouse_back binding switches workspace"
client click forward
sleep 0.2
[ "$(ctl workspaces | awk '$NF == "focused" { print $2 }')" = 3 ] && ok "an unbound button does nothing" \
	|| bad "an unbound button does nothing"
ctl dispatch workspace 1 > /dev/null
config
sleep 0.3
alive

# 4c. a fullscreen game asking to tear, with tearing allowed: the frame path must hold
config "allow_tearing = true"
sleep 0.3
client window 1 2 tear > "$T/tear.out" 2>&1 &
CT=$!
if wait_for "$T/tear.out" "1 windows open"; then
	sleep 0.3
	ctl windows | grep -q "fullscreen.*aro-test" && ok "tearing game goes fullscreen" \
		|| bad "tearing game goes fullscreen"
fi
wait "$CT" && ok "frames keep coming with tearing allowed" || bad "frames keep coming with tearing allowed"
config
sleep 0.3
alive

# 4b. an app that takes the shortcuts gets them while focused, and only then
focused_ws() { ctl workspaces | awk '$NF == "focused" { print $2 }'; }
if command -v wtype > /dev/null; then
	kb() { env -i PATH="$PATH" XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 wtype "$@"; }
	kb -s 7000 -k Shift_L &
	KB=$!
	sleep 0.5
	client inhibit 6 > "$T/inh.out" 2>&1 &
	CI=$!
	if wait_for "$T/inh.out" "^active"; then
		ok "shortcut inhibitor active while focused"
		kb -M logo -k 2 -m logo; sleep 0.3
		[ "$(focused_ws)" = 1 ] && ok "super+2 went to the app" || bad "super+2 went to the app"
		ctl dispatch workspace 2 > /dev/null; sleep 0.3
		tail -n 1 "$T/inh.out" | grep -q "^inactive" && ok "inhibitor dropped when focus leaves" \
			|| bad "inhibitor dropped when focus leaves"
		kb -M logo -k 1 -m logo; sleep 0.3
		[ "$(focused_ws)" = 1 ] && ok "super+1 works again" || bad "super+1 works again"
	else
		bad "shortcut inhibitor active while focused"
	fi
	wait "$CI" || bad "inhibit client exited with an error"
	kill "$KB" 2> /dev/null
	wait "$KB" 2> /dev/null
	alive
else
	echo "skip  shortcut inhibiting (no wtype)"
fi

fi

if [ "$MODE" = multi ]; then

# 4c2. a monitor rule opens the window on that screen
for m in HEADLESS-1 HEADLESS-2; do
	config "rule = app_id:aro-test monitor $m"
	sleep 0.3
	client window 1 2 > "$T/mon.out" 2>&1 &
	CO=$!
	if wait_for "$T/mon.out" "1 windows open"; then
		ctl -j windows | grep -q "\"output\":\"$m\"" && ok "rule: monitor $m" \
			|| bad "rule: monitor $m: $(ctl -j windows | grep -o '"output":"[^"]*"')"
	fi
	wait "$CO"
done
config
alive

# 4d. a workspace moves to the next monitor with its windows, and back
client window 2 4 > "$T/mv.out" 2>&1 &
CV=$!
if wait_for "$T/mv.out" "2 windows open"; then
	on() { ctl -j windows | grep -o '"output":"[^"]*"' | sort -u | tr -d '\n'; }
	start=$(on)
	moved=
	for dir in right left; do
		ctl dispatch move_workspace $dir > /dev/null
		sleep 0.3
		[ "$(on)" != "$start" ] && { moved=$dir; break; }
	done
	if [ -n "$moved" ]; then
		[ "$(ctl -j windows | grep -o '"output":"[^"]*"' | sort -u | wc -l)" -eq 1 ] \
			&& ok "workspace moves to the other monitor with both windows" \
			|| bad "workspace moves to the other monitor with both windows"
		back=left; [ "$moved" = left ] && back=right
		ctl dispatch move_workspace $back > /dev/null
		sleep 0.3
		[ "$(on)" = "$start" ] && ok "and moves back" || bad "and moves back"
	else
		bad "workspace moves to the other monitor"
	fi
fi
wait "$CV"
alive

# 4d2. a window dragged onto an empty monitor stays there
config "rule = app_id:aro-test monitor HEADLESS-1" "focus_follows_mouse = false"
sleep 0.3
client window 2 4 > "$T/drag.out" 2>&1 &
CD=$!
if wait_for "$T/drag.out" "2 windows open"; then
	# the pointer spans both monitors, 2560 wide, in click's 1280 units
	read -r x y w < <(ctl windows | awk 'NR == 2 { i = ($1 == "*") ? 2 : 1; split($(i + 4), a, "[x+]"); print a[3], a[4], a[1] }')
	ox=$(ctl -j monitors | tr '}' '\n' | grep '"name":"HEADLESS-1"' | grep -oE '"x":-?[0-9]+' | cut -d: -f2)
	other=$(( ox == 0 ? 1280 : 0 ))
	client click left $(( (x + w / 2) / 2 )) $((y + 12)) $(( (other + 640) / 2 )) 360
	sleep 0.5
	[ "$(ctl -j windows | grep -o '"output":"[^"]*"' | sort -u | wc -l)" -eq 2 ] \
		&& ok "drag: a window dropped on an empty monitor stays there" \
		|| bad "drag: a window dropped on an empty monitor stays there: $(ctl windows)"
fi
wait "$CD"
config
alive

# 5b. a scroll strip on one monitor draws nothing on the monitor next to it
config "layout = scroll"
sleep 0.3
client window 4 5 > "$T/clip.out" 2>&1 &
CC=$!
if wait_for "$T/clip.out" "4 windows open"; then
	sleep 0.4
	mon=$(ctl -j windows | grep -o '"output":"[^"]*"' | head -n 1 | cut -d'"' -f4)
	ox=$(ctl -j monitors | tr '}' '\n' | grep "\"name\":\"$mon\"" | grep -oE '"x":-?[0-9]+' | cut -d: -f2)
	other=$(( ox == 0 ? 1280 : 0 ))
	if command -v grim > /dev/null; then
		px=$(env -i XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 \
			grim -g "$((other + 640)),360 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tx1 | tr -d ' \n')
		[ "$px" != "203040" ] && ok "scroll strip stays on its own monitor ($px next door)" \
			|| bad "scroll strip drew on the other monitor"
	else
		echo "skip  scroll clipping (no grim)"
	fi
fi
wait "$CC"
config
sleep 0.3
alive


fi

if [ "$MODE" = main ]; then

# 4z. the wallpaper picker: aropaper makes its thumbnails, a reload rebuilds it
if [ -x "$(dirname "$ARO")/aropaper" ]; then
	art="$(dirname "$0")/../data/wallpaper"
	mkdir -p "$T/walls/more"
	cp "$art/aro-wallpaper-1920x1080.png" "$T/walls/one.png"
	cp "$art/aro-wallpaper-dwindle-1920x1080.png" "$T/walls/more/two.png"
	config "wallpaper_dir = $T/walls"
	sleep 0.3
	ctl dispatch wallpapers > /dev/null
	thumbs() { ls "$T/.cache/aro/thumbnails" 2>/dev/null | grep -c '\.png$'; }
	for _ in $(seq 100); do [ "$(thumbs)" -ge 2 ] && break; sleep 0.1; done
	[ "$(thumbs)" -eq 2 ] && ok "the wallpaper picker makes thumbnails" \
		|| bad "the wallpaper picker makes thumbnails: $(thumbs) of 2"
	config "wallpaper_dir = $T/walls" "font = Sans 11"
	sleep 0.3
	alive
	ok "the wallpaper picker survives a reload while open"
	ctl dispatch wallpapers > /dev/null
	ctl dispatch wallpapers > /dev/null
	ctl dispatch wallpapers > /dev/null
	sleep 0.5
	alive
	ok "the wallpaper picker opens again while it fades out"
	config
	sleep 0.3
else
	echo "skip  wallpaper picker (no aropaper)"
fi

# 5. saving the config while running: the reload freed strings still in use
before=$(grep -c "config reloaded" "$T/aro.log")
for c in "bar = false" "bar = true" "font = Monospace 11" "bar = auto" \
	"cursor_theme = 'No Such Theme'" "font_small = \"Sans 8\"" "layout = monocle" \
	"this line is wrong" "bar = false" "layout = dwindle"; do
	config "$c"
	sleep 0.3
	alive
done
after=$(grep -c "config reloaded" "$T/aro.log")
[ $((after - before)) -ge 10 ] && ok "ten live config reloads" \
	|| bad "ten live config reloads: only $((after - before)) happened"

fi

# 6. a clean shutdown, which runs every teardown step
ctl dispatch quit > /dev/null
for _ in $(seq 50); do
	kill -0 "$PID" 2>/dev/null || break
	sleep 0.1
done
if kill -0 "$PID" 2>/dev/null; then
	bad "aro quits when asked"
else
	wait "$PID"
	code=$?
	PID=
	[ "$code" -eq 0 ] && ok "aro quits cleanly" || bad "aro quits cleanly: exit code $code"
fi

if grep -qE "AddressSanitizer|runtime error|Assertion .* failed" "$T/aro.err"; then
	bad "no sanitizer or assertion reports"
	grep -E -A 15 "AddressSanitizer|runtime error|Assertion .* failed" "$T/aro.err" | head -n 40
else
	ok "no sanitizer or assertion reports"
fi

exit "$FAILED"
