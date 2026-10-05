/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_BACKEND_COMMAND_V1_H
#define RINGPU_PUBLIC_BACKEND_COMMAND_V1_H

#include <stddef.h>
#include <stdint.h>

#include "platform_bridge.h"

#define RIN_GPU_BACKEND_PUSH_CONSTANT_BYTES_V1 128u

#if defined(RIN_SHADER_PUSH_CONSTANT_BYTES) && \
    defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(RIN_GPU_BACKEND_PUSH_CONSTANT_BYTES_V1 ==
                   RIN_SHADER_PUSH_CONSTANT_BYTES,
               "RinGPU backend push-constant ABI drift");
#endif

/* SPDX-License-Identifier: MIT */
/* Shared backend payload schemas for the public RinGPU runtime and OS-Core
 * adapter. The API-owned descriptor types used below must be declared first;
 * keeping those headers out of this file lets OS-Core retain its separate
 * private API ABI while using these canonical backend records. */

typedef enum RinGpuBackendFamilyV1 {
    RIN_GPU_BACKEND_FAMILY_UNKNOWN = 0u,
    RIN_GPU_BACKEND_FAMILY_SOFTWARE = 1u,
    RIN_GPU_BACKEND_FAMILY_INTEL = 2u,
    RIN_GPU_BACKEND_FAMILY_AMD = 3u,
    RIN_GPU_BACKEND_FAMILY_NVIDIA = 4u,
    RIN_GPU_BACKEND_FAMILY_VIRTIO = 5u
} RinGpuBackendFamilyV1;

typedef struct RinGpuBackendBufferCopyV1 {
    uint64_t destination_cookie;
    uint64_t destination_offset;
    uint64_t source_cookie;
    uint64_t source_offset;
    uint64_t size_bytes;
} RinGpuBackendBufferCopyV1;

typedef struct RinGpuBackendImageCopyV1 {
    uint64_t destination_cookie;
    uint64_t source_cookie;
    RinGpuImageCopyRegionV1 region;
} RinGpuBackendImageCopyV1;

/* The bridge transition record is independent of the API-specific transition
 * type. aspect_mask is meaningful in the common contract; older OS-Core
 * layouts that only carried reserved must validate and convert it explicitly. */
typedef struct RinGpuBackendImageTransitionDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    uint32_t before_state;
    uint32_t after_state;
    uint32_t flags;
    uint32_t aspect_mask;
} RinGpuBackendImageTransitionDescV1;

typedef struct RinGpuBackendImageTransitionV1 {
    uint64_t image_cookie;
    RinGpuBackendImageTransitionDescV1 transition;
} RinGpuBackendImageTransitionV1;

typedef struct RinGpuBackendImageBlitDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t source_mip_level;
    uint32_t source_array_layer;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t destination_mip_level;
    uint32_t destination_array_layer;
    uint32_t destination_x;
    uint32_t destination_y;
    uint32_t destination_width;
    uint32_t destination_height;
    uint32_t filter;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendImageBlitDescV1;

typedef struct RinGpuBackendImageBlitV1 {
    uint64_t destination_cookie;
    uint64_t source_cookie;
    RinGpuBackendImageBlitDescV1 blit;
} RinGpuBackendImageBlitV1;

typedef struct RinGpuBackendImageResolveDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t source_mip_level;
    uint32_t source_array_layer;
    uint32_t destination_mip_level;
    uint32_t destination_array_layer;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t destination_x;
    uint32_t destination_y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendImageResolveDescV1;

typedef struct RinGpuBackendImageResolveV1 {
    uint64_t destination_cookie;
    uint64_t source_cookie;
    RinGpuBackendImageResolveDescV1 resolve;
} RinGpuBackendImageResolveV1;

typedef struct RinGpuBackendBufferClearV1 {
    uint64_t destination_cookie;
    uint64_t offset;
    uint64_t size_bytes;
    uint32_t pattern;
    uint32_t reserved;
} RinGpuBackendBufferClearV1;

typedef struct RinGpuBackendImageClearV1 {
    uint64_t destination_cookie;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t aspects;
    uint32_t flags;
    float color_red;
    float color_green;
    float color_blue;
    float color_alpha;
    float depth;
    uint32_t stencil;
    uint32_t reserved;
} RinGpuBackendImageClearV1;

typedef struct RinGpuBackendImageOwnershipTransferDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    uint32_t source_family_index;
    uint32_t source_engine_index;
    uint32_t destination_family_index;
    uint32_t destination_engine_index;
    uint32_t before_state;
    uint32_t after_state;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendImageOwnershipTransferDescV1;

