// SPDX-License-Identifier: MIT
#ifndef RIN_SUBSYSTEMS_RINGPU_SOFTWARE_BACKEND_H
#define RIN_SUBSYSTEMS_RINGPU_SOFTWARE_BACKEND_H

/* The implementation uses the installed public software ABI. Private core
 * declarations remain included by software_backend.c where needed; this
 * header is intentionally not an alternate public contract. */
#include <ringpu/software.h>

#define RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION 1u

typedef struct RinGpuSoftwareMemoryPoolStatsV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t enabled;
    uint32_t active_suballocations;
    uint64_t backing_allocation_count;
    uint64_t reserved_bytes;
    uint64_t suballocated_bytes;
} RinGpuSoftwareMemoryPoolStatsV1;

int ringpu_software_backend_query_memory_pool_stats(
    const RinGpuSoftwareBackend* backend,
    RinGpuSoftwareMemoryPoolStatsV1* stats_out);

/* Internal implementation details used by the software backend's optional
 * memory ownership/binding operations. Other backends provide their own
 * RinGpuBackendOpsV1 callbacks. */
int ringpu_software_backend_create_memory(void* opaque, uint64_t size_bytes,
                                          uint8_t** bytes_out);
void ringpu_software_backend_destroy_memory(void* opaque, uint8_t* bytes,
                                            uint64_t size_bytes);
int ringpu_software_backend_bind_buffer(
    void* opaque, const RinGpuBufferDescV1* desc, uint8_t* memory,
    uint64_t memory_size, uint64_t offset_bytes, uint64_t* cookie_out);
int ringpu_software_backend_bind_image(
    void* opaque, const RinGpuImageDescV1* desc, uint64_t allocation_bytes,
    uint8_t* memory, uint64_t memory_size, uint64_t offset_bytes,
    uint64_t* cookie_out);

#endif /* RIN_SUBSYSTEMS_RINGPU_SOFTWARE_BACKEND_H */
