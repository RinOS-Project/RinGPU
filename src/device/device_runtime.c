/* SPDX-License-Identifier: MIT */
#include "device_runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
static uint32_t ringpu_device_lock_exchange(volatile uint32_t* value,
                                            uint32_t replacement) {
    return (uint32_t)_InterlockedExchange((volatile long*)(void*)value,
                                          (long)replacement);
}
static void ringpu_device_lock_store(volatile uint32_t* value,
                                     uint32_t replacement) {
    (void)_InterlockedExchange((volatile long*)(void*)value,
                               (long)replacement);
}
#else
static uint32_t ringpu_device_lock_exchange(volatile uint32_t* value,
                                            uint32_t replacement) {
    return __atomic_exchange_n(value, replacement, __ATOMIC_ACQUIRE);
}
static void ringpu_device_lock_store(volatile uint32_t* value,
                                     uint32_t replacement) {
    __atomic_store_n(value, replacement, __ATOMIC_RELEASE);
}
#endif

#define RIN_GPU_DEVICE_RUNTIME_MAGIC UINT64_C(0x52474452554e5631)

typedef struct RinGpuDeviceQueueState {
    uint64_t submitted_sequence;
    uint64_t submitted_value;
    uint64_t completed_sequence;
    uint64_t completed_value;
} RinGpuDeviceQueueState;

typedef struct RinGpuDeviceInFlight {
    uint32_t active;
    uint32_t queue_id;
    uint64_t sequence;
    uint64_t command_cookie;
    uint64_t completion_value;
    uint64_t device_epoch;
    uint64_t deadline_ns;
    uint64_t submitted_at_ns;
} RinGpuDeviceInFlight;

typedef struct RinGpuDeviceRuntimeState {
    uint64_t magic;
    uint32_t flags;
    uint32_t reset_count;
    uint32_t in_flight_count;
    uint32_t reserved0;
    RinGpuDeviceBackendV1 backend;
    RinGpuDeviceBackendV1 admitted_backend;
    RinGpuDiagnosticsRuntime* diagnostics;
    uint64_t device_epoch;
    uint64_t last_monotonic_ns;
    uint64_t iommu_map_generation;
    RinGpuDeviceQueueState queues[RIN_GPU_DEVICE_MAX_QUEUES];
    RinGpuDeviceInFlight in_flight[RIN_GPU_DEVICE_MAX_IN_FLIGHT];
    volatile uint32_t api_lock;
    uint32_t callback_active;
    uint64_t guard_hash;
} RinGpuDeviceRuntimeState;

typedef struct RinGpuDeviceCompletionBuffer {
    uint64_t prefix_canary[2];
    RinGpuDeviceCompletionV1 values[RIN_GPU_DEVICE_MAX_IN_FLIGHT];
    uint64_t suffix_canary[2];
} RinGpuDeviceCompletionBuffer;

_Static_assert(sizeof(RinGpuDeviceRuntimeState) <= sizeof(RinGpuDeviceRuntime),
               "RinGPU device runtime opaque state is too small");

static int ringpu_device_binding_valid(
    const RinGpuDeviceRuntimeState* state);

static RinGpuDeviceRuntimeState* ringpu_device_state(
    RinGpuDeviceRuntime* runtime) {
    return (RinGpuDeviceRuntimeState*)(void*)runtime;
}

static int ringpu_device_overlap(const void* left, size_t left_size,
                                 const void* right, size_t right_size) {
    uintptr_t left_address;
    uintptr_t right_address;

    if (!left || !right || left_size == 0u || right_size == 0u) return 0;
    left_address = (uintptr_t)left;
    right_address = (uintptr_t)right;
    if (left_address <= right_address) {
        return right_address - left_address < left_size;
    }
    return left_address - right_address < right_size;
}

static int ringpu_device_all_zero(const uint64_t* values, size_t count) {
    size_t index;

    for (index = 0u; index < count; index++) {
        if (values[index] != 0u) return 0;
    }
    return 1;
}

