# RinGPU host software benchmark baseline

- Captured: 2026-10-04
- Host: Windows x64
- Compiler: MSVC 19.44
- CMake configuration: Release, NMake Makefiles
- Backend: RinGPU software runtime (headless command/memory workloads and
  software-surface frame callback); no physical GPU is measured
- Warmup: 1,000 iterations per workload
- Samples: 10,000 iterations per workload
- Command: `ringpu-host-benchmark --iterations 10000 --warmup 1000`

```csv
benchmark,backend,compiler,iterations,warmup,min_ns,median_ns,p95_ns,mean_ns,ops_per_second,checksum
command_roundtrip,software-host,MSVC,10000,1000,1900,2000,2100,1999.88,500030.00,7520809532115622283
command_record_only,software-host,MSVC,10000,1000,100,200,200,159.82,6257039.17,7044755603595969307
queue_submit_wait,software-host,MSVC,10000,1000,1000,1200,1500,1304.58,766530.22,3315812164222256491
memory_buffer_bind_roundtrip,software-host,MSVC,10000,1000,1100,1700,1800,1645.17,607839.92,17783083566719713899
compute_bind_group_churn,software-host,MSVC,10000,1000,200,200,400,313.89,3185829.43,13216034095836386187
software_frame_roundtrip,software-host,MSVC,10000,1000,9600,10700,14200,12503.05,79980.48,14993509307719700843
```

These values are a reproducibility example for this host/configuration, not a
portable performance target or physical-GPU baseline. Re-run on the target
host before drawing regression conclusions.
