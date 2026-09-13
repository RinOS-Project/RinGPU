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
        category > RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER ||
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
        category > RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER ||
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
        category > RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER ||
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

int rin_gpu_differential_compare_backend_stats(
    const struct RinGpuSoftwareBackendStatsV1* expected,
    const struct RinGpuSoftwareBackendStatsV1* actual,
    RinGpuDifferentialReportV1* report) {
    const RinGpuSoftwareBackendStatsV1* left =
        (const RinGpuSoftwareBackendStatsV1*)expected;
    const RinGpuSoftwareBackendStatsV1* right =
        (const RinGpuSoftwareBackendStatsV1*)actual;
    const uint64_t expected_values[] = {
        left ? left->submitted_commands : 0u,
        left ? left->copy_commands : 0u,
        left ? left->draw_commands : 0u,
        left ? left->dispatch_commands : 0u,
        left ? left->transition_commands : 0u,
        left ? left->barrier_commands : 0u,
        left ? left->present_commands : 0u,
        left ? left->output_hash : 0u,
        left ? left->output_hash_count : 0u,
        left ? left->deterministic_seed : 0u};
    const uint64_t actual_values[] = {
        right ? right->submitted_commands : 0u,
        right ? right->copy_commands : 0u,
        right ? right->draw_commands : 0u,
        right ? right->dispatch_commands : 0u,
        right ? right->transition_commands : 0u,
        right ? right->barrier_commands : 0u,
        right ? right->present_commands : 0u,
        right ? right->output_hash : 0u,
        right ? right->output_hash_count : 0u,
        right ? right->deterministic_seed : 0u};
    if (!left || !right || left->struct_size < sizeof(*left) ||
        right->struct_size < sizeof(*right) ||
        left->version != RIN_GPU_SOFTWARE_BACKEND_VERSION ||
        right->version != RIN_GPU_SOFTWARE_BACKEND_VERSION ||
        !report_valid(report))
        return RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT;
    return rin_gpu_differential_compare_u64(
        RIN_GPU_DIFFERENTIAL_RESOURCE_STATE, expected_values, actual_values,
        sizeof(expected_values) / sizeof(expected_values[0]), report);
}
