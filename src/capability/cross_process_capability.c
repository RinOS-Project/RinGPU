/* SPDX-License-Identifier: MIT */
#include "cross_process_capability.h"

#include <stddef.h>
#include <string.h>

#define RIN_GPU_CROSS_PROCESS_CAPABILITY_MAGIC UINT64_C(0x52494e4350415031)
#define RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_MAGIC \
    UINT64_C(0x52494e4350415032)

typedef struct RinGpuCrossProcessCapabilityEntryV2Internal {
    uint64_t token;
    uint64_t owner_process_id;
    uint64_t owner_process_instance_cookie;
    uint64_t recipient_process_id;
    uint64_t recipient_process_instance_cookie;
    uint64_t device_generation;
    uint64_t resource_id;
    uint32_t rights;
    uint32_t active_lease_count;
    uint32_t live;
    uint32_t revoked;
} RinGpuCrossProcessCapabilityEntryV2Internal;

typedef struct RinGpuCrossProcessCapabilityLeaseEntryV2Internal {
    uint64_t token;
    uint64_t lease_id;
    uint64_t recipient_process_id;
    uint64_t recipient_process_instance_cookie;
    uint32_t live;
    uint32_t reserved;
} RinGpuCrossProcessCapabilityLeaseEntryV2Internal;

typedef struct RinGpuCrossProcessCapabilityRuntimeV2Internal {
    uint64_t magic;
    uint64_t next_token;
    uint64_t next_lease_id;
    uint32_t live_count;
    uint32_t active_lease_count;
    RinGpuCrossProcessCapabilityEntryV2Internal entries[
        RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX];
    RinGpuCrossProcessCapabilityLeaseEntryV2Internal leases[
        RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2];
} RinGpuCrossProcessCapabilityRuntimeV2Internal;

_Static_assert(
    sizeof(RinGpuCrossProcessCapabilityRuntimeV2Internal) ==
        sizeof(RinGpuCrossProcessCapabilityRuntimeV2),
    "V2 cross-process capability runtime storage drift");

