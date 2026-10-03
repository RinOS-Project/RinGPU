# RinGPU core/device backend boundary

Applications create resources through the common `ringpu_create_*` and
`ringpu_runtime_create_*` entrypoints, using versioned RinGPU descriptors and
opaque `RinGpuHandle` values. The common core owns descriptor validation,
limits, object slots/generations, dependency references, and result semantics.
Backend-specific creation is delegated through the versioned
`RinGpuBackendOpsV1` table in `src/core/core.h`.

The factory boundary is one-way: common buffer, image, sampler, shader,
pipeline, and bind-group factories validate and normalize their inputs, then
call the corresponding backend create callback and store its returned cookie
in the common object slot. `ringpu_destroy` releases the matching backend
object through its destroy callback. Recorded commands use the common
`RinGpuBackendCommandV1` representation and reach the device only through
`submit_commands`; native API handles do not escape into common descriptors.

`ringpu_core_init` checks backend ABI version/size and the required creation,
destruction, and submission callbacks before accepting a backend. The runtime
copies the accepted operation table. A null `backend_ops` explicitly selects
the software/reference path, which requires its presentation callbacks. A
non-null table requires an external backend family and context; it is passed
to the core as supplied and is not replaced with software operations.

This is the current RinGPU/OS-Core integration boundary, not a promise that
Vulkan, D3D, native presentation, or physical resource callbacks are
implemented. `RinGpuBackendOpsV1` remains an internal source interface rather
than a separately packaged third-party driver ABI. Common runtime factory
routing is host-tested by `ringpu-runtime`; physical backend conformance is
tracked by the backend-specific TODOs.
