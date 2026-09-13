# RinGPU source classification

This table is the boundary record for the current RinOS `ringpu` sources.
Only the portable parts listed under A and B are in this repository.  The
remaining files stay in OS-Core or in test/support trees and are not copied
through an include path into the public build.

| Source family | Classification | Public boundary |
| --- | --- | --- |
| `core.c`, `core.h` | B: split | The versioned public ABI is in `include/ringpu/ringpu.h`; the copied core is portable runtime policy and owns no PCI/MMIO object. |
| `device_runtime.*`, `fault_policy.*` | B | Logical device records, bounded feature flags, and recovery policy only. Physical adapter discovery remains a backend concern. |
| `memory_runtime.*` | B | Allocation accounting and resource lifetime policy; no physical address or kernel pointer is exposed. |
| `cross_process_capability.*` | B | Opaque, generation-bound capability validation; process transport is supplied by OS-Core. |
| `diagnostics.*`, `shader.c` | A | Portable diagnostic records and RSH1 validation. |
| `software_backend.*` | A | Host software backend using the same portable runtime record contracts. |
| `differential.*` | D | Test/support only; not linked into the public runtime target. |
| `dxgi_*` | C | OS-Core/compatibility implementation; no DXGI type is in the native public ABI. |
| `vulkan_*` | C | OS-Core/compatibility implementation; no Vulkan type is in the native public ABI. |
| `presentation_*`, `page_flip_*` | C | Physical presentation and page-flip ownership stays with OS-Core. |
| `pci_*` | C | PCI enumeration and vendor policy are privileged backend implementation. |
| `spirv_frontend.*` | C/D | Compiler/frontend integration belongs to the toolchain or compatibility layer, not the native runtime. |
| `product_runtime.*` | C | Product/device policy is OS-Core policy, not a public hardware ABI. |

The public tree intentionally does not claim to implement the C/D rows.  An
OS-Core backend may adapt them to the public opaque handles through the
versioned backend operation table documented by the API stability policy.
