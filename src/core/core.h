// SPDX-License-Identifier: MIT
#ifndef RIN_SUBSYSTEMS_RINGPU_CORE_H
#define RIN_SUBSYSTEMS_RINGPU_CORE_H

#include <stddef.h>
#include <stdint.h>

#define RIN_GPU_COMMAND_RECORDING 1u
#define RIN_GPU_COMMAND_EXECUTABLE 2u
#define RIN_GPU_SHADER_CACHE_INDEX_NONE UINT32_MAX

#include <ringpu/ringpu.h>

#include "../validation/diagnostics.h"

#define RIN_GPU_CORE_MAX_OBJECTS 256u
#define RIN_GPU_CORE_MAX_COMMANDS 4096u
#define RIN_GPU_CORE_MAX_IMAGE_SUBRESOURCES 4096u
#define RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE 64u

typedef enum RinGpuBackendFamilyV1 {
    RIN_GPU_BACKEND_FAMILY_UNKNOWN = 0u,
    RIN_GPU_BACKEND_FAMILY_SOFTWARE = 1u,
    RIN_GPU_BACKEND_FAMILY_INTEL = 2u,
    RIN_GPU_BACKEND_FAMILY_AMD = 3u,
    RIN_GPU_BACKEND_FAMILY_NVIDIA = 4u,
    RIN_GPU_BACKEND_FAMILY_VIRTIO = 5u
} RinGpuBackendFamilyV1;

typedef enum RinGpuBackendCommandTypeV1 {
    RIN_GPU_BACKEND_COMMAND_COPY_BUFFER = 1,
    RIN_GPU_BACKEND_COMMAND_COPY_IMAGE = 2,
    RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE = 3,
    RIN_GPU_BACKEND_COMMAND_DISPATCH = 4,
    RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER = 5,
    RIN_GPU_BACKEND_COMMAND_DRAW = 6,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS = 7,
    RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS = 8,
    RIN_GPU_BACKEND_COMMAND_PRESENT = 9,
    RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES = 10,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED = 11,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH = 12,
    RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER = 13,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX = 14,
    RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE = 15,
    RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2 = 16,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2 = 17,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL = 18,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT = 19,
    RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER = 20,
    RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE = 21,
    RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE = 22,
    RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2 = 23,
    RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2 = 24,
    RIN_GPU_BACKEND_COMMAND_SET_PUSH_CONSTANTS = 25,
    RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY = 26,
    RIN_GPU_BACKEND_COMMAND_END_QUERY = 27,
    RIN_GPU_BACKEND_COMMAND_RESET_QUERY = 28,
    RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP = 29
} RinGpuBackendCommandTypeV1;

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

typedef struct RinGpuBackendImageBlitV1 {
    uint64_t destination_cookie;
    uint64_t source_cookie;
    RinGpuImageBlitV1 blit;
} RinGpuBackendImageBlitV1;

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

typedef struct RinGpuBackendImageTransitionV1 {
    uint64_t image_cookie;
    RinGpuImageTransitionV1 transition;
} RinGpuBackendImageTransitionV1;

