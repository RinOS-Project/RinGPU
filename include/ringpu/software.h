/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_SOFTWARE_H
#define RINGPU_PUBLIC_SOFTWARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RinGpuSoftwareBackend RinGpuSoftwareBackend;
typedef struct RinGpuBackendOpsV1 RinGpuBackendOpsV1;

#define RIN_GPU_SOFTWARE_BACKEND_VERSION 1u
#define RIN_GPU_SOFTWARE_BACKEND_FLAG_DETERMINISTIC UINT32_C(0x00000001)
#define RIN_GPU_SOFTWARE_BACKEND_FLAG_COLLECT_STATS UINT32_C(0x00000002)
#define RIN_GPU_SOFTWARE_BACKEND_FLAGS_KNOWN \
    (RIN_GPU_SOFTWARE_BACKEND_FLAG_DETERMINISTIC | \
     RIN_GPU_SOFTWARE_BACKEND_FLAG_COLLECT_STATS)

typedef struct RinGpuSoftwareBackendDescV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t max_total_bytes;
    uint32_t flags;
    uint32_t reserved0;
} RinGpuSoftwareBackendDescV1;

typedef struct RinGpuSoftwareBackendStatsV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
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
} RinGpuSoftwareBackendStatsV1;

int ringpu_software_backend_create(const RinGpuSoftwareBackendDescV1* desc,
                                   RinGpuSoftwareBackend** backend_out);
void ringpu_software_backend_destroy(RinGpuSoftwareBackend* backend);
int ringpu_software_backend_query_stats(
    const RinGpuSoftwareBackend* backend,
    RinGpuSoftwareBackendStatsV1* stats_out);
const RinGpuBackendOpsV1* ringpu_software_backend_ops(void);

#ifdef __cplusplus
}
#endif

#endif
