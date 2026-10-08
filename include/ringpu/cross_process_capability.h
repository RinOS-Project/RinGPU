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
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION UINT32_C(2)
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2 128u

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
    RIN_GPU_CROSS_PROCESS_IPC = -6,
    RIN_GPU_CROSS_PROCESS_BUSY = -7
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

/* V2 is recipient-bound and must be serialized by its owning service. Each
 * successful acquire creates a unique lease; revocation blocks new acquires
 * and reports BUSY until every exact consumer lease is released. The peer
 * records passed here must come from rin_channel_receive_with_peer_v1, never
 * from identity fields in message payloads. */
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_RUNTIME_STATE_QWORDS 1220u
typedef struct RinGpuCrossProcessCapabilityRuntimeV2 {
    uint64_t opaque[RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_RUNTIME_STATE_QWORDS];
} RinGpuCrossProcessCapabilityRuntimeV2;

int rin_gpu_cross_process_capability_v2_init(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime);
int rin_gpu_cross_process_capability_v2_issue(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_owner,
    const RinGpuCrossProcessCapabilityGrantDescV2* desc,
    RinGpuCrossProcessCapabilityTokenV2* token_out);
int rin_gpu_cross_process_capability_v2_acquire(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_recipient,
    const RinGpuCrossProcessCapabilityAcquireRequestV2* request,
    RinGpuCrossProcessCapabilityLeaseV2* lease_out);
int rin_gpu_cross_process_capability_v2_release_lease(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_recipient,
    const RinGpuCrossProcessCapabilityReleaseLeaseV2* request);
int rin_gpu_cross_process_capability_v2_revoke(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_owner,
    const RinGpuCrossProcessCapabilityTokenV2* token,
    uint32_t* outstanding_lease_count_out);

typedef struct RinGpuCrossProcessCapabilityIpcClientV1 {
    RinChannel channel;
    uint64_t next_request_id;
    uint32_t reserved[2];
} RinGpuCrossProcessCapabilityIpcClientV1;

int rin_gpu_cross_process_capability_ipc_init(
    RinGpuCrossProcessCapabilityIpcClientV1* client, RinChannel channel);
int rin_gpu_cross_process_capability_ipc_connect(
    RinGpuCrossProcessCapabilityIpcClientV1* client);
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