typedef struct RinGpuBackendImageOwnershipTransferV1 {
    uint64_t image_cookie;
    RinGpuImageOwnershipTransferV1 transfer;
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
    uint8_t data[RIN_SHADER_PUSH_CONSTANT_BYTES];
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
    RinGpuBlendTargetV1 blend_targets[RIN_GPU_MAX_COLOR_TARGETS];
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

typedef struct RinGpuBackendCommandV1 {
    uint32_t type;
    uint32_t reserved;
    union {
        RinGpuBackendBufferCopyV1 buffer_copy;
        RinGpuBackendImageCopyV1 image_copy;
        RinGpuBackendImageBlitV1 image_blit;
        RinGpuBackendBufferClearV1 buffer_clear;
        RinGpuBackendImageClearV1 image_clear;
        RinGpuBackendImageTransitionV1 image_transition;
        RinGpuBackendImageOwnershipTransferV1 image_ownership_transfer;
        RinGpuBackendDispatchV1 dispatch;
        RinGpuBackendComputeBarrierV1 compute_barrier;
        RinGpuBackendGraphicsBarrierV1 graphics_barrier;
        RinGpuBackendComputeBarrierV2 compute_barrier_v2;
    RinGpuBackendGraphicsBarrierV2 graphics_barrier_v2;
    RinGpuBackendPushConstantsV1 push_constants;
    RinGpuBackendQueryV1 query;
        RinGpuBackendDrawV1 draw;
        RinGpuBackendRenderPassBeginV1 render_pass_begin;
        RinGpuBackendRenderPassMrtBeginV1 render_pass_mrt_begin;
        RinGpuBackendPresentV1 present;
        RinGpuBackendDrawVerticesV1 draw_vertices;
        RinGpuBackendDrawVerticesV2 draw_vertices_v2;
        RinGpuBackendDrawIndexedV1 draw_indexed;
        RinGpuBackendDrawIndexedV2 draw_indexed_v2;
        RinGpuBackendDrawIndexedBaseVertexV1 draw_indexed_base_vertex;
        RinGpuBackendRenderPassDepthBeginV1 render_pass_depth_begin;
        RinGpuBackendRenderPassDepthStencilBeginV1 render_pass_depth_stencil_begin;
        RinGpuBackendRasterStateV1 raster_state;
    } value;
} RinGpuBackendCommandV1;

typedef struct RinGpuBackendOpsV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*create_buffer)(void* context, const RinGpuBufferDescV1* desc,
                         uint64_t* cookie);
    void (*destroy_buffer)(void* context, uint64_t cookie);
    /* Optional: required only to create a CPU-visible buffer.  Success means
     * the complete source range is visible to subsequent device work. */
    int (*upload_buffer)(void* context, uint64_t cookie,
                         uint64_t destination_offset, const void* source,
                         uint64_t size_bytes);
    int (*create_image)(void* context, const RinGpuImageDescV1* desc,
                        uint64_t allocation_bytes, uint64_t* cookie);
    void (*destroy_image)(void* context, uint64_t cookie);
    /* Optional: required only to create a CPU-visible image.  The core passes
     * canonical non-zero pitches and success makes the complete upload region
     * visible to subsequent device work. */
    int (*upload_image)(void* context, uint64_t cookie,
                        const RinGpuImageUploadV1* upload,
                        const void* source, uint64_t source_size);
    int (*create_sampler)(void* context, const RinGpuSamplerDescV1* desc,
                          uint64_t* cookie);
    void (*destroy_sampler)(void* context, uint64_t cookie);
    int (*create_shader_module)(void* context, const void* rin_shader_ir,
                                uint64_t shader_size,
                                const RinShaderInfoV1* info,
                                uint64_t* cookie);
    void (*destroy_shader_module)(void* context, uint64_t cookie);
    int (*create_compute_pipeline)(void* context,
                                   uint64_t shader_module_cookie,
                                   const RinShaderInfoV1* shader_info,
                                   uint64_t* cookie);
    void (*destroy_compute_pipeline)(void* context, uint64_t cookie);
    int (*create_graphics_pipeline)(
        void* context, uint64_t vertex_shader_cookie,
        const RinShaderInfoV1* vertex_shader_info,
        uint64_t fragment_shader_cookie,
        const RinShaderInfoV1* fragment_shader_info,
        const RinGpuBackendGraphicsPipelineDescV1* desc,
        uint64_t* cookie);
    void (*destroy_graphics_pipeline)(void* context, uint64_t cookie);
    int (*create_compute_bind_group)(
        void* context, uint64_t pipeline_cookie,
        const RinGpuBackendBufferBindingV1* bindings, uint32_t binding_count,
        uint64_t* cookie);
    void (*destroy_compute_bind_group)(void* context, uint64_t cookie);
    int (*create_graphics_bind_group)(
        void* context, uint64_t pipeline_cookie,
        const RinGpuBackendGraphicsBindingV1* bindings,
        uint32_t binding_count,
        uint64_t* cookie);
    void (*destroy_graphics_bind_group)(void* context, uint64_t cookie);
    int (*submit_commands)(void* context,
                           const RinGpuBackendCommandV1* commands,
                           uint32_t command_count);
    /* Query callbacks are optional for backends that do not advertise the
     * bounded query profile. A submission containing a query is rejected when
     * these callbacks are absent; it is never reported as an empty success. */
    int (*get_query_result)(void* context, uint64_t query_cookie,
                            uint32_t query_type,
                            uint64_t values[RIN_GPU_QUERY_RESULT_VALUE_COUNT],
                            uint32_t* available);
    int (*get_timestamp_period)(void* context, uint64_t* period_nanoseconds);
    void (*destroy_query)(void* context, uint64_t query_cookie);
    /* Optional. Waits for all commands submitted before the call to finish. */
    int (*wait_for_completion)(void* context, uint64_t timeout_ns);
    /* Optional. Reads a canonical CPU-readable COPY_SOURCE image region. */
    int (*readback_image)(void* context, uint64_t cookie,
                          const RinGpuImageReadbackV1* readback,
                          void* destination, uint64_t destination_size);
} RinGpuBackendOpsV1;

