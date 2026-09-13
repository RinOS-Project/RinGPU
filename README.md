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

`ringpu/runtime.h` is the supported host integration seam for a software
surface.  It keeps `RinGpuCore`, diagnostics, and software-backend records
opaque while exposing the queue, command-list, image, transition, and submit
operations needed by a compatibility layer such as RinGL.  Consumers should
link the exported `RinGPU::RinGPU` target (or the installed `ringpu` package)
and must not include `src/` headers.
