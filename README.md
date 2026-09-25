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

The public version is `RINGPU_API_VERSION == 1`.  ABI records use an explicit
`struct_size`, version, and reserved fields; handles are opaque integers and
are never process pointers or physical addresses.

`ringpu_shader_validate_resource()` is the bounded public resource-catalog
adapter for RSH1 shader blobs. It copies a `TYPE_SHADER` entry into
caller-owned storage and validates it without filesystem access or hidden
allocation; the catalog/path authority remains with the application.

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
