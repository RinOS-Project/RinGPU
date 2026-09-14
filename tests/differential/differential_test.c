/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "differential.h"

static void test_integer_and_normalized_bytes(void) {
    const uint8_t expected[] = {0u, 127u, 255u, 64u};
    const uint8_t actual[] = {0u, 128u, 254u, 64u};
    const uint8_t stencil[] = {0u, 127u, 255u};
    RinGpuDifferentialReportV1 report;

    assert(rin_gpu_differential_report_init(&report) == 0);
    assert(rin_gpu_differential_compare_bytes(
               RIN_GPU_DIFFERENTIAL_COLOR, expected, actual,
               sizeof(expected), 0u, &report) ==
           RIN_GPU_DIFFERENTIAL_MISMATCH);
    assert(report.mismatched_values == 2u);
    assert((report.mismatch_flags & RIN_GPU_DIFFERENTIAL_FLAG_COLOR) != 0u);

    assert(rin_gpu_differential_report_init(&report) == 0);
    assert(rin_gpu_differential_compare_bytes(
               RIN_GPU_DIFFERENTIAL_READBACK, expected, actual,
               sizeof(expected), 1u, &report) ==
           RIN_GPU_DIFFERENTIAL_MATCH);
    assert(report.mismatched_values == 0u);

    assert(rin_gpu_differential_compare_bytes(
               RIN_GPU_DIFFERENTIAL_STENCIL, stencil, stencil,
               sizeof(stencil), 0u, &report) == RIN_GPU_DIFFERENTIAL_MATCH);
}

static void test_float_tolerance_by_category(void) {
    RinGpuDifferentialPolicyV1 policy;
    RinGpuDifferentialReportV1 report;
    const float expected[] = {1.0f, 2.0f};
    const float close[] = {1.0f, 2.0001f};
    const float far[] = {1.0f, 2.1f};
    const float derivative_expected[] = {0.25f};
    const float derivative_actual[] = {0.2505f};
    const float texture_expected[] = {0.5f};
    const float texture_actual[] = {0.5005f};

    assert(rin_gpu_differential_policy_init(&policy, 1u, 0.0002f, 0.0f,
                                            0.0f) == 0);
    assert(rin_gpu_differential_report_init(&report) == 0);
    assert(rin_gpu_differential_compare_f32(
               RIN_GPU_DIFFERENTIAL_DEPTH, expected, close, 2u, &policy,
               &report) == RIN_GPU_DIFFERENTIAL_MATCH);
    assert(rin_gpu_differential_compare_f32(
               RIN_GPU_DIFFERENTIAL_DEPTH, expected, far, 2u, &policy,
               &report) == RIN_GPU_DIFFERENTIAL_MISMATCH);
    assert((report.mismatch_flags & RIN_GPU_DIFFERENTIAL_FLAG_DEPTH) != 0u);

    assert(rin_gpu_differential_compare_f32(
               RIN_GPU_DIFFERENTIAL_DERIVATIVE, derivative_expected,
               derivative_actual, 1u, &policy, &report) ==
           RIN_GPU_DIFFERENTIAL_MISMATCH);
    assert(rin_gpu_differential_policy_init(&policy, 0u, 0.0f, 0.001f,
                                            0.001f) == 0);
    assert(rin_gpu_differential_compare_f32(
               RIN_GPU_DIFFERENTIAL_DERIVATIVE, derivative_expected,
               derivative_actual, 1u, &policy, &report) ==
           RIN_GPU_DIFFERENTIAL_MATCH);
    assert(rin_gpu_differential_compare_f32(
               RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER, texture_expected,
               texture_actual, 1u, &policy, &report) ==
           RIN_GPU_DIFFERENTIAL_MATCH);
}

static void test_reference_stats(void) {
    RinGpuDifferentialBackendStatsV1 expected;
    RinGpuDifferentialBackendStatsV1 actual;
    RinGpuDifferentialReportV1 report;

    memset(&expected, 0, sizeof(expected));
    expected.struct_size = sizeof(expected);
    expected.version = RIN_GPU_DIFFERENTIAL_BACKEND_STATS_VERSION;
    expected.valid_fields = RIN_GPU_DIFFERENTIAL_BACKEND_FIELDS_KNOWN;
    expected.submitted_commands = 3u;
    expected.present_commands = 1u;
    expected.output_hash = UINT64_C(0x1234);
    expected.output_hash_count = 1u;
    expected.deterministic_seed = UINT64_C(0x55);
    actual = expected;
    assert(rin_gpu_differential_report_init(&report) == 0);
    assert(rin_gpu_differential_compare_backend_snapshot(&expected, &actual,
                                                         &report) == 0);
    actual.output_hash++;
    assert(rin_gpu_differential_compare_backend_snapshot(&expected, &actual,
                                                         &report) ==
           RIN_GPU_DIFFERENTIAL_MISMATCH);
    assert((report.mismatch_flags & RIN_GPU_DIFFERENTIAL_FLAG_RESOURCE_STATE) !=
           0u);

    actual = expected;
    actual.valid_fields &=
        ~RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST;
    assert(rin_gpu_differential_compare_backend_snapshot(&expected, &actual,
                                                         &report) ==
           RIN_GPU_DIFFERENTIAL_MISMATCH);
}

static void test_fence_and_state_order(void) {
    const uint64_t expected[] = {11u, 12u, 13u};
    const uint64_t actual[] = {11u, 13u, 12u};
    RinGpuDifferentialReportV1 report;

    assert(rin_gpu_differential_report_init(&report) == 0);
    assert(rin_gpu_differential_compare_u64(
               RIN_GPU_DIFFERENTIAL_FENCE_ORDER, expected, actual, 3u,
               &report) == RIN_GPU_DIFFERENTIAL_MISMATCH);
    assert(report.mismatched_values == 2u);
    assert(rin_gpu_differential_compare_u64(
               RIN_GPU_DIFFERENTIAL_RESOURCE_STATE, expected, expected, 3u,
               &report) == RIN_GPU_DIFFERENTIAL_MATCH);
}

static void test_invalid_nan_does_not_match(void) {
    RinGpuDifferentialPolicyV1 policy;
    RinGpuDifferentialReportV1 report;
    const float expected[] = {0.0f};
    float actual[1];
    const uint32_t nan_bits = UINT32_C(0x7fc00001);

    memcpy(actual, &nan_bits, sizeof(actual[0]));

    assert(rin_gpu_differential_policy_init(&policy, 0u, 0.0f, 0.0f,
                                            0.0f) == 0);
    assert(rin_gpu_differential_report_init(&report) == 0);
    assert(rin_gpu_differential_compare_f32(
               RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER, expected, actual, 1u,
               &policy, &report) == RIN_GPU_DIFFERENTIAL_MISMATCH);
}

int main(void) {
    test_integer_and_normalized_bytes();
    test_float_tolerance_by_category();
    test_fence_and_state_order();
    test_reference_stats();
    test_invalid_nan_does_not_match();
    return 0;
}