typedef struct RinGpuCoreConfigV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t handle_secret;
    uint64_t max_buffer_size;
    uint64_t max_image_size;
    uint64_t max_total_allocation_size;
    uint32_t max_image_dimension;
    uint32_t max_image_layers;
    uint32_t max_image_mip_levels;
    uint32_t max_image_sample_count;
    RinGpuAdapterInfoV1 adapter;
    RinGpuBackendOpsV1 backend;
    void* backend_context;
    const RinGpuDisplayInfoV1* displays;
    uint32_t display_count;
    uint32_t reserved0;
    /* Optional caller-owned diagnostic ring. It is validated at init and
     * receives pointer-free resource/submission events from the core. */
    RinGpuDiagnosticsRuntime* diagnostics;
    /* The cache never crosses a core instance, but this discriminator keeps
     * backend realizations explicit when a core is recreated. */
    uint32_t backend_family;
    uint32_t reserved1;
} RinGpuCoreConfigV1;

typedef struct RinGpuShaderModuleCacheEntry {
    uint8_t* rin_shader_ir;
    uint64_t shader_size;
    uint64_t validated_hash;
    uint64_t device_generation;
    uint64_t backend_cookie;
    RinShaderInfoV1 info;
    uint32_t backend_family;
    uint32_t reference_count;
    uint8_t occupied;
    uint8_t reserved[3];
} RinGpuShaderModuleCacheEntry;

typedef struct RinGpuRecordedCommand {
    uint32_t type;
    int32_t base_vertex;
    RinGpuHandle destination;
    RinGpuHandle source;
    RinGpuHandle auxiliary;
    RinGpuHandle resources;
    union {
        struct {
            uint64_t destination_offset;
            uint64_t source_offset;
            uint64_t size_bytes;
        } buffer_copy;
        RinGpuBufferClearV1 buffer_clear;
        RinGpuImageCopyRegionV1 image_copy;
        RinGpuImageBlitV1 image_blit;
        RinGpuImageClearV1 image_clear;
        RinGpuImageTransitionV1 image_transition;
        RinGpuImageOwnershipTransferV1 image_ownership_transfer;
        RinGpuDispatchV1 dispatch;
        RinGpuComputeBarrierV1 compute_barrier;
        RinGpuGraphicsBarrierV1 graphics_barrier;
        RinGpuComputeBarrierV2 compute_barrier_v2;
        RinGpuGraphicsBarrierV2 graphics_barrier_v2;
        RinGpuPushConstantsV1 push_constants;
        struct {
            uint32_t query_type;
            uint32_t reserved;
        } query;
        RinGpuDrawV1 draw;
        RinGpuRenderPassDescV1 render_pass;
        RinGpuRenderPassMrtDescV1 render_pass_mrt;
        RinGpuPresentV1 present;
        RinGpuDrawVerticesV1 draw_vertices;
        RinGpuDrawVerticesV2 draw_vertices_v2;
        RinGpuDrawIndexedV1 draw_indexed;
        RinGpuDrawIndexedV2 draw_indexed_v2;
        RinGpuRenderPassDepthDescV1 render_pass_depth;
        RinGpuRenderPassDepthStencilDescV1 render_pass_depth_stencil;
        RinGpuRasterStateV5 raster_state;
    } value;
} RinGpuRecordedCommand;