static uint64_t ringpu_device_hash(const RinGpuDeviceRuntimeState* state) {
    const uint8_t* bytes = (const uint8_t*)(const void*)state;
    const size_t size = offsetof(RinGpuDeviceRuntimeState, api_lock);
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int ringpu_device_lock(RinGpuDeviceRuntime* runtime,
                              RinGpuDeviceRuntimeState** state_out) {
    RinGpuDeviceRuntimeState* state;

    if (!runtime || !state_out) return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    state = ringpu_device_state(runtime);
    if (state->magic != RIN_GPU_DEVICE_RUNTIME_MAGIC) {
        return RIN_GPU_DEVICE_STATE;
    }
    if (ringpu_device_lock_exchange(&state->api_lock, 1u) != 0u) {
        return RIN_GPU_DEVICE_BUSY;
    }
    if (state->magic != RIN_GPU_DEVICE_RUNTIME_MAGIC) {
        ringpu_device_lock_store(&state->api_lock, 0u);
        return RIN_GPU_DEVICE_STATE;
    }
    if (!ringpu_device_binding_valid(state)) {
        state->flags = RIN_GPU_DEVICE_STATUS_LOST;
        ringpu_device_lock_store(&state->api_lock, 0u);
        return RIN_GPU_DEVICE_PROTOCOL;
    }
    *state_out = state;
    return RIN_GPU_DEVICE_OK;
}

static void ringpu_device_unlock(RinGpuDeviceRuntimeState* state) {
    ringpu_device_lock_store(&state->api_lock, 0u);
}

static void ringpu_device_mark_lost(RinGpuDeviceRuntimeState* state) {
    state->flags = RIN_GPU_DEVICE_STATUS_LOST;
    if (state->diagnostics) {
        (void)rin_gpu_diagnostics_record_simple(
            state->diagnostics, RIN_GPU_DIAGNOSTIC_DEVICE_LOST,
            state->last_monotonic_ns, 0u, 0u, state->device_epoch, 0u,
            RIN_GPU_DEVICE_STATUS_LOST);
    }
}

static void ringpu_device_diagnostic(RinGpuDeviceRuntimeState* state,
                                     uint32_t type, uint64_t value0,
                                     uint64_t value1, uint32_t status) {
    if (!state || !state->diagnostics) return;
    (void)rin_gpu_diagnostics_record_simple(
        state->diagnostics, type, state->last_monotonic_ns, 0u, 0u, value0,
        value1, status);
}

static int ringpu_device_callback_begin(RinGpuDeviceRuntimeState* state) {
    if (state->callback_active != 0u) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_PROTOCOL;
    }
    state->callback_active = 1u;
    state->guard_hash = ringpu_device_hash(state);
    return RIN_GPU_DEVICE_OK;
}

static int ringpu_device_callback_end(RinGpuDeviceRuntimeState* state) {
    const int unchanged = state->guard_hash == ringpu_device_hash(state);

    state->guard_hash = 0u;
    state->callback_active = 0u;
    if (!unchanged) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_PROTOCOL;
    }
    return RIN_GPU_DEVICE_OK;
}

static int ringpu_device_backend_valid(const RinGpuDeviceBackendV1* backend) {
    const uint32_t required = RIN_GPU_DEVICE_CAP_DMA_ISOLATED |
                              RIN_GPU_DEVICE_CAP_RESET;

    if (!backend || backend->struct_size != sizeof(*backend) ||
        backend->version != RIN_GPU_DEVICE_RUNTIME_VERSION ||
        (backend->capabilities & ~RIN_GPU_DEVICE_CAP_KNOWN) != 0u ||
        (backend->capabilities & required) != required ||
        backend->queue_count == 0u ||
        backend->queue_count > RIN_GPU_DEVICE_MAX_QUEUES ||
        backend->max_in_flight == 0u ||
        backend->max_in_flight > RIN_GPU_DEVICE_MAX_IN_FLIGHT ||
        backend->max_reset_attempts == 0u ||
        backend->max_reset_attempts > RIN_GPU_DEVICE_MAX_RESET_ATTEMPTS ||
        backend->minimum_timeout_ns < RIN_GPU_DEVICE_MIN_TIMEOUT_NS ||
        backend->maximum_timeout_ns > RIN_GPU_DEVICE_MAX_TIMEOUT_NS ||
        backend->minimum_timeout_ns > backend->maximum_timeout_ns ||
        backend->iommu_domain_cookie == 0u ||
        backend->iommu_map_generation == 0u || !backend->context ||
        !backend->now_ns || !backend->submit || !backend->poll ||
        !backend->reset ||
        (((backend->capabilities & RIN_GPU_DEVICE_CAP_POWER) != 0u) !=
         (backend->set_power != NULL)) ||
        !ringpu_device_all_zero(backend->reserved,
                                RIN_GPU_DEVICE_BACKEND_RESERVED_QWORDS)) {
        return 0;
    }
    return 1;
}

