/* SPDX-License-Identifier: MIT */
#include "memory_runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RIN_GPU_MEMORY_RUNTIME_MAGIC UINT64_C(0x52474d454d563031)
#define RIN_GPU_MEMORY_ALLOCATION_TAG UINT32_C(0x4d41)
#define RIN_GPU_MEMORY_LEASE_TAG UINT32_C(0x4d4c)

#define RIN_GPU_MEMORY_SLOT_FREE 0u
#define RIN_GPU_MEMORY_SLOT_ACTIVE 1u
#define RIN_GPU_MEMORY_SLOT_PREPARING 2u
#define RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP 3u
#define RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY 4u
#define RIN_GPU_MEMORY_SLOT_RETIRED 5u

#define RIN_GPU_MEMORY_LEASE_FREE 0u
#define RIN_GPU_MEMORY_LEASE_ACTIVE 1u
#define RIN_GPU_MEMORY_LEASE_RETIRED 2u

typedef struct RinGpuMemoryAllocationSlot {
    uint32_t state;
    uint32_t generation;
    uint32_t heap;
    uint32_t flags;
    uint32_t lease_count;
    uint32_t cpu_upload_pending;
    uint64_t requested_size;
    uint64_t allocation_size;
    uint64_t alignment;
    uint64_t gpu_virtual_address;
    uint64_t heap_offset;
    uint64_t backing_cookie;
    uint64_t mapping_cookie;
    uint64_t last_used;
} RinGpuMemoryAllocationSlot;

typedef struct RinGpuMemoryLeaseSlot {
    uint32_t state;
    uint32_t generation;
    uint32_t allocation_index;
    uint32_t allocation_generation;
} RinGpuMemoryLeaseSlot;

typedef struct RinGpuMemoryRuntimeState {
    uint64_t magic;
    uint32_t flags;
    uint32_t active_allocation_count;
    uint32_t active_lease_count;
    uint32_t reserved0;
    RinGpuMemoryBackendV1 backend;
    RinGpuMemoryBackendV1 admitted_backend;
    RinGpuMemoryCpuUploadFn cpu_upload;
    uint64_t iommu_map_generation;
    uint64_t device_epoch;
    uint64_t next_allocation_serial;
    uint64_t local_heap_used;
    uint64_t system_heap_used;
    uint64_t gpu_virtual_used;
    RinGpuMemoryAllocationSlot allocations[RIN_GPU_MEMORY_MAX_ALLOCATIONS];
    RinGpuMemoryLeaseSlot leases[RIN_GPU_MEMORY_MAX_LEASES];
    volatile uint32_t api_lock;
    uint32_t callback_active;
    uint64_t guard_hash;
} RinGpuMemoryRuntimeState;

/* Backend callbacks execute outside the runtime lock.  Their context is
 * platform-owned and may nevertheless contain an accidental pointer back to
 * this owner.  Keep the complete non-lock state so a callback cannot publish
 * a partially modified heap, mapping, lease, or backend binding. */
typedef struct RinGpuMemoryCallbackSnapshot {
    uint8_t state_prefix[offsetof(RinGpuMemoryRuntimeState, api_lock)];
} RinGpuMemoryCallbackSnapshot;

_Static_assert(sizeof(RinGpuMemoryRuntimeState) <=
                   sizeof(RinGpuMemoryRuntime),
               "RinGPU memory runtime opaque state is too small");

static int ringpu_memory_binding_valid(
    const RinGpuMemoryRuntimeState* state);

static RinGpuMemoryRuntimeState* ringpu_memory_state(
    RinGpuMemoryRuntime* runtime) {
    return (RinGpuMemoryRuntimeState*)(void*)runtime;
}

static int ringpu_memory_overlap(const void* left, size_t left_size,
                                 const void* right, size_t right_size) {
    uintptr_t left_address;
    uintptr_t right_address;

    if (!left || !right || left_size == 0u || right_size == 0u) return 0;
    left_address = (uintptr_t)left;
    right_address = (uintptr_t)right;
    if (left_address <= right_address) {
        return right_address - left_address < left_size;
    }
    return left_address - right_address < right_size;
}

static int ringpu_memory_all_zero(const uint64_t* values, size_t count) {
    size_t index;

    for (index = 0u; index < count; index++) {
        if (values[index] != 0u) return 0;
    }
    return 1;
}

