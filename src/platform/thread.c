/* SPDX-License-Identifier: MIT */
#include "thread.h"

#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <sched.h>
#else
#error "RinGPU platform resource locking requires MSVC, GCC, or Clang atomics"
#endif

void ringpu_platform_resource_lock_acquire(volatile long* lock) {
    if (!lock) return;
#if defined(_MSC_VER)
    while (_InterlockedExchange(lock, 1L) != 0L)
        (void)SwitchToThread();
#else
    while (__atomic_exchange_n(lock, 1L, __ATOMIC_ACQUIRE) != 0L)
        (void)sched_yield();
#endif
}

void ringpu_platform_resource_lock_release(volatile long* lock) {
    if (!lock) return;
#if defined(_MSC_VER)
    (void)_InterlockedExchange(lock, 0L);
#else
    __atomic_store_n(lock, 0L, __ATOMIC_RELEASE);
#endif
}
