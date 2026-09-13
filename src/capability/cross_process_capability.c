/* SPDX-License-Identifier: MIT */
#include "cross_process_capability.h"

#include <stddef.h>
#include <string.h>

#define RIN_GPU_CROSS_PROCESS_CAPABILITY_MAGIC UINT64_C(0x52494e4350415031)

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
    if (index == RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX)
        return RIN_GPU_CROSS_PROCESS_LIMIT;
    token = runtime->next_token++;
    if (token == 0u) return RIN_GPU_CROSS_PROCESS_LIMIT;
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
