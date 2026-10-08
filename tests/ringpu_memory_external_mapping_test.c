/* SPDX-License-Identifier: MIT */
#include <ringpu/memory.h>

#include <string.h>

typedef struct MappingFixture {
    uint64_t next_cookie;
    uint32_t backing_count;
    uint32_t destroy_count;
    uint32_t gpu_map_count;
    uint32_t gpu_unmap_count;
    uint32_t external_map_count;
    uint32_t external_unmap_count;
    uint32_t external_live_count;
    uint32_t fail_external_map_with_cookie;
    uint32_t fail_external_unmap_once;
    uint64_t last_backing_cookie;
    uint64_t last_address_space_cookie;
    uint64_t last_address_space_generation;
    uint64_t last_device_generation;
} MappingFixture;

static int create_backing(void* context, uint32_t heap, uint32_t flags,
                          uint64_t heap_offset, uint64_t allocation_size,
                          uint64_t alignment, uint64_t* backing_cookie_out) {
    MappingFixture* fixture = (MappingFixture*)context;
    (void)heap;
    (void)flags;
    (void)heap_offset;
    (void)allocation_size;
    (void)alignment;
    if (!fixture || !backing_cookie_out) return -1;
    *backing_cookie_out = ++fixture->next_cookie;
    fixture->backing_count++;
    fixture->last_backing_cookie = *backing_cookie_out;
    return 0;
}

static int destroy_backing(void* context, uint64_t backing_cookie) {
    MappingFixture* fixture = (MappingFixture*)context;
    if (!fixture || backing_cookie == 0u || fixture->backing_count == 0u)
        return -1;
    fixture->backing_count--;
    fixture->destroy_count++;
    return 0;
}

static int map_gpuva(void* context, uint64_t domain_cookie,
                     uint64_t generation, uint64_t backing_cookie,
                     uint64_t gpu_virtual_address, uint64_t allocation_size,
                     uint32_t flags, uint64_t* mapping_cookie_out) {
    MappingFixture* fixture = (MappingFixture*)context;
    (void)domain_cookie;
    (void)generation;
    (void)gpu_virtual_address;
    (void)allocation_size;
    (void)flags;
    if (!fixture || backing_cookie == 0u || !mapping_cookie_out) return -1;
    *mapping_cookie_out = ++fixture->next_cookie;
    fixture->gpu_map_count++;
    return 0;
}

static int unmap_gpuva(void* context, uint64_t domain_cookie,
                       uint64_t generation, uint64_t mapping_cookie) {
    MappingFixture* fixture = (MappingFixture*)context;
    (void)domain_cookie;
    (void)generation;
    if (!fixture || mapping_cookie == 0u) return -1;
    fixture->gpu_unmap_count++;
    return 0;
}

static int prepare_rebind(void* context, uint32_t reason,
                          uint64_t domain_cookie, uint64_t old_generation,
                          uint64_t new_generation, uint64_t old_epoch,
                          uint64_t new_epoch) {
    (void)context;
    (void)reason;
    (void)domain_cookie;
    (void)old_generation;
    (void)new_generation;
    (void)old_epoch;
    (void)new_epoch;
    return 0;
}

static int map_external(void* context, uint64_t backing_cookie,
                        uint64_t allocation_size, uint32_t access,
                        const RinGpuMemoryAddressSpaceV1* address_space,
                        uint64_t* address_out,
                        uint64_t* mapping_cookie_out) {
    MappingFixture* fixture = (MappingFixture*)context;
    if (!fixture || backing_cookie == 0u || allocation_size == 0u ||
        access != RIN_GPU_MEMORY_GPU_READ || !address_space || !address_out ||
        !mapping_cookie_out ||
        address_space->kind != RIN_GPU_MEMORY_ADDRESS_SPACE_DISPLAY_FETCH)
        return -1;
    fixture->external_map_count++;
    fixture->last_backing_cookie = backing_cookie;
    fixture->last_address_space_cookie = address_space->owner_cookie;
    fixture->last_address_space_generation = address_space->generation;
    fixture->last_device_generation = address_space->device_generation;
    if (fixture->fail_external_map_with_cookie != 0u) {
        fixture->fail_external_map_with_cookie = 0u;
        *address_out = 0u;
        *mapping_cookie_out = UINT64_C(0xe000000000000001);
        fixture->external_live_count++;
        return -1;
    }
    *address_out = UINT64_C(0x80000000);
    *mapping_cookie_out = UINT64_C(0xe000000000000002);
    fixture->external_live_count++;
    return 0;
}

