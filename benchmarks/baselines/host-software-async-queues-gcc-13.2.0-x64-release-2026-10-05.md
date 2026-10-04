# RinGPU independent transfer/compute queue benchmark

- Captured: 2026-10-05
- Host: Windows x64
- Compiler: GCC 13.2.0, MinGW-w64; CMake Release (`-O3 -DNDEBUG`)
- Backend: explicit RinGPU software runtime; no physical GPU is measured
- Warmup: 1,000 paired iterations
- Samples: 10,000 paired iterations per process, two process runs
- Command: `ringpu-host-benchmark --iterations 10000 --warmup 1000`
- Comparison: independent COPY and COMPUTE queues; serial mode waits after
  each submission, paired mode submits both before waiting. Execution order
  alternates within each run.

Each pair copies 4 KiB from a patterned source into a zero-initialized
destination and byte-compares the complete destination. An independent compute
dispatch increments a storage-buffer value, which is read back and checked on
every iteration. Both variants produce the same checksum.

```csv
run,benchmark,min_ns,median_ns,p95_ns,mean_ns,checksum
1,transfer_compute_serial_wait_each,5500,5700,9100,6244.74,10808315568527074331
1,transfer_compute_submit_both_then_wait,5500,5700,9100,6217.38,10808315568527074331
2,transfer_compute_serial_wait_each,5500,5900,9400,7106.67,10808315568527074331
2,transfer_compute_submit_both_then_wait,5500,5900,9500,7203.41,10808315568527074331
```

The paired-submit variant showed no repeatable latency improvement: medians
were identical within both runs, and its second-run p95 was slightly higher.
The benchmark and correctness coverage are useful host/software evidence, but
this backend does not establish physical queue overlap. OP-004 remains open
until an asynchronous native/physical backend demonstrates the intended
performance benefit; no physical performance claim is made here.
