/* Diagnostic-only pthread_create interposer; see thread_affinity.md. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*create_fn)(pthread_t *, const pthread_attr_t *,
                         void *(*)(void *), void *);

static create_fn real_create;
static int target_cpus[CPU_SETSIZE];
static size_t target_count;
static _Atomic unsigned long next_thread;

static void fail(const char *reason) {
    fprintf(stderr, "MM_DIAG_THREAD_CPUS: %s\n", reason);
    _exit(125);
}

__attribute__((constructor)) static void configure(void) {
    /* POSIX permits converting dlsym's result to a function pointer. */
    *(void **)(&real_create) = dlsym(RTLD_NEXT, "pthread_create");
    if (!real_create)
        fail("cannot resolve the real pthread_create");

    const char *value = getenv("MM_DIAG_THREAD_CPUS");
    if (!value || strcmp(value, "none") == 0) {
        fputs("MM_DIAG_THREAD_CPUS=none (delegated unchanged)\n", stderr);
        return;
    }
    if (!*value)
        fail("expected none or a comma-separated CPU list");

    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        fail("cannot read the initial thread's allowed CPUs");

    for (const char *p = value; *p;) {
        if (*p < '0' || *p > '9' || target_count == CPU_SETSIZE)
            fail("invalid CPU list");
        errno = 0;
        char *end;
        long cpu = strtol(p, &end, 10);
        if (errno || cpu < 0 || cpu >= CPU_SETSIZE ||
            (*end && *end != ','))
            fail("invalid CPU number");
        if (!CPU_ISSET((int)cpu, &allowed))
            fail("requested CPU is outside the initial thread's allowed set");
        target_cpus[target_count++] = (int)cpu;
        if (!*end)
            break;
        p = end + 1;
        if (!*p)
            fail("trailing comma in CPU list");
    }
    fprintf(stderr, "MM_DIAG_THREAD_CPUS=%s (creation order, repeated)\n", value);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    if (!real_create)
        fail("pthread_create called before diagnostic initialization");
    if (!target_count)
        return real_create(thread, attr, start, arg);

    /* pthread_attr_t is opaque: a bytewise copy is not a safe clone. The
       tested std::thread path supplies NULL. Reject other callers explicitly. */
    if (attr)
        return ENOTSUP;

    pthread_attr_t pinned;
    int result = pthread_getattr_default_np(&pinned);
    if (result)
        return result;
    unsigned long index = atomic_fetch_add_explicit(&next_thread, 1,
                                                    memory_order_relaxed);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(target_cpus[index % target_count], &cpus);
    result = pthread_attr_setaffinity_np(&pinned, sizeof(cpus), &cpus);
    if (!result)
        result = real_create(thread, &pinned, start, arg);
    pthread_attr_destroy(&pinned);
    return result;
}
