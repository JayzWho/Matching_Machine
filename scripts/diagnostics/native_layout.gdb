# Observation only: read the first throughput engine address from the unchanged
# x86-64 binary. Detach before its constructor or timed loop runs. The launcher's
# exit status and process CPU telemetry describe GDB, not the benchmark; only
# the target's completed Google Benchmark JSON supplies benchmark observations.
# Invoke with gdb -q -nx -batch -x this-file --args bench_baseline_mutex ...
set pagination off
set confirm off
set debuginfod enabled off
set startup-with-shell off
set disable-randomization off
set print thread-events off
show disable-randomization

# At the first instruction of this constructor, the SysV AMD64 ABI passes
# `this` in RDI. No debug information or recompiled benchmark is needed.
break *'MutexMatchingEngine::MutexMatchingEngine()'
commands
  silent
  # Size/alignment below are verified for this repository's current GCC build.
  printf "MM_LAYOUT address=%p mod64=%lu mod4096=%lu size=216 align=8 offset=%lu source=native-gdb-first-constructor\n", (void*)$rdi, ((unsigned long)$rdi % 64), ((unsigned long)$rdi % 4096), ((unsigned long)$rdi % 64)
  detach
  quit
end
run
