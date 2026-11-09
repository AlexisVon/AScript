#!/usr/bin/env bash
# run.sh — build and run the Qt session/runner tests headlessly (Qt Core only).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

cmake -S . -B build -DBUILD_GUI_TESTS=ON >/dev/null
cmake --build build --target axc-gui-test axc-hint-test axc-runner -j7 >/dev/null

export LD_LIBRARY_PATH="bin:bin/alxlib"
# keep QSettings (breakpoints, runner options) out of the developer's config
export XDG_CONFIG_HOME="$(mktemp -d)"
bin/axc-hint-test
bin/axc-gui-test bin/axc-runner

for i in $(seq 1 5); do
    bin/axc-gui-test bin/axc-runner >/dev/null
done
echo "repeat: 5 clean runs"