static int ringpu_memory_power_of_two(uint64_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static int ringpu_memory_range(uint64_t base, uint64_t size) {
    return size != 0u && base <= UINT64_MAX - size;
}

static int ringpu_memory_align_up(uint64_t value, uint64_t alignment,
                                  uint64_t* aligned_out) {
    const uint64_t mask = alignment - 1u;

    if (!aligned_out || !ringpu_memory_power_of_two(alignment) ||
        value > UINT64_MAX - mask) {
        return 0;
    }
    *aligned_out = (value + mask) & ~mask;
    return 1;
}

static uint64_t ringpu_memory_hash(const RinGpuMemoryRuntimeState* state) {
    const uint8_t* bytes = (const uint8_t*)(const void*)state;
    const size_t size = offsetof(RinGpuMemoryRuntimeState, api_lock);
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void ringpu_memory_mark_lost(RinGpuMemoryRuntimeState* state) {
    state->flags = RIN_GPU_MEMORY_STATUS_LOST;
}

static void ringpu_memory_callback_snapshot_capture(
    const RinGpuMemoryRuntimeState* state,
    RinGpuMemoryCallbackSnapshot* snapshot) {
    memcpy(snapshot->state_prefix, state,
           sizeof(snapshot->state_prefix));
}

static int ringpu_memory_callback_snapshot_same(
    const RinGpuMemoryRuntimeState* state,
    const RinGpuMemoryCallbackSnapshot* snapshot) {
    const uint8_t* current = (const uint8_t*)state;
    const uint8_t* saved = snapshot ? snapshot->state_prefix : NULL;
    size_t index;

    if (!current || !saved) return 0;
    for (index = 0u; index < sizeof(snapshot->state_prefix); ++index)
        if (current[index] != saved[index]) return 0;
    return 1;
}

static void ringpu_memory_callback_snapshot_restore(
    RinGpuMemoryRuntimeState* state,
    const RinGpuMemoryCallbackSnapshot* snapshot) {
    if (!state || !snapshot) return;
    memcpy(state, snapshot->state_prefix, sizeof(snapshot->state_prefix));
}

static int ringpu_memory_callback_begin(
    RinGpuMemoryRuntimeState* state,
    RinGpuMemoryCallbackSnapshot* snapshot) {
    if (!state || !snapshot) return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    if (state->callback_active != 0u) {
        ringpu_memory_mark_lost(state);
        return RIN_GPU_MEMORY_PROTOCOL;
    }
    ringpu_memory_callback_snapshot_capture(state, snapshot);
    state->callback_active = 1u;
    state->guard_hash = ringpu_memory_hash(state);
    return RIN_GPU_MEMORY_OK;
}

static int ringpu_memory_callback_end(
    RinGpuMemoryRuntimeState* state,
    const RinGpuMemoryCallbackSnapshot* snapshot) {
    const int unchanged = state && snapshot &&
                          state->guard_hash == ringpu_memory_hash(state) &&
                          ringpu_memory_callback_snapshot_same(state, snapshot);

    if (!state) return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    state->guard_hash = 0u;
    state->callback_active = 0u;
    if (!unchanged) {
        ringpu_memory_callback_snapshot_restore(state, snapshot);
        ringpu_memory_mark_lost(state);
        return RIN_GPU_MEMORY_PROTOCOL;
    }
    return RIN_GPU_MEMORY_OK;
}

static int ringpu_memory_lock(RinGpuMemoryRuntime* runtime,
                              RinGpuMemoryRuntimeState** state_out) {
    RinGpuMemoryRuntimeState* state;

    if (!runtime || !state_out) return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    state = ringpu_memory_state(runtime);
    if (state->magic != RIN_GPU_MEMORY_RUNTIME_MAGIC) {
        return RIN_GPU_MEMORY_STATE;
    }
    if (__atomic_exchange_n(&state->api_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        return RIN_GPU_MEMORY_BUSY;
    }
    if (state->magic != RIN_GPU_MEMORY_RUNTIME_MAGIC) {
        __atomic_store_n(&state->api_lock, 0u, __ATOMIC_RELEASE);
        return RIN_GPU_MEMORY_STATE;
    }
    if (!ringpu_memory_binding_valid(state)) {
        state->flags = RIN_GPU_MEMORY_STATUS_LOST;
        __atomic_store_n(&state->api_lock, 0u, __ATOMIC_RELEASE);
        return RIN_GPU_MEMORY_PROTOCOL;
    }
    *state_out = state;
    return RIN_GPU_MEMORY_OK;
}

static void ringpu_memory_unlock(RinGpuMemoryRuntimeState* state) {
    __atomic_store_n(&state->api_lock, 0u, __ATOMIC_RELEASE);
}

static int ringpu_memory_backend_valid(
    const RinGpuMemoryBackendV1* backend) {
    const uint32_t heap_caps = RIN_GPU_MEMORY_CAP_LOCAL_HEAP |
                               RIN_GPU_MEMORY_CAP_SYSTEM_HEAP;
    if (!backend) return 0;
    const int local =
        (backend->capabilities & RIN_GPU_MEMORY_CAP_LOCAL_HEAP) != 0u;
    const int system =
        (backend->capabilities & RIN_GPU_MEMORY_CAP_SYSTEM_HEAP) != 0u;
    const int sync =
        (backend->capabilities & RIN_GPU_MEMORY_CAP_CPU_SYNC) != 0u;
    const int upload =
        (backend->capabilities & RIN_GPU_MEMORY_CAP_CPU_UPLOAD) != 0u;

    if (backend->struct_size != sizeof(*backend) ||
        backend->version != RIN_GPU_MEMORY_RUNTIME_VERSION ||
        (backend->capabilities & ~RIN_GPU_MEMORY_CAP_KNOWN) != 0u ||
        (backend->capabilities & heap_caps) == 0u ||
        backend->max_allocations == 0u ||
        backend->max_allocations > RIN_GPU_MEMORY_MAX_ALLOCATIONS ||
        !ringpu_memory_power_of_two(backend->page_size) ||
        backend->page_size < RIN_GPU_MEMORY_MIN_PAGE_SIZE ||
        backend->page_size > RIN_GPU_MEMORY_MAX_PAGE_SIZE ||
        !ringpu_memory_range(backend->gpu_virtual_base,
                             backend->gpu_virtual_size) ||
        (backend->gpu_virtual_base & (backend->page_size - 1u)) != 0u ||
        (backend->gpu_virtual_size & (backend->page_size - 1u)) != 0u ||
        backend->iommu_domain_cookie == 0u ||
        backend->iommu_map_generation == 0u ||
        backend->device_epoch == 0u || backend->handle_secret == 0u ||
        !backend->context || !backend->create_backing ||
        !backend->destroy_backing || !backend->map || !backend->unmap ||
        !backend->prepare_rebind || (sync != (backend->sync != NULL)) ||
        (upload &&
         (!backend->cpu_upload_ops ||
          backend->cpu_upload_ops->struct_size !=
              sizeof(*backend->cpu_upload_ops) ||
          backend->cpu_upload_ops->version !=
              RIN_GPU_MEMORY_CPU_UPLOAD_OPS_VERSION ||
          !backend->cpu_upload_ops->upload ||
          !ringpu_memory_all_zero(backend->cpu_upload_ops->reserved, 2u))) ||
        (!upload && backend->cpu_upload_ops != NULL) ||
        !ringpu_memory_all_zero(
            backend->reserved, RIN_GPU_MEMORY_BACKEND_RESERVED_QWORDS)) {
        return 0;
    }
    if (local) {
        if (!ringpu_memory_range(backend->local_heap_base,
                                 backend->local_heap_size) ||
            (backend->local_heap_base & (backend->page_size - 1u)) != 0u ||
            (backend->local_heap_size & (backend->page_size - 1u)) != 0u) {
            return 0;
        }
    } else if (backend->local_heap_base != 0u ||
               backend->local_heap_size != 0u) {
        return 0;
    }
    if (system) {
        if (backend->system_heap_size == 0u ||
            (backend->system_heap_size & (backend->page_size - 1u)) != 0u) {
            return 0;
        }
    } else if (backend->system_heap_size != 0u) {
        return 0;
    }
    return 1;
}

static int ringpu_memory_backend_equal(
    const RinGpuMemoryBackendV1* first,
    const RinGpuMemoryBackendV1* second) {
    const uint8_t* left;
    const uint8_t* right;
    size_t index;

    if (!first || !second) return 0;
    left = (const uint8_t*)(const void*)first;
    right = (const uint8_t*)(const void*)second;
    for (index = 0u; index < sizeof(*first); index++) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static int ringpu_memory_binding_valid(
    const RinGpuMemoryRuntimeState* state) {
    return state && ringpu_memory_backend_valid(&state->backend) &&
           ringpu_memory_backend_equal(&state->backend,
                                       &state->admitted_backend) &&
           state->cpu_upload ==
               (state->admitted_backend.cpu_upload_ops
                    ? state->admitted_backend.cpu_upload_ops->upload
                    : NULL);
}

static uint64_t ringpu_memory_encode(const RinGpuMemoryRuntimeState* state,
                                     uint32_t tag, uint32_t index,
                                     uint32_t generation) {
    const uint64_t raw = ((uint64_t)generation << 32) |
                         ((uint64_t)tag << 16) | (uint64_t)(index + 1u);
    return raw ^ state->backend.handle_secret;
}

static int ringpu_memory_decode(const RinGpuMemoryRuntimeState* state,
                                uint64_t handle, uint32_t expected_tag,
                                uint32_t maximum, uint32_t* index_out,
                                uint32_t* generation_out) {
    const uint64_t raw = handle ^ state->backend.handle_secret;
    const uint32_t encoded_index = (uint32_t)(raw & UINT64_C(0xffff));
    const uint32_t tag = (uint32_t)((raw >> 16) & UINT64_C(0xffff));

    if (handle == 0u || tag != expected_tag || encoded_index == 0u ||
        encoded_index > maximum || (uint32_t)(raw >> 32) == 0u) {
        return 0;
    }
    *index_out = encoded_index - 1u;
    *generation_out = (uint32_t)(raw >> 32);
    return 1;
}

static RinGpuMemoryAllocationSlot* ringpu_memory_lookup_allocation(
    RinGpuMemoryRuntimeState* state, uint64_t handle, uint32_t* index_out) {
    uint32_t index;
    uint32_t generation;
    RinGpuMemoryAllocationSlot* slot;

    if (!ringpu_memory_decode(state, handle, RIN_GPU_MEMORY_ALLOCATION_TAG,
                              state->backend.max_allocations, &index,
                              &generation)) {
        return NULL;
    }
    slot = &state->allocations[index];
    if (slot->state != RIN_GPU_MEMORY_SLOT_ACTIVE ||
        slot->generation != generation) {
        return NULL;
    }
    if (index_out) *index_out = index;
    return slot;
}

static RinGpuMemoryLeaseSlot* ringpu_memory_lookup_lease(
    RinGpuMemoryRuntimeState* state, uint64_t handle, uint32_t* index_out) {
    uint32_t index;
    uint32_t generation;
    RinGpuMemoryLeaseSlot* slot;

    if (!ringpu_memory_decode(state, handle, RIN_GPU_MEMORY_LEASE_TAG,
                              RIN_GPU_MEMORY_MAX_LEASES, &index,
                              &generation)) {
        return NULL;
    }
    slot = &state->leases[index];
    if (slot->state != RIN_GPU_MEMORY_LEASE_ACTIVE ||
        slot->generation != generation) {
        return NULL;
    }
    if (index_out) *index_out = index;
    return slot;
}

static int ringpu_memory_slot_occupies_range(
    const RinGpuMemoryAllocationSlot* slot) {
    return slot->state == RIN_GPU_MEMORY_SLOT_ACTIVE ||
           slot->state == RIN_GPU_MEMORY_SLOT_PREPARING ||
           slot->state == RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP ||
           slot->state == RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY;
}

static int ringpu_memory_backing_cookie_unique(
    const RinGpuMemoryRuntimeState* state,
    const RinGpuMemoryAllocationSlot* candidate, uint64_t cookie) {
    uint32_t index;

    if (cookie == 0u) return 0;
    for (index = 0u; index < state->backend.max_allocations; index++) {
        const RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        if (slot != candidate &&
            ringpu_memory_slot_occupies_range(slot) &&
            slot->backing_cookie == cookie) {
            return 0;
        }
    }
    return 1;
}

static int ringpu_memory_live_mapping_cookie_unique(
    const RinGpuMemoryRuntimeState* state,
    const RinGpuMemoryAllocationSlot* candidate, uint64_t cookie) {
    uint32_t index;

    if (cookie == 0u) return 0;
    for (index = 0u; index < state->backend.max_allocations; index++) {
        const RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        if (slot != candidate && slot->mapping_cookie == cookie &&
            (slot->state == RIN_GPU_MEMORY_SLOT_ACTIVE ||
             slot->state == RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP)) {
            return 0;
        }
    }
    return 1;
}

static int ringpu_memory_find_range(
    const RinGpuMemoryRuntimeState* state, uint32_t heap, uint64_t base,
    uint64_t range_size, uint64_t size, uint64_t alignment,
    int gpu_virtual, const RinGpuMemoryAllocationSlot* ignored,
    uint64_t* address_out) {
    const uint64_t limit = base + range_size;
    uint64_t candidate;
    uint32_t attempt;

    if (!ringpu_memory_align_up(base, alignment, &candidate)) return 0;
    for (attempt = 0u; attempt <= state->backend.max_allocations; attempt++) {
        uint32_t index;
        int conflict = 0;

        if (candidate > limit || size > limit - candidate) return 0;
        for (index = 0u; index < state->backend.max_allocations; index++) {
            const RinGpuMemoryAllocationSlot* slot =
                &state->allocations[index];
            uint64_t occupied_start;
            uint64_t occupied_end;

            if (slot == ignored ||
                !ringpu_memory_slot_occupies_range(slot) ||
                (!gpu_virtual && slot->heap != heap)) {
                continue;
            }
            occupied_start = gpu_virtual ? slot->gpu_virtual_address
                                         : slot->heap_offset;
            occupied_end = occupied_start + slot->allocation_size;
            if (candidate < occupied_end &&
                occupied_start < candidate + size) {
                if (!ringpu_memory_align_up(occupied_end, alignment,
                                            &candidate)) {
                    return 0;
                }
                conflict = 1;
                break;
            }
        }
        if (!conflict) {
            *address_out = candidate;
            return 1;
        }
    }
    return 0;
}

static void ringpu_memory_release_allocation_slot(
    RinGpuMemoryAllocationSlot* slot) {
    const uint32_t generation = slot->generation;

    memset(slot, 0, sizeof(*slot));
    if (generation == UINT32_MAX) {
        slot->state = RIN_GPU_MEMORY_SLOT_RETIRED;
        slot->generation = UINT32_MAX;
    } else {
        slot->generation = generation + 1u;
    }
}

static void ringpu_memory_release_lease_slot(RinGpuMemoryLeaseSlot* slot) {
    const uint32_t generation = slot->generation;

    memset(slot, 0, sizeof(*slot));
    if (generation == UINT32_MAX) {
        slot->state = RIN_GPU_MEMORY_LEASE_RETIRED;
        slot->generation = UINT32_MAX;
    } else {
        slot->generation = generation + 1u;
    }
}

static RinGpuMemoryAllocationSlot* ringpu_memory_prepare_allocation_slot(
    RinGpuMemoryRuntimeState* state, uint32_t* index_out,
    uint64_t* handle_out) {
    uint32_t index;

    for (index = 0u; index < state->backend.max_allocations; index++) {
        RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        uint64_t handle;

        if (slot->state != RIN_GPU_MEMORY_SLOT_FREE) continue;
        if (slot->generation == 0u) slot->generation = 1u;
        handle = ringpu_memory_encode(
            state, RIN_GPU_MEMORY_ALLOCATION_TAG, index, slot->generation);
        if (handle == 0u) {
            if (slot->generation == UINT32_MAX) {
                slot->state = RIN_GPU_MEMORY_SLOT_RETIRED;
                continue;
            }
            slot->generation++;
            handle = ringpu_memory_encode(
                state, RIN_GPU_MEMORY_ALLOCATION_TAG, index,
                slot->generation);
        }
        slot->state = RIN_GPU_MEMORY_SLOT_PREPARING;
        *index_out = index;
        *handle_out = handle;
        return slot;
    }
    return NULL;
}

static RinGpuMemoryLeaseSlot* ringpu_memory_prepare_lease_slot(
    RinGpuMemoryRuntimeState* state, uint32_t* index_out,
    uint64_t* handle_out) {
    uint32_t index;

    for (index = 0u; index < RIN_GPU_MEMORY_MAX_LEASES; index++) {
        RinGpuMemoryLeaseSlot* slot = &state->leases[index];
        uint64_t handle;

        if (slot->state != RIN_GPU_MEMORY_LEASE_FREE) continue;
        if (slot->generation == 0u) slot->generation = 1u;
        handle = ringpu_memory_encode(
            state, RIN_GPU_MEMORY_LEASE_TAG, index, slot->generation);
        if (handle == 0u) {
            if (slot->generation == UINT32_MAX) {
                slot->state = RIN_GPU_MEMORY_LEASE_RETIRED;
                continue;
            }
            slot->generation++;
            handle = ringpu_memory_encode(
                state, RIN_GPU_MEMORY_LEASE_TAG, index, slot->generation);
        }
        *index_out = index;
        *handle_out = handle;
        return slot;
    }
    return NULL;
}

static void ringpu_memory_account_backing(
    RinGpuMemoryRuntimeState* state, RinGpuMemoryAllocationSlot* slot) {
    if (slot->heap == RIN_GPU_MEMORY_HEAP_LOCAL) {
        state->local_heap_used += slot->allocation_size;
    } else {
        state->system_heap_used += slot->allocation_size;
    }
    state->gpu_virtual_used += slot->allocation_size;
}

static void ringpu_memory_unaccount_backing(
    RinGpuMemoryRuntimeState* state, RinGpuMemoryAllocationSlot* slot) {
    if (slot->heap == RIN_GPU_MEMORY_HEAP_LOCAL) {
        state->local_heap_used -= slot->allocation_size;
    } else {
        state->system_heap_used -= slot->allocation_size;
    }
    state->gpu_virtual_used -= slot->allocation_size;
}

static int ringpu_memory_call_destroy(
    RinGpuMemoryRuntimeState* state, uint64_t backing_cookie) {
    RinGpuMemoryCallbackSnapshot snapshot;
    int backend_result;
    int result = ringpu_memory_callback_begin(state, &snapshot);

    if (result != RIN_GPU_MEMORY_OK) return result;
    backend_result = state->backend.destroy_backing(
        state->backend.context, backing_cookie);
    result = ringpu_memory_callback_end(state, &snapshot);
    memset(&snapshot, 0, sizeof(snapshot));
    if (result != RIN_GPU_MEMORY_OK) return result;
    return backend_result == 0 ? RIN_GPU_MEMORY_OK
                               : RIN_GPU_MEMORY_BACKEND_FAILED;
}

static int ringpu_memory_call_unmap(
    RinGpuMemoryRuntimeState* state, uint64_t generation,
    uint64_t mapping_cookie) {
    RinGpuMemoryCallbackSnapshot snapshot;
    int backend_result;
    int result = ringpu_memory_callback_begin(state, &snapshot);

    if (result != RIN_GPU_MEMORY_OK) return result;
    backend_result = state->backend.unmap(
        state->backend.context, state->backend.iommu_domain_cookie,
        generation, mapping_cookie);
    result = ringpu_memory_callback_end(state, &snapshot);
    memset(&snapshot, 0, sizeof(snapshot));
    if (result != RIN_GPU_MEMORY_OK) return result;
    return backend_result == 0 ? RIN_GPU_MEMORY_OK
                               : RIN_GPU_MEMORY_BACKEND_FAILED;
}

static int ringpu_memory_cleanup_slot(RinGpuMemoryRuntimeState* state,
                                       RinGpuMemoryAllocationSlot* slot) {
    int result;

    if (slot->state == RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP) {
        result = ringpu_memory_call_unmap(
            state, state->iommu_map_generation, slot->mapping_cookie);
        if (result != RIN_GPU_MEMORY_OK) {
            ringpu_memory_mark_lost(state);
            return result;
        }
        slot->mapping_cookie = 0u;
        slot->state = RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY;
    }
    if (slot->state != RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY ||
        slot->backing_cookie == 0u) {
        return RIN_GPU_MEMORY_PROTOCOL;
    }
    result = ringpu_memory_call_destroy(state, slot->backing_cookie);
    if (result != RIN_GPU_MEMORY_OK) return result;
    ringpu_memory_unaccount_backing(state, slot);
    ringpu_memory_release_allocation_slot(slot);
    return RIN_GPU_MEMORY_OK;
}

static int ringpu_memory_space_available(
    const RinGpuMemoryRuntimeState* state,
    const RinGpuMemoryAllocationDescV1* desc, uint64_t allocation_size,
    uint64_t alignment) {
    if (!state || !desc || allocation_size == 0u || alignment == 0u)
        return 0;
    if (desc->heap == RIN_GPU_MEMORY_HEAP_SYSTEM &&
        (state->system_heap_used > state->backend.system_heap_size ||
         allocation_size > state->backend.system_heap_size -
                                state->system_heap_used)) {
        return 0;
    }
    if (desc->heap == RIN_GPU_MEMORY_HEAP_LOCAL &&
        !ringpu_memory_find_range(
            state, desc->heap, state->backend.local_heap_base,
            state->backend.local_heap_size, allocation_size, alignment, 0,
            NULL, &(uint64_t){0u})) {
        return 0;
    }
    if (!ringpu_memory_find_range(
            state, desc->heap, state->backend.gpu_virtual_base,
            state->backend.gpu_virtual_size, allocation_size, alignment, 1,
            NULL, &(uint64_t){0u})) {
        return 0;
    }
    return 1;
}

static int ringpu_memory_evict_one(RinGpuMemoryRuntimeState* state,
                                   uint32_t preferred_heap) {
    RinGpuMemoryAllocationSlot* candidate = NULL;
    uint32_t index;

    if (!state) return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    for (index = 0u; index < state->backend.max_allocations; ++index) {
        RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        const int preferred = slot->heap == preferred_heap;

        /* CPU-visible allocations are pinned: callers may still hold their
         * backing for upload/readback even when no GPU lease is active. */
        if (slot->state != RIN_GPU_MEMORY_SLOT_ACTIVE ||
            slot->lease_count != 0u ||
            (slot->flags & RIN_GPU_MEMORY_CPU_VISIBLE) != 0u) {
            continue;
        }
        if (!candidate ||
            (preferred && candidate->heap != preferred_heap) ||
            (preferred == (candidate->heap == preferred_heap) &&
             slot->last_used < candidate->last_used)) {
            candidate = slot;
        }
    }
    if (!candidate) return RIN_GPU_MEMORY_NO_SPACE;
    if (state->active_allocation_count == 0u) {
        ringpu_memory_mark_lost(state);
        return RIN_GPU_MEMORY_PROTOCOL;
    }
    state->active_allocation_count--;
    candidate->state = candidate->mapping_cookie == 0u
                          ? RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY
                          : RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP;
    return ringpu_memory_cleanup_slot(state, candidate);
}

static int ringpu_memory_mapping_cookie_unique(
    const uint64_t* cookies, uint32_t count, uint64_t candidate) {
    uint32_t index;

    if (candidate == 0u) return 0;
    for (index = 0u; index < count; index++) {
        if (cookies[index] == candidate) return 0;
    }
    return 1;
}

static int ringpu_memory_map_all(RinGpuMemoryRuntimeState* state) {
    RinGpuMemoryCallbackSnapshot callback_snapshot;
    uint64_t cookies[RIN_GPU_MEMORY_MAX_ALLOCATIONS];
    uint32_t mapped_indices[RIN_GPU_MEMORY_MAX_ALLOCATIONS];
    uint32_t mapped_count = 0u;
    uint32_t index;
    int failure = RIN_GPU_MEMORY_BACKEND_FAILED;

    memset(cookies, 0, sizeof(cookies));
    memset(mapped_indices, 0, sizeof(mapped_indices));
    for (index = 0u; index < state->backend.max_allocations; index++) {
        RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        uint64_t mapping_cookie = 0u;
        int backend_result;
        int result;

        if (slot->state != RIN_GPU_MEMORY_SLOT_ACTIVE) continue;
        result = ringpu_memory_callback_begin(state, &callback_snapshot);
        if (result != RIN_GPU_MEMORY_OK) return result;
        backend_result = state->backend.map(
            state->backend.context, state->backend.iommu_domain_cookie,
            state->iommu_map_generation, slot->backing_cookie,
            slot->gpu_virtual_address, slot->allocation_size, slot->flags,
            &mapping_cookie);
        result = ringpu_memory_callback_end(state, &callback_snapshot);
        memset(&callback_snapshot, 0, sizeof(callback_snapshot));
        if (result != RIN_GPU_MEMORY_OK) return result;
        if (backend_result == 0 &&
            ringpu_memory_mapping_cookie_unique(cookies, mapped_count,
                                                mapping_cookie)) {
            cookies[mapped_count] = mapping_cookie;
            mapped_indices[mapped_count] = index;
            mapped_count++;
            continue;
        }
        if (mapping_cookie != 0u &&
            !ringpu_memory_mapping_cookie_unique(cookies, mapped_count,
                                                 mapping_cookie)) {
            ringpu_memory_mark_lost(state);
            return RIN_GPU_MEMORY_PROTOCOL;
        }
        failure = backend_result == 0 || mapping_cookie != 0u
                      ? RIN_GPU_MEMORY_PROTOCOL
                      : RIN_GPU_MEMORY_BACKEND_FAILED;
        if (mapping_cookie != 0u) {
            result = ringpu_memory_call_unmap(
                state, state->iommu_map_generation, mapping_cookie);
            if (result != RIN_GPU_MEMORY_OK) {
                ringpu_memory_mark_lost(state);
                return result;
            }
        }
        break;
    }
    if (index != state->backend.max_allocations) {
        while (mapped_count != 0u) {
            int result;
            mapped_count--;
            result = ringpu_memory_call_unmap(
                state, state->iommu_map_generation, cookies[mapped_count]);
            if (result != RIN_GPU_MEMORY_OK) {
                ringpu_memory_mark_lost(state);
                return result;
            }
        }
        return failure;
    }
    for (index = 0u; index < mapped_count; index++) {
        state->allocations[mapped_indices[index]].mapping_cookie =
            cookies[index];
    }
    state->flags = RIN_GPU_MEMORY_STATUS_READY;
    return RIN_GPU_MEMORY_OK;
}

static void ringpu_memory_clear_all_leases(
    RinGpuMemoryRuntimeState* state) {
    uint32_t index;

    for (index = 0u; index < state->backend.max_allocations; index++) {
        state->allocations[index].lease_count = 0u;
    }
    for (index = 0u; index < RIN_GPU_MEMORY_MAX_LEASES; index++) {
        if (state->leases[index].state == RIN_GPU_MEMORY_LEASE_ACTIVE) {
            ringpu_memory_release_lease_slot(&state->leases[index]);
        }
    }
    state->active_lease_count = 0u;
}

int rin_gpu_memory_runtime_init(RinGpuMemoryRuntime* runtime,
                                const RinGpuMemoryBackendV1* backend) {
    RinGpuMemoryBackendV1 backend_copy;
    RinGpuMemoryRuntimeState* state;

    if (!runtime || !backend ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), backend,
                              sizeof(*backend))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    memcpy(&backend_copy, backend, sizeof(backend_copy));
    if (!ringpu_memory_backend_valid(&backend_copy) ||
        ((uintptr_t)backend_copy.context >= (uintptr_t)runtime &&
         (uintptr_t)backend_copy.context - (uintptr_t)runtime <
             sizeof(*runtime)) ||
        (backend_copy.cpu_upload_ops &&
         ringpu_memory_overlap(runtime, sizeof(*runtime),
                               backend_copy.cpu_upload_ops,
                               sizeof(*backend_copy.cpu_upload_ops)))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    state = ringpu_memory_state(runtime);
    if (state->magic == RIN_GPU_MEMORY_RUNTIME_MAGIC) {
        return RIN_GPU_MEMORY_STATE;
    }
    memset(runtime, 0, sizeof(*runtime));
    state->magic = RIN_GPU_MEMORY_RUNTIME_MAGIC;
    state->flags = RIN_GPU_MEMORY_STATUS_READY;
    state->backend = backend_copy;
    state->admitted_backend = backend_copy;
    if (backend_copy.cpu_upload_ops) {
        state->cpu_upload = backend_copy.cpu_upload_ops->upload;
    }
    state->iommu_map_generation = backend_copy.iommu_map_generation;
    state->device_epoch = backend_copy.device_epoch;
    state->next_allocation_serial = 1u;
    return RIN_GPU_MEMORY_OK;
}

int rin_gpu_memory_allocate(RinGpuMemoryRuntime* runtime,
                            const RinGpuMemoryAllocationDescV1* descriptor,
                            uint64_t* allocation_handle_out) {
    RinGpuMemoryCallbackSnapshot callback_snapshot;
    RinGpuMemoryAllocationDescV1 desc;
    RinGpuMemoryAllocationSlot* slot;
    RinGpuMemoryRuntimeState* state;
    uint64_t allocation_size;
    uint64_t alignment;
    uint64_t handle;
    uint64_t backing_cookie = 0u;
    uint64_t mapping_cookie = 0u;
    uint32_t slot_index;
    int backend_result;
    int result;

    if (!runtime || !descriptor || !allocation_handle_out ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), descriptor,
                              sizeof(*descriptor)) ||
        ringpu_memory_overlap(runtime, sizeof(*runtime),
                              allocation_handle_out,
                              sizeof(*allocation_handle_out))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    memcpy(&desc, descriptor, sizeof(desc));
    *allocation_handle_out = 0u;
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_MEMORY_STATUS_READY) {
        result = RIN_GPU_MEMORY_STATE;
        goto done;
    }
    if (desc.struct_size != sizeof(desc) ||
        desc.version != RIN_GPU_MEMORY_RUNTIME_VERSION ||
        (desc.heap != RIN_GPU_MEMORY_HEAP_LOCAL &&
         desc.heap != RIN_GPU_MEMORY_HEAP_SYSTEM) ||
        (desc.flags & ~RIN_GPU_MEMORY_FLAG_KNOWN) != 0u ||
        (desc.flags & (RIN_GPU_MEMORY_GPU_READ |
                       RIN_GPU_MEMORY_GPU_WRITE)) == 0u ||
        desc.size_bytes == 0u ||
        !ringpu_memory_all_zero(desc.reserved, 4u) ||
        (desc.heap == RIN_GPU_MEMORY_HEAP_LOCAL &&
         (state->backend.capabilities & RIN_GPU_MEMORY_CAP_LOCAL_HEAP) ==
             0u) ||
        (desc.heap == RIN_GPU_MEMORY_HEAP_SYSTEM &&
         (state->backend.capabilities & RIN_GPU_MEMORY_CAP_SYSTEM_HEAP) ==
             0u) ||
        ((desc.flags & RIN_GPU_MEMORY_CPU_VISIBLE) != 0u &&
         (state->backend.capabilities & RIN_GPU_MEMORY_CAP_CPU_UPLOAD) ==
             0u)) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    alignment = desc.alignment == 0u ? state->backend.page_size
                                     : desc.alignment;
    if (!ringpu_memory_power_of_two(alignment) ||
        alignment < state->backend.page_size ||
        alignment > RIN_GPU_MEMORY_MAX_ALIGNMENT ||
        !ringpu_memory_align_up(desc.size_bytes, state->backend.page_size,
                                &allocation_size) ||
        allocation_size == 0u) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    /* An eviction cannot make an allocation larger than the admitted heap or
     * GPU virtual address range. Reject that impossible request before
     * touching an existing idle allocation. */
    if ((desc.heap == RIN_GPU_MEMORY_HEAP_LOCAL &&
         allocation_size > state->backend.local_heap_size) ||
        (desc.heap == RIN_GPU_MEMORY_HEAP_SYSTEM &&
         allocation_size > state->backend.system_heap_size) ||
        allocation_size > state->backend.gpu_virtual_size) {
        result = RIN_GPU_MEMORY_NO_SPACE;
        goto done;
    }
    for (;;) {
        const int space = ringpu_memory_space_available(
            state, &desc, allocation_size, alignment);

        if (space && state->next_allocation_serial != UINT64_MAX) {
            slot = ringpu_memory_prepare_allocation_slot(
                state, &slot_index, &handle);
            if (slot) break;
        }
        result = ringpu_memory_evict_one(state, desc.heap);
        if (result != RIN_GPU_MEMORY_OK) {
            if (result == RIN_GPU_MEMORY_NO_SPACE && space)
                result = RIN_GPU_MEMORY_LIMIT;
            goto done;
        }
    }
    slot->heap = desc.heap;
    slot->flags = desc.flags;
    slot->requested_size = desc.size_bytes;
    slot->allocation_size = allocation_size;
    slot->alignment = alignment;
    slot->last_used = state->next_allocation_serial++;
    if (desc.heap == RIN_GPU_MEMORY_HEAP_LOCAL &&
        !ringpu_memory_find_range(
            state, desc.heap, state->backend.local_heap_base,
            state->backend.local_heap_size, allocation_size, alignment, 0,
            slot, &slot->heap_offset)) {
        ringpu_memory_release_allocation_slot(slot);
        result = RIN_GPU_MEMORY_NO_SPACE;
        goto done;
    }
    if (!ringpu_memory_find_range(
            state, desc.heap, state->backend.gpu_virtual_base,
            state->backend.gpu_virtual_size, allocation_size, alignment, 1,
            slot, &slot->gpu_virtual_address)) {
        ringpu_memory_release_allocation_slot(slot);
        result = RIN_GPU_MEMORY_NO_SPACE;
        goto done;
    }

    result = ringpu_memory_callback_begin(state, &callback_snapshot);
    if (result != RIN_GPU_MEMORY_OK) goto done;
    backend_result = state->backend.create_backing(
        state->backend.context, slot->heap, slot->flags, slot->heap_offset,
        slot->allocation_size, slot->alignment, &backing_cookie);
    result = ringpu_memory_callback_end(state, &callback_snapshot);
    memset(&callback_snapshot, 0, sizeof(callback_snapshot));
    if (result != RIN_GPU_MEMORY_OK) goto done;
    if (backend_result == 0 && backing_cookie != 0u &&
        !ringpu_memory_backing_cookie_unique(state, slot, backing_cookie)) {
        slot->backing_cookie = backing_cookie;
        ringpu_memory_account_backing(state, slot);
        ringpu_memory_mark_lost(state);
        result = RIN_GPU_MEMORY_PROTOCOL;
        goto done;
    }
    if (backend_result != 0 || backing_cookie == 0u) {
        result = backend_result == 0 || backing_cookie != 0u
                     ? RIN_GPU_MEMORY_PROTOCOL
                     : RIN_GPU_MEMORY_BACKEND_FAILED;
        if (backing_cookie == 0u) {
            ringpu_memory_release_allocation_slot(slot);
            goto done;
        }
        slot->backing_cookie = backing_cookie;
        slot->state = RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY;
        ringpu_memory_account_backing(state, slot);
        if (ringpu_memory_cleanup_slot(state, slot) != RIN_GPU_MEMORY_OK &&
            state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
            result = RIN_GPU_MEMORY_LOST;
        }
        goto done;
    }
    slot->backing_cookie = backing_cookie;
    ringpu_memory_account_backing(state, slot);

    result = ringpu_memory_callback_begin(state, &callback_snapshot);
    if (result != RIN_GPU_MEMORY_OK) goto done;
    backend_result = state->backend.map(
        state->backend.context, state->backend.iommu_domain_cookie,
        state->iommu_map_generation, slot->backing_cookie,
        slot->gpu_virtual_address, slot->allocation_size, slot->flags,
        &mapping_cookie);
    result = ringpu_memory_callback_end(state, &callback_snapshot);
    memset(&callback_snapshot, 0, sizeof(callback_snapshot));
    if (result != RIN_GPU_MEMORY_OK) goto done;
    if (backend_result == 0 && mapping_cookie != 0u &&
        !ringpu_memory_live_mapping_cookie_unique(
            state, slot, mapping_cookie)) {
        slot->mapping_cookie = mapping_cookie;
        ringpu_memory_mark_lost(state);
        result = RIN_GPU_MEMORY_PROTOCOL;
        goto done;
    }
    if (backend_result != 0 || mapping_cookie == 0u) {
        result = backend_result == 0 || mapping_cookie != 0u
                     ? RIN_GPU_MEMORY_PROTOCOL
                     : RIN_GPU_MEMORY_BACKEND_FAILED;
        slot->mapping_cookie = mapping_cookie;
        slot->state = mapping_cookie == 0u
                          ? RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY
                          : RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP;
        if (ringpu_memory_cleanup_slot(state, slot) != RIN_GPU_MEMORY_OK &&
            state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
            result = RIN_GPU_MEMORY_LOST;
        }
        goto done;
    }
    slot->mapping_cookie = mapping_cookie;
    slot->state = RIN_GPU_MEMORY_SLOT_ACTIVE;
    state->active_allocation_count++;
    *allocation_handle_out = handle;
    result = RIN_GPU_MEMORY_OK;
    (void)slot_index;

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_query(RinGpuMemoryRuntime* runtime,
                         uint64_t allocation_handle,
                         RinGpuMemoryAllocationInfoV1* info) {
    RinGpuMemoryRuntimeState* state;
    RinGpuMemoryAllocationSlot* slot;
    int result;

    if (!runtime || !info ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), info,
                              sizeof(*info))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    memset(info, 0, sizeof(*info));
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    slot = ringpu_memory_lookup_allocation(state, allocation_handle, NULL);
    if (!slot) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    info->struct_size = sizeof(*info);
    info->version = RIN_GPU_MEMORY_RUNTIME_VERSION;
    info->heap = slot->heap;
    info->flags = slot->flags;
    info->allocation_handle = allocation_handle;
    info->gpu_virtual_address = slot->gpu_virtual_address;
    info->heap_offset = slot->heap_offset;
    info->requested_size_bytes = slot->requested_size;
    info->allocation_size_bytes = slot->allocation_size;
    info->alignment = slot->alignment;
    info->iommu_map_generation = state->iommu_map_generation;
    info->device_epoch = state->device_epoch;
    info->lease_count = slot->lease_count;
    info->state = RIN_GPU_MEMORY_ALLOCATION_ACTIVE;
    result = RIN_GPU_MEMORY_OK;

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_acquire(RinGpuMemoryRuntime* runtime,
                           uint64_t allocation_handle,
                           uint32_t required_gpu_access,
                           uint64_t* lease_handle_out) {
    RinGpuMemoryRuntimeState* state;
    RinGpuMemoryAllocationSlot* allocation;
    RinGpuMemoryLeaseSlot* lease;
    uint64_t lease_handle;
    uint32_t allocation_index;
    uint32_t lease_index;
    int result;

    if (!runtime || !lease_handle_out ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), lease_handle_out,
                              sizeof(*lease_handle_out))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    *lease_handle_out = 0u;
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_MEMORY_STATUS_READY) {
        result = RIN_GPU_MEMORY_STATE;
        goto done;
    }
    if ((required_gpu_access & ~(RIN_GPU_MEMORY_GPU_READ |
                                 RIN_GPU_MEMORY_GPU_WRITE)) != 0u ||
        required_gpu_access == 0u) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    allocation = ringpu_memory_lookup_allocation(
        state, allocation_handle, &allocation_index);
    if (!allocation) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    if ((allocation->flags & required_gpu_access) != required_gpu_access) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    if (allocation->cpu_upload_pending != 0u) {
        result = RIN_GPU_MEMORY_STATE;
        goto done;
    }
    if (allocation->lease_count == UINT32_MAX) {
        result = RIN_GPU_MEMORY_LIMIT;
        goto done;
    }
    lease = ringpu_memory_prepare_lease_slot(
        state, &lease_index, &lease_handle);
    if (!lease) {
        result = RIN_GPU_MEMORY_LIMIT;
        goto done;
    }
    lease->state = RIN_GPU_MEMORY_LEASE_ACTIVE;
    lease->allocation_index = allocation_index;
    lease->allocation_generation = allocation->generation;
    allocation->lease_count++;
    state->active_lease_count++;
    if (state->next_allocation_serial != UINT64_MAX)
        allocation->last_used = state->next_allocation_serial++;
    *lease_handle_out = lease_handle;
    result = RIN_GPU_MEMORY_OK;
    (void)lease_index;

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_release(RinGpuMemoryRuntime* runtime,
                           uint64_t lease_handle) {
    RinGpuMemoryRuntimeState* state;
    RinGpuMemoryLeaseSlot* lease;
    RinGpuMemoryAllocationSlot* allocation;
    int result;

    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    lease = ringpu_memory_lookup_lease(state, lease_handle, NULL);
    if (!lease) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    allocation = &state->allocations[lease->allocation_index];
    if (allocation->state != RIN_GPU_MEMORY_SLOT_ACTIVE ||
        allocation->generation != lease->allocation_generation ||
        allocation->lease_count == 0u ||
        state->active_lease_count == 0u) {
        ringpu_memory_mark_lost(state);
        result = RIN_GPU_MEMORY_PROTOCOL;
        goto done;
    }
    allocation->lease_count--;
    state->active_lease_count--;
    ringpu_memory_release_lease_slot(lease);
    result = RIN_GPU_MEMORY_OK;

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_sync(RinGpuMemoryRuntime* runtime,
                        uint64_t allocation_handle, uint32_t action,
                        uint64_t offset, uint64_t length) {
    RinGpuMemoryCallbackSnapshot callback_snapshot;
    RinGpuMemoryRuntimeState* state;
    RinGpuMemoryAllocationSlot* slot;
    int backend_result;
    int result;

    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_MEMORY_STATUS_READY) {
        result = RIN_GPU_MEMORY_STATE;
        goto done;
    }
    slot = ringpu_memory_lookup_allocation(state, allocation_handle, NULL);
    if (!slot) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    if ((slot->flags & RIN_GPU_MEMORY_CPU_VISIBLE) == 0u ||
        (state->backend.capabilities & RIN_GPU_MEMORY_CAP_CPU_SYNC) == 0u ||
        (action != RIN_GPU_MEMORY_SYNC_CPU_TO_DEVICE &&
         action != RIN_GPU_MEMORY_SYNC_DEVICE_TO_CPU) ||
        length == 0u || offset > slot->requested_size ||
        length > slot->requested_size - offset) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    result = ringpu_memory_callback_begin(state, &callback_snapshot);
    if (result != RIN_GPU_MEMORY_OK) goto done;
    backend_result = state->backend.sync(
        state->backend.context, slot->backing_cookie, action, offset, length);
    result = ringpu_memory_callback_end(state, &callback_snapshot);
    memset(&callback_snapshot, 0, sizeof(callback_snapshot));
    if (result != RIN_GPU_MEMORY_OK) goto done;
    if (backend_result != 0) {
        ringpu_memory_mark_lost(state);
        result = RIN_GPU_MEMORY_BACKEND_FAILED;
        goto done;
    }
    result = RIN_GPU_MEMORY_OK;

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_upload(RinGpuMemoryRuntime* runtime,
                          uint64_t allocation_handle, uint64_t offset,
                          const void* source, uint64_t length) {
    RinGpuMemoryCallbackSnapshot callback_snapshot;
    RinGpuMemoryRuntimeState* state;
    RinGpuMemoryAllocationSlot* slot;
    int backend_result;
    int result;

    if (!runtime || !source || length == 0u || length > (uint64_t)SIZE_MAX ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), source,
                              (size_t)length)) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_MEMORY_STATUS_READY) {
        result = RIN_GPU_MEMORY_STATE;
        goto done;
    }
    slot = ringpu_memory_lookup_allocation(state, allocation_handle, NULL);
    if (!slot) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    if ((slot->flags & RIN_GPU_MEMORY_CPU_VISIBLE) == 0u ||
        !state->cpu_upload ||
        offset > slot->requested_size ||
        length > slot->requested_size - offset) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    if (slot->lease_count != 0u) {
        result = RIN_GPU_MEMORY_BUSY;
        goto done;
    }
    result = ringpu_memory_callback_begin(state, &callback_snapshot);
    if (result != RIN_GPU_MEMORY_OK) goto done;
    backend_result = state->cpu_upload(
        state->backend.context, slot->backing_cookie, offset, source, length);
    result = ringpu_memory_callback_end(state, &callback_snapshot);
    memset(&callback_snapshot, 0, sizeof(callback_snapshot));
    if (result != RIN_GPU_MEMORY_OK) goto done;
    if (backend_result != 0) {
        slot->cpu_upload_pending = 1u;
        result = RIN_GPU_MEMORY_BACKEND_FAILED;
        goto done;
    }
    if ((state->backend.capabilities & RIN_GPU_MEMORY_CAP_CPU_SYNC) != 0u) {
        result = ringpu_memory_callback_begin(state, &callback_snapshot);
        if (result != RIN_GPU_MEMORY_OK) goto done;
        backend_result = state->backend.sync(
            state->backend.context, slot->backing_cookie,
            RIN_GPU_MEMORY_SYNC_CPU_TO_DEVICE, offset, length);
        result = ringpu_memory_callback_end(state, &callback_snapshot);
        memset(&callback_snapshot, 0, sizeof(callback_snapshot));
        if (result != RIN_GPU_MEMORY_OK) goto done;
        if (backend_result != 0) {
            ringpu_memory_mark_lost(state);
            result = RIN_GPU_MEMORY_BACKEND_FAILED;
            goto done;
        }
    }
    if (offset == 0u && length == slot->requested_size) {
        slot->cpu_upload_pending = 0u;
    }
    result = RIN_GPU_MEMORY_OK;

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_destroy_allocation(RinGpuMemoryRuntime* runtime,
                                      uint64_t allocation_handle) {
    RinGpuMemoryRuntimeState* state;
    RinGpuMemoryAllocationSlot* slot;
    int result;

    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
        goto done;
    }
    slot = ringpu_memory_lookup_allocation(state, allocation_handle, NULL);
    if (!slot) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    if (slot->lease_count != 0u) {
        result = RIN_GPU_MEMORY_BUSY;
        goto done;
    }
    if (state->active_allocation_count == 0u) {
        ringpu_memory_mark_lost(state);
        result = RIN_GPU_MEMORY_PROTOCOL;
        goto done;
    }
    state->active_allocation_count--;
    slot->state = slot->mapping_cookie == 0u
                      ? RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY
                      : RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP;
    result = ringpu_memory_cleanup_slot(state, slot);

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_revoke_all(RinGpuMemoryRuntime* runtime) {
    RinGpuMemoryRuntimeState* state;
    uint32_t index;
    int result;

    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;

    /* A process-exit revoke is deliberately allowed after a backend failure:
     * the caller has already quiesced the device and must be able to retry
     * only the cleanup that did not complete.  The normal allocation APIs
     * remain closed by LOST/REBIND_REQUIRED as usual. */
    ringpu_memory_clear_all_leases(state);
    result = RIN_GPU_MEMORY_OK;
    for (index = 0u; index < state->backend.max_allocations; ++index) {
        RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        int cleanup_result;

        if (slot->state == RIN_GPU_MEMORY_SLOT_ACTIVE) {
            if (state->active_allocation_count == 0u) {
                ringpu_memory_mark_lost(state);
                result = RIN_GPU_MEMORY_PROTOCOL;
                continue;
            }
            state->active_allocation_count--;
            slot->state = slot->mapping_cookie == 0u
                              ? RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY
                              : RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP;
        }
        if (slot->state != RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP &&
            slot->state != RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY) {
            continue;
        }
        cleanup_result = ringpu_memory_cleanup_slot(state, slot);
        if (cleanup_result != RIN_GPU_MEMORY_OK &&
            result == RIN_GPU_MEMORY_OK) {
            result = cleanup_result;
        }
    }
    if (state->active_allocation_count != 0u ||
        state->active_lease_count != 0u) {
        ringpu_memory_mark_lost(state);
        if (result == RIN_GPU_MEMORY_OK) result = RIN_GPU_MEMORY_PROTOCOL;
    }
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_rebind(RinGpuMemoryRuntime* runtime,
                          const RinGpuMemoryRebindV1* rebind) {
    RinGpuMemoryCallbackSnapshot callback_snapshot;
    RinGpuMemoryRebindV1 request;
    RinGpuMemoryRuntimeState* state;
    uint32_t index;
    int backend_result;
    int result;

    if (!runtime || !rebind ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), rebind,
                              sizeof(*rebind))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    memcpy(&request, rebind, sizeof(request));
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
        goto done;
    }
    if (state->flags != RIN_GPU_MEMORY_STATUS_READY) {
        result = RIN_GPU_MEMORY_STATE;
        goto done;
    }
    if (request.struct_size != sizeof(request) ||
        request.version != RIN_GPU_MEMORY_RUNTIME_VERSION ||
        (request.reason != RIN_GPU_MEMORY_REBIND_RESET &&
         request.reason != RIN_GPU_MEMORY_REBIND_RESUME) ||
        request.flags != 0u ||
        !ringpu_memory_all_zero(request.reserved, 2u)) {
        result = RIN_GPU_MEMORY_INVALID_ARGUMENT;
        goto done;
    }
    if (request.expected_iommu_map_generation !=
            state->iommu_map_generation ||
        request.expected_device_epoch != state->device_epoch ||
        request.new_iommu_map_generation <= state->iommu_map_generation ||
        state->device_epoch == UINT64_MAX ||
        request.new_device_epoch != state->device_epoch + 1u) {
        result = RIN_GPU_MEMORY_STALE;
        goto done;
    }
    for (index = 0u; index < state->backend.max_allocations; index++) {
        const uint32_t slot_state = state->allocations[index].state;
        if (slot_state == RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP ||
            slot_state == RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY ||
            slot_state == RIN_GPU_MEMORY_SLOT_PREPARING) {
            result = RIN_GPU_MEMORY_BUSY;
            goto done;
        }
    }
    result = ringpu_memory_callback_begin(state, &callback_snapshot);
    if (result != RIN_GPU_MEMORY_OK) goto done;
    backend_result = state->backend.prepare_rebind(
        state->backend.context, request.reason,
        state->backend.iommu_domain_cookie,
        state->iommu_map_generation, request.new_iommu_map_generation,
        state->device_epoch, request.new_device_epoch);
    result = ringpu_memory_callback_end(state, &callback_snapshot);
    memset(&callback_snapshot, 0, sizeof(callback_snapshot));
    if (result != RIN_GPU_MEMORY_OK) goto done;
    if (backend_result != 0) {
        result = RIN_GPU_MEMORY_BACKEND_FAILED;
        goto done;
    }
    state->iommu_map_generation = request.new_iommu_map_generation;
    state->device_epoch = request.new_device_epoch;
    state->flags = RIN_GPU_MEMORY_STATUS_REBIND_REQUIRED;
    ringpu_memory_clear_all_leases(state);
    for (index = 0u; index < state->backend.max_allocations; index++) {
        if (state->allocations[index].state == RIN_GPU_MEMORY_SLOT_ACTIVE) {
            state->allocations[index].mapping_cookie = 0u;
        }
    }
    result = ringpu_memory_map_all(state);

done:
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_recover_mappings(RinGpuMemoryRuntime* runtime) {
    RinGpuMemoryRuntimeState* state;
    int result = ringpu_memory_lock(runtime, &state);

    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) {
        result = RIN_GPU_MEMORY_LOST;
    } else if (state->flags != RIN_GPU_MEMORY_STATUS_REBIND_REQUIRED) {
        result = RIN_GPU_MEMORY_STATE;
    } else {
        result = ringpu_memory_map_all(state);
    }
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_maintain(RinGpuMemoryRuntime* runtime,
                            uint32_t* cleaned_count_out) {
    RinGpuMemoryRuntimeState* state;
    uint32_t cleaned = 0u;
    uint32_t index;
    int result;

    if (!runtime || !cleaned_count_out ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), cleaned_count_out,
                              sizeof(*cleaned_count_out))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    *cleaned_count_out = 0u;
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    result = RIN_GPU_MEMORY_OK;
    for (index = 0u; index < state->backend.max_allocations; index++) {
        RinGpuMemoryAllocationSlot* slot = &state->allocations[index];
        int cleanup_result;

        if (slot->state != RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP &&
            slot->state != RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY) {
            continue;
        }
        cleanup_result = ringpu_memory_cleanup_slot(state, slot);
        if (cleanup_result == RIN_GPU_MEMORY_OK) {
            cleaned++;
        } else {
            result = cleanup_result;
            if (state->flags == RIN_GPU_MEMORY_STATUS_LOST) break;
        }
    }
    *cleaned_count_out = cleaned;
    ringpu_memory_unlock(state);
    return result;
}

