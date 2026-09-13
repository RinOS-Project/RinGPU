# `core.c` responsibility map

`src/core/core.c` is the current portable logical-device implementation.  It
does not perform PCI, MMIO, DMA, IRQ, firmware, or physical presentation
work.  The function groups below are the extraction seams for the remaining
split work; each group is deliberately described by its existing public
contract rather than by a speculative wrapper.

| Region | Current responsibility | Future extraction seam |
| --- | --- | --- |
| 1--228 | core readiness, diagnostics, generation-bound handles, and device-generation query | `core/object_table.c` (slot/handle allocation extracted and linked by `core.c`); shader cache lifecycle is in `shader/modules.c` |
| 230--319 | bounded arithmetic, resource upload readiness, format, primitive, image allocation, transfer-layout, and image-state validation | `validation/resource.c` (extracted and linked by `core.c`) |
| 320--554 | display records, blend/sampler/raster state validation | `validation/pipeline.c` (extracted and linked by `core.c`) |
| 584--1179 | vertex attribute/binding and shader varying layout validation | `validation/graphics_layout.c` (extracted and linked by `core.c`); render-state validation remains in `core.c` |
| 1186--1494 | command reference ownership and command recording | `command/record.c` (allocation and all command reference release paths extracted and linked by `core.c`) |
| 1496--1765 | logical device initialization, shutdown, generation, adapter, and display queries | `device/logical_device.c` |
| 1767--2211 | pipeline and bind-group creation; resource and shader module creation/inspection is extracted | `resource/resources.c`, `shader/modules.c`, `pipeline/pipelines.c`, `pipeline/bind_groups.c` |
| 2213--3337 | compute/graphics pipeline and bind-group creation | `pipeline/pipelines.c`, `pipeline/bind_groups.c` |
| 3339--4016 | queue, command-list, copy, readback, and dispatch recording | `command/commands.c`; image transition and compute/graphics barrier recording is extracted to `sync/barriers.c` |
| 4018--4887 | render-pass setup and attachment/state validation | `presentation/render_pass.c` |
| 4962--5988 | raster state, draw encoding, and pass close; present encoding is extracted to `presentation/present.c` | `command/draw.c`, `presentation/present.c` |
| 5990--7681 | backend command translation and submit-time validation | `backend/submit.c`; fence creation/value/wait is extracted and linked by `sync/fences.c` |
| 7703--end | typed object destruction and final resource cleanup | `core/object_lifetime.c` |

The public `RinGpuRuntime` wrapper is already separated in `src/runtime.c`;
the extraction work above must preserve that opaque boundary and keep the
software backend behind `RinGpuBackendOpsV1`.