static int ringpu_device_backend_equal(
    const RinGpuDeviceBackendV1* first,
    const RinGpuDeviceBackendV1* second) {
    const uint8_t* left;
    const uint8_t* right;
    size_t index;

    if (!first || !second) return 0;
    left = (const uint8_t*)(const void*)first;
    right = (const uint8_t*)(const void*)second;
    for (index = 0u; index < sizeof(*first); index++) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static int ringpu_device_binding_valid(
    const RinGpuDeviceRuntimeState* state) {
    return state && ringpu_device_backend_valid(&state->backend) &&
           ringpu_device_backend_equal(&state->backend,
                                       &state->admitted_backend);
}

static int ringpu_device_now(RinGpuDeviceRuntimeState* state,
                             uint64_t* now_out) {
    uint64_t now;
    int result;

    result = ringpu_device_callback_begin(state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    now = state->backend.now_ns(state->backend.context);
    result = ringpu_device_callback_end(state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (now == 0u || now < state->last_monotonic_ns) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_PROTOCOL;
    }
    state->last_monotonic_ns = now;
    *now_out = now;
    return RIN_GPU_DEVICE_OK;
}

static int ringpu_device_deadline(uint64_t now, uint64_t timeout,
                                  uint64_t* deadline_out) {
    if (timeout > UINT64_MAX - now) return RIN_GPU_DEVICE_PROTOCOL;
    *deadline_out = now + timeout;
    return RIN_GPU_DEVICE_OK;
}

static RinGpuDeviceInFlight* ringpu_device_free_slot(
    RinGpuDeviceRuntimeState* state) {
    uint32_t index;

    for (index = 0u; index < state->backend.max_in_flight; index++) {
        if (state->in_flight[index].active == 0u) {
            return &state->in_flight[index];
        }
    }
    return NULL;
}

static RinGpuDeviceInFlight* ringpu_device_find_slot(
    RinGpuDeviceRuntimeState* state, uint32_t queue_id, uint64_t sequence,
    uint64_t completion_value, uint64_t device_epoch, uint32_t* index_out) {
    uint32_t index;

    for (index = 0u; index < state->backend.max_in_flight; index++) {
        RinGpuDeviceInFlight* slot = &state->in_flight[index];
        if (slot->active != 0u && slot->queue_id == queue_id &&
            slot->sequence == sequence &&
            slot->completion_value == completion_value &&
            slot->device_epoch == device_epoch) {
            if (index_out) *index_out = index;
            return slot;
        }
    }
    return NULL;
}

static int ringpu_device_submission_unique(
    const RinGpuDeviceRuntimeState* state,
    const RinGpuDeviceSubmissionV1* submission) {
    uint32_t index;

    for (index = 0u; index < state->backend.max_in_flight; index++) {
        const RinGpuDeviceInFlight* slot = &state->in_flight[index];
        if (slot->active != 0u &&
            (slot->command_cookie == submission->command_cookie ||
             (slot->queue_id == submission->queue_id &&
              slot->completion_value == submission->completion_value))) {
            return 0;
        }
    }
    return 1;
}

static void ringpu_device_report(const RinGpuDeviceRuntimeState* state,
                                 RinGpuDevicePollReportV1* report,
                                 uint32_t flags, uint32_t completed_count,
                                 uint32_t failed_count, uint64_t now) {
    memset(report, 0, sizeof(*report));
    report->struct_size = sizeof(*report);
    report->version = RIN_GPU_DEVICE_RUNTIME_VERSION;
    report->flags = flags;
    report->completed_count = completed_count;
    report->failed_count = failed_count;
    report->in_flight_count = state->in_flight_count;
    report->reset_count = state->reset_count;
    report->device_epoch = state->device_epoch;
    report->observed_monotonic_ns = now;
}

static void ringpu_device_abort_all(RinGpuDeviceRuntimeState* state) {
    uint32_t queue_id;

    memset(state->in_flight, 0, sizeof(state->in_flight));
    state->in_flight_count = 0u;
    for (queue_id = 0u; queue_id < state->backend.queue_count; queue_id++) {
        state->queues[queue_id].completed_sequence =
            state->queues[queue_id].submitted_sequence;
    }
}

static int ringpu_device_recover(RinGpuDeviceRuntimeState* state,
                                 uint64_t now, uint32_t reason_flags,
                                 RinGpuDevicePollReportV1* report) {
    const uint32_t failed_count = state->in_flight_count;
    const uint64_t old_map_generation = state->iommu_map_generation;
    uint64_t new_generation = 0u;
    uint64_t deadline;
    uint64_t new_epoch;
    int backend_result;
    int result;

    if (state->reset_count >= state->backend.max_reset_attempts ||
        state->device_epoch == UINT64_MAX) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_LOST;
    }
    result = ringpu_device_deadline(now, state->backend.maximum_timeout_ns,
                                    &deadline);
    if (result != RIN_GPU_DEVICE_OK) {
        ringpu_device_mark_lost(state);
        return result;
    }
    new_epoch = state->device_epoch + 1u;
    result = ringpu_device_callback_begin(state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    backend_result = state->backend.reset(
        state->backend.context, state->device_epoch, new_epoch, deadline,
        &new_generation);
    result = ringpu_device_callback_end(state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (backend_result != 0) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_BACKEND_FAILED;
    }
    if (new_generation <= state->iommu_map_generation) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_PROTOCOL;
    }

    ringpu_device_abort_all(state);
    ringpu_device_diagnostic(state, RIN_GPU_DIAGNOSTIC_RESET,
                             state->device_epoch, new_epoch, reason_flags);
    state->device_epoch = new_epoch;
    state->iommu_map_generation = new_generation;
    if (state->diagnostics &&
        rin_gpu_diagnostics_advance_generation(state->diagnostics, new_epoch) !=
            0) {
        ringpu_device_mark_lost(state);
        return RIN_GPU_DEVICE_PROTOCOL;
    }
    ringpu_device_diagnostic(state, RIN_GPU_DIAGNOSTIC_MAPPING,
                             old_map_generation, new_generation, 0u);
    state->reset_count++;
    ringpu_device_report(state, report,
                         reason_flags | RIN_GPU_DEVICE_REPORT_RESET, 0u,
                         failed_count, now);
    return RIN_GPU_DEVICE_OK;
}

int rin_gpu_device_runtime_init(RinGpuDeviceRuntime* runtime,
                                const RinGpuDeviceBackendV1* backend) {
    RinGpuDeviceBackendV1 backend_copy;
    RinGpuDeviceRuntimeState* state;
    uint64_t now;
    int result;

    if (!runtime || !backend ||
        ringpu_device_overlap(runtime, sizeof(*runtime), backend,
                              sizeof(*backend))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memcpy(&backend_copy, backend, sizeof(backend_copy));
    if (!ringpu_device_backend_valid(&backend_copy) ||
        ((uintptr_t)backend_copy.context >= (uintptr_t)runtime &&
         (uintptr_t)backend_copy.context - (uintptr_t)runtime <
             sizeof(*runtime))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    state = ringpu_device_state(runtime);
    if (state->magic == RIN_GPU_DEVICE_RUNTIME_MAGIC) {
        return RIN_GPU_DEVICE_STATE;
    }
    memset(runtime, 0, sizeof(*runtime));
    state->magic = RIN_GPU_DEVICE_RUNTIME_MAGIC;
    state->flags = RIN_GPU_DEVICE_STATUS_ACTIVE;
    state->backend = backend_copy;
    state->admitted_backend = backend_copy;
    state->device_epoch = 1u;
    state->iommu_map_generation = backend_copy.iommu_map_generation;
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) {
        memset(runtime, 0, sizeof(*runtime));
        return result;
    }
    (void)now;
    return RIN_GPU_DEVICE_OK;
}

int rin_gpu_device_runtime_submit(
    RinGpuDeviceRuntime* runtime,
    const RinGpuDeviceSubmissionV1* submission) {
    RinGpuDeviceSubmissionV1 candidate;
    RinGpuDeviceSubmissionV1 immutable_candidate;
    RinGpuDeviceRuntimeState* state;
    RinGpuDeviceQueueState* queue;
    RinGpuDeviceInFlight* slot;
    uint64_t now;
    uint64_t minimum_deadline;
    uint64_t maximum_deadline;
    int backend_result;
    int result;

    if (!runtime || !submission ||
        ringpu_device_overlap(runtime, sizeof(*runtime), submission,
                              sizeof(*submission))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memcpy(&candidate, submission, sizeof(candidate));
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags == RIN_GPU_DEVICE_STATUS_LOST) {
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    if (candidate.struct_size != sizeof(candidate) ||
        candidate.version != RIN_GPU_DEVICE_RUNTIME_VERSION ||
        candidate.flags != 0u || candidate.reserved != 0u ||
        candidate.queue_id >= state->backend.queue_count ||
        candidate.command_cookie == 0u || candidate.sequence == 0u ||
        candidate.completion_value == 0u) {
        result = RIN_GPU_DEVICE_INVALID_ARGUMENT;
        goto done;
    }
    if (candidate.iommu_domain_cookie !=
            state->backend.iommu_domain_cookie ||
        candidate.iommu_map_generation != state->iommu_map_generation ||
        candidate.device_epoch != state->device_epoch) {
        result = RIN_GPU_DEVICE_STALE;
        goto done;
    }
    queue = &state->queues[candidate.queue_id];
    if (queue->submitted_sequence == UINT64_MAX ||
        candidate.sequence != queue->submitted_sequence + 1u ||
        candidate.completion_value <= queue->submitted_value) {
        result = RIN_GPU_DEVICE_STALE;
        goto done;
    }
    if (state->in_flight_count >= state->backend.max_in_flight) {
        result = RIN_GPU_DEVICE_LIMIT;
        goto done;
    }
    if (!ringpu_device_submission_unique(state, &candidate)) {
        result = RIN_GPU_DEVICE_STALE;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    result = ringpu_device_deadline(now, state->backend.minimum_timeout_ns,
                                    &minimum_deadline);
    if (result != RIN_GPU_DEVICE_OK) {
        ringpu_device_mark_lost(state);
        goto done;
    }
    result = ringpu_device_deadline(now, state->backend.maximum_timeout_ns,
                                    &maximum_deadline);
    if (result != RIN_GPU_DEVICE_OK) {
        ringpu_device_mark_lost(state);
        goto done;
    }
    if (candidate.deadline_ns < minimum_deadline ||
        candidate.deadline_ns > maximum_deadline) {
        result = RIN_GPU_DEVICE_TIMEOUT;
        goto done;
    }
    slot = ringpu_device_free_slot(state);
    if (!slot) {
        result = RIN_GPU_DEVICE_LIMIT;
        goto done;
    }
    immutable_candidate = candidate;
    result = ringpu_device_callback_begin(state);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    backend_result = state->backend.submit(state->backend.context, &candidate);
    result = ringpu_device_callback_end(state);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    if (memcmp(&candidate, &immutable_candidate, sizeof(candidate)) != 0) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    if (backend_result != 0) {
        result = RIN_GPU_DEVICE_BACKEND_FAILED;
        goto done;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = 1u;
    slot->queue_id = candidate.queue_id;
    slot->sequence = candidate.sequence;
    slot->command_cookie = candidate.command_cookie;
    slot->completion_value = candidate.completion_value;
    slot->device_epoch = candidate.device_epoch;
    slot->deadline_ns = candidate.deadline_ns;
    slot->submitted_at_ns = now;
    queue->submitted_sequence = candidate.sequence;
    queue->submitted_value = candidate.completion_value;
    state->in_flight_count++;
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_prepare_submission(
    RinGpuDeviceRuntime* runtime, uint32_t queue_id, uint64_t command_cookie,
    RinGpuDeviceSubmissionV1* submission_out) {
    RinGpuDeviceRuntimeState* state;
    RinGpuDeviceQueueState* queue;
    uint64_t now;
    uint64_t deadline;
    int result;

    if (!submission_out) return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    if (runtime && ringpu_device_overlap(runtime, sizeof(*runtime),
                                         submission_out,
                                         sizeof(*submission_out))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memset(submission_out, 0, sizeof(*submission_out));
    if (!runtime || command_cookie == 0u) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags == RIN_GPU_DEVICE_STATUS_LOST) {
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    if (queue_id >= state->backend.queue_count) {
        result = RIN_GPU_DEVICE_INVALID_ARGUMENT;
        goto done;
    }
    queue = &state->queues[queue_id];
    if (queue->submitted_sequence == UINT64_MAX ||
        queue->submitted_value == UINT64_MAX) {
        result = RIN_GPU_DEVICE_LIMIT;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    /* Use the runtime's largest admitted window.  Submit validates the
     * snapshot again, so a concurrent producer or a clock discontinuity is
     * rejected before backend work rather than being guessed at by callers. */
    result = ringpu_device_deadline(now, state->backend.maximum_timeout_ns,
                                    &deadline);
    if (result != RIN_GPU_DEVICE_OK) {
        ringpu_device_mark_lost(state);
        goto done;
    }
    submission_out->struct_size = sizeof(*submission_out);
    submission_out->version = RIN_GPU_DEVICE_RUNTIME_VERSION;
    submission_out->queue_id = queue_id;
    submission_out->sequence = queue->submitted_sequence + 1u;
    submission_out->command_cookie = command_cookie;
    submission_out->completion_value = queue->submitted_value + 1u;
    submission_out->iommu_domain_cookie = state->backend.iommu_domain_cookie;
    submission_out->iommu_map_generation = state->iommu_map_generation;
    submission_out->device_epoch = state->device_epoch;
    submission_out->deadline_ns = deadline;
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_poll(RinGpuDeviceRuntime* runtime,
                               RinGpuDevicePollReportV1* report) {
    static const uint64_t canary0 = UINT64_C(0xa13bc9475d2e608f);
    static const uint64_t canary1 = UINT64_C(0x6f802ed547c93ba1);
    RinGpuDeviceCompletionBuffer buffer;
    RinGpuDeviceRuntimeState* state;
    uint64_t expected_sequence[RIN_GPU_DEVICE_MAX_QUEUES];
    uint64_t now;
    uint32_t count = UINT32_MAX;
    uint64_t seen = 0u;
    uint32_t completed_count = 0u;
    uint32_t faulted = 0u;
    uint32_t index;
    int backend_result;
    int result;

    if (!runtime || !report ||
        ringpu_device_overlap(runtime, sizeof(*runtime), report,
                              sizeof(*report))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags == RIN_GPU_DEVICE_STATUS_LOST) {
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    memset(&buffer, 0, sizeof(buffer));
    buffer.prefix_canary[0] = canary0;
    buffer.prefix_canary[1] = canary1;
    buffer.suffix_canary[0] = canary1;
    buffer.suffix_canary[1] = canary0;
    result = ringpu_device_callback_begin(state);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    backend_result = state->backend.poll(
        state->backend.context, buffer.values,
        state->backend.max_in_flight, &count);
    result = ringpu_device_callback_end(state);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    if (backend_result != 0) {
        result = RIN_GPU_DEVICE_BACKEND_FAILED;
        goto done;
    }
    if (buffer.prefix_canary[0] != canary0 ||
        buffer.prefix_canary[1] != canary1 ||
        buffer.suffix_canary[0] != canary1 ||
        buffer.suffix_canary[1] != canary0 ||
        count == UINT32_MAX || count > state->backend.max_in_flight ||
        count > state->in_flight_count) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    for (index = 0u; index < state->backend.queue_count; index++) {
        expected_sequence[index] =
            state->queues[index].completed_sequence == UINT64_MAX
                ? 0u
                : state->queues[index].completed_sequence + 1u;
    }
    for (index = 0u; index < count; index++) {
        const RinGpuDeviceCompletionV1* completion = &buffer.values[index];
        uint32_t slot_index;

        if (completion->struct_size != sizeof(*completion) ||
            completion->version != RIN_GPU_DEVICE_RUNTIME_VERSION ||
            completion->queue_id >= state->backend.queue_count ||
            completion->status > RIN_GPU_DEVICE_COMPLETION_FAULT ||
            completion->sequence == 0u ||
            completion->completion_value == 0u ||
            completion->device_epoch != state->device_epoch ||
            completion->reserved != 0u ||
            completion->sequence != expected_sequence[completion->queue_id] ||
            !ringpu_device_find_slot(
                state, completion->queue_id, completion->sequence,
                completion->completion_value, completion->device_epoch,
                &slot_index) ||
            (seen & (UINT64_C(1) << slot_index)) != 0u) {
            ringpu_device_mark_lost(state);
            result = RIN_GPU_DEVICE_PROTOCOL;
            goto done;
        }
        seen |= UINT64_C(1) << slot_index;
        expected_sequence[completion->queue_id]++;
        if (completion->status == RIN_GPU_DEVICE_COMPLETION_FAULT) {
            faulted = 1u;
        }
    }
    if (faulted != 0u) {
        result = ringpu_device_recover(
            state, now, RIN_GPU_DEVICE_REPORT_DEVICE_FAULT, report);
        goto done;
    }
    for (index = 0u; index < count; index++) {
        const RinGpuDeviceCompletionV1* completion = &buffer.values[index];
        RinGpuDeviceInFlight* slot = ringpu_device_find_slot(
            state, completion->queue_id, completion->sequence,
            completion->completion_value, completion->device_epoch, NULL);

        if (state->diagnostics && now >= slot->submitted_at_ns)
            (void)rin_gpu_diagnostics_add_timing(
                state->diagnostics, now - slot->submitted_at_ns);
        state->queues[completion->queue_id].completed_sequence =
            completion->sequence;
        state->queues[completion->queue_id].completed_value =
            completion->completion_value;
        memset(slot, 0, sizeof(*slot));
        state->in_flight_count--;
        completed_count++;
    }
    ringpu_device_report(state, report, 0u, completed_count, 0u, now);
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_accept_irq(
    RinGpuDeviceRuntime* runtime,
    const RinGpuDeviceCompletionV1* completion_input,
    RinGpuDevicePollReportV1* report) {
    RinGpuDeviceCompletionV1 completion;
    RinGpuDeviceRuntimeState* state;
    RinGpuDeviceInFlight* slot;
    uint64_t expected_sequence;
    uint64_t now;
    int result;

    if (!runtime || !completion_input || !report ||
        ringpu_device_overlap(runtime, sizeof(*runtime), completion_input,
                              sizeof(*completion_input)) ||
        ringpu_device_overlap(runtime, sizeof(*runtime), report,
                              sizeof(*report))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memcpy(&completion, completion_input, sizeof(completion));
    memset(report, 0, sizeof(*report));
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags == RIN_GPU_DEVICE_STATUS_LOST) {
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    if (completion.struct_size != sizeof(completion) ||
        completion.version != RIN_GPU_DEVICE_RUNTIME_VERSION ||
        completion.queue_id >= state->backend.queue_count ||
        completion.status > RIN_GPU_DEVICE_COMPLETION_FAULT ||
        completion.sequence == 0u || completion.completion_value == 0u ||
        completion.device_epoch != state->device_epoch ||
        completion.reserved != 0u) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    expected_sequence = state->queues[completion.queue_id].completed_sequence ==
            UINT64_MAX
        ? 0u
        : state->queues[completion.queue_id].completed_sequence + 1u;
    if (completion.sequence != expected_sequence ||
        !ringpu_device_find_slot(state, completion.queue_id,
                                 completion.sequence,
                                 completion.completion_value,
                                 completion.device_epoch, NULL)) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    if (completion.status == RIN_GPU_DEVICE_COMPLETION_FAULT) {
        result = ringpu_device_recover(
            state, now, RIN_GPU_DEVICE_REPORT_DEVICE_FAULT, report);
        goto done;
    }
    slot = ringpu_device_find_slot(state, completion.queue_id,
                                   completion.sequence,
                                   completion.completion_value,
                                   completion.device_epoch, NULL);
    if (!slot) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    state->queues[completion.queue_id].completed_sequence =
        completion.sequence;
    state->queues[completion.queue_id].completed_value =
        completion.completion_value;
    if (state->diagnostics && now >= slot->submitted_at_ns)
        (void)rin_gpu_diagnostics_add_timing(
            state->diagnostics, now - slot->submitted_at_ns);
    memset(slot, 0, sizeof(*slot));
    state->in_flight_count--;
    ringpu_device_report(state, report, 0u, 1u, 0u, now);
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_watchdog(RinGpuDeviceRuntime* runtime,
                                   RinGpuDevicePollReportV1* report) {
    RinGpuDeviceRuntimeState* state;
    uint64_t now;
    uint32_t index;
    int result;

    if (!runtime || !report ||
        ringpu_device_overlap(runtime, sizeof(*runtime), report,
                              sizeof(*report))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags == RIN_GPU_DEVICE_STATUS_LOST) {
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    for (index = 0u; index < state->backend.max_in_flight; index++) {
        if (state->in_flight[index].active != 0u &&
            state->in_flight[index].deadline_ns <= now) {
            result = ringpu_device_recover(
                state, now, RIN_GPU_DEVICE_REPORT_WATCHDOG, report);
            goto done;
        }
    }
    ringpu_device_report(state, report, 0u, 0u, 0u, now);
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_reset(RinGpuDeviceRuntime* runtime,
                                 RinGpuDevicePollReportV1* report) {
    RinGpuDeviceRuntimeState* state;
    uint64_t now;
    int result;

    if (!runtime || !report ||
        ringpu_device_overlap(runtime, sizeof(*runtime), report,
                              sizeof(*report))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) {
        result = state->flags == RIN_GPU_DEVICE_STATUS_LOST
                     ? RIN_GPU_DEVICE_LOST
                     : RIN_GPU_DEVICE_STATE;
        goto done;
    }
    if ((state->backend.capabilities & RIN_GPU_DEVICE_CAP_RESET) == 0u) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    result = ringpu_device_recover(state, now, 0u, report);

done:
    ringpu_device_unlock(state);
    return result;
}

static int ringpu_device_power(RinGpuDeviceRuntime* runtime,
                               uint32_t power_state) {
    RinGpuDeviceRuntimeState* state;
    uint64_t old_map_generation;
    uint64_t new_generation = 0u;
    uint64_t new_epoch;
    uint64_t deadline;
    uint64_t now;
    int backend_result;
    int result;

    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->flags == RIN_GPU_DEVICE_STATUS_LOST) {
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    if ((state->backend.capabilities & RIN_GPU_DEVICE_CAP_POWER) == 0u) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    if ((power_state == RIN_GPU_DEVICE_POWER_SUSPEND &&
         state->flags != RIN_GPU_DEVICE_STATUS_ACTIVE) ||
        (power_state == RIN_GPU_DEVICE_POWER_RESUME &&
         state->flags != RIN_GPU_DEVICE_STATUS_SUSPENDED)) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    if (state->in_flight_count != 0u) {
        result = RIN_GPU_DEVICE_BUSY;
        goto done;
    }
    if (power_state == RIN_GPU_DEVICE_POWER_RESUME &&
        state->device_epoch == UINT64_MAX) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_LOST;
        goto done;
    }
    result = ringpu_device_now(state, &now);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    old_map_generation = state->iommu_map_generation;
    result = ringpu_device_deadline(now, state->backend.maximum_timeout_ns,
                                    &deadline);
    if (result != RIN_GPU_DEVICE_OK) {
        ringpu_device_mark_lost(state);
        goto done;
    }
    new_epoch = state->device_epoch;
    if (power_state == RIN_GPU_DEVICE_POWER_RESUME) new_epoch++;
    result = ringpu_device_callback_begin(state);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    backend_result = state->backend.set_power(
        state->backend.context, power_state, state->device_epoch, new_epoch,
        deadline, &new_generation);
    result = ringpu_device_callback_end(state);
    if (result != RIN_GPU_DEVICE_OK) goto done;
    if (backend_result != 0) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_BACKEND_FAILED;
        goto done;
    }
    if ((power_state == RIN_GPU_DEVICE_POWER_SUSPEND &&
         new_generation != state->iommu_map_generation) ||
        (power_state == RIN_GPU_DEVICE_POWER_RESUME &&
         new_generation <= state->iommu_map_generation)) {
        ringpu_device_mark_lost(state);
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    if (power_state == RIN_GPU_DEVICE_POWER_SUSPEND) {
        state->flags = RIN_GPU_DEVICE_STATUS_SUSPENDED;
    } else {
        state->flags = RIN_GPU_DEVICE_STATUS_ACTIVE;
        state->device_epoch = new_epoch;
        state->iommu_map_generation = new_generation;
        if (state->diagnostics &&
            rin_gpu_diagnostics_advance_generation(
                state->diagnostics, new_epoch) != 0) {
            ringpu_device_mark_lost(state);
            result = RIN_GPU_DEVICE_PROTOCOL;
            goto done;
        }
        ringpu_device_diagnostic(state, RIN_GPU_DIAGNOSTIC_MAPPING,
                                 old_map_generation, new_generation, 0u);
    }
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_suspend(RinGpuDeviceRuntime* runtime) {
    if (!runtime) return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    return ringpu_device_power(runtime, RIN_GPU_DEVICE_POWER_SUSPEND);
}

int rin_gpu_device_runtime_resume(RinGpuDeviceRuntime* runtime) {
    if (!runtime) return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    return ringpu_device_power(runtime, RIN_GPU_DEVICE_POWER_RESUME);
}

int rin_gpu_device_runtime_attach_diagnostics(
    RinGpuDeviceRuntime* runtime, RinGpuDiagnosticsRuntime* diagnostics) {
    RinGpuDeviceRuntimeState* state;
    RinGpuDiagnosticsStatsV1 stats;
    int result;

    if (!runtime || !diagnostics ||
        ringpu_device_overlap(runtime, sizeof(*runtime), diagnostics,
                              sizeof(*diagnostics)))
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    stats.version = RIN_GPU_DIAGNOSTICS_VERSION;
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->diagnostics && state->diagnostics != diagnostics) {
        result = RIN_GPU_DEVICE_STATE;
        goto done;
    }
    if (rin_gpu_diagnostics_get_stats(diagnostics, &stats) != 0 ||
        stats.device_generation != state->device_epoch) {
        result = RIN_GPU_DEVICE_PROTOCOL;
        goto done;
    }
    state->diagnostics = diagnostics;
    result = RIN_GPU_DEVICE_OK;

done:
    ringpu_device_unlock(state);
    return result;
}

int rin_gpu_device_runtime_get_status(RinGpuDeviceRuntime* runtime,
                                     RinGpuDeviceStatusV1* status) {
    RinGpuDeviceRuntimeState* state;
    uint32_t queue_id;
    int result;

    if (!runtime || !status ||
        ringpu_device_overlap(runtime, sizeof(*runtime), status,
                              sizeof(*status))) {
        return RIN_GPU_DEVICE_INVALID_ARGUMENT;
    }
    memset(status, 0, sizeof(*status));
    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    status->struct_size = sizeof(*status);
    status->version = RIN_GPU_DEVICE_RUNTIME_VERSION;
    status->flags = state->flags;
    status->queue_count = state->backend.queue_count;
    status->device_epoch = state->device_epoch;
    status->iommu_domain_cookie = state->backend.iommu_domain_cookie;
    status->iommu_map_generation = state->iommu_map_generation;
    status->last_monotonic_ns = state->last_monotonic_ns;
    status->in_flight_count = state->in_flight_count;
    status->reset_count = state->reset_count;
    status->max_in_flight = state->backend.max_in_flight;
    status->max_reset_attempts = state->backend.max_reset_attempts;
    for (queue_id = 0u; queue_id < state->backend.queue_count; queue_id++) {
        status->completed_values[queue_id] =
            state->queues[queue_id].completed_value;
    }
    ringpu_device_unlock(state);
    return RIN_GPU_DEVICE_OK;
}

int rin_gpu_device_runtime_destroy(RinGpuDeviceRuntime* runtime) {
    RinGpuDeviceRuntimeState* state;
    int result;

    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    if (state->in_flight_count != 0u) {
        result = state->flags == RIN_GPU_DEVICE_STATUS_LOST
                     ? RIN_GPU_DEVICE_LOST
                     : RIN_GPU_DEVICE_BUSY;
        ringpu_device_unlock(state);
        return result;
    }
    memset(runtime, 0, sizeof(*runtime));
    return RIN_GPU_DEVICE_OK;
}

int rin_gpu_device_runtime_abandon_in_flight(RinGpuDeviceRuntime* runtime) {
    RinGpuDeviceRuntimeState* state;
    int result;

    result = ringpu_device_lock(runtime, &state);
    if (result != RIN_GPU_DEVICE_OK) return result;
    ringpu_device_mark_lost(state);
    memset(state->in_flight, 0, sizeof(state->in_flight));
    state->in_flight_count = 0u;
    ringpu_device_unlock(state);
    return RIN_GPU_DEVICE_OK;
}