typedef struct RinGpuBackendImageOwnershipTransferV1 {
    uint64_t image_cookie;
    RinGpuBackendImageOwnershipTransferDescV1 transfer;
} RinGpuBackendImageOwnershipTransferV1;

typedef struct RinGpuBackendBufferBindingV1 {
    uint64_t buffer_cookie;
    uint64_t offset;
    uint64_t size_bytes;
    uint32_t access;
    uint32_t reserved;
} RinGpuBackendBufferBindingV1;

typedef struct RinGpuBackendGraphicsBindingV1 {
    uint64_t resource_cookie;
    uint64_t offset;
    uint64_t size_bytes;
    uint32_t binding;
    uint32_t kind;
    uint32_t access;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t flags;
} RinGpuBackendGraphicsBindingV1;

typedef struct RinGpuBackendDispatchV1 {
    uint64_t pipeline_cookie;
    uint64_t bind_group_cookie;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
    uint32_t reserved;
} RinGpuBackendDispatchV1;

typedef struct RinGpuBackendDispatchIndirectV1 {
    uint64_t pipeline_cookie;
    uint64_t bind_group_cookie;
    uint64_t indirect_buffer_cookie;
    uint64_t indirect_offset;
} RinGpuBackendDispatchIndirectV1;

typedef struct RinGpuBackendComputeBarrierV1 {
    uint32_t source_access;
    uint32_t destination_access;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendComputeBarrierV1;

typedef struct RinGpuBackendGraphicsBarrierV1 {
    uint32_t source_access;
    uint32_t destination_access;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendGraphicsBarrierV1;

typedef struct RinGpuBackendComputeBarrierV2 {
    uint32_t source_stage;
    uint32_t destination_stage;
    uint32_t source_access;
    uint32_t destination_access;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendComputeBarrierV2;

typedef struct RinGpuBackendGraphicsBarrierV2 {
    uint32_t source_stage;
    uint32_t destination_stage;
    uint32_t source_access;
    uint32_t destination_access;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBackendGraphicsBarrierV2;

typedef struct RinGpuBackendPushConstantsV1 {
    uint32_t flags;
    uint32_t reserved;
    uint8_t data[RIN_GPU_BACKEND_PUSH_CONSTANT_BYTES_V1];
} RinGpuBackendPushConstantsV1;

typedef struct RinGpuBackendQueryV1 {
    uint64_t query_cookie;
    uint32_t query_type;
    uint32_t reserved;
} RinGpuBackendQueryV1;

typedef struct RinGpuBackendVertexAttributeV1 {
    uint32_t location;
    uint32_t format;
    uint32_t offset;
    uint32_t flags;
    uint32_t binding;
} RinGpuBackendVertexAttributeV1;

typedef struct RinGpuBackendVertexBufferBindingV1 {
    uint32_t binding;
    uint32_t reserved;
    uint64_t buffer_cookie;
    uint64_t offset;
} RinGpuBackendVertexBufferBindingV1;

typedef struct RinGpuBackendVaryingV1 {
    uint32_t vertex_output_location;
    uint32_t fragment_input_location;
    uint32_t type;
    uint32_t interpolation;
} RinGpuBackendVaryingV1;

typedef struct RinGpuBackendRasterStateV1 {
    float viewport_x;
    float viewport_y;
    float viewport_width;
    float viewport_height;
    float min_depth;
    float max_depth;
    int32_t scissor_x;
    int32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    uint32_t scissor_enabled;
    uint32_t polygon_offset_fill_enabled;
    float polygon_offset_factor;
    float polygon_offset_units;
    float line_width;
    uint32_t sample_coverage_enabled;
    float sample_coverage_value;
    uint32_t sample_coverage_invert;
    uint32_t dither_enabled;
    uint32_t reserved;
} RinGpuBackendRasterStateV1;

typedef struct RinGpuBackendGraphicsPipelineDescV1 {
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
    RinGpuBackendVertexAttributeV1
        vertex_attributes[RIN_GPU_MAX_VERTEX_ATTRIBUTES];
    RinGpuVertexBufferLayoutV1
        vertex_bindings[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
    RinGpuBackendVaryingV1 varyings[RIN_GPU_MAX_VARYINGS];
    uint32_t resource_kinds[RIN_SHADER_MAX_RESOURCES];
    uint32_t independent_blend_enabled;
    uint32_t independent_blend_mask;
    RinGpuBackendBlendTargetRecordV1 blend_targets[RIN_GPU_MAX_COLOR_TARGETS];
} RinGpuBackendGraphicsPipelineDescV1;

typedef struct RinGpuBackendDrawV1 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuBackendDrawV1;

typedef struct RinGpuBackendRenderPassBeginV1 {
    uint64_t color_target_cookie;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t load_op;
    uint32_t store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    uint32_t flags;
    uint32_t reserved;
    uint32_t color_write_mask;
    uint32_t reserved1;
    RinGpuClearRegionV1 clear_region;
} RinGpuBackendRenderPassBeginV1;

typedef struct RinGpuBackendRenderPassMrtBeginV1 {
    uint64_t color_target_cookies[RIN_GPU_MAX_COLOR_TARGETS];
    uint32_t color_mip_levels[RIN_GPU_MAX_COLOR_TARGETS];
    uint32_t color_array_layers[RIN_GPU_MAX_COLOR_TARGETS];
    uint32_t active_color_mask;
    uint32_t reserved0;
    uint64_t depth_target_cookie;
    uint64_t stencil_target_cookie;
    uint32_t depth_mip_level;
    uint32_t depth_array_layer;
    uint32_t stencil_mip_level;
    uint32_t stencil_array_layer;
    uint32_t color_load_op;
    uint32_t color_store_op;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    float clear_depth;
    uint32_t clear_stencil;
    uint32_t stencil_write_mask;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t color_write_mask;
    uint32_t reserved2;
    RinGpuClearRegionV1 clear_region;
} RinGpuBackendRenderPassMrtBeginV1;

typedef struct RinGpuBackendPresentV1 {
    uint64_t image_cookie;
    uint32_t display_id;
    uint32_t flags;
} RinGpuBackendPresentV1;

typedef struct RinGpuBackendDrawVerticesV1 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint64_t vertex_buffer_cookie;
    uint64_t vertex_offset;
    uint32_t vertex_stride;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
    uint32_t reserved;
} RinGpuBackendDrawVerticesV1;

typedef struct RinGpuBackendDrawVerticesV2 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
    uint32_t vertex_binding_count;
    uint32_t reserved;
    RinGpuBackendVertexBufferBindingV1
        vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
} RinGpuBackendDrawVerticesV2;

typedef struct RinGpuBackendDrawIndexedV1 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint64_t vertex_buffer_cookie;
    uint64_t index_buffer_cookie;
    uint64_t vertex_offset;
    uint64_t index_offset;
    uint32_t vertex_stride;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t first_instance;
    uint32_t reserved;
} RinGpuBackendDrawIndexedV1;

typedef struct RinGpuBackendDrawIndexedV2 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint64_t index_buffer_cookie;
    uint64_t index_offset;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t first_instance;
    uint32_t vertex_binding_count;
    uint32_t reserved;
    RinGpuBackendVertexBufferBindingV1
        vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
} RinGpuBackendDrawIndexedV2;

typedef struct RinGpuBackendDrawIndexedBaseVertexV1 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint64_t vertex_buffer_cookie;
    uint64_t index_buffer_cookie;
    uint64_t vertex_offset;
    uint64_t index_offset;
    uint32_t vertex_stride;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t first_instance;
    int32_t base_vertex;
} RinGpuBackendDrawIndexedBaseVertexV1;

