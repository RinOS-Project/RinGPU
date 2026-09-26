/* SPDX-License-Identifier: MIT */
#include "differential.h"

#include <math.h>
#include <string.h>

#include <ringpu/software.h>

static int category_flag(uint32_t category, uint32_t* flag_out) {
    if (!flag_out) return 0;
    switch (category) {
        case RIN_GPU_DIFFERENTIAL_COLOR:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_COLOR;
            return 1;
        case RIN_GPU_DIFFERENTIAL_DEPTH:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_DEPTH;
            return 1;
        case RIN_GPU_DIFFERENTIAL_STENCIL:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_STENCIL;
            return 1;
        case RIN_GPU_DIFFERENTIAL_READBACK:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_READBACK;
            return 1;
        case RIN_GPU_DIFFERENTIAL_FENCE_ORDER:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_FENCE_ORDER;
            return 1;
        case RIN_GPU_DIFFERENTIAL_RESOURCE_STATE:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_RESOURCE_STATE;
            return 1;
        case RIN_GPU_DIFFERENTIAL_DERIVATIVE:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_DERIVATIVE;
            return 1;
        case RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_TEXTURE_FILTER;
            return 1;
        case RIN_GPU_DIFFERENTIAL_SHADER_RESULT:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_SHADER_RESULT;
            return 1;
        case RIN_GPU_DIFFERENTIAL_CAPABILITY:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_CAPABILITY;
            return 1;
        case RIN_GPU_DIFFERENTIAL_FAULT:
            *flag_out = RIN_GPU_DIFFERENTIAL_FLAG_FAULT;
            return 1;
        default:
            return 0;
    }
}

static int finite_nonnegative(float value) {
    return isfinite(value) && value >= 0.0f;
}

static int policy_valid(const RinGpuDifferentialPolicyV1* policy) {
    return policy && policy->struct_size >= sizeof(*policy) &&
           policy->version == RIN_GPU_DIFFERENTIAL_VERSION &&
           policy->reserved0 == 0u && finite_nonnegative(policy->max_float_abs_error) &&
           finite_nonnegative(policy->derivative_max_abs_error) &&
           finite_nonnegative(policy->texture_filter_max_abs_error) &&
           policy->reserved1 == 0.0f && policy->reserved[0] == 0u &&
           policy->reserved[1] == 0u;
}

static int report_valid(const RinGpuDifferentialReportV1* report) {
    return report && report->struct_size >= sizeof(*report) &&
           report->version == RIN_GPU_DIFFERENTIAL_VERSION &&
           report->reserved0 == 0u && report->reserved[0] == 0u &&
           report->reserved[1] == 0u;
}

static void record_result(uint32_t category, int mismatch,
                          RinGpuDifferentialReportV1* report) {
    uint32_t flag = 0u;
    if (!category_flag(category, &flag)) return;
    ++report->compared_values;
    if (mismatch) {
        ++report->mismatched_values;
        report->mismatch_flags |= flag;
    }
}

static uint32_t f32_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t f32_ordered(uint32_t bits) {
    if ((bits & UINT32_C(0x80000000)) != 0u)
        return ~bits + 1u;
    return bits | UINT32_C(0x80000000);
}

static uint64_t f32_ulp_distance(float expected, float actual) {
    uint32_t left = f32_ordered(f32_bits(expected));
    uint32_t right = f32_ordered(f32_bits(actual));
    return left >= right ? (uint64_t)(left - right) : (uint64_t)(right - left);
}

static float policy_abs_error(uint32_t category,
                              const RinGpuDifferentialPolicyV1* policy) {
    if (category == RIN_GPU_DIFFERENTIAL_DERIVATIVE)
        return policy->derivative_max_abs_error;
    if (category == RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER)
        return policy->texture_filter_max_abs_error;
    return policy->max_float_abs_error;
}

int rin_gpu_differential_policy_init(
    RinGpuDifferentialPolicyV1* policy, uint32_t max_float_ulp,
    float max_float_abs_error, float derivative_max_abs_error,
    float texture_filter_max_abs_error) {
    if (!policy || !finite_nonnegative(max_float_abs_error) ||
        !finite_nonnegative(derivative_max_abs_error) ||
        !finite_nonnegative(texture_filter_max_abs_error))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    memset(policy, 0, sizeof(*policy));
    policy->struct_size = sizeof(*policy);
    policy->version = RIN_GPU_DIFFERENTIAL_VERSION;
    policy->max_float_ulp = max_float_ulp;
    policy->max_float_abs_error = max_float_abs_error;
    policy->derivative_max_abs_error = derivative_max_abs_error;
    policy->texture_filter_max_abs_error = texture_filter_max_abs_error;
    return RIN_GPU_DIFFERENTIAL_MATCH;
}

