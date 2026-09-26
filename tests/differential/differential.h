/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_DIFFERENTIAL_H
#define RINGPU_DIFFERENTIAL_H

#include <stdint.h>

#define RIN_GPU_DIFFERENTIAL_VERSION 1u

typedef enum RinGpuDifferentialResult {
    RIN_GPU_DIFFERENTIAL_MATCH = 0,
    RIN_GPU_DIFFERENTIAL_MISMATCH = 1,
    RIN_GPU_DIFFERENTIAL_INVALID_ARGUMENT = -1,
    RIN_GPU_DIFFERENTIAL_BACKEND_ERROR = -2
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
    RIN_GPU_DIFFERENTIAL_SHADER_RESULT = 9,
    RIN_GPU_DIFFERENTIAL_CAPABILITY = 10,
    RIN_GPU_DIFFERENTIAL_FAULT = 11,
    RIN_GPU_DIFFERENTIAL_CATEGORY_COUNT = 11
} RinGpuDifferentialCategory;

#define RIN_GPU_DIFFERENTIAL_FLAG_COLOR UINT32_C(1u << 0)
#define RIN_GPU_DIFFERENTIAL_FLAG_DEPTH UINT32_C(1u << 1)
#define RIN_GPU_DIFFERENTIAL_FLAG_STENCIL UINT32_C(1u << 2)
#define RIN_GPU_DIFFERENTIAL_FLAG_READBACK UINT32_C(1u << 3)
#define RIN_GPU_DIFFERENTIAL_FLAG_FENCE_ORDER UINT32_C(1u << 4)
#define RIN_GPU_DIFFERENTIAL_FLAG_RESOURCE_STATE UINT32_C(1u << 5)
#define RIN_GPU_DIFFERENTIAL_FLAG_DERIVATIVE UINT32_C(1u << 6)
#define RIN_GPU_DIFFERENTIAL_FLAG_TEXTURE_FILTER UINT32_C(1u << 7)
#define RIN_GPU_DIFFERENTIAL_FLAG_SHADER_RESULT UINT32_C(1u << 8)
#define RIN_GPU_DIFFERENTIAL_FLAG_CAPABILITY UINT32_C(1u << 9)
#define RIN_GPU_DIFFERENTIAL_FLAG_FAULT UINT32_C(1u << 10)

#define RIN_GPU_DIFFERENTIAL_BACKEND_STATS_VERSION 1u
#define RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_COMMAND_COUNTERS UINT32_C(1u << 0)
#define RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST UINT32_C(1u << 1)
#define RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_DETERMINISTIC_SEED UINT32_C(1u << 2)
#define RIN_GPU_DIFFERENTIAL_BACKEND_FIELDS_KNOWN \
    (RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_COMMAND_COUNTERS | \
     RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_PRESENT_DIGEST | \
     RIN_GPU_DIFFERENTIAL_BACKEND_FIELD_DETERMINISTIC_SEED)

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

/* Backend-neutral observation record. A software or hardware adapter fills
 * only fields it actually measured and advertises them through valid_fields.
 * The comparator rejects zero/unknown masks and mismatched masks, so an
 * unsupported hardware observation cannot become a false match. */
typedef struct RinGpuDifferentialBackendStatsV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t valid_fields;
    uint32_t reserved0;
    uint64_t submitted_commands;
    uint64_t copy_commands;
    uint64_t draw_commands;
    uint64_t dispatch_commands;
    uint64_t transition_commands;
    uint64_t barrier_commands;
    uint64_t present_commands;
    uint64_t output_hash;
    uint64_t output_hash_count;
    uint64_t deterministic_seed;
    uint64_t reserved[2];
} RinGpuDifferentialBackendStatsV1;

#define RIN_GPU_DIFFERENTIAL_WORKLOAD_VERSION 1u
#define RIN_GPU_DIFFERENTIAL_WORKLOAD_ID_MAX 64u
#define RIN_GPU_DIFFERENTIAL_COMMAND_STREAM_MAX 8192u

