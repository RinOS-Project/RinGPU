/* SPDX-License-Identifier: MIT */
#ifndef RIN_SUBSYSTEMS_RINGPU_DIAGNOSTICS_H
#define RIN_SUBSYSTEMS_RINGPU_DIAGNOSTICS_H

#include <stdint.h>

#define RIN_GPU_DIAGNOSTICS_VERSION 1u
#define RIN_GPU_DIAGNOSTICS_RUNTIME_STATE_QWORDS 4096u
#define RIN_GPU_DIAGNOSTICS_MAX_EVENTS 256u

typedef enum RinGpuDiagnosticEventTypeV1 {
    RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE = 1,
    RIN_GPU_DIAGNOSTIC_SUBMISSION = 2,
    RIN_GPU_DIAGNOSTIC_QUEUE = 3,
    RIN_GPU_DIAGNOSTIC_FENCE = 4,
    RIN_GPU_DIAGNOSTIC_PRESENT = 5,
    RIN_GPU_DIAGNOSTIC_RESET = 6,
    RIN_GPU_DIAGNOSTIC_MAPPING = 7,
    RIN_GPU_DIAGNOSTIC_DEVICE_LOST = 8,
    RIN_GPU_DIAGNOSTIC_MEMORY_PRESSURE = 9,
    RIN_GPU_DIAGNOSTIC_QUEUE_WAIT = 10,
    RIN_GPU_DIAGNOSTIC_PRESENT_MISS = 11,
    RIN_GPU_DIAGNOSTIC_EVENT_TYPE_COUNT = 11
} RinGpuDiagnosticEventTypeV1;

typedef struct RinGpuDiagnosticEventV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t type;
    uint32_t flags;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t device_generation;
    uint64_t resource_cookie;
    uint64_t queue_cookie;
    uint64_t value0;
    uint64_t value1;
    uint32_t status;
    uint32_t reserved0;
    uint64_t reserved[2];
} RinGpuDiagnosticEventV1;

typedef struct RinGpuDiagnosticsStatsV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t device_generation;
    uint64_t next_sequence;
    uint32_t event_count;
    uint32_t reserved0;
    uint64_t dropped_count;
    uint64_t event_counts[RIN_GPU_DIAGNOSTIC_EVENT_TYPE_COUNT];
} RinGpuDiagnosticsStatsV1;

/* Monotonic execution counters are separate from the event-ring ABI so the
 * fixed V1 stats record remains layout-compatible. Counts are saturating;
 * the elapsed value is measured for work observed in flight by the device
 * runtime. */
typedef struct RinGpuDiagnosticsCountersV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t submitted_commands;
    uint64_t gpu_busy_time_ns;
    uint64_t queue_wait_count;
    uint64_t reset_count;
    uint64_t fault_count;
    uint64_t memory_pressure_count;
    uint64_t present_miss_count;
} RinGpuDiagnosticsCountersV1;

typedef struct RinGpuDiagnosticsRuntime {
    uint64_t opaque[RIN_GPU_DIAGNOSTICS_RUNTIME_STATE_QWORDS];
} RinGpuDiagnosticsRuntime;

int rin_gpu_diagnostics_init(RinGpuDiagnosticsRuntime* runtime,
                             uint64_t device_generation);
int rin_gpu_diagnostics_advance_generation(
    RinGpuDiagnosticsRuntime* runtime, uint64_t next_device_generation);
int rin_gpu_diagnostics_record(
    RinGpuDiagnosticsRuntime* runtime,
    const RinGpuDiagnosticEventV1* event);
int rin_gpu_diagnostics_record_simple(
    RinGpuDiagnosticsRuntime* runtime, uint32_t type,
    uint64_t timestamp_ns, uint64_t resource_cookie, uint64_t queue_cookie,
    uint64_t value0, uint64_t value1, uint32_t status);
/* cursor is a sequence number; zero starts at the oldest retained event.
 * When capacity is smaller than the available range, only capacity events
 * are copied and LIMIT is returned with a resumable next_cursor. */
int rin_gpu_diagnostics_read(
    RinGpuDiagnosticsRuntime* runtime, uint64_t cursor, uint32_t capacity,
    RinGpuDiagnosticEventV1* events_out, uint32_t* event_count_out,
    uint64_t* next_cursor_out);
int rin_gpu_diagnostics_get_stats(
    RinGpuDiagnosticsRuntime* runtime, RinGpuDiagnosticsStatsV1* stats_out);
int rin_gpu_diagnostics_get_counters(
    RinGpuDiagnosticsRuntime* runtime,
    RinGpuDiagnosticsCountersV1* counters_out);
int rin_gpu_diagnostics_add_timing(
    RinGpuDiagnosticsRuntime* runtime, uint64_t gpu_busy_time_ns);

#if defined(__cplusplus)
static_assert(sizeof(RinGpuDiagnosticEventV1) == 96u,
              "RinGPU diagnostic event drift");
static_assert(sizeof(RinGpuDiagnosticsStatsV1) == 128u,
              "RinGPU diagnostic stats drift");
static_assert(sizeof(RinGpuDiagnosticsCountersV1) == 64u,
              "RinGPU diagnostic counters drift");
#else
_Static_assert(sizeof(RinGpuDiagnosticEventV1) == 96u,
               "RinGPU diagnostic event drift");
_Static_assert(sizeof(RinGpuDiagnosticsStatsV1) == 128u,
               "RinGPU diagnostic stats drift");
_Static_assert(sizeof(RinGpuDiagnosticsCountersV1) == 64u,
               "RinGPU diagnostic counters drift");
#endif

#endif /* RIN_SUBSYSTEMS_RINGPU_DIAGNOSTICS_H */
