# RinGPU

RinGPU is the portable RinOS GPU runtime boundary. This repository owns the
versioned API records, opaque handles, RSH1 validator, diagnostics, software
backend, and portable runtime policy. Hardware discovery and privileged
physical execution remain OS-Core backend responsibilities.

## Standalone host build

```text
cmake -S . -B build -DRINGPU_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Meson is supported as well:

```text
meson setup build-meson
meson test -C build-meson
```

The installed public headers are under `ringpu/`. The cross-process capability
header also consumes the versioned `rin/gpu_capability.h` and `rin/ipc.h`
headers from RinOS-SDK; install or expose RinOS-SDK alongside RinGPU. `src/`
contains the portable implementation and backend-internal records; it is
intentionally not an OS-Core include path.

The core `RinGPU` shared library has no direct RinOS channel/service imports.
Cross-process IPC helpers are built as the separate `RinGPUIpc` companion
(`RinGPU::RinGPUIpc` in CMake, or `ringpu_ipc_dep` in Meson). Applications that
use those helpers link the companion and their normal RinOS-SDK implementation;
applications using only the GPU runtime link `RinGPU` without that OS service
dependency. Set `RINGPU_BUILD_RINOS_IPC_ADAPTER=OFF` in CMake or
`-Dbuild_rinos_ipc_adapter=false` in Meson to omit the companion explicitly.

The public version is `RINGPU_API_VERSION == 1`.  ABI records use an explicit
`struct_size`, version, and reserved fields; handles are opaque integers and
are never process pointers or physical addresses.

`ringpu_shader_validate_resource()` is the bounded public resource-catalog
adapter for RSH1 shader blobs. It copies a `TYPE_SHADER` entry into
caller-owned storage and validates it without filesystem access or hidden
allocation; null or zero-capacity storage is rejected before a path callback,
and the catalog/path authority remains with the application.

`ringpu/runtime.h` is the supported host integration seam for a software
surface.  It keeps `RinGpuCore`, diagnostics, and software-backend records
opaque while exposing the queue, command-list, image, transition, and submit
operations needed by a compatibility layer such as RinGL.  Consumers should
link the exported `RinGPU::RinGPU` target (or the installed `ringpu` package)
and must not include `src/` headers.

The same runtime seam exposes validated buffer/image copy, resolve, and clear
commands. Command-list reference release covers every transfer command, so a
completed clear or blit cannot leave a resource falsely busy; malformed
regions, state, format, and aspect combinations still fail closed.

CPU-visible buffers also have an explicit readback owner through
`ringpu_runtime_readback_buffer`. The software backend copies only validated
ranges after upload completion; backends without a readback callback return a
state error instead of fabricating a mapping or success.

## Software backend CPU memory budget

Each software backend enforces the `max_total_bytes` value from its creation
descriptor. All software backend instances in one loaded RinGPU library image
also share `RIN_GPU_SOFTWARE_BACKEND_MAX_AGGREGATE_BYTES` (1 GiB). The shared
counter includes backend objects, resource and execution allocations, and
reserved memory-pool blocks, including idle cached blocks. A failed aggregate
reservation first reclaims idle blocks owned by the requesting backend. A
successful free returns its charge to both limits.

The aggregate limit bounds this software backend's allocations in one loaded
library image. Caller-owned storage supplied by callbacks and physical GPU or
driver allocations are outside this CPU budget; separately linked copies of
RinGPU maintain independent counters.

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | Portable RinOS GPU runtime boundary for versioned records, opaque handles, validators, diagnostics, software execution, and presentation policy. |
| Supported API | Installed headers under include/ringpu; see ringpu.h, runtime.h, software.h, presentation.h, compatibility.h, and capability APIs. |
| Unsupported API | Physical discovery and privileged execution are OS-Core backend duties; unexposed hardware features are unsupported. |
| ownership | Handles are opaque integers, never pointers or physical addresses. Caller owns supplied buffers and resources. |
| thread-safety | Synchronize shared device, queue, command-list, and resource mutation; independent objects are separate. |
| limits | RINGPU_API_VERSION is 1; records are sized/versioned and resource/command bounds are declared in headers. |
| errors | Public result codes distinguish invalid, unsupported, and exhausted-resource paths; failures do not report success. |
| ABI stability | Public records carry struct_size, version, and reserved fields. Preserve the declared API version. |
| security | RSH1 resources are validated from caller data without filesystem authority; src headers are private. |
| build | README provides standalone CMake and Meson builds; enable RINGPU_BUILD_TESTS for host tests. |
| test | Use CTest or meson test as shown in the README. No tests/builds were run for this README update. |
