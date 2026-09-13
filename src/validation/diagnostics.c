/* SPDX-License-Identifier: MIT */
#include "diagnostics.h"

#include <stddef.h>
#include <string.h>

#define RIN_GPU_DIAGNOSTICS_MAGIC UINT64_C(0x52494e4449414731)

typedef struct RinGpuDiagnosticsState {
    uint64_t magic;
    uint64_t device_generation;
    uint64_t next_sequence;
    uint64_t dropped_count;
    uint32_t event_count;
    uint32_t reserved0;
    uint64_t event_counts[RIN_GPU_DIAGNOSTIC_EVENT_TYPE_COUNT];
    RinGpuDiagnosticsCountersV1 counters;
    RinGpuDiagnosticEventV1 events[RIN_GPU_DIAGNOSTICS_MAX_EVENTS];
} RinGpuDiagnosticsState;

static RinGpuDiagnosticsState* diagnostics_state(
    RinGpuDiagnosticsRuntime* runtime) {
    return runtime ? (RinGpuDiagnosticsState*)runtime->opaque : NULL;
}

static int overlaps_runtime(const RinGpuDiagnosticsRuntime* runtime,
                            const void* pointer, size_t size) {
    uintptr_t first;
    uintptr_t end;
    uintptr_t candidate;
    if (!runtime || !pointer || size == 0u) return 0;
    first = (uintptr_t)runtime;
    if (size > UINTPTR_MAX - first) return 1;
    end = first + sizeof(*runtime);
    candidate = (uintptr_t)pointer;
    if (size > UINTPTR_MAX - candidate) return 1;
    return candidate < end && first < candidate + size;
}

static int valid_event(const RinGpuDiagnosticsState* state,
                       const RinGpuDiagnosticEventV1* event) {
    return state && state->magic == RIN_GPU_DIAGNOSTICS_MAGIC && event &&
           event->struct_size >= sizeof(*event) &&
           event->version == RIN_GPU_DIAGNOSTICS_VERSION &&
           event->type >= RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE &&
           event->type <= RIN_GPU_DIAGNOSTIC_PRESENT_MISS &&
           event->flags == 0u && event->sequence == 0u &&
           event->device_generation == state->device_generation &&
           event->reserved0 == 0u && event->reserved[0] == 0u &&
           event->reserved[1] == 0u;
}

static void add_saturating(uint64_t* destination, uint64_t value) {
    if (destination == NULL || value == 0u)
        return;
    if (UINT64_MAX - *destination < value)
        *destination = UINT64_MAX;
    else
        *destination += value;
}

static void account_event(RinGpuDiagnosticsState* state,
                          const RinGpuDiagnosticEventV1* event) {
    if (state == NULL || event == NULL)
        return;
    switch (event->type) {
    case RIN_GPU_DIAGNOSTIC_SUBMISSION:
        add_saturating(&state->counters.submitted_commands, event->value0);
        break;
    case RIN_GPU_DIAGNOSTIC_QUEUE_WAIT:
        add_saturating(&state->counters.queue_wait_count, 1u);
        break;
    case RIN_GPU_DIAGNOSTIC_RESET:
        add_saturating(&state->counters.reset_count, 1u);
        break;
    case RIN_GPU_DIAGNOSTIC_DEVICE_LOST:
        add_saturating(&state->counters.fault_count, 1u);
        break;
    case RIN_GPU_DIAGNOSTIC_MEMORY_PRESSURE:
        add_saturating(&state->counters.memory_pressure_count, 1u);
        break;
    case RIN_GPU_DIAGNOSTIC_PRESENT_MISS:
        add_saturating(&state->counters.present_miss_count, 1u);
        break;
    default:
        break;
    }
}

int rin_gpu_diagnostics_init(RinGpuDiagnosticsRuntime* runtime,
                             uint64_t device_generation) {
    RinGpuDiagnosticsState* state;
    if (!runtime || device_generation == 0u) return -1;
    state = diagnostics_state(runtime);
    memset(state, 0, sizeof(*state));
    state->magic = RIN_GPU_DIAGNOSTICS_MAGIC;
    state->device_generation = device_generation;
    state->next_sequence = 1u;
    state->counters.struct_size = sizeof(state->counters);
    state->counters.version = RIN_GPU_DIAGNOSTICS_VERSION;
    return 0;
}

int rin_gpu_diagnostics_advance_generation(
    RinGpuDiagnosticsRuntime* runtime, uint64_t next_device_generation) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);
    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC ||
        next_device_generation == 0u)
        return -1;
    if (next_device_generation <= state->device_generation) return -2;
    state->device_generation = next_device_generation;
    return 0;
}

