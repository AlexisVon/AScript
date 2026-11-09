#!/bin/bash
# net link tests — require -DBUILD_LINKS=ON
set -u

HERE=$(dirname "$(realpath "$0")")
ROOT=$(realpath "$HERE/../../..")
CLI="$ROOT/bin/axc-cli"
LINK_DIR="$ROOT/bin/links"

[ -x "$CLI" ] || { echo "missing $CLI"; exit 1; }
[ -f "$LINK_DIR/net.so" ] || { echo "missing $LINK_DIR/net.so (configure with -DBUILD_LINKS=ON)"; exit 1; }
export LD_LIBRARY_PATH="$ROOT/bin:$ROOT/bin/alxlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$HERE"

if ! out=$("$CLI" -p "$LINK_DIR" test_net.axc 2>&1); then
    echo "$out"
    echo "FAIL net exit"
    exit 1
fi
echo "$out"
if ! grep -q "0 failed" <<< "$out"; then
    echo "FAIL net summary"
    exit 1
fi
echo "net tests passed"
