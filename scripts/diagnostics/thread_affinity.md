# Thread placement diagnostic

This Linux/glibc interposer selects the CPU of each newly created worker before
its user start routine executes. It changes placement, so results belong to a
diagnostic condition and are not production performance claims. Engine and
benchmark workloads are unchanged.

Build from the repository root:

```bash
mkdir -p build/diagnostics
cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
  scripts/diagnostics/thread_affinity.c -o build/diagnostics/thread_affinity.so \
  -ldl -pthread
python3 scripts/diagnostics/test_thread_affinity.py
```

Pass the absolute shared-library path in `LD_PRELOAD` for the benchmark child
process only. Set `MM_DIAG_THREAD_CPUS=none` to delegate calls unchanged, `1,3`
to place successive workers on CPU 1 then CPU 3 repeatedly, or `3,1` to reverse
the direction. A one-CPU list, such as `3`, pins every new worker there. The
initial thread retains its existing affinity. Every listed CPU must belong to
the initial thread's allowed set; a malformed configuration exits with status
125 before the workload starts. One configuration line is written to stderr.

The mutex pipeline creates the consumer first and the producer second, and
creates a new pair in each iteration. Thus `1,3` means consumer on CPU 1 and
producer on CPU 3. Other benchmarks may create one worker while the initial
thread performs the other role: their initial thread needs a separate placement
control. Creation order is the only role identification; additional threads
would change the mapping and must be checked against source before reuse.

In pin mode, the interposer copies the glibc default thread attributes and sets
their CPU affinity. A caller supplying non-null attributes receives `ENOTSUP`;
the caller's attributes remain untouched. The benchmark's `std::thread` path
uses null attributes. In `none` mode, all attributes are passed unchanged.
The creation counter advances on each attempted creation after default
attributes are obtained, so a failed creation can change subsequent placement.
Do not combine this diagnostic with another `pthread_create` interposer.

Preloading the library and adding environment strings changes process layout.
Keep the library loaded for both `none` and pinned comparisons; retain a separate
unmodified-launch reference to check the preload effect. Keep argument and
environment string lengths consistent where possible and record actual
addresses. In particular, `none` has four characters while `1,3` has three;
compensate elsewhere when testing a stack-layout hypothesis. Disabling ASLR
alone does not make varying argument/environment layouts equivalent. Keep
output filenames the same length across launches.

The integration test builds a temporary copy and a small pthread harness. It
checks inherited affinity, caller-supplied attributes, single-CPU and alternating
placement in both directions, rejection of non-null attributes in pin mode, and
invalid configuration. It does not run benchmarks or enter measurement mode.
