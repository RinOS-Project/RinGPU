/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PLATFORM_THREAD_H
#define RINGPU_PLATFORM_THREAD_H

#include <ringpu/platform.h>

void ringpu_platform_resource_lock_acquire(
    volatile long* lock, RinGpuPlatformYieldThreadCallbackV1 yield_thread,
    void* context);
void ringpu_platform_resource_lock_release(volatile long* lock);

#endif
