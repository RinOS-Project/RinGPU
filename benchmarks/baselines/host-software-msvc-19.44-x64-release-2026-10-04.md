# RinGPU host software benchmark baseline

- Captured: 2026-10-04
- Host: Windows x64
- Compiler: MSVC 19.44
- CMake configuration: Release, NMake Makefiles
- Backend: RinGPU's headless software runtime; no physical GPU is measured
- Warmup: 1,000 iterations per workload
- Samples: 10,000 iterations per workload
- Command: `ringpu-host-benchmark --iterations 10000 --warmup 1000`

```csv
benchmark,backend,compiler,iterations,warmup,min_ns,median_ns,p95_ns,mean_ns,ops_per_second,checksum
command_roundtrip,software-host,MSVC,10000,1000,1800,2000,3100,2172.95,460203.87,7520809532115622283
memory_buffer_bind_roundtrip,software-host,MSVC,10000,1000,1100,1300,1800,1525.27,655621.63,17783083566719713899
compute_bind_group_churn,software-host,MSVC,10000,1000,100,300,400,287.46,3478744.87,13216034095836386187
```

These values are a reproducibility example for this host/configuration, not a
portable performance target or physical-GPU baseline. Re-run on the target
host before drawing regression conclusions.