int rin_gpu_differential_report_init(RinGpuDifferentialReportV1* report) {
    if (!report) return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->struct_size = sizeof(*report);
    report->version = RIN_GPU_DIFFERENTIAL_VERSION;
    return RIN_GPU_DIFFERENTIAL_MATCH;
}

int rin_gpu_differential_compare_bytes(
    uint32_t category, const uint8_t* expected, const uint8_t* actual,
    uint64_t size, uint32_t max_abs_delta,
    RinGpuDifferentialReportV1* report) {
    uint64_t index;
    int mismatch = 0;
    if (category < RIN_GPU_DIFFERENTIAL_COLOR ||
        category > RIN_GPU_DIFFERENTIAL_FAULT ||
        !report_valid(report) ||
        (size != 0u && (!expected || !actual)))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    for (index = 0u; index < size; ++index) {
        uint32_t left = expected[index];
        uint32_t right = actual[index];
        uint32_t delta = left >= right ? left - right : right - left;
        if (delta > max_abs_delta) mismatch = 1;
        record_result(category, delta > max_abs_delta, report);
    }
    return mismatch ? RIN_GPU_DIFFERENTIAL_MISMATCH
                    : RIN_GPU_DIFFERENTIAL_MATCH;
}

int rin_gpu_differential_compare_u64(
    uint32_t category, const uint64_t* expected, const uint64_t* actual,
    uint64_t count, RinGpuDifferentialReportV1* report) {
    uint64_t index;
    int mismatch = 0;
    if (category < RIN_GPU_DIFFERENTIAL_COLOR ||
        category > RIN_GPU_DIFFERENTIAL_FAULT ||
        !report_valid(report) ||
        (count != 0u && (!expected || !actual)))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    for (index = 0u; index < count; ++index) {
        int different = expected[index] != actual[index];
        mismatch |= different;
        record_result(category, different, report);
    }
    return mismatch ? RIN_GPU_DIFFERENTIAL_MISMATCH
                    : RIN_GPU_DIFFERENTIAL_MATCH;
}

int rin_gpu_differential_compare_f32(
    uint32_t category, const float* expected, const float* actual,
    uint64_t count, const RinGpuDifferentialPolicyV1* policy,
    RinGpuDifferentialReportV1* report) {
    uint64_t index;
    int mismatch = 0;
    float absolute_tolerance;
    if (category < RIN_GPU_DIFFERENTIAL_COLOR ||
        category > RIN_GPU_DIFFERENTIAL_FAULT ||
        !policy_valid(policy) ||
        !report_valid(report) || (count != 0u && (!expected || !actual)))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    absolute_tolerance = policy_abs_error(category, policy);
    for (index = 0u; index < count; ++index) {
        float left = expected[index];
        float right = actual[index];
        int different;
        if (!isfinite(left) || !isfinite(right)) {
            different = f32_bits(left) != f32_bits(right) || isnan(left) ||
                        isnan(right);
        } else {
            float delta = fabsf(left - right);
            different = delta > absolute_tolerance &&
                        f32_ulp_distance(left, right) > policy->max_float_ulp;
        }
        mismatch |= different;
        record_result(category, different, report);
    }
    return mismatch ? RIN_GPU_DIFFERENTIAL_MISMATCH
                    : RIN_GPU_DIFFERENTIAL_MATCH;
}

static int backend_stats_valid(
    const RinGpuDifferentialBackendStatsV1* stats) {
    if (!stats || stats->struct_size < sizeof(*stats) ||
        stats->version != RIN_GPU_DIFFERENTIAL_BACKEND_STATS_VERSION ||
        stats->reserved0 != 0u || stats->valid_fields == 0u ||
        (stats->valid_fields & ~RIN_GPU_DIFFERENTIAL_BACKEND_FIELDS_KNOWN) !=
            0u || stats->reserved[0] != 0u || stats->reserved[1] != 0u)
        return 0;
    if ((stats->valid_fields &
         RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST) != 0u &&
        stats->output_hash_count == 0u)
        return 0;
    return 1;
}

