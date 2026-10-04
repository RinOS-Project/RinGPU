// SPDX-License-Identifier: MIT
#include "cache.h"

#include "../platform/thread.h"

#include <stdlib.h>
#include <string.h>

static uint64_t ringpu_pipeline_cache_hash(const void* descriptor,
                                           uint32_t descriptor_size)
{
    const uint8_t* bytes = (const uint8_t*)descriptor;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t index = 0u; index < descriptor_size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t ringpu_pipeline_cache_generation(const RinGpuCore* core)
{
    uint64_t generation = ringpu_core_device_generation(core->diagnostics);
    return generation == 0u ? core->device_generation : generation;
}

static int ringpu_pipeline_cache_key_matches(
    const RinGpuPipelineCacheEntry* entry, const RinGpuCore* core,
    uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size, uint64_t descriptor_hash,
    uint64_t device_generation)
{
    if (!entry || !entry->occupied ||
        entry->pipeline_kind != pipeline_kind ||
        entry->shader_cookie_a != shader_cookie_a ||
        entry->shader_cookie_b != shader_cookie_b ||
        entry->backend_family != core->backend_family ||
        entry->device_generation != device_generation ||
        entry->descriptor_hash != descriptor_hash ||
        entry->descriptor_size != descriptor_size)
        return 0;
    if (descriptor_size == 0u) return descriptor == NULL;
    return descriptor != NULL && entry->descriptor_snapshot != NULL &&
           memcmp(entry->descriptor_snapshot, descriptor,
                  descriptor_size) == 0;
}

static void ringpu_pipeline_cache_lock(RinGpuCore* core)
{
    ringpu_platform_resource_lock_acquire(
        &core->pipeline_cache_lock, core->platform_yield_thread,
        core->platform_scheduler_context);
}

static void ringpu_pipeline_cache_unlock(RinGpuCore* core)
{
    ringpu_platform_resource_lock_release(&core->pipeline_cache_lock);
}

static void ringpu_pipeline_cache_touch_locked(
    RinGpuCore* core, RinGpuPipelineCacheEntry* entry)
{
    if (core->pipeline_cache_clock == UINT64_MAX) {
        core->pipeline_cache_clock = 0u;
        for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_PIPELINE_CACHE;
             ++index) {
            if (core->pipeline_cache[index].occupied)
                core->pipeline_cache[index].last_used = 0u;
        }
    }
    entry->last_used = ++core->pipeline_cache_clock;
}

static void ringpu_pipeline_cache_destroy_backend(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t backend_cookie)
{
    if (pipeline_kind == RIN_GPU_PIPELINE_CACHE_COMPUTE)
        core->backend.destroy_compute_pipeline(core->backend_context,
                                               backend_cookie);
    else
        core->backend.destroy_graphics_pipeline(core->backend_context,
                                                backend_cookie);
}

static RinGpuPipelineCacheEntry* ringpu_pipeline_cache_find_locked(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size, uint64_t descriptor_hash,
    uint64_t device_generation, uint32_t* index_out)
{
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_PIPELINE_CACHE;
         ++index) {
        RinGpuPipelineCacheEntry* entry = &core->pipeline_cache[index];
        if (!ringpu_pipeline_cache_key_matches(
                entry, core, pipeline_kind, shader_cookie_a,
                shader_cookie_b, descriptor, descriptor_size,
                descriptor_hash, device_generation))
            continue;
        if (index_out) *index_out = index;
        return entry;
    }
    return NULL;
}

static int ringpu_pipeline_cache_arguments_valid(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size)
{
    if (!core || shader_cookie_a == 0u ||
        (pipeline_kind != RIN_GPU_PIPELINE_CACHE_COMPUTE &&
         pipeline_kind != RIN_GPU_PIPELINE_CACHE_GRAPHICS) ||
        (descriptor_size == 0u ? descriptor != NULL : descriptor == NULL))
        return 0;
    if (pipeline_kind == RIN_GPU_PIPELINE_CACHE_COMPUTE)
        return shader_cookie_b == 0u && descriptor_size == 0u;
    return shader_cookie_b != 0u && descriptor_size != 0u;
}

