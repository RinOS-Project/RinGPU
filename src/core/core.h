// SPDX-License-Identifier: MIT
#ifndef RIN_SUBSYSTEMS_RINGPU_CORE_H
#define RIN_SUBSYSTEMS_RINGPU_CORE_H

#include <stddef.h>
#include <stdint.h>

#define RIN_GPU_COMMAND_RECORDING 1u
#define RIN_GPU_COMMAND_EXECUTABLE 2u
#define RIN_GPU_SHADER_CACHE_INDEX_NONE UINT32_MAX

#include <ringpu/ringpu.h>
#include <ringpu/platform.h>
#include <ringpu/backend_command_v1.h>

#include "../validation/diagnostics.h"

#define RIN_GPU_CORE_MAX_OBJECTS 256u
#define RIN_GPU_CORE_MAX_COMMANDS 4096u
#define RIN_GPU_CORE_MAX_IMAGE_SUBRESOURCES 4096u
#define RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE 64u

#include <ringpu/backend_ops_v1.h>

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
    RinGpuPlatformYieldThreadCallbackV1 platform_yield_thread;
    void* platform_scheduler_context;
    RinGpuPlatformDiagnosticCallbackV1 platform_diagnostic_callback;
    void* platform_diagnostic_context;
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
        RinGpuImageResolveV1 image_resolve;
        RinGpuImageClearV1 image_clear;
        RinGpuImageTransitionV1 image_transition;
        RinGpuImageOwnershipTransferV1 image_ownership_transfer;
        RinGpuDispatchV1 dispatch;
        RinGpuDispatchIndirectV1 dispatch_indirect;
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
        RinGpuDrawIndirectV1 draw_indirect;
        RinGpuDrawIndexedIndirectV1 draw_indexed_indirect;
        RinGpuRenderPassDepthDescV1 render_pass_depth;
        RinGpuRenderPassDepthStencilDescV1 render_pass_depth_stencil;
        RinGpuRasterStateV5 raster_state;
    } value;
} RinGpuRecordedCommand;

typedef struct RinGpuObjectSlot {
    uint32_t generation;
    uint16_t type;
    uint8_t occupied;
    uint8_t destroy_pending;
    uint64_t last_use_serial;
    union {
        struct {
            uint64_t size_bytes;
            uint64_t backend_cookie;
            uint32_t usage;
            uint32_t flags;
            uint32_t reference_count;
            uint32_t cpu_upload_pending;
            RinGpuHandle memory_handle;
            uint64_t memory_offset;
            uint64_t memory_size;
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
            RinGpuHandle memory_handle;
            uint64_t memory_offset;
            uint64_t memory_size;
        } image;
        struct {
            void* allocation;
            uint64_t size_bytes;
            uint64_t alignment;
            uint32_t flags;
            uint32_t reference_count;
        } memory;
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
            uint32_t reference_count;
            uint64_t values[RIN_GPU_QUERY_RESULT_VALUE_COUNT];
        } query;
    } value;
} RinGpuObjectSlot;

struct RinGpuCore {
    /* Serializes buffer/command-list lifetime transactions and object
     * retirement for host runtimes. It does not make recording to the same
     * command list concurrently safe. */
    volatile long object_operation_lock;
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
    RinGpuPlatformYieldThreadCallbackV1 platform_yield_thread;
    void* platform_scheduler_context;
    RinGpuPlatformDiagnosticCallbackV1 platform_diagnostic_callback;
    void* platform_diagnostic_context;
    uint64_t device_generation;
    uint32_t backend_family;
    uint64_t submitted_serial;
    uint64_t completed_serial;
    uint64_t command_reference_serial;
    RinGpuObjectSlot objects[RIN_GPU_CORE_MAX_OBJECTS];
    RinGpuShaderModuleCacheEntry
        shader_module_cache[RIN_GPU_CORE_MAX_SHADER_MODULE_CACHE];
    uint8_t initialized;
    uint8_t device_lost;
    uint8_t marking_command_references;
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
_Static_assert(sizeof(RinGpuBackendDrawIndirectV1) == 832u,
               "RinGPU backend indirect draw drift");
_Static_assert(sizeof(RinGpuBackendDrawIndexedIndirectV1) == 864u,
               "RinGPU backend indirect indexed draw drift");
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
_Static_assert(sizeof(RinGpuBackendCommandV1) == 872u,
               "RinGPU backend command drift");
#endif

int ringpu_core_init(RinGpuCore* core, const RinGpuCoreConfigV1* config);
void ringpu_core_shutdown(RinGpuCore* core);
void ringpu_core_mark_device_lost(RinGpuCore* core);
uint64_t ringpu_core_device_generation(
    const RinGpuDiagnosticsRuntime* diagnostics);
int ringpu_core_ready(const RinGpuCore* core);
void ringpu_core_resource_lock(RinGpuCore* core);
void ringpu_core_resource_unlock(RinGpuCore* core);
void ringpu_core_diagnostic(RinGpuCore* core, uint32_t type,
                            uint64_t resource_cookie, uint64_t queue_cookie,
                            uint64_t value0, uint64_t value1, int status);
int ringpu_collect_deferred(RinGpuCore* core);
/* Returns the generation currently owned by the core's diagnostics/runtime
 * source.  This remains readable while a core is lost so an embedding can
 * label the replacement transaction without treating the old core as usable.
 */
int ringpu_get_device_generation(const RinGpuCore* core,
                                 uint64_t* generation_out);

#endif /* RIN_SUBSYSTEMS_RINGPU_CORE_H */
