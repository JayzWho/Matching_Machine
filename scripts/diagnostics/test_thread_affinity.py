#!/usr/bin/env python3
"""Integration checks for the diagnostic interposer, outside measurement mode."""
import os
from pathlib import Path
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
HARNESS = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct observation { cpu_set_t allowed; int cpu; int status; };

static void *observe(void *arg) {
    struct observation *o = arg;
    o->status = pthread_getaffinity_np(pthread_self(), sizeof(o->allowed), &o->allowed);
    o->cpu = sched_getcpu();
    return NULL;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

int main(int argc, char **argv) {
    CHECK(argc >= 3);
    cpu_set_t initial, after;
    CHECK(sched_getaffinity(0, sizeof(initial), &initial) == 0);
    int first_cpu = atoi(argv[2]);

    if (strcmp(argv[1], "nonnull") == 0) {
        pthread_attr_t attr;
        CHECK(pthread_attr_init(&attr) == 0);
        cpu_set_t target, original, unchanged;
        CPU_ZERO(&target);
        CPU_SET(first_cpu, &target);
        CHECK(pthread_attr_setaffinity_np(&attr, sizeof(target), &target) == 0);
        CHECK(pthread_attr_getaffinity_np(&attr, sizeof(original), &original) == 0);
        pthread_t thread;
        struct observation o = {0};
        CHECK(pthread_create(&thread, &attr, observe, &o) == ENOTSUP);
        CHECK(pthread_attr_getaffinity_np(&attr, sizeof(unchanged), &unchanged) == 0);
        CHECK(CPU_EQUAL(&original, &unchanged));
        CHECK(pthread_attr_destroy(&attr) == 0);
        return 0;
    }

    for (int i = 0; i < 6; ++i) {
        pthread_t thread;
        struct observation o = {0};
        CHECK(pthread_create(&thread, NULL, observe, &o) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(o.status == 0);
        if (strcmp(argv[1], "none") == 0) {
            CHECK(CPU_EQUAL(&initial, &o.allowed));
            CHECK(CPU_ISSET(o.cpu, &initial));
        } else {
            int expected = atoi(argv[2 + i % (argc - 2)]);
            CHECK(CPU_COUNT(&o.allowed) == 1);
            CHECK(CPU_ISSET(expected, &o.allowed));
            CHECK(o.cpu == expected);
        }
    }
    CHECK(sched_getaffinity(0, sizeof(after), &after) == 0);
    CHECK(CPU_EQUAL(&initial, &after));

    if (strcmp(argv[1], "none") == 0) {
        pthread_attr_t attr;
        CHECK(pthread_attr_init(&attr) == 0);
        cpu_set_t target;
        CPU_ZERO(&target);
        CPU_SET(first_cpu, &target);
        CHECK(pthread_attr_setaffinity_np(&attr, sizeof(target), &target) == 0);
        pthread_t thread;
        struct observation o = {0};
        CHECK(pthread_create(&thread, &attr, observe, &o) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(pthread_attr_destroy(&attr) == 0);
        CHECK(o.status == 0 && o.cpu == first_cpu);
        CHECK(CPU_EQUAL(&target, &o.allowed));
    }
    return 0;
}
"""


def main():
    cpus = sorted(os.sched_getaffinity(0))
    with tempfile.TemporaryDirectory(prefix="mm-thread-affinity-test-") as tmp:
        tmp = Path(tmp)
        library = tmp / "thread_affinity.so"
        source = tmp / "test.c"
        harness = tmp / "test"
        source.write_text(HARNESS)
        flags = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
        subprocess.run([*flags, "-fPIC", "-shared", str(HERE / "thread_affinity.c"),
                        "-o", str(library), "-ldl", "-pthread"], check=True)
        subprocess.run([*flags, str(source), "-o", str(harness), "-pthread"], check=True)
        env = os.environ.copy()
        env["LD_PRELOAD"] = str(library)

        cases = [(None, "none", cpus[:1]), ("none", "none", cpus[:1]),
                 (str(cpus[0]), "pin", cpus[:1]),
                 (str(cpus[0]), "nonnull", cpus[:1])]
        if len(cpus) >= 2:
            for pair in (cpus[:2], cpus[:2][::-1]):
                cases.append((",".join(map(str, pair)), "pin", pair))
        else:
            print("SKIP alternating two-CPU checks: only one CPU is allowed")
        for setting, mode, expected in cases:
            env.pop("MM_DIAG_THREAD_CPUS", None)
            if setting is not None:
                env["MM_DIAG_THREAD_CPUS"] = setting
            result = subprocess.run([str(harness), mode, *map(str, expected)],
                                    env=env, text=True, capture_output=True)
            if result.returncode:
                raise AssertionError(f"{setting=}, {mode=}: {result.stderr}")
            assert result.stderr.count("MM_DIAG_THREAD_CPUS=") == 1, result.stderr

        invalid = ["", "-1", "1,,3", "none,", "0 ", "1024", "999999999999999999999"]
        outside = next((cpu for cpu in range(1024) if cpu not in cpus), None)
        if outside is not None:
            invalid.append(str(outside))
        for setting in invalid:
            env["MM_DIAG_THREAD_CPUS"] = setting
            result = subprocess.run([str(harness), "none", str(cpus[0])],
                                    env=env, text=True, capture_output=True)
            assert result.returncode == 125, (setting, result.returncode, result.stderr)
        print(f"PASS: {len(cases)} placement/delegation cases, {len(invalid)} rejected configurations")


if __name__ == "__main__":
    main()