static int unmap_external(void* context, uint64_t mapping_cookie) {
    MappingFixture* fixture = (MappingFixture*)context;
    if (!fixture || mapping_cookie == 0u ||
        fixture->external_live_count == 0u)
        return -1;
    fixture->external_unmap_count++;
    if (fixture->fail_external_unmap_once != 0u) {
        fixture->fail_external_unmap_once = 0u;
        return -1;
    }
    fixture->external_live_count--;
    return 0;
}

static void init_backend(RinGpuMemoryBackendV3* backend,
                         RinGpuMemoryExternalMappingOpsV1* external_ops,
                         MappingFixture* fixture) {
    memset(backend, 0, sizeof(*backend));
    memset(external_ops, 0, sizeof(*external_ops));
    backend->struct_size = sizeof(*backend);
    backend->version = RIN_GPU_MEMORY_BACKEND_V3_VERSION;
    backend->v1.struct_size = sizeof(backend->v1);
    backend->v1.version = RIN_GPU_MEMORY_RUNTIME_VERSION;
    backend->v1.capabilities = RIN_GPU_MEMORY_CAP_SYSTEM_HEAP;
    backend->v1.max_allocations = 4u;
    backend->v1.page_size = RIN_GPU_MEMORY_MIN_PAGE_SIZE;
    backend->v1.system_heap_size = UINT64_C(0x1000000);
    backend->v1.gpu_virtual_base = UINT64_C(0x40000000);
    backend->v1.gpu_virtual_size = UINT64_C(0x1000000);
    backend->v1.iommu_domain_cookie = 41u;
    backend->v1.iommu_map_generation = 1u;
    backend->v1.device_epoch = 1u;
    backend->v1.handle_secret = UINT64_C(0x4d41504558543331);
    backend->v1.context = fixture;
    backend->v1.create_backing = create_backing;
    backend->v1.destroy_backing = destroy_backing;
    backend->v1.map = map_gpuva;
    backend->v1.unmap = unmap_gpuva;
    backend->v1.prepare_rebind = prepare_rebind;
    external_ops->struct_size = sizeof(*external_ops);
    external_ops->version = RIN_GPU_MEMORY_EXTERNAL_MAPPING_OPS_VERSION;
    external_ops->context = fixture;
    external_ops->map = map_external;
    external_ops->unmap = unmap_external;
    backend->external_mapping_ops = external_ops;
}