typedef struct RinGpuObjectSlot {
    uint32_t generation;
    uint16_t type;
    uint8_t occupied;
    uint8_t reserved;
    union {
        struct {
            uint64_t size_bytes;
            uint64_t backend_cookie;
            uint32_t usage;
            uint32_t flags;
            uint32_t reference_count;
            uint32_t cpu_upload_pending;
        } buffer;
        struct {
            RinGpuImageDescV1 descriptor;
            uint64_t allocation_bytes;
            uint64_t backend_cookie;
            uint32_t* subresource_states;
            uint32_t subresource_count;
            /* CPU uploads become usable only after every subresource has
             * been replaced successfully. */
            uint8_t* cpu_upload_complete;
            uint32_t reference_count;
            uint32_t cpu_upload_pending;
            uint32_t owner_family_index;
            uint32_t owner_engine_index;
        } image;
        struct {
            RinGpuSamplerDescV1 descriptor;
            uint64_t backend_cookie;
            uint32_t reference_count;
            uint32_t reserved;
        } sampler;
        struct {
            uint8_t* rin_shader_ir;
            uint64_t shader_size;
            uint64_t backend_cookie;
            RinShaderInfoV1 info;
            uint32_t reference_count;
            uint32_t cache_index;
        } shader_module;
        struct {
            RinGpuHandle shader_module;
            uint64_t backend_cookie;
            uint32_t resource_count;
            uint32_t reference_count;
            uint32_t resource_access[RIN_SHADER_MAX_RESOURCES];
            uint32_t resource_kinds[RIN_SHADER_MAX_RESOURCES];
        } compute_pipeline;
        struct {
            RinGpuHandle vertex_shader;
            RinGpuHandle fragment_shader;
            uint64_t backend_cookie;
            uint32_t color_format;
            uint32_t primitive_topology;
            uint32_t vertex_input_count;
            uint32_t vertex_stride;
            uint32_t vertex_binding_count;
            RinGpuVertexBufferLayoutV1
                vertex_bindings[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
            uint32_t depth_format;
            uint32_t depth_compare;
            uint32_t depth_write_enabled;
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
            uint32_t resource_count;
            uint32_t reference_count;
            uint32_t resource_access[RIN_SHADER_MAX_RESOURCES];
            uint32_t resource_kinds[RIN_SHADER_MAX_RESOURCES];
        } graphics_pipeline;
        struct {
            RinGpuHandle pipeline;
            uint64_t backend_cookie;
            RinGpuBufferBindingV1* bindings;
            uint32_t binding_count;
            uint32_t reference_count;
        } compute_bind_group;
        struct {
            RinGpuHandle pipeline;
            uint64_t backend_cookie;
            RinGpuGraphicsBindingV1* bindings;
            uint32_t binding_count;
            uint32_t reference_count;
        } graphics_bind_group;
        struct {
            uint32_t capabilities;
            uint32_t family_index;
            uint32_t engine_index;
            uint32_t reserved;
        } queue;
        struct {
            RinGpuRecordedCommand* commands;
            uint32_t count;
            uint32_t capacity;
            uint32_t capabilities;
            uint32_t state;
            RinGpuHandle render_target;
            RinGpuHandle render_color_targets[RIN_GPU_MAX_COLOR_TARGETS];
            RinGpuHandle render_depth_target;
            RinGpuHandle render_stencil_target;
            uint32_t render_mip_level;
            uint32_t render_array_layer;
            uint32_t render_color_mip_levels[RIN_GPU_MAX_COLOR_TARGETS];
            uint32_t render_color_array_layers[RIN_GPU_MAX_COLOR_TARGETS];
            uint32_t active_color_mask;
            uint32_t render_depth_mip_level;
            uint32_t render_depth_array_layer;
            uint32_t render_stencil_mip_level;
            uint32_t render_stencil_array_layer;
            uint32_t render_pass_active;
            uint32_t reserved;
            RinGpuHandle graphics_bind_group;
        } command_list;
        struct {
            uint64_t value;
        } fence;
        struct {
            uint32_t query_type;
            uint32_t active;
            uint32_t available;
            uint32_t reserved;
            uint64_t values[RIN_GPU_QUERY_RESULT_VALUE_COUNT];
        } query;
    } value;
} RinGpuObjectSlot;

struct RinGpuCore {
    uint64_t handle_secret;
    uint64_t max_buffer_size;
    uint64_t max_image_size;
    uint64_t max_total_allocation_size;
    uint64_t allocated_bytes;
    uint32_t max_image_dimension;
    uint32_t max_image_layers;
    uint32_t max_image_mip_levels;
    uint32_t max_image_sample_count;
    RinGpuAdapterInfoV1 adapter;
    RinGpuDisplayInfoV1 displays[RIN_GPU_MAX_DISPLAYS];
    uint32_t display_count;
    RinGpuBackendOpsV1 backend;
    void* backend_context;
    RinGpuDiagnosticsRuntime* diagnostics;
    uint64_t device_generation;
    uint32_t backend_family;
    RinGpuObjectSlot objects[RIN_GPU_CORE_MAX_OBJECTS];
    RinGpuShaderModuleCacheEntry
        shader_module_cache[RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE];
    uint8_t initialized;
    uint8_t device_lost;
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinGpuBackendBufferCopyV1) == 40u,
               "RinGPU backend buffer-copy drift");
