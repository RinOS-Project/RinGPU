# RinGPU image resource-state contract

This document fixes the semantics of the seven image states currently exposed
by `RinGpuImageState`. It describes the portable RinGPU contract and candidate
native representations; it does not claim that Vulkan or RinDx barrier
backends exist or have been validated.

## State and operation mapping

State eligibility is checked against both the image descriptor and its usage.
The `ringpu_image_state_allowed` checks below are combined with the stricter
image-descriptor validation performed at creation.

| RinGPU state (ABI value) | Eligibility | Common access intent and operations | Vulkan image-layout candidate | D3D12 state candidate |
|---|---|---|---|---|
| `UNDEFINED` (0) | Any valid image; initial state of every mip/layer | No access; contents must not be relied upon. It is a valid before or after state, with transition to it discarding logical contents. | `VK_IMAGE_LAYOUT_UNDEFINED` is suitable as an initial old layout only. A backend must not use it as `newLayout`; after a logical transition to UNDEFINED it must track a valid native layout separately. | `D3D12_RESOURCE_STATE_COMMON` is only an initial-state analogue; COMMON does not itself discard contents. |
| `COPY_SOURCE` (1) | `COPY_SOURCE` usage | Transfer read: image copy/blit/resolve source. CPU readback additionally requires `CPU_READABLE` and the caller to wait for completion. | `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` | `D3D12_RESOURCE_STATE_COPY_SOURCE` |
| `COPY_DESTINATION` (2) | `COPY_DESTINATION` usage | Transfer write: image copy/blit/resolve destination and image-clear command. Full CPU upload also establishes this state for the uploaded subresource. | `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` | `D3D12_RESOURCE_STATE_COPY_DEST` |
| `COLOR_TARGET` (3) | `COLOR_TARGET` usage and a color format; descriptor validation further excludes formats/usages outside the portable profile | Color-attachment access: render-pass attachment clear/load/store and graphics output. Render-pass use additionally requires a 2D, single-sample image. Presentation is a separate state and usage. | `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` | `D3D12_RESOURCE_STATE_RENDER_TARGET` |
| `PRESENT` (4) | `PRESENT` usage; valid present descriptors are 2D, one mip/layer/sample, RGBA8/BGRA8 UNORM, and also color targets | External presentation read. Submit validates the display geometry/format and requires this state before present. | `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` | `D3D12_RESOURCE_STATE_PRESENT` (an alias of COMMON) |
| `DEPTH_TARGET` (5) | `DEPTH_STENCIL` usage and a depth/stencil format (`D32_FLOAT`, `D32_FLOAT_S8_UINT`, or `S8_UINT`) | Depth/stencil attachment access for the aspects supported by the selected format; render-pass use additionally requires 2D and single-sample. The API currently has no aspect field. | `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` | `D3D12_RESOURCE_STATE_DEPTH_WRITE` |
| `SHADER_READ` (6) | One sample and either `SAMPLED` with a sampled color/depth format, or `STORAGE` on a 2D `R8_UNORM` image | Shader read-only access. The storage-only R8 case is still read-only in this state; shader writes have no image state in the current API. | `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` for sampled color; `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL` for sampled depth/stencil; `VK_IMAGE_LAYOUT_GENERAL` for the storage-image case | Pixel/non-pixel shader-resource state bits selected for the consuming shader stage(s) |

`UNDEFINED` is a content-validity boundary, not a promise that newly allocated
memory was physically cleared. `PRESENT` is not a graphics capability, and a
present-only queue does not gain image-transition capability. `SHADER_READ`
does not mean shader-write or general read/write access. The current image
descriptor validator further limits float and sRGB formats to copy, sampled,
and color-target usage; BC1 excludes color-target usage; depth/stencil formats
exclude color-target, present, and storage usage; and the presentable profile
is RGBA8/BGRA8 UNORM only.

## Transition and state-tracking rules

- State is tracked independently for each `(array_layer, mip_level)` pair.
  The transition record selects a rectangular mip/layer range; aspect selection
  is not represented and remains part of RG-013.
- A transition must have distinct states, a non-empty in-bounds range, and both
  states must be eligible for the image. Recording is allowed only outside a
  render pass on a command list with COPY or GRAPHICS capability.
- At queue submission, every selected subresource must equal
  `before_state`. The submission's state changes are staged and become visible
  through `ringpu_get_image_state` only if backend submission succeeds. A
  validation/backend failure leaves committed states unchanged.
- Image commands validate the state they consume: copy/blit/resolve and clear
  use COPY states; attachment operations use COLOR_TARGET or DEPTH_TARGET;
  shader bindings use SHADER_READ; present uses PRESENT. CPU image readback
  requires COPY_SOURCE.
- Image queue ownership is tracked separately from image state and is
  transferred through the image-wide ownership-transfer record. A state change
  alone does not transfer queue ownership.

## Native-mapping boundary

The native values in the table are semantic candidates, not emitted barriers.
The image transition record contains neither pipeline-stage nor access masks,
and `SHADER_READ` does not identify the consuming shader stage. Separate
`RinGpuComputeBarrierV2` and `RinGpuGraphicsBarrierV2` records do carry source /
destination stage and READ/WRITE access masks, but they have no resource or
subresource selector and therefore do not complete image layout transitions.
A backend must derive stage/access details from consuming operations and
descriptors; it must use GENERAL for the storage-image case in Vulkan. PRESENT
also requires the platform/presentation synchronization contract. Emitted
barriers and their synchronization guarantees belong to VK-010 and DX-005, not
to this common-state definition.

Buffer usages such as vertex, index, uniform, storage-write, and indirect
arguments are not members of `RinGpuImageState`; this image transition API does
not claim to provide buffer barriers for them. `RinGpuImageOwnershipTransferV1`
is a separate ownership operation, and mip/layer/aspect range completion is
tracked by RG-013.

## Source and native-reference index

Current implementation evidence:

- `include/ringpu/ringpu.h`: state values, image usages, transition record.
- `src/validation/resource.c`: state eligibility and image descriptor rules.
- `src/sync/barriers.c`: transition recording validation.
- `src/queue/submit.c`: per-subresource before-state validation and commit-on-
  successful-submit behavior.
- `src/resource/resources.c`: initial state, CPU upload state, and readback
  checks.
- `tests/ringpu_runtime_test.c` and
  `tests/ringpu_resource_requirements_test.c`: host software-path transition
  and usage coverage.

Native representation references (mapping guidance only):

- Vulkan `VkImageLayout`: <https://docs.vulkan.org/refpages/latest/refpages/source/VkImageLayout.html>
- Vulkan descriptor image layouts: <https://docs.vulkan.org/spec/latest/chapters/descriptors.html>
- D3D12 resource states: <https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states>
