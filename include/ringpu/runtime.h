// SPDX-License-Identifier: MIT
#ifndef RINGPU_PUBLIC_RUNTIME_H
#define RINGPU_PUBLIC_RUNTIME_H

#include <stdint.h>

#include "ringpu.h"
#include "software.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_RUNTIME_VERSION 1u

typedef struct RinGpuRuntime RinGpuRuntime;

/* Public host/runtime seam for a caller-owned software surface.  The
 * implementation owns the logical device, command recorder, and software
 * backend; the caller owns only callback storage and the final surface. */
typedef struct RinGpuRuntimeSoftwareSurfaceDescV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t device_generation;
    uint64_t handle_secret;
    uint64_t max_buffer_size;
    uint64_t max_image_size;
    uint64_t max_total_allocation_size;
    uint32_t max_image_dimension;
    uint32_t max_image_layers;
    uint32_t max_image_mip_levels;
    uint32_t max_image_sample_count;
    RinGpuAdapterInfoV1 adapter;
    RinGpuDisplayInfoV1 display;
    RinGpuSoftwarePresentCallbackV1 present_callback;
    void* present_context;
    RinGpuSoftwareAcquireImageCallbackV1 acquire_image;
    void* image_context;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t reserved[2];
} RinGpuRuntimeSoftwareSurfaceDescV1;

int ringpu_runtime_software_surface_create(
    const RinGpuRuntimeSoftwareSurfaceDescV1* desc,
    RinGpuRuntime** runtime_out);
void ringpu_runtime_destroy(RinGpuRuntime* runtime);
int ringpu_runtime_device_lost(const RinGpuRuntime* runtime);
void ringpu_runtime_mark_device_lost(RinGpuRuntime* runtime);
int ringpu_runtime_get_device_generation(const RinGpuRuntime* runtime,
                                         uint64_t* generation_out);

int ringpu_runtime_create_buffer(RinGpuRuntime* runtime,
                                 const RinGpuBufferDescV1* desc,
                                 RinGpuHandle* buffer_out);
int ringpu_runtime_create_memory(RinGpuRuntime* runtime,
                                 const RinGpuMemoryDescV1* desc,
                                 RinGpuHandle* memory_out);
int ringpu_runtime_bind_buffer_memory(
    RinGpuRuntime* runtime, RinGpuHandle buffer,
    const RinGpuResourceMemoryBindingV1* binding);
int ringpu_runtime_upload_buffer(RinGpuRuntime* runtime,
                                 RinGpuHandle buffer, uint64_t destination_offset,
                                 const void* source, uint64_t size_bytes);

int ringpu_runtime_create_queue(RinGpuRuntime* runtime,
                                const RinGpuQueueDescV1* desc,
                                RinGpuHandle* queue_out);
int ringpu_runtime_create_queue_v2(RinGpuRuntime* runtime,
                                   const RinGpuQueueDescV2* desc,
                                   RinGpuHandle* queue_out);
int ringpu_runtime_create_fence(RinGpuRuntime* runtime,
                                uint64_t initial_value,
                                RinGpuHandle* fence_out);
int ringpu_runtime_create_command_list(
    RinGpuRuntime* runtime, const RinGpuCommandListDescV1* desc,
    RinGpuHandle* command_list_out);
int ringpu_runtime_create_image(RinGpuRuntime* runtime,
                                const RinGpuImageDescV1* desc,
                                RinGpuHandle* image_out);
int ringpu_runtime_bind_image_memory(
    RinGpuRuntime* runtime, RinGpuHandle image,
    const RinGpuResourceMemoryBindingV1* binding);
int ringpu_runtime_upload_image(RinGpuRuntime* runtime, RinGpuHandle image,
                                const RinGpuImageUploadV1* upload,
                                const void* source, uint64_t source_size);
int ringpu_runtime_create_sampler(RinGpuRuntime* runtime,
                                  const RinGpuSamplerDescV1* desc,
                                  RinGpuHandle* sampler_out);
int ringpu_runtime_create_shader_module(RinGpuRuntime* runtime,
                                        const void* rin_shader_ir,
                                        uint64_t shader_size,
                                        RinGpuHandle* shader_module_out);