int main(void) {
    MappingFixture fixture;
    RinGpuMemoryRuntime runtime;
    RinGpuMemoryBackendV3 backend;
    RinGpuMemoryExternalMappingOpsV1 external_ops;
    RinGpuMemoryAllocationDescV1 descriptor;
    RinGpuMemoryAddressSpaceV1 address_space;
    RinGpuMemoryAllocationInfoV1 allocation_info;
    RinGpuMemoryRebindV1 rebind;
    RinGpuMemoryStatusV1 status;
    uint64_t allocation = 0u;
    uint64_t address = 0u;
    uint64_t lease = 0u;
    int result = 1;

    memset(&fixture, 0, sizeof(fixture));
    memset(&runtime, 0, sizeof(runtime));
    init_backend(&backend, &external_ops, &fixture);
    if (rin_gpu_memory_runtime_init_v3(&runtime, &backend) !=
        RIN_GPU_MEMORY_OK)
        return 1;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.struct_size = sizeof(descriptor);
    descriptor.version = RIN_GPU_MEMORY_RUNTIME_VERSION;
    descriptor.heap = RIN_GPU_MEMORY_HEAP_SYSTEM;
    descriptor.flags = RIN_GPU_MEMORY_GPU_READ;
    descriptor.size_bytes = 8192u;
    descriptor.alignment = RIN_GPU_MEMORY_MIN_PAGE_SIZE;
    if (rin_gpu_memory_allocate(&runtime, &descriptor, &allocation) !=
            RIN_GPU_MEMORY_OK || allocation == 0u)
        goto done;

    memset(&address_space, 0, sizeof(address_space));
    address_space.struct_size = sizeof(address_space);
    address_space.version = RIN_GPU_MEMORY_ADDRESS_SPACE_VERSION;
    address_space.kind = RIN_GPU_MEMORY_ADDRESS_SPACE_DISPLAY_FETCH;
    address_space.owner_cookie = 7u;
    address_space.generation = 12u;
    address_space.device_generation = 27u;
    if (rin_gpu_memory_map_external(
            &runtime, allocation, RIN_GPU_MEMORY_GPU_READ, &address_space,
            &address, &lease) != RIN_GPU_MEMORY_OK ||
        address != UINT64_C(0x80000000) || lease == 0u ||
        fixture.external_live_count != 1u ||
        fixture.last_address_space_cookie != address_space.owner_cookie ||
        fixture.last_address_space_generation != address_space.generation ||
        fixture.last_device_generation != address_space.device_generation ||
        fixture.last_backing_cookie == 0u ||
        rin_gpu_memory_release(&runtime, lease) != RIN_GPU_MEMORY_BUSY ||
        rin_gpu_memory_destroy_allocation(&runtime, allocation) !=
            RIN_GPU_MEMORY_BUSY)
        goto done;

    memset(&rebind, 0, sizeof(rebind));
    rebind.struct_size = sizeof(rebind);
    rebind.version = RIN_GPU_MEMORY_RUNTIME_VERSION;
    rebind.reason = RIN_GPU_MEMORY_REBIND_RESET;
    rebind.expected_iommu_map_generation = 1u;
    rebind.new_iommu_map_generation = 2u;
    rebind.expected_device_epoch = 1u;
    rebind.new_device_epoch = 2u;
    if (rin_gpu_memory_rebind(&runtime, &rebind) != RIN_GPU_MEMORY_BUSY ||
        rin_gpu_memory_revoke_all(&runtime) != RIN_GPU_MEMORY_BUSY)
        goto done;

    fixture.fail_external_unmap_once = 1u;
    if (rin_gpu_memory_unmap_external(&runtime, lease) !=
            RIN_GPU_MEMORY_BACKEND_FAILED ||
        rin_gpu_memory_query(&runtime, allocation, &allocation_info) !=
            RIN_GPU_MEMORY_OK ||
        allocation_info.lease_count != 1u ||
        rin_gpu_memory_destroy_allocation(&runtime, allocation) !=
            RIN_GPU_MEMORY_BUSY ||
        rin_gpu_memory_unmap_external(&runtime, lease) != RIN_GPU_MEMORY_OK ||
        fixture.external_live_count != 0u ||
        rin_gpu_memory_destroy_allocation(&runtime, allocation) !=
            RIN_GPU_MEMORY_OK)
        goto done;

    if (rin_gpu_memory_allocate(&runtime, &descriptor, &allocation) !=
            RIN_GPU_MEMORY_OK ||
        (fixture.fail_external_map_with_cookie = 1u,
         rin_gpu_memory_map_external(
             &runtime, allocation, RIN_GPU_MEMORY_GPU_READ, &address_space,
             &address, &lease)) != RIN_GPU_MEMORY_CLEANUP_PENDING ||
        address != 0u || lease == 0u || fixture.external_live_count != 1u)
        goto done;
    if (rin_gpu_memory_get_status(&runtime, &status) != RIN_GPU_MEMORY_OK ||
        status.active_lease_count != 1u || status.cleanup_pending_count != 0u ||
        rin_gpu_memory_unmap_external(&runtime, lease) != RIN_GPU_MEMORY_OK ||
        fixture.external_live_count != 0u ||
        rin_gpu_memory_rebind(&runtime, &rebind) != RIN_GPU_MEMORY_OK ||
        rin_gpu_memory_revoke_all(&runtime) != RIN_GPU_MEMORY_OK ||
        fixture.backing_count != 0u || fixture.destroy_count != 2u ||
        fixture.external_unmap_count != 3u ||
        rin_gpu_memory_runtime_destroy(&runtime) != RIN_GPU_MEMORY_OK)
        goto done;
    result = 0;

done:
    return result;
}
