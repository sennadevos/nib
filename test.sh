#!/usr/bin/env bash
# Offscreen test suite: exercises the real engine and the real key path without
# ever mapping a window, so it is safe to run on a session someone is using.
#
# Deliberately does NOT test C-y / C-p: on Wayland the clipboard is only
# available to the focused client, and taking ownership then exiting destroys
# whatever the user had copied.
set -uo pipefail

NIB=${NIB:-./nib}
TMP=$(mktemp -d)
PORT=${PORT:-8099}
export QT_QPA_PLATFORM=offscreen
# GPU compositing is pointless offscreen, and on some hosts a real smooth
# scroll animation segfaults QtWebEngine's viz compositor
# (CompositorFrameSinkSupport::UpdateNeedsBeginFramesInternal) under the
# offscreen platform. Software rasterization avoids that path.
export QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu ${QTWEBENGINE_CHROMIUM_FLAGS:-}"
# Isolated profile: don't churn the user's real one, and don't collide with
# a nib (or another suite run) already holding the profile lock.
export XDG_DATA_HOME="$TMP/data" XDG_CACHE_HOME="$TMP/cache"
fails=0

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/plain.html" <<'EOF'
<title>plain</title><body style="margin:0">
<div style="height:4000px;background:linear-gradient(#fff,#333)">tall</div>
EOF
cat > "$TMP/input.html" <<'EOF'
<title>input</title><body style="margin:0">
<input autofocus><div style="height:4000px">tall</div>
EOF
cat > "$TMP/shell.html" <<'EOF'
<title>shell</title><body style="margin:0;overflow:hidden">
<div style="height:100vh;overflow-y:auto"><div style="height:5000px">inner</div></div>
EOF
cat > "$TMP/hints.html" <<'EOF'
<title>hints</title><body style="margin:0">
<a href="#target">one</a> <a href="#other">two</a> <input id="field">
<div style="height:3000px"></div><div id="target">bottom</div>
EOF
cat > "$TMP/caret.html" <<'EOF'
<title>caret</title><body style="margin:0">
<p>alpha beta gamma delta</p>
EOF
printf '#!/bin/sh\necho "HANDOFF: $*" >> %s/handoff.log\n' "$TMP" > "$TMP/ext.sh"
chmod +x "$TMP/ext.sh"

run() {  # run MODE ARGS... -> prints nib: lines
	local mode=$1; shift
	NIB_DEBUG="$mode" NIB_EXTERNAL="$TMP/ext.sh" "$NIB" "$@" \
	    > "$TMP/out.log" 2>&1 &
	local pid=$!
	sleep "${WAIT:-12}"
	kill $pid 2>/dev/null
	sleep 1
	grep '^nib:' "$TMP/out.log" || true
}

check() {  # check LABEL OUTPUT PATTERN
	if grep -q "$3" <<<"$2"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		sed 's/^/       /' <<<"$2"
		fails=$((fails + 1))
	fi
}

echo "scroll + focus gate"
out=$(run keys "$TMP/plain.html")
check "3x j scrolls 3 steps"        "$out" 'after 3x j = 192'
out=$(run keys "$TMP/shell.html")
check "inner scroller is targeted"  "$out" 'target=DIV'
out=$(run keys "$TMP/input.html")
check "focused input keeps its keys" "$out" 'editable=1'

echo "scroll limits"
for pg in plain shell; do
	out=$(run bounds "$TMP/$pg.html")
	check "$pg clamps at both ends"  "$out" 'past-bottom .* OK | past-top .* OK'
	check "$pg settles, no bounce"   "$out" 'settle@2200ms .* OK'
done

echo "ctrl bindings"
out=$(run ctrl "$TMP/plain.html")
check "C-j moves one page"          "$out" 'C-j page-down .* OK'

echo "hint mode"
out=$(run hint "$TMP/hints.html")
check "f raises a hint per target"  "$out" 'hint count=3'
check "typing the hint follows it"  "$out" 'hint follow hash=#target OK'
check "gi focuses the text field"   "$out" 'hint gi active=field OK'

echo "caret mode"
out=$(run caret "$TMP/caret.html")
check "v-w-v-w-y yanks a word"      "$out" "caret yank='beta' OK"

echo "app mode"
# exec: $SRV must be python itself, or cleanup kills only the subshell and
# leaks a server that poisons every later run on this port
(cd "$TMP" && exec python3 -m http.server "$PORT" >/dev/null 2>&1) &
SRV=$!
sleep 2
out=$(run app --app "http://localhost:$PORT/plain.html" --scope localhost)
check "single tab, C-t refused"     "$out" 'after C-t tabs=1 OK'
check "off-scope nav refused"       "$out" 'off-scope nav .* OK'
check "off-scope handed off"        "$(cat "$TMP/handoff.log" 2>/dev/null)" 'HANDOFF: https://example.com'

echo "scope rules (no engine needed)"
out=$("$NIB" --app=https://app.slack.com/x --check-scope \
    https://slack.com/a https://evil.com/b http://slack.com.evil.com/c)
check "sibling subdomain allowed"   "$out" 'ALLOW   https://slack.com/a'
check "other domain handed off"     "$out" 'HANDOFF https://evil.com/b'
check "suffix spoof handed off"     "$out" 'HANDOFF http://slack.com.evil.com/c'
out=$("$NIB" --app=https://www.bbc.co.uk --check-scope https://other.co.uk/)
check "two-part suffix respected"   "$out" 'HANDOFF https://other.co.uk/'

echo
if [ "$fails" -eq 0 ]; then
	echo "all checks passed"
else
	echo "$fails check(s) failed"
fi
exit "$fails"