int rin_gpu_diagnostics_record(
    RinGpuDiagnosticsRuntime* runtime,
    const RinGpuDiagnosticEventV1* event) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);
    RinGpuDiagnosticEventV1 copy;
    uint32_t index;
    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC ||
        overlaps_runtime(runtime, event, sizeof(*event)) ||
        !valid_event(state, event))
        return -1;
    if (state->next_sequence == UINT64_MAX) return -3;
    copy = *event;
    copy.struct_size = sizeof(copy);
    copy.sequence = state->next_sequence++;
    index = (uint32_t)((copy.sequence - 1u) %
                       RIN_GPU_DIAGNOSTICS_MAX_EVENTS);
    if (state->event_count == RIN_GPU_DIAGNOSTICS_MAX_EVENTS) {
        ++state->dropped_count;
    } else {
        ++state->event_count;
    }
    state->events[index] = copy;
    ++state->event_counts[copy.type - 1u];
    account_event(state, &copy);
    return 0;
}

int rin_gpu_diagnostics_record_simple(
    RinGpuDiagnosticsRuntime* runtime, uint32_t type,
    uint64_t timestamp_ns, uint64_t resource_cookie, uint64_t queue_cookie,
    uint64_t value0, uint64_t value1, uint32_t status) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);
    RinGpuDiagnosticEventV1 event;
    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC ||
        type < RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE ||
        type > RIN_GPU_DIAGNOSTIC_PRESENT_MISS) return -1;
    memset(&event, 0, sizeof(event));
    event.struct_size = sizeof(event);
    event.version = RIN_GPU_DIAGNOSTICS_VERSION;
    event.type = type;
    event.timestamp_ns = timestamp_ns;
    event.device_generation = state->device_generation;
    event.resource_cookie = resource_cookie;
    event.queue_cookie = queue_cookie;
    event.value0 = value0;
    event.value1 = value1;
    event.status = status;
    return rin_gpu_diagnostics_record(runtime, &event);
}

int rin_gpu_diagnostics_read(
    RinGpuDiagnosticsRuntime* runtime, uint64_t cursor, uint32_t capacity,
    RinGpuDiagnosticEventV1* events_out, uint32_t* event_count_out,
    uint64_t* next_cursor_out) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);
    uint64_t oldest;
    uint64_t available;
    uint32_t count;
    if (event_count_out) *event_count_out = 0u;
    if (next_cursor_out) *next_cursor_out = cursor;
    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC ||
        !event_count_out || !next_cursor_out ||
        (capacity != 0u && !events_out) ||
        overlaps_runtime(runtime, events_out,
                         sizeof(*events_out) * (size_t)capacity))
        return -1;
    oldest = state->next_sequence - state->event_count;
    if (cursor == 0u) cursor = oldest;
    if (cursor < oldest || cursor > state->next_sequence) {
        *next_cursor_out = oldest;
        return -2;
    }
    available = state->next_sequence - cursor;
    if (available == 0u) {
        *next_cursor_out = cursor;
        return 0;
    }
    if (capacity == 0u) return -4;
    count = available > capacity ? capacity : (uint32_t)available;
    for (uint32_t index = 0u; index < count; ++index) {
        uint64_t sequence = cursor + index;
        events_out[index] = state->events[(sequence - 1u) %
                                           RIN_GPU_DIAGNOSTICS_MAX_EVENTS];
    }
    *event_count_out = count;
    *next_cursor_out = cursor + count;
    return available > capacity ? -3 : 0;
}

int rin_gpu_diagnostics_get_stats(
    RinGpuDiagnosticsRuntime* runtime, RinGpuDiagnosticsStatsV1* stats_out) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);
    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC || !stats_out ||
        overlaps_runtime(runtime, stats_out, sizeof(*stats_out)) ||
        stats_out->struct_size < sizeof(*stats_out) ||
        stats_out->version != RIN_GPU_DIAGNOSTICS_VERSION)
        return -1;
    memset(stats_out, 0, sizeof(*stats_out));
    stats_out->struct_size = sizeof(*stats_out);
    stats_out->version = RIN_GPU_DIAGNOSTICS_VERSION;
    stats_out->device_generation = state->device_generation;
    stats_out->next_sequence = state->next_sequence;
    stats_out->event_count = state->event_count;
    stats_out->dropped_count = state->dropped_count;
    memcpy(stats_out->event_counts, state->event_counts,
           sizeof(stats_out->event_counts));
    return 0;
}

int rin_gpu_diagnostics_get_counters(
    RinGpuDiagnosticsRuntime* runtime,
    RinGpuDiagnosticsCountersV1* counters_out) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);

    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC ||
        !counters_out || overlaps_runtime(runtime, counters_out,
                                           sizeof(*counters_out)) ||
        counters_out->struct_size < sizeof(*counters_out) ||
        counters_out->version != RIN_GPU_DIAGNOSTICS_VERSION)
        return -1;
    *counters_out = state->counters;
    return 0;
}

int rin_gpu_diagnostics_add_timing(
    RinGpuDiagnosticsRuntime* runtime, uint64_t gpu_busy_time_ns) {
    RinGpuDiagnosticsState* state = diagnostics_state(runtime);

    if (!state || state->magic != RIN_GPU_DIAGNOSTICS_MAGIC)
        return -1;
    add_saturating(&state->counters.gpu_busy_time_ns, gpu_busy_time_ns);
    return 0;
}