int ringpu_pipeline_cache_acquire(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size, uint64_t* backend_cookie_out,
    uint32_t* cache_index_out)
{
    RinGpuPipelineCacheEntry* entry;
    uint64_t descriptor_hash;
    uint64_t device_generation;
    uint32_t cache_index = RIN_GPU_PIPELINE_CACHE_INDEX_NONE;
    if (backend_cookie_out) *backend_cookie_out = 0u;
    if (cache_index_out)
        *cache_index_out = RIN_GPU_PIPELINE_CACHE_INDEX_NONE;
    if (!backend_cookie_out || !cache_index_out ||
        !ringpu_pipeline_cache_arguments_valid(
            core, pipeline_kind, shader_cookie_a, shader_cookie_b,
            descriptor, descriptor_size))
        return 0;
    descriptor_hash = ringpu_pipeline_cache_hash(descriptor,
                                                  descriptor_size);
    device_generation = ringpu_pipeline_cache_generation(core);
    ringpu_pipeline_cache_lock(core);
    entry = ringpu_pipeline_cache_find_locked(
        core, pipeline_kind, shader_cookie_a, shader_cookie_b, descriptor,
        descriptor_size, descriptor_hash, device_generation, &cache_index);
    if (!entry || entry->reference_count == UINT32_MAX) {
        ringpu_pipeline_cache_unlock(core);
        return 0;
    }
    entry->reference_count++;
    ringpu_pipeline_cache_touch_locked(core, entry);
    *backend_cookie_out = entry->backend_cookie;
    *cache_index_out = cache_index;
    ringpu_pipeline_cache_unlock(core);
    return 1;
}

int ringpu_pipeline_cache_publish(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size, uint64_t created_backend_cookie,
    uint64_t* backend_cookie_out, uint32_t* cache_index_out)
{
    RinGpuPipelineCacheEntry* entry;
    RinGpuPipelineCacheEntry evicted = {0};
    int has_evicted = 0;
    void* descriptor_copy = NULL;
    uint64_t descriptor_hash;
    uint64_t device_generation;
    uint32_t cache_index = RIN_GPU_PIPELINE_CACHE_INDEX_NONE;
    if (backend_cookie_out) *backend_cookie_out = created_backend_cookie;
    if (cache_index_out)
        *cache_index_out = RIN_GPU_PIPELINE_CACHE_INDEX_NONE;
    if (!backend_cookie_out || !cache_index_out ||
        created_backend_cookie == 0u ||
        !ringpu_pipeline_cache_arguments_valid(
            core, pipeline_kind, shader_cookie_a, shader_cookie_b,
            descriptor, descriptor_size))
        return 0;
    descriptor_hash = ringpu_pipeline_cache_hash(descriptor,
                                                  descriptor_size);
    device_generation = ringpu_pipeline_cache_generation(core);

    if (descriptor_size != 0u) {
        descriptor_copy = malloc(descriptor_size);
        if (!descriptor_copy) return 0;
        memcpy(descriptor_copy, descriptor, descriptor_size);
    }

    ringpu_pipeline_cache_lock(core);
    entry = ringpu_pipeline_cache_find_locked(
        core, pipeline_kind, shader_cookie_a, shader_cookie_b, descriptor,
        descriptor_size, descriptor_hash, device_generation, &cache_index);
    if (entry && entry->reference_count != UINT32_MAX) {
        entry->reference_count++;
        ringpu_pipeline_cache_touch_locked(core, entry);
        *backend_cookie_out = entry->backend_cookie;
        *cache_index_out = cache_index;
        ringpu_pipeline_cache_unlock(core);
        free(descriptor_copy);
        return 2;
    }
    for (cache_index = 0u; cache_index < RIN_GPU_CORE_MAX_PIPELINE_CACHE;
         ++cache_index) {
        if (!core->pipeline_cache[cache_index].occupied) break;
    }
    if (cache_index == RIN_GPU_CORE_MAX_PIPELINE_CACHE) {
        uint64_t oldest_use = UINT64_MAX;
        uint32_t oldest_index = RIN_GPU_CORE_MAX_PIPELINE_CACHE;
        for (uint32_t index = 0u;
             index < RIN_GPU_CORE_MAX_PIPELINE_CACHE; ++index) {
            RinGpuPipelineCacheEntry* candidate =
                &core->pipeline_cache[index];
            if (candidate->reference_count == 0u &&
                candidate->last_used <= oldest_use) {
                oldest_use = candidate->last_used;
                oldest_index = index;
            }
        }
        if (oldest_index == RIN_GPU_CORE_MAX_PIPELINE_CACHE) {
            ringpu_pipeline_cache_unlock(core);
            free(descriptor_copy);
            return 0;
        }
        cache_index = oldest_index;
        evicted = core->pipeline_cache[cache_index];
        has_evicted = 1;
    }
    entry = &core->pipeline_cache[cache_index];
    memset(entry, 0, sizeof(*entry));
    entry->shader_cookie_a = shader_cookie_a;
    entry->shader_cookie_b = shader_cookie_b;
    entry->backend_cookie = created_backend_cookie;
    entry->device_generation = device_generation;
    entry->descriptor_hash = descriptor_hash;
    entry->descriptor_snapshot = descriptor_copy;
    entry->backend_family = core->backend_family;
    entry->pipeline_kind = pipeline_kind;
    entry->reference_count = 1u;
    entry->descriptor_size = descriptor_size;
    entry->occupied = 1u;
    ringpu_pipeline_cache_touch_locked(core, entry);
    *cache_index_out = cache_index;
    ringpu_pipeline_cache_unlock(core);
    if (has_evicted) {
        free(evicted.descriptor_snapshot);
        ringpu_pipeline_cache_destroy_backend(
            core, evicted.pipeline_kind, evicted.backend_cookie);
    }
    return 1;
}

