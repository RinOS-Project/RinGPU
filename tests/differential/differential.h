/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_DIFFERENTIAL_H
#define RINGPU_DIFFERENTIAL_H

#include <stdint.h>

#define RIN_GPU_DIFFERENTIAL_VERSION 1u

typedef enum RinGpuDifferentialResult {
    RIN_GPU_DIFFERENTIAL_MATCH = 0,
    RIN_GPU_DIFFERENTIAL_MISMATCH = 1,
    RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT = -1
} RinGpuDifferentialResult;

typedef enum RinGpuDifferentialCategory {
    RIN_GPU_DIFFERENTIAL_COLOR = 1,
    RIN_GPU_DIFFERENTIAL_DEPTH = 2,
    RIN_GPU_DIFFERENTIAL_STENCIL = 3,
    RIN_GPU_DIFFERENTIAL_READBACK = 4,
    RIN_GPU_DIFFERENTIAL_FENCE_ORDER = 5,
    RIN_GPU_DIFFERENTIAL_RESOURCE_STATE = 6,
    RIN_GPU_DIFFERENTIAL_DERIVATIVE = 7,
    RIN_GPU_DIFFERENTIAL_TEXTURE_FILTER = 8,
    RIN_GPU_DIFFERENTIAL_CATEGORY_COUNT = 8
} RinGpuDifferentialCategory;

#define RIN_GPU_DIFFERENTIAL_FLAG_COLOR UINT32_C(1u << 0)
#define RIN_GPU_DIFFERENTIAL_FLAG_DEPTH UINT32_C(1u << 1)
#define RIN_GPU_DIFFERENTIAL_FLAG_STENCIL UINT32_C(1u << 2)
#define RIN_GPU_DIFFERENTIAL_FLAG_READBACK UINT32_C(1u << 3)
#define RIN_GPU_DIFFERENTIAL_FLAG_FENCE_ORDER UINT32_C(1u << 4)
#define RIN_GPU_DIFFERENTIAL_FLAG_RESOURCE_STATE UINT32_C(1u << 5)
#define RIN_GPU_DIFFERENTIAL_FLAG_DERIVATIVE UINT32_C(1u << 6)
#define RIN_GPU_DIFFERENTIAL_FLAG_TEXTURE_FILTER UINT32_C(1u << 7)

typedef struct RinGpuDifferentialPolicyV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t max_float_ulp;
    uint32_t reserved0;
    float max_float_abs_error;
    float derivative_max_abs_error;
    float texture_filter_max_abs_error;
    float reserved1;
    uint64_t reserved[2];
} RinGpuDifferentialPolicyV1;

typedef struct RinGpuDifferentialReportV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t mismatch_flags;
    uint32_t reserved0;
    uint64_t compared_values;
    uint64_t mismatched_values;
    uint64_t reserved[2];
} RinGpuDifferentialReportV1;

#include <ringpu/software.h>

/* Initializes a policy with exact integer/normalized comparisons and an
 * explicit Float32 tolerance.  A zero Float32 tolerance means exact bits. */
int rin_gpu_differential_policy_init(
    RinGpuDifferentialPolicyV1* policy, uint32_t max_float_ulp,
    float max_float_abs_error, float derivative_max_abs_error,
    float texture_filter_max_abs_error);
int rin_gpu_differential_report_init(RinGpuDifferentialReportV1* report);

/* Byte comparisons are exact when max_abs_delta is zero.  A non-zero delta is
 * intended only for a caller-selected normalized format and is applied per
 * byte; it is never used for floating-point data. */
int rin_gpu_differential_compare_bytes(
    uint32_t category, const uint8_t* expected, const uint8_t* actual,
    uint64_t size, uint32_t max_abs_delta,
    RinGpuDifferentialReportV1* report);
int rin_gpu_differential_compare_u64(
    uint32_t category, const uint64_t* expected, const uint64_t* actual,
    uint64_t count, RinGpuDifferentialReportV1* report);
int rin_gpu_differential_compare_f32(
    uint32_t category, const float* expected, const float* actual,
    uint64_t count, const RinGpuDifferentialPolicyV1* policy,
    RinGpuDifferentialReportV1* report);

/* Compares command counters and the deterministic presentation digest from
 * two real backend runs.  No missing or unsupported backend result can be
 * converted into a match. */
int rin_gpu_differential_compare_backend_stats(
    const struct RinGpuSoftwareBackendStatsV1* expected,
    const struct RinGpuSoftwareBackendStatsV1* actual,
    RinGpuDifferentialReportV1* report);

#if defined(__cplusplus)
static_assert(sizeof(RinGpuDifferentialPolicyV1) == 48u,
              "RinGPU differential policy drift");
static_assert(sizeof(RinGpuDifferentialReportV1) == 48u,
              "RinGPU differential report drift");
#else
_Static_assert(sizeof(RinGpuDifferentialPolicyV1) == 48u,
               "RinGPU differential policy drift");
_Static_assert(sizeof(RinGpuDifferentialReportV1) == 48u,
               "RinGPU differential report drift");
#endif

#endif /* RINGPU_DIFFERENTIAL_H */
