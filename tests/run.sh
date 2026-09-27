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
