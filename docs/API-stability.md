# RinGPU API stability

RinGPU uses `RIN_GPU_ABI_VERSION == 1` and versioned, size-prefixed public
records. Enum values are append-only. Unknown flags, truncated records,
invalid handles, malformed RSH1 input, and backend version mismatches are
errors; callers must not infer a successful operation from a zeroed output.

`RinGpuHandle` values are opaque capability handles. They are not pointers,
physical addresses, PCI identifiers, or vendor register values. The public
ABI contains no kernel object layout.

Copy, transfer, and destruction semantics for handles are defined in
[`object-ownership.md`](object-ownership.md).

`ringpu_get_adapter_info` reports adapter identity and the queue capabilities
admitted by the core. `ringpu_get_adapter_capabilities` reports those queue
capabilities together with the configured resource limits and optional
backend features. Timestamp-query support requires both query-result and
timestamp-period callbacks; image-readback and completion-wait bits reflect
their corresponding optional callbacks. These bits describe the installed
backend operation table and do not claim physical-device support beyond it.

The common object factory and backend-operation boundary is described in
[`backend-interface-contract.md`](backend-interface-contract.md). It is a
source integration interface, not a public third-party driver ABI.

The software backend and the RSH1 validator are portable host components.
Physical discovery, MMIO, DMA, IRQ, firmware, and reset ownership stay in the
OS-Core backend and are connected through the versioned backend operation table.
