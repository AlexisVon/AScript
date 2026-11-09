#!/bin/bash
# library path tests — {script} path in .ax, and the search_paths baked into an axp
set -u

HERE=$(dirname "$(realpath "$0")")
ROOT=$(realpath "$HERE/../../..")
CLI="$ROOT/bin/axc-cli"
LINK_SO="$ROOT/test/cli/link/base.so"

[ -x "$CLI" ] || { echo "missing $CLI"; exit 1; }
[ -f "$LINK_SO" ] || make -C "$ROOT/test/cli" >/dev/null || { echo "missing $LINK_SO"; exit 1; }
export LD_LIBRARY_PATH="$ROOT/bin:$ROOT/bin/alxlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/lib" "$tmp/run"
cp "$LINK_SO" "$tmp/lib/"
cat >"$tmp/main.axc" <<'AXC'
link "base.so" as b;
$io_print("abs=%1\n", b.mt.abs(-5));
AXC
cat >"$tmp/app.ax" <<'AX'
{"main": "main.axc", "path": ["{script}/lib"]}
AX

failed=0
check() {
    if grep -qF "$2" <<< "$1"; then
        echo "PASS $3"
    else
        echo "FAIL $3 (missing '$2' in: $1)"
        failed=1
    fi
}

out=$(cd "$tmp/run" && "$CLI" "$tmp/app.ax" 2>&1) || { echo "$out"; echo "FAIL ax exit"; exit 1; }
check "$out" "abs=5" ".ax {script} path"

out=$(cd "$tmp/run" && "$CLI" -c "$tmp/app.ax" 2>&1) || { echo "$out"; echo "FAIL compile exit"; exit 1; }
[ -f "$tmp/app.axp" ] || { echo "FAIL missing $tmp/app.axp"; exit 1; }
out=$(cd "$tmp/run" && "$CLI" "$tmp/app.axp" 2>&1) || { echo "$out"; echo "FAIL axp exit"; exit 1; }
check "$out" "abs=5" "axp hint search_paths"

[ "$failed" -eq 0 ] && echo "libpaths tests passed"
exit "$failed"
