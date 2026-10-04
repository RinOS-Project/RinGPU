/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PLATFORM_THREAD_H
#define RINGPU_PLATFORM_THREAD_H

void ringpu_platform_resource_lock_acquire(volatile long* lock);
void ringpu_platform_resource_lock_release(volatile long* lock);

#endif
