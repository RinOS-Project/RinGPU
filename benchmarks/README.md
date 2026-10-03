# RinGPU host runtime benchmark

`ringpu-host-benchmark` is an optional host-only benchmark for the real
headless RinGPU software runtime. It records one latency sample per complete
command roundtrip: command-list create, 4 KiB buffer clear recording, close,
queue submit, fence wait, 4 KiB readback, byte-for-byte validation, and command
list destruction. Every measured and warmup iteration executes the same API
path; a failed operation or mismatched readback exits with an error.

Configure the graphics API host build with `-DRINGPU_BUILD_BENCHMARKS=ON`, then
build `ringpu-host-benchmark`. With `RINGPU_BUILD_TESTS=ON`, CTest also registers
a short benchmark smoke test. The executable accepts `--iterations N` for the
number of measured samples and `--warmup N` for unmeasured setup iterations;
defaults are 1000 and 100. It emits one CSV header and one result row with
minimum, median, p95, mean latency, operations per second, compiler family, and
a readback checksum. Windows uses QueryPerformanceCounter; POSIX hosts use
`CLOCK_MONOTONIC`.

The numbers characterize the host software backend only. They are suitable for
repeatable software-runtime comparisons under a recorded compiler/host setup,
not as physical-GPU or presentation-performance claims. Compare runs only when
the host, compiler, configuration, and iteration settings are held constant.