typedef struct RinGpuBackendDrawIndirectV1 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint64_t indirect_buffer_cookie;
    uint64_t indirect_offset;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t draw_count;
    uint32_t stride;
    uint32_t vertex_binding_count;
    uint32_t reserved;
    RinGpuBackendVertexBufferBindingV1
        vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
} RinGpuBackendDrawIndirectV1;

typedef struct RinGpuBackendDrawIndexedIndirectV1 {
    uint64_t pipeline_cookie;
    uint64_t color_target_cookie;
    uint64_t bind_group_cookie;
    uint64_t index_buffer_cookie;
    uint64_t indirect_buffer_cookie;
    uint64_t index_offset;
    uint64_t indirect_offset;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t draw_count;
    uint32_t stride;
    uint32_t vertex_count;
    uint32_t vertex_binding_count;
    int32_t base_vertex;
    uint32_t reserved;
    RinGpuBackendVertexBufferBindingV1
        vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
} RinGpuBackendDrawIndexedIndirectV1;

typedef struct RinGpuBackendRenderPassDepthBeginV1 {
    uint64_t color_target_cookie;
    uint64_t depth_target_cookie;
    uint32_t color_mip_level;
    uint32_t color_array_layer;
    uint32_t depth_mip_level;
    uint32_t depth_array_layer;
    uint32_t color_load_op;
    uint32_t color_store_op;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    float clear_depth;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    uint32_t clear_stencil;
    uint32_t stencil_write_mask;
    uint32_t reserved2;
    uint32_t color_write_mask;
    uint32_t reserved3;
    RinGpuClearRegionV1 clear_region;
} RinGpuBackendRenderPassDepthBeginV1;

