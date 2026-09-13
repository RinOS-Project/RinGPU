// SPDX-License-Identifier: MIT
#include "modules.h"

#include "../core/object_table.h"
#include "../validation/shader.h"

#include <stdlib.h>
#include <string.h>

static uint64_t ringpu_shader_hash(const void* bytes, uint64_t size)
{
    const uint8_t* data = (const uint8_t*)bytes;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint64_t index = 0u; index < size; index++) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int ringpu_shader_cache_key_matches(
    const RinGpuShaderModuleCacheEntry* entry, const void* shader_ir,
    uint64_t shader_size, uint64_t validated_hash, uint64_t device_generation,
    uint32_t backend_family)
{
    return entry && entry->occupied && entry->shader_size == shader_size &&
           entry->validated_hash == validated_hash &&
           entry->device_generation == device_generation &&
           entry->backend_family == backend_family && entry->rin_shader_ir &&
           memcmp(entry->rin_shader_ir, shader_ir, (size_t)shader_size) == 0;
}

static RinGpuShaderModuleCacheEntry* ringpu_shader_cache_find(
    RinGpuCore* core, const void* shader_ir, uint64_t shader_size,
    uint64_t validated_hash, uint64_t device_generation,
    uint32_t backend_family, uint32_t* index_out)
{
    for (uint32_t index = 0u;
         index < RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE; index++) {
        RinGpuShaderModuleCacheEntry* entry = &core->shader_module_cache[index];
        if (!ringpu_shader_cache_key_matches(
                entry, shader_ir, shader_size, validated_hash,
                device_generation, backend_family))
            continue;
        if (index_out) *index_out = index;
        return entry;
    }
    return NULL;
}

void ringpu_shader_cache_release(RinGpuCore* core, uint32_t index)
{
    RinGpuShaderModuleCacheEntry* entry;
    if (!core || index >= RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE) return;
    entry = &core->shader_module_cache[index];
    if (!entry->occupied || entry->reference_count == 0u) return;
    entry->reference_count--;
    if (entry->reference_count != 0u) return;
    core->backend.destroy_shader_module(core->backend_context,
                                       entry->backend_cookie);
    free(entry->rin_shader_ir);
    memset(entry, 0, sizeof(*entry));
}

static void ringpu_shader_cache_discard_stale(RinGpuCore* core,
                                              uint64_t device_generation,
                                              uint32_t backend_family)
{
    if (!core) return;
    for (uint32_t index = 0u;
         index < RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE; index++) {
        RinGpuShaderModuleCacheEntry* entry = &core->shader_module_cache[index];
        if (!entry->occupied || entry->reference_count != 0u ||
            (entry->device_generation == device_generation &&
             entry->backend_family == backend_family))
            continue;
        core->backend.destroy_shader_module(core->backend_context,
                                            entry->backend_cookie);
        free(entry->rin_shader_ir);
        memset(entry, 0, sizeof(*entry));
    }
}

static int ringpu_shader_cache_insert(
    RinGpuCore* core, uint8_t* shader_ir, uint64_t shader_size,
    uint64_t validated_hash, uint64_t device_generation,
    uint32_t backend_family, const RinShaderInfoV1* info, uint64_t cookie,
    uint32_t* index_out)
{
    if (!core || !shader_ir || !info || !index_out) return 0;
    for (uint32_t index = 0u;
         index < RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE; index++) {
        RinGpuShaderModuleCacheEntry* entry = &core->shader_module_cache[index];
        if (entry->occupied) continue;
        entry->rin_shader_ir = shader_ir;
        entry->shader_size = shader_size;
        entry->validated_hash = validated_hash;
        entry->device_generation = device_generation;
        entry->backend_cookie = cookie;
        entry->info = *info;
        entry->backend_family = backend_family;
        entry->reference_count = 1u;
        entry->occupied = 1u;
        *index_out = index;
        return 1;
    }
    return 0;
}

