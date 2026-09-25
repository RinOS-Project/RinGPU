/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_MEMORY_H
#define RINGPU_PUBLIC_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Public memory ABI.  The implementation is in src/memory, but every
 * consumer uses this header so the ABI cannot be accidentally private to
 * OS-Core.  Records are versioned and size-prefixed; reserved fields are
 * required to be zero by the implementation. */
#define RIN_GPU_MEMORY_RUNTIME_VERSION 1u
#define RIN_GPU_MEMORY_RUNTIME_STATE_QWORDS 1024u
#define RIN_GPU_MEMORY_MAX_ALLOCATIONS 64u
#define RIN_GPU_MEMORY_MAX_LEASES 64u
#define RIN_GPU_MEMORY_MIN_PAGE_SIZE UINT64_C(4096)
#define RIN_GPU_MEMORY_MAX_PAGE_SIZE UINT64_C(2097152)
#define RIN_GPU_MEMORY_MAX_ALIGNMENT UINT64_C(2097152)

#define RIN_GPU_MEMORY_CAP_LOCAL_HEAP UINT32_C(0x00000001)
#define RIN_GPU_MEMORY_CAP_SYSTEM_HEAP UINT32_C(0x00000002)
#define RIN_GPU_MEMORY_CAP_CPU_SYNC UINT32_C(0x00000004)
#define RIN_GPU_MEMORY_CAP_CPU_UPLOAD UINT32_C(0x00000008)
#define RIN_GPU_MEMORY_CAP_KNOWN                                      \
    (RIN_GPU_MEMORY_CAP_LOCAL_HEAP | RIN_GPU_MEMORY_CAP_SYSTEM_HEAP | \
     RIN_GPU_MEMORY_CAP_CPU_SYNC | RIN_GPU_MEMORY_CAP_CPU_UPLOAD)

#define RIN_GPU_MEMORY_HEAP_LOCAL 1u
#define RIN_GPU_MEMORY_HEAP_SYSTEM 2u

#define RIN_GPU_MEMORY_GPU_READ UINT32_C(0x00000001)
#define RIN_GPU_MEMORY_GPU_WRITE UINT32_C(0x00000002)
#define RIN_GPU_MEMORY_CPU_VISIBLE UINT32_C(0x00000004)
#define RIN_GPU_MEMORY_ZEROED UINT32_C(0x00000008)
#define RIN_GPU_MEMORY_FLAG_KNOWN                                  \
    (RIN_GPU_MEMORY_GPU_READ | RIN_GPU_MEMORY_GPU_WRITE |           \
     RIN_GPU_MEMORY_CPU_VISIBLE | RIN_GPU_MEMORY_ZEROED)

#define RIN_GPU_MEMORY_STATUS_READY UINT32_C(0x00000001)
#define RIN_GPU_MEMORY_STATUS_REBIND_REQUIRED UINT32_C(0x00000002)
#define RIN_GPU_MEMORY_STATUS_LOST UINT32_C(0x00000004)

#define RIN_GPU_MEMORY_ALLOCATION_ACTIVE 1u
#define RIN_GPU_MEMORY_ALLOCATION_CLEANUP_PENDING 2u

#define RIN_GPU_MEMORY_SYNC_CPU_TO_DEVICE 1u
#define RIN_GPU_MEMORY_SYNC_DEVICE_TO_CPU 2u

#define RIN_GPU_MEMORY_REBIND_RESET 1u
#define RIN_GPU_MEMORY_REBIND_RESUME 2u

#if UINTPTR_MAX == UINT32_MAX
#define RIN_GPU_MEMORY_BACKEND_RESERVED_QWORDS 8u
#else
#define RIN_GPU_MEMORY_BACKEND_RESERVED_QWORDS 4u
#endif

typedef enum RinGpuMemoryRuntimeResult {
    RIN_GPU_MEMORY_OK = 0,
    RIN_GPU_MEMORY_INVALID_ARGUMENT = -1,
    RIN_GPU_MEMORY_STATE = -2,
    RIN_GPU_MEMORY_BUSY = -3,
    RIN_GPU_MEMORY_NO_SPACE = -4,
    RIN_GPU_MEMORY_BACKEND_FAILED = -5,
    RIN_GPU_MEMORY_PROTOCOL = -6,
    RIN_GPU_MEMORY_LOST = -7,
    RIN_GPU_MEMORY_STALE = -8,
    RIN_GPU_MEMORY_LIMIT = -9
} RinGpuMemoryRuntimeResult;

typedef struct RinGpuMemoryAllocationDescV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t heap;
    uint32_t flags;
    uint64_t size_bytes;
    uint64_t alignment;
    uint64_t reserved[4];
} RinGpuMemoryAllocationDescV1;

