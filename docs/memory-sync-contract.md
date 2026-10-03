# RinGPU GPU mapping and CPU cache synchronization

`RinGpuMemoryMapFn` and `RinGpuMemoryUnmapFn` manage the GPU virtual-address
mapping in the backend's IOMMU/device domain; they do not expose or revoke a
CPU pointer. A successful unmap means the translation is gone and all required
device/IOTLB invalidation has completed. The current public memory API exposes
CPU-visible upload and explicit cache synchronization, but no CPU map/unmap
pointer pair.

For a CPU-visible allocation with `RIN_GPU_MEMORY_CAP_CPU_SYNC`,
`rin_gpu_memory_sync` accepts a non-empty byte range bounded by the requested
allocation size:

- `RIN_GPU_MEMORY_SYNC_CPU_TO_DEVICE` flushes preceding CPU writes before new
  device work consumes the range.
- `RIN_GPU_MEMORY_SYNC_DEVICE_TO_CPU` invalidates stale CPU cache lines after
  the caller has waited for the relevant device work to complete.

The backend owns the cache-maintenance atom size. It may round the operation
outward to cache-line boundaries, but must constrain maintenance to the backing
allocation and must not report success until the requested bytes are visible.
`rin_gpu_memory_upload` copies the supplied bytes and automatically performs
CPU-to-device synchronization when that capability is advertised. A failed
upload does not synchronize; a failed explicit cache-sync callback marks the
memory runtime lost rather than reporting visible data.

`ringpu_memory_runtime_test.c` models a non-coherent backend with separate CPU
and device byte arrays and a 64-byte cache atom. It checks outward-rounded
flush/invalidate behavior, byte visibility, adjacent-byte preservation, and
GPU map/unmap callbacks. This is host mock coverage only; it does not validate
physical cache instructions, DMA, or a device's actual coherency domain.
