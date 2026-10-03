# RinGPU host runtime benchmark

`ringpu-host-benchmark` is an optional host-only benchmark for the real
headless RinGPU software runtime. It reports three workloads:

- `command_roundtrip`: command-list create, 4 KiB buffer clear recording,
  close, queue submit, fence wait, 4 KiB readback, byte-for-byte validation,
  and command-list destruction;
- `memory_buffer_bind_roundtrip`: dedicated memory create, buffer create and
  bind, 4 KiB upload/readback with byte-for-byte validation, then destruction;
- `compute_bind_group_churn`: create and destroy a validated typed buffer bind
  group against a real compute pipeline and storage buffer.

Every measured and warmup iteration executes the same API path; a failed
operation or mismatched readback exits with an error.

Configure the graphics API host build with `-DRINGPU_BUILD_BENCHMARKS=ON`, then
build `ringpu-host-benchmark`. With `RINGPU_BUILD_TESTS=ON`, CTest also registers
a short benchmark smoke test. The executable accepts `--iterations N` for the
number of measured samples and `--warmup N` for unmeasured setup iterations;
defaults are 1000 and 100. It emits one CSV header and one row per workload with
minimum, median, p95, mean latency, operations per second, compiler family, and
a workload checksum. Windows uses QueryPerformanceCounter; POSIX hosts use
`CLOCK_MONOTONIC`.

The numbers characterize the host software backend only. They are suitable for
repeatable software-runtime comparisons under a recorded compiler/host setup,
not as physical-GPU or presentation-performance claims. Compare runs only when
the host, compiler, configuration, and iteration settings are held constant.
The first committed software-host sample is in
`baselines/host-software-msvc-19.44-x64-release-2026-10-04.md`.
