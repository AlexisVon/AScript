#!/usr/bin/env bash
# run.sh — build and run the axc-runner protocol replay tests.
# A normal run covers every scenario; repeated text runs catch leaked processes.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

cmake -S . -B build -DBUILD_REPLAY=ON >/dev/null
cmake --build build --target axc-runner axc-replay -j7 >/dev/null

export LD_LIBRARY_PATH="bin:bin/alxlib"
bin/axc-replay bin/axc-runner

# performance: the same compute-only script through the CLI and through a runner.
# A statically linked CLI (CLI_STATIC=ON, the default build) is not a like-for-like
# baseline - it avoids the shared AlxLib and runs faster - so only compare when dynamic.
bench=/tmp/axc-bench.axc
printf 'var i = 0;\nwhile (i < 10000000) { i++; }\n' > "$bench"
if ldd bin/axc-cli 2>/dev/null | grep -q "libaxc-core"; then
    t0=$(date +%s%N)
    bin/axc-cli "$bench" >/dev/null
    t1=$(date +%s%N)
    echo "bench: cli $(( (t1 - t0) / 1000000 )) ms"
else
    echo "bench: cli is statically linked; comparison skipped (-DCLI_STATIC=OFF for a baseline)"
fi
bin/axc-replay bin/axc-runner bench | grep 'bench: runner'

# leak check: sequential runs must not leave runners behind
for i in $(seq 1 100); do
    bin/axc-replay bin/axc-runner text >/dev/null
done
if pgrep -x axc-runner >/dev/null; then
    echo "FAIL: leftover axc-runner after 100 runs"
    exit 1
fi
echo "repeat: 100 clean runs"
