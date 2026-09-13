/* SPDX-License-Identifier: MIT */
#ifndef RIN_GPU_CROSS_PROCESS_CAPABILITY_H
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION 1u
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX 64u

#define RIN_GPU_CROSS_PROCESS_RIGHT_READ  0x00000001u
#define RIN_GPU_CROSS_PROCESS_RIGHT_WRITE 0x00000002u
#define RIN_GPU_CROSS_PROCESS_RIGHT_PRESENT 0x00000004u
#define RIN_GPU_CROSS_PROCESS_RIGHT_MASK \
    (RIN_GPU_CROSS_PROCESS_RIGHT_READ | \
     RIN_GPU_CROSS_PROCESS_RIGHT_WRITE | \
     RIN_GPU_CROSS_PROCESS_RIGHT_PRESENT)

typedef enum RinGpuCrossProcessCapabilityResult {
    RIN_GPU_CROSS_PROCESS_OK = 0,
    RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT = -1,
    RIN_GPU_CROSS_PROCESS_PROTOCOL = -2,
    RIN_GPU_CROSS_PROCESS_LIMIT = -3,
    RIN_GPU_CROSS_PROCESS_STALE = -4,
    RIN_GPU_CROSS_PROCESS_DENIED = -5
} RinGpuCrossProcessCapabilityResult;

/* This ABI contains identifiers only. No raw pointer or address-space value
 * is transferable between processes. */
typedef struct RinGpuCrossProcessCapabilityDescV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t owner_process_id;
    uint64_t device_generation;
    uint64_t resource_id;
    uint32_t rights;
    uint32_t reserved[2];
} RinGpuCrossProcessCapabilityDescV1;

typedef struct RinGpuCrossProcessCapabilityTokenV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t token;
    uint64_t owner_process_id;
    uint64_t device_generation;
    uint32_t rights;
    uint32_t reserved[2];
} RinGpuCrossProcessCapabilityTokenV1;

typedef struct RinGpuCrossProcessCapabilityRequestV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t token;
    uint64_t process_id;
    uint64_t device_generation;
    uint32_t required_rights;
    uint32_t reserved[2];
} RinGpuCrossProcessCapabilityRequestV1;

typedef struct RinGpuCrossProcessCapabilityEntryV1 {
    uint64_t token;
    uint64_t owner_process_id;
    uint64_t device_generation;
    uint64_t resource_id;
    uint32_t rights;
    uint32_t live;
} RinGpuCrossProcessCapabilityEntryV1;

typedef struct RinGpuCrossProcessCapabilityRuntime {
    uint64_t magic;
    uint64_t next_token;
    uint32_t live_count;
    uint32_t reserved;
    RinGpuCrossProcessCapabilityEntryV1 entries[
        RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX];
} RinGpuCrossProcessCapabilityRuntime;

int rin_gpu_cross_process_capability_init(
    RinGpuCrossProcessCapabilityRuntime* runtime);

int rin_gpu_cross_process_capability_issue(
    RinGpuCrossProcessCapabilityRuntime* runtime,
    const RinGpuCrossProcessCapabilityDescV1* desc,
    RinGpuCrossProcessCapabilityTokenV1* token_out);

int rin_gpu_cross_process_capability_validate(
    const RinGpuCrossProcessCapabilityRuntime* runtime,
    const RinGpuCrossProcessCapabilityRequestV1* request,
    uint64_t* resource_id_out, uint32_t* rights_out);

int rin_gpu_cross_process_capability_release(
    RinGpuCrossProcessCapabilityRuntime* runtime,
    const RinGpuCrossProcessCapabilityTokenV1* token);

int rin_gpu_cross_process_capability_revoke_process(
    RinGpuCrossProcessCapabilityRuntime* runtime, uint64_t process_id,
    uint32_t* revoked_count_out);

#ifdef __cplusplus
}
#endif

#endif