#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_COLOR UINT32_C(1u << 0)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_DEPTH UINT32_C(1u << 1)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_STENCIL UINT32_C(1u << 2)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_SHADER UINT32_C(1u << 3)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_FENCE_ORDER UINT32_C(1u << 4)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_RESOURCE_STATE UINT32_C(1u << 5)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_CAPABILITY UINT32_C(1u << 6)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELD_FAULT UINT32_C(1u << 7)
#define RIN_GPU_DIFFERENTIAL_SNAPSHOT_FIELDS_KNOWN UINT32_C(0xff)

typedef struct RinGpuDifferentialWorkloadV1 {
    uint32_t struct_size;
    uint32_t version;
    char id[RIN_GPU_DIFFERENTIAL_WORKLOAD_ID_MAX];
    const uint8_t* command_stream;
    uint64_t command_stream_bytes;
    uint32_t expected_fields;
    uint32_t reserved0;
    uint64_t required_capabilities;
    uint64_t deterministic_seed;
    uint64_t reserved[2];
} RinGpuDifferentialWorkloadV1;

typedef struct RinGpuDifferentialSnapshotV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t valid_fields;
    uint32_t reserved0;
    RinGpuDifferentialBackendStatsV1 backend;
    const uint8_t* color;
    uint64_t color_bytes;
    const float* depth;
    uint64_t depth_values;
    const uint8_t* stencil;
    uint64_t stencil_bytes;
    const uint8_t* shader_result;
    uint64_t shader_result_bytes;
    const uint64_t* fence_order;
    uint64_t fence_values;
    const uint64_t* resource_state;
    uint64_t resource_values;
    uint64_t capability_mask;
    uint64_t unsupported_capability_mask;
    uint32_t device_lost;
    uint32_t fault_code;
    uint64_t fault_sequence;
    uint64_t reserved[2];
} RinGpuDifferentialSnapshotV1;

typedef int (*RinGpuDifferentialRunBackendFn)(
    void* context, const RinGpuDifferentialWorkloadV1* workload,
    RinGpuDifferentialSnapshotV1* snapshot_out);

typedef struct RinGpuDifferentialBackendAdapterV1 {
    uint32_t struct_size;
    uint32_t version;
    RinGpuDifferentialRunBackendFn run;
    void* context;
    uint64_t reserved[2];
} RinGpuDifferentialBackendAdapterV1;

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

/* Compares observations from two real backend runs. Both backends must
 * advertise the same non-empty set of measured fields; a missing digest or
 * counter set is an explicit mismatch rather than an implicit match. */
int rin_gpu_differential_compare_backend_snapshot(
    const RinGpuDifferentialBackendStatsV1* expected,
    const RinGpuDifferentialBackendStatsV1* actual,
    RinGpuDifferentialReportV1* report);

/* Compatibility adapter for the software backend's existing stats record. */
int rin_gpu_differential_compare_backend_stats(
    const struct RinGpuSoftwareBackendStatsV1* expected,
    const struct RinGpuSoftwareBackendStatsV1* actual,
    RinGpuDifferentialReportV1* report);

/* Execute one immutable, validated workload through two real backend
 * adapters and compare every observation the workload requested.  The
 * adapters own their returned storage until this function returns; a missing
 * physical provider is an explicit backend error and can never become a
 * software/physical match. */
int rin_gpu_differential_run_pair(
    const RinGpuDifferentialWorkloadV1* workload,
    const RinGpuDifferentialBackendAdapterV1* expected,
    const RinGpuDifferentialBackendAdapterV1* actual,
    const RinGpuDifferentialPolicyV1* policy,
    RinGpuDifferentialReportV1* report);

#if defined(__cplusplus)
static_assert(sizeof(RinGpuDifferentialPolicyV1) == 48u,
              "RinGPU differential policy drift");
static_assert(sizeof(RinGpuDifferentialReportV1) == 48u,
              "RinGPU differential report drift");
static_assert(sizeof(RinGpuDifferentialBackendStatsV1) == 112u,
              "RinGPU differential backend stats drift");
#else
_Static_assert(sizeof(RinGpuDifferentialPolicyV1) == 48u,
               "RinGPU differential policy drift");
_Static_assert(sizeof(RinGpuDifferentialReportV1) == 48u,
               "RinGPU differential report drift");
_Static_assert(sizeof(RinGpuDifferentialBackendStatsV1) == 112u,
               "RinGPU differential backend stats drift");
#endif

#endif /* RINGPU_DIFFERENTIAL_H */
