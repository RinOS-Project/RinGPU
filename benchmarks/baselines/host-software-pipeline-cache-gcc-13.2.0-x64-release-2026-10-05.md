# RinGPU host pipeline-cache benchmark

- Captured: 2026-10-05
- Host: Windows x64
- Compiler: GCC 13.2.0, MinGW-w64 UCRT POSIX/SEH, target `x86_64-w64-mingw32`
- Build: CMake Release, static RinGPU library
- Backend: RinGPU software runtime; no physical GPU is measured
- Warmup: 1,000 iterations per workload
- Samples: 10,000 iterations per workload, three separate process runs
- Command: `ringpu-host-benchmark --iterations 10000 --warmup 1000`
- Comparison: both workloads create and destroy the same compute pipeline 16
  times per iteration; checksums match exactly

The uncached workload keeps 64 distinct cache entries occupied by live
pipelines, so each target-pipeline realization bypasses retention and is
destroyed with its public handle. For the cached workload those blockers are
destroyed, the target pipeline is warmed once, and subsequent handle churn
reuses its idle backend realization. Shader modules remain alive in both
workloads; the benchmark does not claim reuse across shader-module destruction.

```csv
run,benchmark,median_ns,p95_ns,checksum
1,pipeline_create_destroy_uncached_batch16,5700,8900,7202323347024834395
1,pipeline_create_destroy_cached_batch16,2500,2800,7202323347024834395
2,pipeline_create_destroy_uncached_batch16,5700,8700,7202323347024834395
2,pipeline_create_destroy_cached_batch16,2600,4800,7202323347024834395
3,pipeline_create_destroy_uncached_batch16,5700,8900,7202323347024834395
3,pipeline_create_destroy_cached_batch16,2600,4700,7202323347024834395
```

Across the three runs, the median-of-run-medians decreased from 5.7 µs to
2.6 µs (about 54%); checksums were identical. Median p95 decreased from 8.9 µs
to 4.7 µs. This is a host/software-backend result for pipeline-handle churn,
not a physical GPU creation-time or frame-performance claim. Re-run on the
target backend before drawing hardware-specific conclusions.
