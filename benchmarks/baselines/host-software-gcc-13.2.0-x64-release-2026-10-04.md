# RinGPU host allocation-fragmentation benchmark

- Captured: 2026-10-04
- Host: Windows x64
- Compiler: GCC 13.2.0, MinGW-w64 UCRT POSIX/SEH, target `x86_64-w64-mingw32`
- Build: direct compile with `-O2 -Wall -Wextra -Wpedantic -Werror`
- Backend: RinGPU software runtime; no physical GPU is measured
- Warmup: 1,000 iterations per workload
- Samples: 10,000 iterations per workload, two separate process runs
- Command: `ringpu-host-benchmark --iterations 10000 --warmup 1000`
- Comparison order: baseline and suballocator workloads run in alternating
  pairs; each run verifies matching checksums

Both workloads execute the same sixteen 4 KiB allocations, alternating frees,
eight 8 KiB allocations, and final cleanup. The suballocator workload differs
only by enabling the opt-in 64 KiB software-host pools.

```csv
run,benchmark,min_ns,median_ns,p95_ns,mean_ns,ops_per_second,checksum
1,allocation_fragmentation_baseline,4900,5300,8200,5749.16,173938.45,2987712031837709339
1,allocation_fragmentation_suballocator,4800,5000,6700,5354.79,186748.69,2987712031837709339
2,allocation_fragmentation_baseline,5000,5500,8300,6174.00,161969.55,2987712031837709339
2,allocation_fragmentation_suballocator,4900,5200,6800,5634.28,177484.97,2987712031837709339
```

Across these runs, the suballocator reduced median latency by about 5.5–5.7%
and p95 latency by about 18%, with matching workload checksums. This is a
measurable but host-specific software-backend result, not a physical allocator
or GPU performance claim. Re-run on the target host before drawing regression
conclusions.
