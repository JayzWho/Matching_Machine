#!/bin/bash
# Builds mutex_shutdown_stress.cpp against one version of the mutex benchmark
# source, with the compiler, flags and link inputs of the Release tree.
#
#   build_mutex_shutdown_stress.sh SOURCE OUTPUT
#
# SOURCE is a bench_baseline_mutex.cpp, for example one extracted with
# `git show <commit>:benchmarks/bench_baseline_mutex.cpp`. Requires a built
# CMake Unix Makefiles tree in build/release.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 SOURCE OUTPUT" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "$here/../../build/release" && pwd)
source=$(readlink -f "$1")
output=$(readlink -m "$2")
target=$build/CMakeFiles/bench_baseline_mutex.dir

flags=()
for name in DEFINES INCLUDES FLAGS; do
    line=$(grep "^CXX_$name = " "$target/flags.make")
    read -r -a words <<< "${line#CXX_$name = }"
    flags+=("${words[@]}")
done
read -r -a link < "$target/link.txt"

"${link[0]}" "${flags[@]}" -DBENCH_SOURCE="\"$source\"" \
    -c "$here/mutex_shutdown_stress.cpp" -o "$output.o"

command=()
for ((i = 0; i < ${#link[@]}; i++)); do
    word=${link[i]}
    case $word in
        CMakeFiles/bench_baseline_mutex.dir/benchmarks/bench_baseline_mutex.cpp.o) command+=("$output.o") ;;
        -Wl,--dependency-file=*) ;;
        -o) command+=(-o "$output"); i=$((i + 1)) ;;
        *) command+=("$word") ;;
    esac
done
(cd "$build" && "${command[@]}")
rm -f "$output.o"
echo "$output"
