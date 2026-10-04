/* SPDX-License-Identifier: MIT */
#include <ringpu/platform.h>

#include "thread.h"

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#include <pthread.h>
#include <sched.h>
#endif

typedef struct TestState {
    volatile long lock;
    volatile long yield_count;
    volatile long entered_count;
} TestState;

static long atomic_read(volatile long* value) {
#if defined(_MSC_VER)
    return _InterlockedCompareExchange(value, 0L, 0L);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static long atomic_increment(volatile long* value) {
#if defined(_MSC_VER)
    return _InterlockedIncrement(value);
#else
    return __atomic_add_fetch(value, 1L, __ATOMIC_ACQ_REL);
#endif
}

static void host_yield(void) {
#if defined(_WIN32)
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void scheduler_yield(void* context) {
    TestState* state = (TestState*)context;
    (void)atomic_increment(&state->yield_count);
    host_yield();
}

static void worker_enter(TestState* state) {
    ringpu_platform_resource_lock_acquire(&state->lock, scheduler_yield,
                                          state);
    (void)atomic_increment(&state->entered_count);
    ringpu_platform_resource_lock_release(&state->lock);
}

#if defined(_WIN32)
static DWORD WINAPI worker(void* context) {
    worker_enter((TestState*)context);
    return 0u;
}
#else
static void* worker(void* context) {
    worker_enter((TestState*)context);
    return NULL;
}
#endif

int main(void) {
    TestState state = {0};
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif
    uint32_t spin;

    ringpu_platform_resource_lock_acquire(&state.lock, NULL, NULL);
#if defined(_WIN32)
    thread = CreateThread(NULL, 0u, worker, &state, 0u, NULL);
    if (thread == NULL) {
        ringpu_platform_resource_lock_release(&state.lock);
        return 1;
    }
#else
    if (pthread_create(&thread, NULL, worker, &state) != 0) {
        ringpu_platform_resource_lock_release(&state.lock);
        return 1;
    }
#endif

    for (spin = 0u; spin < 1000000u && atomic_read(&state.yield_count) == 0L;
         ++spin)
        host_yield();
    ringpu_platform_resource_lock_release(&state.lock);

#if defined(_WIN32)
    if (WaitForSingleObject(thread, 5000u) != WAIT_OBJECT_0) {
        (void)CloseHandle(thread);
        return 2;
    }
    (void)CloseHandle(thread);
#else
    if (pthread_join(thread, NULL) != 0) return 2;
#endif

    if (atomic_read(&state.yield_count) == 0L ||
        atomic_read(&state.entered_count) != 1L ||
        atomic_read(&state.lock) != 0L)
        return 3;
    return 0;
}
