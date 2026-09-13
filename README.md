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

The installed public headers are under `ringpu/`. `src/` contains the
portable implementation and backend-internal records; it is intentionally
not an OS-Core include path.
