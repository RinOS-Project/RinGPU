/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_COMPATIBILITY_H
#define RINGPU_PUBLIC_COMPATIBILITY_H

#include <stdint.h>

#include "ringpu.h"

/* Graphics API adapters use this pointer-free translation record to hand a
 * validated pipeline plan to RinGPU.  It is deliberately generic: no
 * Vulkan, DXGI, D3D, or native window type appears in the contract. */
typedef struct RinGpuGraphicsPipelineBackendVertexAttributeV1 {
    uint32_t location;
    uint32_t format;
    uint32_t offset;
    uint32_t flags;
    uint32_t binding;
} RinGpuGraphicsPipelineBackendVertexAttributeV1;

typedef struct RinGpuGraphicsPipelineBackendVaryingV1 {
    uint32_t vertex_output_location;
    uint32_t fragment_input_location;
    uint32_t type;
    uint32_t interpolation;
} RinGpuGraphicsPipelineBackendVaryingV1;

typedef struct RinGpuGraphicsPipelineBackendDescV1 {
    uint32_t color_format;
    uint32_t primitive_topology;
    uint32_t flags;
    uint32_t reserved;
    uint32_t vertex_input_count;
    uint32_t vertex_stride;
    uint32_t vertex_binding_count;
    uint32_t depth_format;
    uint32_t depth_compare;
    uint32_t depth_write_enabled;
    uint32_t resource_count;
    uint32_t blend_enabled;
    uint32_t source_color_factor;
    uint32_t destination_color_factor;
    uint32_t color_operation;
    uint32_t source_alpha_factor;
    uint32_t destination_alpha_factor;
    uint32_t alpha_operation;
    float blend_constant_red;
    float blend_constant_green;
    float blend_constant_blue;
    float blend_constant_alpha;
    uint32_t color_write_mask;
    uint32_t cull_mode;
    uint32_t front_face;
    uint32_t stencil_test_enabled;
    uint32_t stencil_compare;
    uint32_t stencil_reference;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_fail_operation;
    uint32_t stencil_depth_fail_operation;
    uint32_t stencil_pass_operation;
    uint32_t separate_stencil_enabled;
    uint32_t back_stencil_compare;
    uint32_t back_stencil_reference;
    uint32_t back_stencil_read_mask;
    uint32_t back_stencil_write_mask;
    uint32_t back_stencil_fail_operation;
    uint32_t back_stencil_depth_fail_operation;
    uint32_t back_stencil_pass_operation;
    uint32_t position_output_location;
    uint32_t varying_count;
    RinGpuGraphicsPipelineBackendVertexAttributeV1
        vertex_attributes[RIN_GPU_MAX_VERTEX_ATTRIBUTES];
    RinGpuVertexBufferLayoutV1
        vertex_bindings[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
    RinGpuGraphicsPipelineBackendVaryingV1
        varyings[RIN_GPU_MAX_VARYINGS];
    uint32_t resource_kinds[RIN_SHADER_MAX_RESOURCES];
} RinGpuGraphicsPipelineBackendDescV1;

#endif /* RINGPU_PUBLIC_COMPATIBILITY_H */