int ringpu_runtime_create_graphics_pipeline_vertex(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    RinGpuHandle* pipeline_out);
int ringpu_runtime_create_graphics_pipeline_vertex_bindings(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, RinGpuHandle* pipeline_out);
int ringpu_runtime_create_graphics_pipeline_native(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline_out);
int ringpu_runtime_create_graphics_pipeline_native_v2(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline_out);
int ringpu_runtime_create_graphics_pipeline_native_vertex_bindings(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline_out);
int ringpu_runtime_create_graphics_pipeline_native_vertex_bindings_v2(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline_out);
int ringpu_runtime_create_compute_pipeline(
    RinGpuRuntime* runtime, const RinGpuComputePipelineDescV1* desc,
    RinGpuHandle* pipeline_out);
int ringpu_runtime_create_graphics_bind_group_typed(
    RinGpuRuntime* runtime, RinGpuHandle pipeline,
    const RinGpuGraphicsBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group_out);
int ringpu_runtime_create_compute_bind_group(
    RinGpuRuntime* runtime, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group_out);
int ringpu_runtime_command_list_reset(RinGpuRuntime* runtime,
                                      RinGpuHandle command_list);
int ringpu_runtime_command_list_close(RinGpuRuntime* runtime,
                                      RinGpuHandle command_list);
int ringpu_runtime_command_transition_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageTransitionV1* transition);
int ringpu_runtime_command_resolve_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, RinGpuHandle source,
    const RinGpuImageResolveV1* resolve);
int ringpu_runtime_command_transfer_image_ownership(
    RinGpuRuntime* runtime, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageOwnershipTransferV1* transfer);
int ringpu_runtime_command_begin_render_pass(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassDescV1* render_pass);
int ringpu_runtime_command_begin_render_pass_mrt(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassMrtDescV1* render_pass);
int ringpu_runtime_command_begin_render_pass_depth(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassDepthDescV1* render_pass);
int ringpu_runtime_command_begin_render_pass_depth_stencil(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassDepthStencilDescV1* render_pass);
int ringpu_runtime_command_bind_graphics_resources(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle bind_group);
int ringpu_runtime_command_set_raster_state(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRasterStateV1* state);
int ringpu_runtime_command_draw(RinGpuRuntime* runtime,
                                RinGpuHandle command_list,
                                const RinGpuDrawV1* draw);
int ringpu_runtime_command_draw_vertices(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawVerticesV1* draw);
int ringpu_runtime_command_draw_vertices_v2(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawVerticesV2* draw);
int ringpu_runtime_command_draw_indexed(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndexedV1* draw);
int ringpu_runtime_command_draw_indexed_v2(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndexedV2* draw);
int ringpu_runtime_command_dispatch(RinGpuRuntime* runtime,
                                    RinGpuHandle command_list,
                                    const RinGpuDispatchV1* dispatch);
int ringpu_runtime_command_dispatch_indirect(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDispatchIndirectV1* dispatch);
int ringpu_runtime_command_draw_indirect(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndirectV1* draw);
int ringpu_runtime_command_draw_indexed_indirect(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndexedIndirectV1* draw);
int ringpu_runtime_command_end_render_pass(RinGpuRuntime* runtime,
                                           RinGpuHandle command_list);
int ringpu_runtime_command_present(RinGpuRuntime* runtime,
                                   RinGpuHandle command_list,
                                   const RinGpuPresentV1* present);
int ringpu_runtime_wait_fence(RinGpuRuntime* runtime, RinGpuHandle fence,
                              uint64_t value, uint64_t timeout_ns);
int ringpu_runtime_readback_image(
    RinGpuRuntime* runtime, RinGpuHandle image,
    const RinGpuImageReadbackV1* readback, void* destination,
    uint64_t destination_size);
int ringpu_runtime_destroy_object(RinGpuRuntime* runtime,
                                  RinGpuHandle object);
int ringpu_runtime_queue_submit(RinGpuRuntime* runtime, RinGpuHandle queue,
                                const RinGpuSubmitInfoV1* submit);

#ifdef __cplusplus
}
#endif

#endif /* RINGPU_PUBLIC_RUNTIME_H */
