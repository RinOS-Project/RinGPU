// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_GRAPHICS_LAYOUT_VALIDATION_H
#define RIN_GPU_GRAPHICS_LAYOUT_VALIDATION_H

#include "../core/core.h"

int ringpu_build_vertex_layout(
    uint32_t shader_input_count, int explicit_layout,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    uint32_t vertex_stride, RinGpuBackendGraphicsPipelineDescV1* desc);
int ringpu_build_vertex_layout_bindings(
    uint32_t shader_input_count, const RinGpuVertexAttributeV2* attributes,
    uint32_t attribute_count, const RinGpuVertexBufferLayoutV1* bindings,
    uint32_t binding_count, RinGpuBackendGraphicsPipelineDescV1* desc);
int ringpu_build_varying_layout(
    const RinGpuObjectSlot* vertex_shader,
    const RinGpuObjectSlot* fragment_shader,
    uint32_t position_output_location, uint32_t native_flags,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuBackendGraphicsPipelineDescV1* desc);

#endif /* RIN_GPU_GRAPHICS_LAYOUT_VALIDATION_H */
