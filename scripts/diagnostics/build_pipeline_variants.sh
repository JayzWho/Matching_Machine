#!/usr/bin/env bash
# Build the three variants of the two pipeline benchmarks compared by the
# validation recording of the pipeline fixes:
#   c0  before the fixes                       (master, e4a48b8)
#   c1  the two threads pinned                 (e88ec88)
#   c2  the two threads pinned, the pool fixed (bf5f9fe)
# Each commit is checked out in a detached worktree under build/variants/src/
# and configured like the Release tree, with the dependency sources of
# build/release instead of a download. The binaries land in build/variants/cN/,
# whose paths have equal length, because the length of a launch's arguments
# moves its stack.
#
# Usage: scripts/diagnostics/build_pipeline_variants.sh [--remove-worktrees]
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
DEPS="$ROOT/build/release/_deps"
declare -A COMMITS=([c0]=e4a48b8 [c1]=e88ec88 [c2]=bf5f9fe)

if [ "${1:-}" = "--remove-worktrees" ]; then
    for v in c0 c1 c2; do
        git -C "$ROOT" worktree remove --force "$ROOT/build/variants/src/$v" 2>/dev/null || true
    done
    exit 0
fi

for v in c0 c1 c2; do
    src="$ROOT/build/variants/src/$v"
    [ -d "$src" ] || git -C "$ROOT" worktree add --detach "$src" "${COMMITS[$v]}" >/dev/null
    [ "$(git -C "$src" rev-parse --short=7 HEAD)" = "${COMMITS[$v]}" ] || { echo "$src is not at ${COMMITS[$v]}" >&2; exit 1; }
    cmake -S "$src" -B "$ROOT/build/variants/$v" -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$DEPS/googletest-src" \
        -DFETCHCONTENT_SOURCE_DIR_BENCHMARK="$DEPS/benchmark-src" >/dev/null
    cmake --build "$ROOT/build/variants/$v" -j"$(nproc)" --target bench_matching_engine bench_baseline_mutex >/dev/null
done
cd "$ROOT" && sha256sum build/variants/c?/bench_matching_engine build/variants/c?/bench_baseline_mutex