typedef struct RinGpuBackendRenderPassDepthStencilBeginV1 {
    uint64_t color_target_cookie;
    uint64_t depth_target_cookie;
    uint64_t stencil_target_cookie;
    uint32_t color_mip_level;
    uint32_t color_array_layer;
    uint32_t depth_mip_level;
    uint32_t depth_array_layer;
    uint32_t stencil_mip_level;
    uint32_t stencil_array_layer;
    uint32_t color_load_op;
    uint32_t color_store_op;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    float clear_depth;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    uint32_t clear_stencil;
    uint32_t stencil_write_mask;
    uint32_t reserved2;
    uint32_t color_write_mask;
    uint32_t reserved3;
    RinGpuClearRegionV1 clear_region;
} RinGpuBackendRenderPassDepthStencilBeginV1;

/* Payload members shared by the canonical runtime record and OS-Core's
 * compact private record. The transition type is the only field whose
 * internal representation differs; the versioned adapter maps it explicitly.
 * Keeping this list here prevents the common command schema from drifting. */
#define RIN_GPU_BACKEND_COMMAND_SHARED_PAYLOAD_FIELDS_V1(transition_type)    \
    RinGpuBackendBufferCopyV1 buffer_copy;                                   \
    RinGpuBackendImageCopyV1 image_copy;                                     \
    transition_type image_transition;                                        \
    RinGpuBackendDispatchV1 dispatch;                                        \
    RinGpuBackendComputeBarrierV1 compute_barrier;                           \
    RinGpuBackendGraphicsBarrierV1 graphics_barrier;                         \
    RinGpuBackendDrawV1 draw;                                                \
    RinGpuBackendRenderPassBeginV1 render_pass_begin;                        \
    RinGpuBackendRenderPassMrtBeginV1 render_pass_mrt_begin;                 \
    RinGpuBackendPresentV1 present;                                          \
    RinGpuBackendDrawVerticesV1 draw_vertices;                               \
    RinGpuBackendDrawVerticesV2 draw_vertices_v2;                            \
    RinGpuBackendDrawIndexedV1 draw_indexed;                                 \
    RinGpuBackendDrawIndexedV2 draw_indexed_v2;                              \
    RinGpuBackendDrawIndexedBaseVertexV1 draw_indexed_base_vertex;           \
    RinGpuBackendRenderPassDepthBeginV1 render_pass_depth_begin;             \
    RinGpuBackendRenderPassDepthStencilBeginV1                              \
        render_pass_depth_stencil_begin;                                    \
    RinGpuBackendRasterStateV1 raster_state

/* The single typed V1 command contract shared by the public runtime and all
 * backend adapters. Adapters with private command layouts translate at their
 * versioned boundary and never cast a private command array to this type. */
typedef struct RinGpuBackendCommandV1 {
    uint32_t type;
    uint32_t reserved;
    union {
        RIN_GPU_BACKEND_COMMAND_SHARED_PAYLOAD_FIELDS_V1(
            RinGpuBackendImageTransitionV1);
        RinGpuBackendImageBlitV1 image_blit;
        RinGpuBackendImageResolveV1 image_resolve;
        RinGpuBackendBufferClearV1 buffer_clear;
        RinGpuBackendImageClearV1 image_clear;
        RinGpuBackendImageOwnershipTransferV1 image_ownership_transfer;
        RinGpuBackendDispatchIndirectV1 dispatch_indirect;
        RinGpuBackendComputeBarrierV2 compute_barrier_v2;
        RinGpuBackendGraphicsBarrierV2 graphics_barrier_v2;
        RinGpuBackendPushConstantsV1 push_constants;
        RinGpuBackendQueryV1 query;
        RinGpuBackendDrawIndirectV1 draw_indirect;
        RinGpuBackendDrawIndexedIndirectV1 draw_indexed_indirect;
    } value;
} RinGpuBackendCommandV1;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(RinGpuBackendCommandV1, value) == 8u,
               "RinGPU backend command header layout drift");
_Static_assert(sizeof(((RinGpuBackendCommandV1*)0)->value) ==
                   RIN_GPU_BACKEND_COMMAND_PAYLOAD_BYTES_V1,
               "RinGPU backend command payload layout drift");
_Static_assert(sizeof(RinGpuBackendCommandV1) ==
                   RIN_GPU_BACKEND_COMMAND_RECORD_SIZE_V1,
               "RinGPU canonical backend-command ABI drift");
#endif

#endif /* RINGPU_PUBLIC_BACKEND_COMMAND_V1_H */