int ringpu_pipeline_cache_release(RinGpuCore* core, uint32_t cache_index,
                                  uint64_t backend_cookie)
{
    if (!core || cache_index >= RIN_GPU_CORE_MAX_PIPELINE_CACHE ||
        backend_cookie == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    ringpu_pipeline_cache_lock(core);
    if (!core->pipeline_cache[cache_index].occupied ||
        core->pipeline_cache[cache_index].backend_cookie != backend_cookie ||
        core->pipeline_cache[cache_index].reference_count == 0u) {
        ringpu_pipeline_cache_unlock(core);
        return RIN_GPU_ERROR_STATE;
    }
    core->pipeline_cache[cache_index].reference_count--;
    if (core->pipeline_cache[cache_index].reference_count != 0u) {
        ringpu_pipeline_cache_unlock(core);
        return RIN_GPU_OK;
    }
    ringpu_pipeline_cache_touch_locked(
        core, &core->pipeline_cache[cache_index]);
    ringpu_pipeline_cache_unlock(core);
    return RIN_GPU_OK;
}

void ringpu_pipeline_cache_evict_shader(RinGpuCore* core,
                                       uint64_t shader_cookie)
{
    RinGpuPipelineCacheEntry retired[RIN_GPU_CORE_MAX_PIPELINE_CACHE];
    uint32_t retired_count = 0u;
    if (!core || shader_cookie == 0u) return;
    ringpu_pipeline_cache_lock(core);
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_PIPELINE_CACHE;
         ++index) {
        RinGpuPipelineCacheEntry* entry = &core->pipeline_cache[index];
        if (!entry->occupied || entry->reference_count != 0u ||
            (entry->shader_cookie_a != shader_cookie &&
             entry->shader_cookie_b != shader_cookie))
            continue;
        retired[retired_count++] = *entry;
        memset(entry, 0, sizeof(*entry));
    }
    ringpu_pipeline_cache_unlock(core);
    for (uint32_t index = 0u; index < retired_count; ++index) {
        free(retired[index].descriptor_snapshot);
        ringpu_pipeline_cache_destroy_backend(
            core, retired[index].pipeline_kind,
            retired[index].backend_cookie);
    }
}

void ringpu_pipeline_cache_clear(RinGpuCore* core)
{
    RinGpuPipelineCacheEntry retired[RIN_GPU_CORE_MAX_PIPELINE_CACHE];
    uint32_t retired_count = 0u;
    if (!core) return;
    ringpu_pipeline_cache_lock(core);
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_PIPELINE_CACHE;
         ++index) {
        RinGpuPipelineCacheEntry* entry = &core->pipeline_cache[index];
        if (!entry->occupied) continue;
        retired[retired_count++] = *entry;
        memset(entry, 0, sizeof(*entry));
    }
    ringpu_pipeline_cache_unlock(core);
    for (uint32_t index = 0u; index < retired_count; ++index) {
        free(retired[index].descriptor_snapshot);
        ringpu_pipeline_cache_destroy_backend(
            core, retired[index].pipeline_kind,
            retired[index].backend_cookie);
    }
}