_Static_assert(sizeof(RinGpuBackendImageCopyV1) == 80u,
               "RinGPU backend image-copy drift");
_Static_assert(sizeof(RinGpuBackendImageTransitionV1) == 48u,
               "RinGPU backend image-transition drift");
_Static_assert(sizeof(RinGpuBackendBufferBindingV1) == 32u,
               "RinGPU backend buffer-binding drift");
_Static_assert(sizeof(RinGpuBackendGraphicsBindingV1) == 48u,
               "RinGPU backend graphics-binding drift");
_Static_assert(sizeof(RinGpuBackendDispatchV1) == 32u,
               "RinGPU backend dispatch drift");
_Static_assert(sizeof(RinGpuBackendComputeBarrierV1) == 16u,
               "RinGPU backend compute-barrier drift");
_Static_assert(sizeof(RinGpuBackendQueryV1) == 16u,
               "RinGPU backend query drift");
_Static_assert(sizeof(RinGpuBackendGraphicsBarrierV1) == 16u,
               "RinGPU backend graphics-barrier drift");
_Static_assert(sizeof(RinGpuBackendComputeBarrierV2) == 24u,
               "RinGPU backend compute-barrier V2 drift");
_Static_assert(sizeof(RinGpuBackendGraphicsBarrierV2) == 24u,
               "RinGPU backend graphics-barrier V2 drift");
_Static_assert(sizeof(RinGpuBackendPushConstantsV1) ==
                   8u + RIN_SHADER_PUSH_CONSTANT_BYTES,
               "RinGPU backend push constants drift");
