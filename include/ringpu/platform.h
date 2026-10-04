/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_PLATFORM_H
#define RINGPU_PUBLIC_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION 1u

typedef void (*RinGpuPlatformYieldThreadCallbackV1)(void* context);

/* OS-core scheduling service used while RinGPU serializes shared resource
 * lifecycle operations. The callback must yield the calling thread; it must
 * not acquire a RinGPU resource lock or re-enter the runtime. The runtime
 * copies this record, while context remains caller-owned for the runtime's
 * lifetime. */
typedef struct RinGpuPlatformThreadSchedulerV1 {
    uint32_t struct_size;
    uint32_t version;
    RinGpuPlatformYieldThreadCallbackV1 yield_thread;
    void* context;
} RinGpuPlatformThreadSchedulerV1;

#ifdef __cplusplus
}
#endif

#endif /* RINGPU_PUBLIC_PLATFORM_H */