int ringpu_create_shader_module(RinGpuCore* core, const void* rin_shader_ir,
                                uint64_t shader_size,
                                RinGpuHandle* shader_module)
{
    RinGpuObjectSlot* slot;
    RinGpuShaderModuleCacheEntry* cached;
    RinShaderInfoV1 info;
    uint8_t* snapshot;
    uint64_t cookie = 0u;
    uint64_t validated_hash;
    uint64_t device_generation;
    uint32_t cache_index = RIN_GPU_SHADER_CACHE_INDEX_NONE;
    uint64_t maximum_size = sizeof(RinShaderHeaderV1) +
        (uint64_t)RIN_SHADER_MAX_INSTRUCTIONS *
            sizeof(RinShaderInstructionV1);
    int shader_result;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!rin_shader_ir || !shader_module ||
        shader_size < sizeof(RinShaderHeaderV1) ||
        shader_size > maximum_size || shader_size > SIZE_MAX) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *shader_module = 0u;
    snapshot = (uint8_t*)malloc((size_t)shader_size);
    if (!snapshot) return RIN_GPU_ERROR_NO_MEMORY;
    memcpy(snapshot, rin_shader_ir, (size_t)shader_size);
    shader_result = ringpu_shader_validate(snapshot, (size_t)shader_size,
                                           &info);
    if (shader_result != RIN_SHADER_OK) {
        free(snapshot);
        return shader_result == RIN_SHADER_ERROR_NO_MEMORY
            ? RIN_GPU_ERROR_NO_MEMORY : RIN_GPU_ERROR_SHADER_INVALID;
    }
    validated_hash = ringpu_shader_hash(snapshot, shader_size);
    device_generation = ringpu_core_device_generation(core->diagnostics);
    if (device_generation == 0u) device_generation = core->device_generation;
    ringpu_shader_cache_discard_stale(core, device_generation,
                                      core->backend_family);
    cached = ringpu_shader_cache_find(
        core, snapshot, shader_size, validated_hash, device_generation,
        core->backend_family, &cache_index);
    if (cached) {
        result = ringpu_allocate(core, RIN_GPU_OBJECT_SHADER_MODULE,
                                 shader_module, &slot);
        if (result != RIN_GPU_OK) {
            free(snapshot);
            *shader_module = 0u;
            return result;
        }
        free(snapshot);
        cached->reference_count++;
        slot->value.shader_module.rin_shader_ir = cached->rin_shader_ir;
        slot->value.shader_module.shader_size = cached->shader_size;
        slot->value.shader_module.backend_cookie = cached->backend_cookie;
        slot->value.shader_module.info = cached->info;
        slot->value.shader_module.cache_index = cache_index;
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE,
                               cached->backend_cookie, 0u,
                               RIN_GPU_OBJECT_SHADER_MODULE, shader_size,
                               RIN_GPU_OK);
        return RIN_GPU_OK;
    }
    result = core->backend.create_shader_module(
        core->backend_context, snapshot, shader_size, &info, &cookie);
    if (result != RIN_GPU_OK) {
        free(snapshot);
        return result;
    }
    result = ringpu_allocate(core, RIN_GPU_OBJECT_SHADER_MODULE,
                             shader_module, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_shader_module(core->backend_context, cookie);
        free(snapshot);
        *shader_module = 0u;
        return result;
    }
    slot->value.shader_module.rin_shader_ir = snapshot;
    slot->value.shader_module.shader_size = shader_size;
    slot->value.shader_module.backend_cookie = cookie;
    slot->value.shader_module.info = info;
    if (!ringpu_shader_cache_insert(
            core, snapshot, shader_size, validated_hash, device_generation,
            core->backend_family, &info, cookie, &cache_index)) {
        slot->value.shader_module.cache_index =
            RIN_GPU_SHADER_CACHE_INDEX_NONE;
    } else {
        slot->value.shader_module.rin_shader_ir =
            core->shader_module_cache[cache_index].rin_shader_ir;
        slot->value.shader_module.cache_index = cache_index;
    }
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_SHADER_MODULE, shader_size,
                           RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_get_shader_info(const RinGpuCore* core,
                           RinGpuHandle shader_module,
                           RinShaderInfoV1* info)
{
    const RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!info) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    result = ringpu_slot_const(core, shader_module,
                               RIN_GPU_OBJECT_SHADER_MODULE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    *info = slot->value.shader_module.info;
    return RIN_GPU_OK;
}