_Static_assert(sizeof(RinGpuBackendVertexAttributeV1) == 20u,
               "RinGPU backend vertex-attribute drift");
_Static_assert(sizeof(RinGpuBackendVertexBufferBindingV1) == 24u,
               "RinGPU backend vertex-buffer binding drift");
_Static_assert(sizeof(RinGpuBackendVaryingV1) == 16u,
               "RinGPU backend varying drift");
_Static_assert(sizeof(RinGpuBackendRasterStateV1) == 80u,
               "RinGPU backend raster state drift");
_Static_assert(sizeof(RinGpuBackendGraphicsPipelineDescV1) == 2180u,
               "RinGPU backend graphics-pipeline descriptor drift");
_Static_assert(sizeof(RinGpuBackendDrawV1) == 56u,
               "RinGPU backend draw drift");
_Static_assert(sizeof(RinGpuBackendRenderPassBeginV1) == 80u,
               "RinGPU backend render-pass begin drift");
/* Keep the backend's pointer-free uint64_t payloads aligned with the public
 * ABI on MinGW i686, whose uint64_t alignment is eight bytes. */
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuBackendRenderPassMrtBeginV1) == 200u,
               "RinGPU backend MRT render-pass begin drift");
#else
_Static_assert(sizeof(RinGpuBackendRenderPassMrtBeginV1) == 196u,
               "RinGPU backend MRT render-pass begin drift");
#endif
_Static_assert(sizeof(RinGpuBackendPresentV1) == 16u,
               "RinGPU backend present drift");
_Static_assert(sizeof(RinGpuBackendDrawVerticesV1) == 72u,
               "RinGPU backend vertex draw drift");
_Static_assert(sizeof(RinGpuBackendDrawVerticesV2) == 824u,
               "RinGPU backend multi-buffer vertex draw drift");
_Static_assert(sizeof(RinGpuBackendDrawIndexedV1) == 96u,
               "RinGPU backend indexed draw drift");
_Static_assert(sizeof(RinGpuBackendDrawIndexedV2) == 848u,
               "RinGPU backend multi-buffer indexed draw drift");
_Static_assert(sizeof(RinGpuBackendDrawIndexedBaseVertexV1) == 96u,
               "RinGPU backend base-vertex indexed draw drift");
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuBackendRenderPassDepthBeginV1) == 136u,
               "RinGPU backend depth render-pass begin drift");
#else
_Static_assert(sizeof(RinGpuBackendRenderPassDepthBeginV1) == 132u,
               "RinGPU backend depth render-pass begin drift");
#endif
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuBackendRenderPassDepthStencilBeginV1) == 152u,
               "RinGPU backend separate depth/stencil render-pass begin drift");
#else
_Static_assert(sizeof(RinGpuBackendRenderPassDepthStencilBeginV1) == 148u,
               "RinGPU backend separate depth/stencil render-pass begin drift");
#endif
_Static_assert(sizeof(RinGpuBackendCommandV1) == 856u,
               "RinGPU backend command drift");
#endif

int ringpu_core_init(RinGpuCore* core, const RinGpuCoreConfigV1* config);
void ringpu_core_shutdown(RinGpuCore* core);
void ringpu_core_mark_device_lost(RinGpuCore* core);
uint64_t ringpu_core_device_generation(
    const RinGpuDiagnosticsRuntime* diagnostics);
int ringpu_core_ready(const RinGpuCore* core);
void ringpu_core_diagnostic(RinGpuCore* core, uint32_t type,
                            uint64_t resource_cookie, uint64_t queue_cookie,
                            uint64_t value0, uint64_t value1, int status);
/* Returns the generation currently owned by the core's diagnostics/runtime
 * source.  This remains readable while a core is lost so an embedding can
 * label the replacement transaction without treating the old core as usable.
 */
int ringpu_get_device_generation(const RinGpuCore* core,
                                 uint64_t* generation_out);

#endif /* RIN_SUBSYSTEMS_RINGPU_CORE_H */