static int descriptor_valid(const RinGpuCrossProcessCapabilityDescV1* desc)
{
    return desc != NULL &&
           desc->struct_size >= sizeof(*desc) &&
           desc->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION &&
           desc->owner_process_id != 0u && desc->device_generation != 0u &&
           desc->resource_id != 0u && desc->rights != 0u &&
           (desc->rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           desc->reserved[0] == 0u && desc->reserved[1] == 0u &&
           desc->reserved[2] == 0u;
}

static int runtime_valid(const RinGpuCrossProcessCapabilityRuntime* runtime)
{
    return runtime != NULL && runtime->magic ==
           RIN_GPU_CROSS_PROCESS_CAPABILITY_MAGIC;
}

static int token_output_valid(const RinGpuCrossProcessCapabilityTokenV1* token)
{
    return token != NULL && token->struct_size >= sizeof(*token) &&
           token->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION &&
           token->reserved[0] == 0u && token->reserved[1] == 0u &&
           token->reserved[2] == 0u;
}

static int request_valid(const RinGpuCrossProcessCapabilityRequestV1* request)
{
    return request != NULL && request->struct_size >= sizeof(*request) &&
           request->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION &&
           request->token != 0u && request->process_id != 0u &&
           request->device_generation != 0u &&
           (request->required_rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           request->reserved[0] == 0u && request->reserved[1] == 0u &&
           request->reserved[2] == 0u;
}

int rin_gpu_cross_process_capability_init(
    RinGpuCrossProcessCapabilityRuntime* runtime)
{
    if (runtime == NULL) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    memset(runtime, 0, sizeof(*runtime));
    runtime->magic = RIN_GPU_CROSS_PROCESS_CAPABILITY_MAGIC;
    runtime->next_token = 1u;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_issue(
    RinGpuCrossProcessCapabilityRuntime* runtime,
    const RinGpuCrossProcessCapabilityDescV1* desc,
    RinGpuCrossProcessCapabilityTokenV1* token_out)
{
    uint32_t index;
    uint64_t token;

    if (!runtime_valid(runtime) || !descriptor_valid(desc) ||
        token_out == NULL || token_out->struct_size < sizeof(*token_out) ||
        token_out->version != RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION ||
        token_out->reserved[0] != 0u || token_out->reserved[1] != 0u ||
        token_out->reserved[2] != 0u)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    for (index = 0u; index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX; ++index) {
        if (runtime->entries[index].live == 0u) break;
    }
    if (index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX ||
        runtime->next_token == 0u)
        return RIN_GPU_CROSS_PROCESS_LIMIT;
    token = runtime->next_token;
    runtime->next_token = token == UINT64_MAX ? 0u : token + 1u;
    runtime->entries[index].token = token;
    runtime->entries[index].owner_process_id = desc->owner_process_id;
    runtime->entries[index].device_generation = desc->device_generation;
    runtime->entries[index].resource_id = desc->resource_id;
    runtime->entries[index].rights = desc->rights;
    runtime->entries[index].live = 1u;
    ++runtime->live_count;
    memset(token_out, 0, sizeof(*token_out));
    token_out->struct_size = sizeof(*token_out);
    token_out->version = RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION;
    token_out->token = token;
    token_out->owner_process_id = desc->owner_process_id;
    token_out->device_generation = desc->device_generation;
    token_out->rights = desc->rights;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_validate(
    const RinGpuCrossProcessCapabilityRuntime* runtime,
    const RinGpuCrossProcessCapabilityRequestV1* request,
    uint64_t* resource_id_out, uint32_t* rights_out)
{
    uint32_t index;

    if (resource_id_out == NULL || rights_out == NULL)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    *resource_id_out = 0u;
    *rights_out = 0u;
    if (!runtime_valid(runtime) || !request_valid(request))
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    for (index = 0u; index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX; ++index) {
        const RinGpuCrossProcessCapabilityEntryV1* entry =
            &runtime->entries[index];
        if (entry->live != 0u && entry->token == request->token) {
            if (entry->owner_process_id != request->process_id ||
                entry->device_generation != request->device_generation)
                return RIN_GPU_CROSS_PROCESS_STALE;
            if ((request->required_rights & entry->rights) !=
                request->required_rights)
                return RIN_GPU_CROSS_PROCESS_DENIED;
            *resource_id_out = entry->resource_id;
            *rights_out = entry->rights;
            return RIN_GPU_CROSS_PROCESS_OK;
        }
    }
    return RIN_GPU_CROSS_PROCESS_STALE;
}

int rin_gpu_cross_process_capability_release(
    RinGpuCrossProcessCapabilityRuntime* runtime,
    const RinGpuCrossProcessCapabilityTokenV1* token)
{
    uint32_t index;

    if (!runtime_valid(runtime) || !token_output_valid(token) ||
        token->token == 0u || token->owner_process_id == 0u ||
        token->device_generation == 0u)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    for (index = 0u; index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX; ++index) {
        RinGpuCrossProcessCapabilityEntryV1* entry = &runtime->entries[index];
        if (entry->live != 0u && entry->token == token->token) {
            if (entry->owner_process_id != token->owner_process_id ||
                entry->device_generation != token->device_generation)
                return RIN_GPU_CROSS_PROCESS_STALE;
            memset(entry, 0, sizeof(*entry));
            --runtime->live_count;
            return RIN_GPU_CROSS_PROCESS_OK;
        }
    }
    return RIN_GPU_CROSS_PROCESS_STALE;
}

int rin_gpu_cross_process_capability_revoke_process(
    RinGpuCrossProcessCapabilityRuntime* runtime, uint64_t process_id,
    uint32_t* revoked_count_out)
{
    uint32_t index;
    uint32_t revoked = 0u;

    if (revoked_count_out == NULL)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    *revoked_count_out = 0u;
    if (!runtime_valid(runtime) || process_id == 0u)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    for (index = 0u; index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX; ++index) {
        RinGpuCrossProcessCapabilityEntryV1* entry = &runtime->entries[index];
        if (entry->live != 0u && entry->owner_process_id == process_id) {
            memset(entry, 0, sizeof(*entry));
            --runtime->live_count;
            ++revoked;
        }
    }
    *revoked_count_out = revoked;
    return RIN_GPU_CROSS_PROCESS_OK;
}

static int peer_identity_valid_v1(const RinIpcPeerIdentityV1* peer)
{
    return peer != NULL && peer->struct_size == sizeof(*peer) &&
           peer->version == RIN_SDK_STRUCT_VERSION_1 &&
           peer->process_id != 0u &&
           peer->process_instance_cookie != 0u && peer->reserved == 0u;
}

static int runtime_v2_valid(
    const RinGpuCrossProcessCapabilityRuntimeV2* runtime)
{
    const RinGpuCrossProcessCapabilityRuntimeV2Internal* state =
        (const RinGpuCrossProcessCapabilityRuntimeV2Internal*)runtime;
    return runtime != NULL &&
           state->magic == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_MAGIC &&
           state->live_count <= RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX &&
           state->active_lease_count <=
               RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2;
}

static int grant_desc_v2_valid(
    const RinGpuCrossProcessCapabilityGrantDescV2* desc)
{
    return desc != NULL && desc->struct_size == sizeof(*desc) &&
           desc->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION &&
           desc->device_generation != 0u && desc->resource_id != 0u &&
           desc->recipient_process_id != 0u &&
           desc->recipient_process_instance_cookie != 0u &&
           desc->rights != 0u &&
           (desc->rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           desc->flags == 0u && desc->reserved[0] == 0u &&
           desc->reserved[1] == 0u;
}

static int token_v2_valid(const RinGpuCrossProcessCapabilityTokenV2* token)
{
    return token != NULL && token->struct_size == sizeof(*token) &&
           token->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION &&
           token->token != 0u && token->device_generation != 0u &&
           token->rights != 0u &&
           (token->rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           token->reserved[0] == 0u && token->reserved[1] == 0u &&
           token->reserved[2] == 0u;
}

static int token_v2_output_valid(
    const RinGpuCrossProcessCapabilityTokenV2* token)
{
    return token != NULL && token->struct_size == sizeof(*token) &&
           token->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION &&
           token->token == 0u && token->device_generation == 0u &&
           token->rights == 0u && token->reserved[0] == 0u &&
           token->reserved[1] == 0u && token->reserved[2] == 0u;
}

static int acquire_request_v2_valid(
    const RinGpuCrossProcessCapabilityAcquireRequestV2* request)
{
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION &&
           request->token != 0u && request->device_generation != 0u &&
           (request->required_rights &
            ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           request->reserved[0] == 0u && request->reserved[1] == 0u &&
           request->reserved[2] == 0u;
}

static int lease_v2_output_valid(
    const RinGpuCrossProcessCapabilityLeaseV2* lease)
{
    return lease != NULL && lease->struct_size == sizeof(*lease) &&
           lease->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION &&
           lease->token == 0u && lease->lease_id == 0u &&
           lease->resource_id == 0u && lease->device_generation == 0u &&
           lease->rights == 0u && lease->reserved[0] == 0u &&
           lease->reserved[1] == 0u && lease->reserved[2] == 0u;
}

static int release_lease_request_v2_valid(
    const RinGpuCrossProcessCapabilityReleaseLeaseV2* request)
{
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION &&
           request->token != 0u && request->lease_id != 0u &&
           request->device_generation != 0u && request->reserved[0] == 0u &&
           request->reserved[1] == 0u;
}

static uint32_t find_v2_entry(
    const RinGpuCrossProcessCapabilityRuntimeV2Internal* state,
    uint64_t token)
{
    uint32_t index;
    for (index = 0u; index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX; ++index)
        if (state->entries[index].live == 1u &&
            state->entries[index].token == token)
            return index;
    return RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX;
}

int rin_gpu_cross_process_capability_v2_init(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime)
{
    RinGpuCrossProcessCapabilityRuntimeV2Internal* state;
    if (runtime == NULL) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    memset(runtime, 0, sizeof(*runtime));
    state = (RinGpuCrossProcessCapabilityRuntimeV2Internal*)runtime;
    state->magic = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_MAGIC;
    state->next_token = 1u;
    state->next_lease_id = 1u;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_v2_issue(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_owner,
    const RinGpuCrossProcessCapabilityGrantDescV2* desc,
    RinGpuCrossProcessCapabilityTokenV2* token_out)
{
    RinGpuCrossProcessCapabilityRuntimeV2Internal* state =
        (RinGpuCrossProcessCapabilityRuntimeV2Internal*)runtime;
    uint32_t index;
    uint64_t token;

    if (!runtime_v2_valid(runtime) ||
        !peer_identity_valid_v1(authenticated_owner) ||
        !grant_desc_v2_valid(desc) ||
        !token_v2_output_valid(token_out))
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    if (authenticated_owner->process_id == desc->recipient_process_id &&
        authenticated_owner->process_instance_cookie ==
            desc->recipient_process_instance_cookie)
        return RIN_GPU_CROSS_PROCESS_DENIED;
    for (index = 0u; index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX; ++index)
        if (state->entries[index].live == 0u) break;
    if (index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX ||
        state->next_token == 0u)
        return RIN_GPU_CROSS_PROCESS_LIMIT;

    token = state->next_token;
    state->next_token = token == UINT64_MAX ? 0u : token + 1u;
    state->entries[index].token = token;
    state->entries[index].owner_process_id =
        authenticated_owner->process_id;
    state->entries[index].owner_process_instance_cookie =
        authenticated_owner->process_instance_cookie;
    state->entries[index].recipient_process_id =
        desc->recipient_process_id;
    state->entries[index].recipient_process_instance_cookie =
        desc->recipient_process_instance_cookie;
    state->entries[index].device_generation = desc->device_generation;
    state->entries[index].resource_id = desc->resource_id;
    state->entries[index].rights = desc->rights;
    state->entries[index].active_lease_count = 0u;
    state->entries[index].live = 1u;
    state->entries[index].revoked = 0u;
    ++state->live_count;

    memset(token_out, 0, sizeof(*token_out));
    token_out->struct_size = sizeof(*token_out);
    token_out->version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
    token_out->token = token;
    token_out->device_generation = desc->device_generation;
    token_out->rights = desc->rights;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_v2_acquire(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_recipient,
    const RinGpuCrossProcessCapabilityAcquireRequestV2* request,
    RinGpuCrossProcessCapabilityLeaseV2* lease_out)
{
    RinGpuCrossProcessCapabilityRuntimeV2Internal* state =
        (RinGpuCrossProcessCapabilityRuntimeV2Internal*)runtime;
    uint32_t entry_index;
    uint32_t lease_index;
    uint64_t lease_id;
    RinGpuCrossProcessCapabilityEntryV2Internal* entry;

    if (!runtime_v2_valid(runtime) ||
        !peer_identity_valid_v1(authenticated_recipient) ||
        !acquire_request_v2_valid(request) ||
        !lease_v2_output_valid(lease_out))
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    entry_index = find_v2_entry(state, request->token);
    if (entry_index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX)
        return RIN_GPU_CROSS_PROCESS_STALE;
    entry = &state->entries[entry_index];
    if (entry->device_generation != request->device_generation)
        return RIN_GPU_CROSS_PROCESS_STALE;
    if (entry->recipient_process_id !=
            authenticated_recipient->process_id ||
        entry->recipient_process_instance_cookie !=
            authenticated_recipient->process_instance_cookie)
        return RIN_GPU_CROSS_PROCESS_DENIED;
    if (entry->revoked != 0u) return RIN_GPU_CROSS_PROCESS_STALE;
    if ((request->required_rights & entry->rights) !=
        request->required_rights)
        return RIN_GPU_CROSS_PROCESS_DENIED;
    if (state->next_lease_id == 0u ||
        state->active_lease_count >=
            RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2 ||
        entry->active_lease_count == UINT32_MAX)
        return RIN_GPU_CROSS_PROCESS_LIMIT;
    for (lease_index = 0u;
         lease_index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2;
         ++lease_index)
        if (state->leases[lease_index].live == 0u) break;
    if (lease_index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2)
        return RIN_GPU_CROSS_PROCESS_LIMIT;

    lease_id = state->next_lease_id;
    state->next_lease_id = lease_id == UINT64_MAX ? 0u : lease_id + 1u;
    state->leases[lease_index].token = entry->token;
    state->leases[lease_index].lease_id = lease_id;
    state->leases[lease_index].recipient_process_id =
        authenticated_recipient->process_id;
    state->leases[lease_index].recipient_process_instance_cookie =
        authenticated_recipient->process_instance_cookie;
    state->leases[lease_index].live = 1u;
    state->leases[lease_index].reserved = 0u;
    ++state->active_lease_count;
    ++entry->active_lease_count;

    memset(lease_out, 0, sizeof(*lease_out));
    lease_out->struct_size = sizeof(*lease_out);
    lease_out->version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
    lease_out->token = entry->token;
    lease_out->lease_id = lease_id;
    lease_out->resource_id = entry->resource_id;
    lease_out->device_generation = entry->device_generation;
    lease_out->rights = entry->rights;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_v2_release_lease(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_recipient,
    const RinGpuCrossProcessCapabilityReleaseLeaseV2* request)
{
    RinGpuCrossProcessCapabilityRuntimeV2Internal* state =
        (RinGpuCrossProcessCapabilityRuntimeV2Internal*)runtime;
    uint32_t lease_index;
    uint32_t entry_index;
    RinGpuCrossProcessCapabilityLeaseEntryV2Internal* lease;
    RinGpuCrossProcessCapabilityEntryV2Internal* entry;

    if (!runtime_v2_valid(runtime) ||
        !peer_identity_valid_v1(authenticated_recipient) ||
        !release_lease_request_v2_valid(request))
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    for (lease_index = 0u;
         lease_index < RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2;
         ++lease_index)
        if (state->leases[lease_index].live == 1u &&
            state->leases[lease_index].token == request->token &&
            state->leases[lease_index].lease_id == request->lease_id)
            break;
    if (lease_index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX_LEASES_V2)
        return RIN_GPU_CROSS_PROCESS_STALE;
    lease = &state->leases[lease_index];
    if (lease->recipient_process_id != authenticated_recipient->process_id ||
        lease->recipient_process_instance_cookie !=
            authenticated_recipient->process_instance_cookie)
        return RIN_GPU_CROSS_PROCESS_DENIED;
    entry_index = find_v2_entry(state, request->token);
    if (entry_index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    entry = &state->entries[entry_index];
    if (entry->device_generation != request->device_generation ||
        entry->active_lease_count == 0u ||
        state->active_lease_count == 0u)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;

    memset(lease, 0, sizeof(*lease));
    --entry->active_lease_count;
    --state->active_lease_count;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_v2_revoke(
    RinGpuCrossProcessCapabilityRuntimeV2* runtime,
    const RinIpcPeerIdentityV1* authenticated_owner,
    const RinGpuCrossProcessCapabilityTokenV2* token,
    uint32_t* outstanding_lease_count_out)
{
    RinGpuCrossProcessCapabilityRuntimeV2Internal* state =
        (RinGpuCrossProcessCapabilityRuntimeV2Internal*)runtime;
    uint32_t entry_index;
    RinGpuCrossProcessCapabilityEntryV2Internal* entry;

    if (outstanding_lease_count_out == NULL)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    *outstanding_lease_count_out = 0u;
    if (!runtime_v2_valid(runtime) ||
        !peer_identity_valid_v1(authenticated_owner) ||
        !token_v2_valid(token))
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    entry_index = find_v2_entry(state, token->token);
    if (entry_index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX)
        return RIN_GPU_CROSS_PROCESS_STALE;
    entry = &state->entries[entry_index];
    if (entry->owner_process_id != authenticated_owner->process_id ||
        entry->owner_process_instance_cookie !=
            authenticated_owner->process_instance_cookie)
        return RIN_GPU_CROSS_PROCESS_DENIED;
    if (entry->device_generation != token->device_generation ||
        entry->rights != token->rights)
        return RIN_GPU_CROSS_PROCESS_STALE;

    entry->revoked = 1u;
    *outstanding_lease_count_out = entry->active_lease_count;
    if (entry->active_lease_count != 0u)
        return RIN_GPU_CROSS_PROCESS_BUSY;
    memset(entry, 0, sizeof(*entry));
    --state->live_count;
    return RIN_GPU_CROSS_PROCESS_OK;
}
