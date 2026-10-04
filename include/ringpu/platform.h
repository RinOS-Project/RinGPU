/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_PLATFORM_H
#define RINGPU_PUBLIC_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION 1u
#define RIN_GPU_PLATFORM_SERVICES_VERSION 1u

/* Event type values delivered to RinGpuPlatformDiagnosticCallbackV1. */
#define RIN_GPU_PLATFORM_DIAGNOSTIC_RESOURCE_CREATE 1u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_SUBMISSION 2u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_QUEUE 3u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_FENCE 4u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_PRESENT 5u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_RESET 6u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_MAPPING 7u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_DEVICE_LOST 8u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_MEMORY_PRESSURE 9u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_QUEUE_WAIT 10u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_PRESENT_MISS 11u
#define RIN_GPU_PLATFORM_DIAGNOSTIC_TYPE_COUNT 11u

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

/* Synchronous OS-core event sink. Calls may originate on any thread and may
 * be concurrent, so callers must synchronize shared callback state. The
 * callback must not re-enter RinGPU. status is the RinGPU result code
 * associated with the event, or zero for informational events. Resource and
 * queue cookies are opaque backend identifiers; value0/value1 are
 * event-specific. */
typedef void (*RinGpuPlatformDiagnosticCallbackV1)(
    void* context, uint32_t type, uint64_t resource_cookie,
    uint64_t queue_cookie, uint64_t value0, uint64_t value1, int32_t status);

/* Optional OS-core integrations used by a runtime. The scheduler record may
 * be all-zero to retain the host scheduler. The runtime copies this record;
 * callback contexts remain caller-owned for the runtime's lifetime. */
typedef struct RinGpuPlatformServicesV1 {
    uint32_t struct_size;
    uint32_t version;
    RinGpuPlatformThreadSchedulerV1 thread_scheduler;
    RinGpuPlatformDiagnosticCallbackV1 diagnostic_callback;
    void* diagnostic_context;
} RinGpuPlatformServicesV1;

#ifdef __cplusplus
}
#endif

#endif /* RINGPU_PUBLIC_PLATFORM_H */
