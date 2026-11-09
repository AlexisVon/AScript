#!/bin/bash
# cv link tests — require -DBUILD_LINKS=ON and the package in lib/opencv
set -u

ROOT=$(realpath "$(dirname "$0")/../../..")
CLI="$ROOT/bin/axc-cli"
LINK_DIR="$ROOT/bin/links"

[ -x "$CLI" ] || { echo "missing $CLI"; exit 1; }
[ -f "$LINK_DIR/cv.so" ] || { echo "missing $LINK_DIR/cv.so (configure with -DBUILD_LINKS=ON)"; exit 1; }
export LD_LIBRARY_PATH="$ROOT/bin:$ROOT/bin/alxlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

failed=0
check() {
    if grep -q "$2" <<< "$out"; then
        echo "PASS $1"
    else
        echo "FAIL $1 (missing '$2')"
        failed=1
    fi
}

if ! out=$("$CLI" -p "$LINK_DIR" "$ROOT/test/cli/cv/smoke.axc" 2>&1); then
    echo "$out"
    echo "FAIL smoke exit"
    exit 1
fi
check version "version: 4.14.0"
check created "created: 64x48 8uc3"
check png "png bytes:"
check decoded "decoded: 64x48 8uc3"
check gray "gray: 8uc1"
check resize "resized: 32x24"
check split "split: 3"
check merge10 "merge10: 8uc10 ch=10"
check convert10 "convert10: 16uc10"
check sobel "sobel: 16sc1"
check rotate "rot: 48x64"
check crop "crop: 16x16"
check file "file: 64x48"
check filters "filters ok"
check arith "arith ok"

if ! out=$("$CLI" -p "$LINK_DIR" "$ROOT/test/cli/cv/errors.axc" 2>&1); then
    echo "$out"
    echo "FAIL errors exit"
    exit 1
fi
check imread "imread: RuntimeError"
check info "info: TypeError"
check crop_err "crop: ArgError"
check merge "merge: ArgError"
check resize_err "resize: ArgError"
check cvt "cvt_color: ArgError"
check threshold "threshold: ArgError"
check imencode "imencode: RuntimeError"
check convert_err "convert: ArgError"

if [ "$failed" -eq 0 ]; then
    echo "cv tests passed"
fi
exit "$failed"
