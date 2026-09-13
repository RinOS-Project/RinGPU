/* SPDX-License-Identifier: MIT */
#ifndef RIN_GPU_CROSS_PROCESS_CAPABILITY_H
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_H

#include <stdint.h>

#include <rin/gpu_capability.h>
#include <rin/ipc.h>

#ifdef __cplusplus
extern "C" {
#endif
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION RIN_GPU_CAPABILITY_IPC_VERSION

typedef struct RinGpuCrossProcessCapabilityEntryV1 {
    uint64_t token;
    uint64_t owner_process_id;
    uint64_t device_generation;
    uint64_t resource_id;
    uint32_t rights;
    uint32_t live;
} RinGpuCrossProcessCapabilityEntryV1;

typedef enum RinGpuCrossProcessCapabilityResult {
    RIN_GPU_CROSS_PROCESS_OK = 0,
    RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT = -1,
    RIN_GPU_CROSS_PROCESS_PROTOCOL = -2,
    RIN_GPU_CROSS_PROCESS_LIMIT = -3,
    RIN_GPU_CROSS_PROCESS_STALE = -4,
    RIN_GPU_CROSS_PROCESS_DENIED = -5,
    RIN_GPU_CROSS_PROCESS_IPC = -6
} RinGpuCrossProcessCapabilityResult;

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

typedef struct RinGpuCrossProcessCapabilityIpcClientV1 {
    RinChannel channel;
    uint64_t next_request_id;
    uint32_t reserved[2];
} RinGpuCrossProcessCapabilityIpcClientV1;

int rin_gpu_cross_process_capability_ipc_init(
    RinGpuCrossProcessCapabilityIpcClientV1* client, RinChannel channel);
int rin_gpu_cross_process_capability_ipc_issue(
    RinGpuCrossProcessCapabilityIpcClientV1* client,
    const RinGpuCrossProcessCapabilityDescV1* desc,
    RinGpuCrossProcessCapabilityTokenV1* token_out);
int rin_gpu_cross_process_capability_ipc_validate(
    RinGpuCrossProcessCapabilityIpcClientV1* client,
    const RinGpuCrossProcessCapabilityRequestV1* request,
    uint64_t* resource_id_out, uint32_t* rights_out);
int rin_gpu_cross_process_capability_ipc_release(
    RinGpuCrossProcessCapabilityIpcClientV1* client,
    const RinGpuCrossProcessCapabilityTokenV1* token);
int rin_gpu_cross_process_capability_ipc_revoke_process(
    RinGpuCrossProcessCapabilityIpcClientV1* client, uint64_t process_id,
    uint32_t* revoked_count_out);

#ifdef __cplusplus
}
#endif

#endif