int rin_gpu_memory_get_status(RinGpuMemoryRuntime* runtime,
                              RinGpuMemoryStatusV1* status) {
    RinGpuMemoryRuntimeState* state;
    uint32_t index;
    int result;

    if (!runtime || !status ||
        ringpu_memory_overlap(runtime, sizeof(*runtime), status,
                              sizeof(*status))) {
        return RIN_GPU_MEMORY_INVALID_ARGUMENT;
    }
    memset(status, 0, sizeof(*status));
    result = ringpu_memory_lock(runtime, &state);
    if (result != RIN_GPU_MEMORY_OK) return result;
    status->struct_size = sizeof(*status);
    status->version = RIN_GPU_MEMORY_RUNTIME_VERSION;
    status->flags = state->flags;
    status->max_allocations = state->backend.max_allocations;
    status->iommu_domain_cookie = state->backend.iommu_domain_cookie;
    status->iommu_map_generation = state->iommu_map_generation;
    status->device_epoch = state->device_epoch;
    status->page_size = state->backend.page_size;
    status->local_heap_size = state->backend.local_heap_size;
    status->local_heap_used = state->local_heap_used;
    status->system_heap_size = state->backend.system_heap_size;
    status->system_heap_used = state->system_heap_used;
    status->gpu_virtual_size = state->backend.gpu_virtual_size;
    status->gpu_virtual_used = state->gpu_virtual_used;
    status->active_allocation_count = state->active_allocation_count;
    status->active_lease_count = state->active_lease_count;
    for (index = 0u; index < state->backend.max_allocations; index++) {
        if (state->allocations[index].state ==
                RIN_GPU_MEMORY_SLOT_PREPARING ||
            state->allocations[index].state ==
                RIN_GPU_MEMORY_SLOT_CLEANUP_UNMAP ||
            state->allocations[index].state ==
                RIN_GPU_MEMORY_SLOT_CLEANUP_DESTROY) {
            status->cleanup_pending_count++;
        } else if (state->allocations[index].state ==
                   RIN_GPU_MEMORY_SLOT_RETIRED) {
            status->retired_slot_count++;
        }
    }
    ringpu_memory_unlock(state);
    return RIN_GPU_MEMORY_OK;
}

int rin_gpu_memory_runtime_destroy(RinGpuMemoryRuntime* runtime) {
    RinGpuMemoryRuntimeState* state;
    uint32_t index;
    int result = ringpu_memory_lock(runtime, &state);

    if (result != RIN_GPU_MEMORY_OK) return result;
    if (state->active_allocation_count != 0u ||
        state->active_lease_count != 0u) {
        ringpu_memory_unlock(state);
        return RIN_GPU_MEMORY_BUSY;
    }
    for (index = 0u; index < state->backend.max_allocations; index++) {
        if (ringpu_memory_slot_occupies_range(&state->allocations[index])) {
            ringpu_memory_unlock(state);
            return RIN_GPU_MEMORY_BUSY;
        }
    }
    memset(runtime, 0, sizeof(*runtime));
    return RIN_GPU_MEMORY_OK;
}
