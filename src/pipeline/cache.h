// SPDX-License-Identifier: MIT
#ifndef RINGPU_PIPELINE_CACHE_H
#define RINGPU_PIPELINE_CACHE_H

#include "../core/core.h"

/* Cache acquisition increments the entry's live-pipeline reference count. */
int ringpu_pipeline_cache_acquire(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size, uint64_t* backend_cookie_out,
    uint32_t* cache_index_out);

/* Returns 0 when this bounded optimization cannot retain the realization,
 * 1 when a new entry is published, and 2 when an equivalent live entry was
 * reused. On return 2, the caller destroys created_backend_cookie. */
int ringpu_pipeline_cache_publish(
    RinGpuCore* core, uint32_t pipeline_kind, uint64_t shader_cookie_a,
    uint64_t shader_cookie_b, const void* descriptor,
    uint32_t descriptor_size, uint64_t created_backend_cookie,
    uint64_t* backend_cookie_out, uint32_t* cache_index_out);

int ringpu_pipeline_cache_release(RinGpuCore* core, uint32_t cache_index,
                                  uint64_t backend_cookie);
void ringpu_pipeline_cache_evict_shader(RinGpuCore* core,
                                       uint64_t shader_cookie);
void ringpu_pipeline_cache_clear(RinGpuCore* core);

#endif