typedef struct RinGpuMemoryAllocationInfoV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t heap;
    uint32_t flags;
    uint64_t allocation_handle;
    uint64_t gpu_virtual_address;
    uint64_t heap_offset;
    uint64_t requested_size_bytes;
    uint64_t allocation_size_bytes;
    uint64_t alignment;
    uint64_t iommu_map_generation;
    uint64_t device_epoch;
    uint32_t lease_count;
    uint32_t state;
    uint64_t reserved;
} RinGpuMemoryAllocationInfoV1;

typedef struct RinGpuMemoryRebindV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t reason;
    uint32_t flags;
    uint64_t expected_iommu_map_generation;
    uint64_t new_iommu_map_generation;
    uint64_t expected_device_epoch;
    uint64_t new_device_epoch;
    uint64_t reserved[2];
} RinGpuMemoryRebindV1;

typedef int (*RinGpuMemoryCreateBackingFn)(
    void* context, uint32_t heap, uint32_t flags, uint64_t heap_offset,
    uint64_t allocation_size, uint64_t alignment,
    uint64_t* backing_cookie_out);
typedef int (*RinGpuMemoryDestroyBackingFn)(void* context,
                                            uint64_t backing_cookie);
typedef int (*RinGpuMemoryMapFn)(
    void* context, uint64_t iommu_domain_cookie,
    uint64_t iommu_map_generation, uint64_t backing_cookie,
    uint64_t gpu_virtual_address, uint64_t allocation_size,
    uint32_t flags, uint64_t* mapping_cookie_out);
/* Success means the translation is gone and all device/IOTLB invalidation
 * required for this mapping has completed. */
typedef int (*RinGpuMemoryUnmapFn)(
    void* context, uint64_t iommu_domain_cookie,
    uint64_t iommu_map_generation, uint64_t mapping_cookie);
typedef int (*RinGpuMemorySyncFn)(
    void* context, uint64_t backing_cookie, uint32_t action,
    uint64_t offset, uint64_t length);
/* A successful upload has copied every source byte into the CPU-visible
 * backing.  On failure the runtime performs no device cache sync and blocks
 * new GPU leases until a successful full-allocation upload retries it. */
typedef int (*RinGpuMemoryCpuUploadFn)(
    void* context, uint64_t backing_cookie, uint64_t offset,
    const void* source, uint64_t length);
/* This table consumes the V1 backend's reserved_context extension slot, so
 * RinGpuMemoryBackendV1 remains layout-compatible.  The runtime snapshots
 * upload at init; the table must remain valid only for that call. */
typedef struct RinGpuMemoryCpuUploadOpsV1 {
    uint32_t struct_size;
    uint32_t version;
    RinGpuMemoryCpuUploadFn upload;
    uint64_t reserved[2];
} RinGpuMemoryCpuUploadOpsV1;
#define RIN_GPU_MEMORY_CPU_UPLOAD_OPS_VERSION 1u
/* Success guarantees that old-epoch execution and every old-generation
 * translation are stopped before any new-generation map callback starts. */
typedef int (*RinGpuMemoryPrepareRebindFn)(
    void* context, uint32_t reason, uint64_t iommu_domain_cookie,
    uint64_t old_iommu_map_generation,
    uint64_t new_iommu_map_generation, uint64_t old_device_epoch,
    uint64_t new_device_epoch);

typedef struct RinGpuMemoryBackendV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t capabilities;
    uint32_t max_allocations;
    uint64_t page_size;
    uint64_t local_heap_base;
    uint64_t local_heap_size;
    uint64_t system_heap_size;
    uint64_t gpu_virtual_base;
    uint64_t gpu_virtual_size;
    uint64_t iommu_domain_cookie;
    uint64_t iommu_map_generation;
    uint64_t device_epoch;
    uint64_t handle_secret;
    void* context;
    RinGpuMemoryCreateBackingFn create_backing;
    RinGpuMemoryDestroyBackingFn destroy_backing;
    RinGpuMemoryMapFn map;
    RinGpuMemoryUnmapFn unmap;
    RinGpuMemorySyncFn sync;
    RinGpuMemoryPrepareRebindFn prepare_rebind;
    /* Required exactly when RIN_GPU_MEMORY_CAP_CPU_UPLOAD is set. */
    const RinGpuMemoryCpuUploadOpsV1* cpu_upload_ops;
    uint64_t reserved[RIN_GPU_MEMORY_BACKEND_RESERVED_QWORDS];
} RinGpuMemoryBackendV1;