int rin_gpu_differential_compare_backend_snapshot(
    const RinGpuDifferentialBackendStatsV1* expected,
    const RinGpuDifferentialBackendStatsV1* actual,
    RinGpuDifferentialReportV1* report) {
    uint64_t expected_values[10];
    uint64_t actual_values[10];
    uint64_t count = 0u;
    uint32_t fields;
    if (!backend_stats_valid(expected) || !backend_stats_valid(actual) ||
        !report_valid(report))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    if (expected->valid_fields != actual->valid_fields) {
        record_result(RIN_GPU_DIFFERENTIAL_RESOURCE_STATE, 1, report);
        return RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    fields = expected->valid_fields;
    if ((fields & RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_COMMAND_COUNTERS) != 0u) {
        expected_values[count] = expected->submitted_commands;
        actual_values[count++] = actual->submitted_commands;
        expected_values[count] = expected->copy_commands;
        actual_values[count++] = actual->copy_commands;
        expected_values[count] = expected->draw_commands;
        actual_values[count++] = actual->draw_commands;
        expected_values[count] = expected->dispatch_commands;
        actual_values[count++] = actual->dispatch_commands;
        expected_values[count] = expected->transition_commands;
        actual_values[count++] = actual->transition_commands;
        expected_values[count] = expected->barrier_commands;
        actual_values[count++] = actual->barrier_commands;
        expected_values[count] = expected->present_commands;
        actual_values[count++] = actual->present_commands;
    }
    if ((fields & RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST) != 0u) {
        expected_values[count] = expected->output_hash;
        actual_values[count++] = actual->output_hash;
        expected_values[count] = expected->output_hash_count;
        actual_values[count++] = actual->output_hash_count;
    }
    if ((fields & RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_DETERMINISTIC_SEED) != 0u) {
        expected_values[count] = expected->deterministic_seed;
        actual_values[count++] = actual->deterministic_seed;
    }
    return rin_gpu_differential_compare_u64(
        RIN_GPU_DIFFERENTIAL_RESOURCE_STATE, expected_values, actual_values,
        count, report);
}

int rin_gpu_differential_compare_backend_stats(
    const struct RinGpuSoftwareBackendStatsV1* expected,
    const struct RinGpuSoftwareBackendStatsV1* actual,
    RinGpuDifferentialReportV1* report) {
    RinGpuDifferentialBackendStatsV1 left;
    RinGpuDifferentialBackendStatsV1 right;
    if (!expected || !actual || expected->struct_size < sizeof(*expected) ||
        actual->struct_size < sizeof(*actual) ||
        expected->version < RIN_GPU_SOFTWARE_BACKEND_VERSION ||
        actual->version < RIN_GPU_SOFTWARE_BACKEND_VERSION)
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    left.struct_size = sizeof(left);
    right.struct_size = sizeof(right);
    left.version = RIN_GPU_DIFFERENTIAL_BACKEND_STATS_VERSION;
    right.version = RIN_GPU_DIFFERENTIAL_BACKEND_STATS_VERSION;
    left.valid_fields =
        RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_COMMAND_COUNTERS |
        RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_DETERMINISTIC_SEED;
    right.valid_fields = left.valid_fields;
    if (expected->output_hash_count != 0u)
        left.valid_fields |= RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST;
    if (actual->output_hash_count != 0u)
        right.valid_fields |= RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST;
    left.submitted_commands = expected->submitted_commands;
    left.copy_commands = expected->copy_commands;
    left.draw_commands = expected->draw_commands;
    left.dispatch_commands = expected->dispatch_commands;
    left.transition_commands = expected->transition_commands;
    left.barrier_commands = expected->barrier_commands;
    left.present_commands = expected->present_commands;
    left.output_hash = expected->output_hash;
    left.output_hash_count = expected->output_hash_count;
    left.deterministic_seed = expected->deterministic_seed;
    right.submitted_commands = actual->submitted_commands;
    right.copy_commands = actual->copy_commands;
    right.draw_commands = actual->draw_commands;
    right.dispatch_commands = actual->dispatch_commands;
    right.transition_commands = actual->transition_commands;
    right.barrier_commands = actual->barrier_commands;
    right.present_commands = actual->present_commands;
    right.output_hash = actual->output_hash;
    right.output_hash_count = actual->output_hash_count;
    right.deterministic_seed = actual->deterministic_seed;
    return rin_gpu_differential_compare_backend_snapshot(&left, &right,
                                                         report);
}

static int workload_valid(const RinGpuDifferentialWorkloadV1* workload)
{
    size_t id_length;
    if (!workload || workload->struct_size < sizeof(*workload) ||
        workload->version != RIN_GPU_DIFFERENTIAL_WORKLOAD_VERSION ||
        !workload->command_stream || workload->command_stream_bytes == 0u ||
        workload->command_stream_bytes > RIN_GPU_DIFFERENTIAL_COMMAND_STREAM_MAX ||
        workload->expected_fields == 0u ||
        (workload->expected_fields &
         ~RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELDS_KNOWN) != 0u ||
        workload->reserved0 != 0u || workload->reserved[0] != 0u ||
        workload->reserved[1] != 0u)
        return 0;
    id_length = 0u;
    while (id_length < sizeof(workload->id) && workload->id[id_length] != '\0')
        ++id_length;
    return id_length != 0u && id_length < sizeof(workload->id);
}

static int snapshot_field_valid(const RinGpuDifferentialSnapshotV1* snapshot,
                                uint32_t field, const void* data,
                                uint64_t count)
{
    return (snapshot->valid_fields & field) == 0u ||
           (count != 0u && data != NULL);
}

static int snapshot_valid(const RinGpuDifferentialWorkloadV1* workload,
                          const RinGpuDifferentialSnapshotV1* snapshot)
{
    if (!snapshot || snapshot->struct_size < sizeof(*snapshot) ||
        snapshot->version != RIN_GPU_DIFFERENTIAL_WORKLOAD_VERSION ||
        snapshot->reserved0 != 0u ||
        (snapshot->valid_fields & ~RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELDS_KNOWN) !=
            0u || snapshot->valid_fields != workload->expected_fields ||
        snapshot->backend.struct_size < sizeof(snapshot->backend) ||
        !backend_stats_valid(&snapshot->backend) ||
        (snapshot->backend.valid_fields &
         RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_DETERMINISTIC_SEED) == 0u ||
        snapshot->backend.deterministic_seed != workload->deterministic_seed ||
        snapshot->device_lost > 1u || snapshot->reserved[0] != 0u ||
        snapshot->reserved[1] != 0u)
        return 0;
    if (!snapshot_field_valid(snapshot,
                              RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_COLOR,
                              snapshot->color, snapshot->color_bytes) ||
        !snapshot_field_valid(snapshot,
                              RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_DEPTH,
                              snapshot->depth, snapshot->depth_values) ||
        !snapshot_field_valid(snapshot,
                              RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_STENCIL,
                              snapshot->stencil, snapshot->stencil_bytes) ||
        !snapshot_field_valid(snapshot,
                              RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_SHADER,
                              snapshot->shader_result,
                              snapshot->shader_result_bytes) ||
        !snapshot_field_valid(snapshot,
                              RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_FENCE_ORDER,
                              snapshot->fence_order, snapshot->fence_values) ||
        !snapshot_field_valid(snapshot,
                              RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_RESOURCE_STATE,
                              snapshot->resource_state,
                              snapshot->resource_values))
        return 0;
    return 1;
}

static int adapter_valid(const RinGpuDifferentialBackendAdapterV1* adapter)
{
    return adapter && adapter->struct_size >= sizeof(*adapter) &&
           adapter->version == RIN_GPU_DIFFERENTIAL_WORKLOAD_VERSION &&
           adapter->run != NULL && adapter->reserved[0] == 0u &&
           adapter->reserved[1] == 0u;
}

int rin_gpu_differential_run_pair(
    const RinGpuDifferentialWorkloadV1* workload,
    const RinGpuDifferentialBackendAdapterV1* expected,
    const RinGpuDifferentialBackendAdapterV1* actual,
    const RinGpuDifferentialPolicyV1* policy,
    RinGpuDifferentialReportV1* report)
{
    RinGpuDifferentialSnapshotV1 expected_snapshot;
    RinGpuDifferentialSnapshotV1 actual_snapshot;
    uint64_t expected_fault[3];
    uint64_t actual_fault[3];
    uint64_t expected_capabilities[2];
    uint64_t actual_capabilities[2];
    int result;
    int mismatch = 0;

    if (!workload_valid(workload) || !adapter_valid(expected) ||
        !adapter_valid(actual) || !policy_valid(policy) ||
        !report_valid(report))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    memset(&expected_snapshot, 0, sizeof(expected_snapshot));
    memset(&actual_snapshot, 0, sizeof(actual_snapshot));
    result = expected->run(expected->context, workload, &expected_snapshot);
    if (result != 0) return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    result = actual->run(actual->context, workload, &actual_snapshot);
    if (result != 0) return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if (!snapshot_valid(workload, &expected_snapshot) ||
        !snapshot_valid(workload, &actual_snapshot))
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if (expected_snapshot.valid_fields != actual_snapshot.valid_fields)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;

    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_CAPABILITY) != 0u &&
        ((expected_snapshot.capability_mask & workload->required_capabilities) !=
             workload->required_capabilities ||
         (expected_snapshot.unsupported_capability_mask &
          workload->required_capabilities) != 0u))
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_CAPABILITY) != 0u &&
        ((actual_snapshot.capability_mask & workload->required_capabilities) !=
             workload->required_capabilities ||
         (actual_snapshot.unsupported_capability_mask &
          workload->required_capabilities) != 0u)) {
        record_result(RIN_GPU_DIFFERENTIAL_CAPABILITY, 1, report);
        mismatch = 1;
    }

    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_COLOR) != 0u &&
        expected_snapshot.color_bytes != actual_snapshot.color_bytes)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_DEPTH) != 0u &&
        expected_snapshot.depth_values != actual_snapshot.depth_values)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_STENCIL) != 0u &&
        expected_snapshot.stencil_bytes != actual_snapshot.stencil_bytes)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_SHADER) != 0u &&
        expected_snapshot.shader_result_bytes != actual_snapshot.shader_result_bytes)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_FENCE_ORDER) != 0u &&
        expected_snapshot.fence_values != actual_snapshot.fence_values)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_RESOURCE_STATE) != 0u &&
        expected_snapshot.resource_values != actual_snapshot.resource_values)
        return RIN_GPU_DIFFERENTIAL_BACKEND_ERROR;

    result = rin_gpu_differential_compare_backend_snapshot(
        &expected_snapshot.backend, &actual_snapshot.backend, report);
    if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT)
        return result;
    mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;

    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_COLOR) != 0u) {
        result = rin_gpu_differential_compare_bytes(
            RIN_GPU_DIFFERENTIAL_COLOR, expected_snapshot.color,
            actual_snapshot.color, expected_snapshot.color_bytes, 0u, report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_DEPTH) != 0u) {
        result = rin_gpu_differential_compare_f32(
            RIN_GPU_DIFFERENTIAL_DEPTH, expected_snapshot.depth,
            actual_snapshot.depth, expected_snapshot.depth_values, policy,
            report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_STENCIL) != 0u) {
        result = rin_gpu_differential_compare_bytes(
            RIN_GPU_DIFFERENTIAL_STENCIL, expected_snapshot.stencil,
            actual_snapshot.stencil, expected_snapshot.stencil_bytes, 0u,
            report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_SHADER) != 0u) {
        result = rin_gpu_differential_compare_bytes(
            RIN_GPU_DIFFERENTIAL_SHADER_RESULT, expected_snapshot.shader_result,
            actual_snapshot.shader_result, expected_snapshot.shader_result_bytes,
            0u, report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_FENCE_ORDER) != 0u) {
        result = rin_gpu_differential_compare_u64(
            RIN_GPU_DIFFERENTIAL_FENCE_ORDER, expected_snapshot.fence_order,
            actual_snapshot.fence_order, expected_snapshot.fence_values, report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_RESOURCE_STATE) != 0u) {
        result = rin_gpu_differential_compare_u64(
            RIN_GPU_DIFFERENTIAL_RESOURCE_STATE,
            expected_snapshot.resource_state, actual_snapshot.resource_state,
            expected_snapshot.resource_values, report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_CAPABILITY) != 0u) {
        expected_capabilities[0] = expected_snapshot.capability_mask;
        expected_capabilities[1] = expected_snapshot.unsupported_capability_mask;
        actual_capabilities[0] = actual_snapshot.capability_mask;
        actual_capabilities[1] = actual_snapshot.unsupported_capability_mask;
        result = rin_gpu_differential_compare_u64(
            RIN_GPU_DIFFERENTIAL_CAPABILITY, expected_capabilities,
            actual_capabilities, 2u, report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    if ((workload->expected_fields &
         RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_FAULT) != 0u) {
        expected_fault[0] = expected_snapshot.device_lost;
        expected_fault[1] = expected_snapshot.fault_code;
        expected_fault[2] = expected_snapshot.fault_sequence;
        actual_fault[0] = actual_snapshot.device_lost;
        actual_fault[1] = actual_snapshot.fault_code;
        actual_fault[2] = actual_snapshot.fault_sequence;
        result = rin_gpu_differential_compare_u64(
            RIN_GPU_DIFFERENTIAL_FAULT, expected_fault, actual_fault, 3u,
            report);
        if (result == RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT) return result;
        mismatch |= result == RIN_GPU_DIFFERENTIAL_MISMATCH;
    }
    return mismatch ? RIN_GPU_DIFFERENTIAL_MISMATCH
                    : RIN_GPU_DIFFERENTIAL_MATCH;
}
