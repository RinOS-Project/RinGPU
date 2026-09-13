# RinGPU API stability

RinGPU uses `RIN_GPU_ABI_VERSION == 1` and versioned, size-prefixed public
records. Enum values are append-only. Unknown flags, truncated records,
invalid handles, malformed RSH1 input, and backend version mismatches are
errors; callers must not infer a successful operation from a zeroed output.

`RinGpuHandle` values are opaque capability handles. They are not pointers,
physical addresses, PCI identifiers, or vendor register values. The public
ABI contains no kernel object layout.

The software backend and the RSH1 validator are portable host components.
Physical discovery, MMIO, DMA, IRQ, firmware, and reset ownership stay in the
OS-Core backend and are connected through the versioned backend operation table.