typedef struct RinGpuMemoryStatusV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t max_allocations;
    uint64_t iommu_domain_cookie;
    uint64_t iommu_map_generation;
    uint64_t device_epoch;
    uint64_t page_size;
    uint64_t local_heap_size;
    uint64_t local_heap_used;
    uint64_t system_heap_size;
    uint64_t system_heap_used;
    uint64_t gpu_virtual_size;
    uint64_t gpu_virtual_used;
    uint32_t active_allocation_count;
    uint32_t active_lease_count;
    uint32_t cleanup_pending_count;
    uint32_t retired_slot_count;
    uint64_t reserved[2];
} RinGpuMemoryStatusV1;

typedef struct RinGpuMemoryRuntime {
    uint64_t opaque[RIN_GPU_MEMORY_RUNTIME_STATE_QWORDS];
} RinGpuMemoryRuntime;

int rin_gpu_memory_runtime_init(RinGpuMemoryRuntime* runtime,
                                const RinGpuMemoryBackendV1* backend);
int rin_gpu_memory_allocate(RinGpuMemoryRuntime* runtime,
                            const RinGpuMemoryAllocationDescV1* descriptor,
                            uint64_t* allocation_handle_out);
int rin_gpu_memory_query(RinGpuMemoryRuntime* runtime,
                         uint64_t allocation_handle,
                         RinGpuMemoryAllocationInfoV1* info);
int rin_gpu_memory_acquire(RinGpuMemoryRuntime* runtime,
                           uint64_t allocation_handle,
                           uint32_t required_gpu_access,
                           uint64_t* lease_handle_out);
int rin_gpu_memory_release(RinGpuMemoryRuntime* runtime,
                           uint64_t lease_handle);
int rin_gpu_memory_sync(RinGpuMemoryRuntime* runtime,
                        uint64_t allocation_handle, uint32_t action,
                        uint64_t offset, uint64_t length);
/* Copies CPU bytes into a CPU-visible allocation.  A non-coherent backend
 * completes CPU_TO_DEVICE sync before success; upload is rejected while the
 * allocation has a GPU lease. */
int rin_gpu_memory_upload(RinGpuMemoryRuntime* runtime,
                          uint64_t allocation_handle, uint64_t offset,
                          const void* source, uint64_t length);
int rin_gpu_memory_destroy_allocation(RinGpuMemoryRuntime* runtime,
                                      uint64_t allocation_handle);
/* Revokes every allocation and GPU lease owned by the runtime.  The caller
 * must have stopped device execution before invoking this process-exit path.
 * Backend cleanup failures leave cleanup-pending slots for maintain/retry. */
int rin_gpu_memory_revoke_all(RinGpuMemoryRuntime* runtime);
int rin_gpu_memory_rebind(RinGpuMemoryRuntime* runtime,
                          const RinGpuMemoryRebindV1* rebind);
int rin_gpu_memory_recover_mappings(RinGpuMemoryRuntime* runtime);
int rin_gpu_memory_maintain(RinGpuMemoryRuntime* runtime,
                            uint32_t* cleaned_count_out);
int rin_gpu_memory_get_status(RinGpuMemoryRuntime* runtime,
                              RinGpuMemoryStatusV1* status);
int rin_gpu_memory_runtime_destroy(RinGpuMemoryRuntime* runtime);

#if defined(__cplusplus)
static_assert(sizeof(RinGpuMemoryAllocationDescV1) == 64u,
              "RinGPU memory allocation descriptor drift");
static_assert(sizeof(RinGpuMemoryAllocationInfoV1) == 96u,
              "RinGPU memory allocation info drift");
static_assert(sizeof(RinGpuMemoryRebindV1) == 64u,
              "RinGPU memory rebind drift");
static_assert(sizeof(RinGpuMemoryBackendV1) == 192u,
              "RinGPU memory backend drift");
static_assert(sizeof(RinGpuMemoryStatusV1) == 128u,
              "RinGPU memory status drift");
#else
_Static_assert(sizeof(RinGpuMemoryAllocationDescV1) == 64u,
               "RinGPU memory allocation descriptor drift");
_Static_assert(sizeof(RinGpuMemoryAllocationInfoV1) == 96u,
               "RinGPU memory allocation info drift");
_Static_assert(sizeof(RinGpuMemoryRebindV1) == 64u,
               "RinGPU memory rebind drift");
_Static_assert(sizeof(RinGpuMemoryBackendV1) == 192u,
               "RinGPU memory backend drift");
_Static_assert(sizeof(RinGpuMemoryStatusV1) == 128u,
               "RinGPU memory status drift");
#endif

#endif /* RINGPU_PUBLIC_MEMORY_H */
