/* SPDX-License-Identifier: MIT */
#include "backend_bridge.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(RinGpuBackendCommandV1) ==
                   RIN_GPU_BACKEND_COMMAND_RECORD_SIZE_V1,
               "common backend command ABI changed; revise the contract");
_Static_assert(sizeof(RinGpuBackendCommandRecordV1) ==
                   RIN_GPU_BACKEND_COMMAND_RECORD_SIZE_V1,
               "canonical command record ABI changed; revise the contract");

static void bridge_call_init(RinGpuPlatformBackendBridgeCallV1* call,
                             uint32_t operation)
{
    memset(call, 0, sizeof(*call));
    call->struct_size = sizeof(*call);
    call->version = RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION;
    call->operation = operation;
}

static int bridge_invoke(
    RinGpuPlatformBackendAdapterV1* adapter,
    RinGpuPlatformBackendBridgeCallV1* call,
    RinGpuPlatformBackendBridgeResponseV1* response)
{
    int result;
    if (response == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(response, 0, sizeof(*response));
    response->struct_size = sizeof(*response);
    response->version = RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION;
    if (adapter == NULL || adapter->bridge == NULL ||
        adapter->bridge->invoke == NULL || adapter->bridge_context == NULL)
        return RIN_GPU_ERROR_STATE;
    result = adapter->bridge->invoke(adapter->bridge_context, call, response);
    if (result > RIN_GPU_OK) return RIN_GPU_ERROR_BACKEND;
    if (response->struct_size != sizeof(*response) ||
        response->version != RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION)
        return RIN_GPU_ERROR_PROTOCOL;
    return result;
}

#define BRIDGE_COOKIE_CREATE(name, operation, record_index, record_type)       \
    static int bridge_create_##name(void* context,                            \
                                    const record_type* desc,                  \
                                    uint64_t* cookie_out)                    \
    {                                                                          \
        RinGpuPlatformBackendAdapterV1* adapter =                              \
            (RinGpuPlatformBackendAdapterV1*)context;                          \
        RinGpuPlatformBackendBridgeCallV1 call;                                \
        RinGpuPlatformBackendBridgeResponseV1 response;                        \
        int result;                                                            \
        if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;         \
        *cookie_out = 0u;                                                      \
        if (desc == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;               \
        bridge_call_init(&call, operation);                                    \
        call.records[record_index] = desc;                                     \
        call.record_sizes[record_index] = sizeof(*desc);                       \
        result = bridge_invoke(adapter, &call, &response);                     \
        *cookie_out = response.values[0];                                      \
        if (result == RIN_GPU_OK && *cookie_out == 0u)                         \
            return RIN_GPU_ERROR_BACKEND;                                     \
        return result;                                                         \
    }

#define BRIDGE_COOKIE_DESTROY(name, operation)                                 \
    static void bridge_destroy_##name(void* context, uint64_t cookie)          \
    {                                                                          \
        RinGpuPlatformBackendAdapterV1* adapter =                              \
            (RinGpuPlatformBackendAdapterV1*)context;                          \
        RinGpuPlatformBackendBridgeCallV1 call;                                \
        RinGpuPlatformBackendBridgeResponseV1 response;                        \
        bridge_call_init(&call, operation);                                    \
        call.values[0] = cookie;                                               \
        (void)bridge_invoke(adapter, &call, &response);                        \
    }

static int bridge_create_buffer(void* context,
                                const RinGpuBufferDescV1* desc,
                                uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if (desc == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_BUFFER);
    call.records[0] = desc;
    call.record_sizes[0] = sizeof(*desc);
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(buffer,
                     RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_BUFFER)

static int bridge_upload_buffer(void* context, uint64_t cookie,
                                uint64_t offset, const void* source,
                                uint64_t size)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_UPLOAD_BUFFER);
    call.values[0] = cookie;
    call.values[1] = offset;
    call.values[2] = size;
    call.data[0] = source;
    call.data_sizes[0] = size;
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

static int bridge_readback_buffer(void* context, uint64_t cookie,
                                  uint64_t offset, void* destination,
                                  uint64_t size)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_READBACK_BUFFER);
    call.values[0] = cookie;
    call.values[1] = offset;
    call.values[2] = size;
    call.outputs[0] = destination;
    call.output_sizes[0] = size;
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

static int bridge_create_image(void* context, const RinGpuImageDescV1* desc,
                               uint64_t allocation_bytes,
                               uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if (desc == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_IMAGE);
    call.values[0] = allocation_bytes;
    call.records[0] = desc;
    call.record_sizes[0] = sizeof(*desc);
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(image,
                     RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_IMAGE)

static int bridge_upload_image(void* context, uint64_t cookie,
                               const RinGpuImageUploadV1* upload,
                               const void* source, uint64_t source_size)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    if (upload == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_UPLOAD_IMAGE);
    call.values[0] = cookie;
    call.records[0] = upload;
    call.record_sizes[0] = sizeof(*upload);
    call.data[0] = source;
    call.data_sizes[0] = source_size;
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

static int bridge_readback_image(void* context, uint64_t cookie,
                                 const RinGpuImageReadbackV1* readback,
                                 void* destination, uint64_t destination_size)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    if (readback == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_READBACK_IMAGE);
    call.values[0] = cookie;
    call.records[0] = readback;
    call.record_sizes[0] = sizeof(*readback);
    call.outputs[0] = destination;
    call.output_sizes[0] = destination_size;
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

static int bridge_create_memory(void* context,
                                const RinGpuMemoryDescV1* desc,
                                void** allocation_out)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (allocation_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *allocation_out = NULL;
    if (desc == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(&call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_MEMORY);
    call.records[0] = desc;
    call.record_sizes[0] = sizeof(*desc);
    result = bridge_invoke((RinGpuPlatformBackendAdapterV1*)context,
                           &call, &response);
    *allocation_out = (void*)(uintptr_t)response.values[0];
    if (result == RIN_GPU_OK && *allocation_out == NULL)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

static void bridge_destroy_memory(void* context, void* allocation,
                                  uint64_t size_bytes)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    bridge_call_init(&call,
                     RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_MEMORY);
    call.values[0] = (uint64_t)(uintptr_t)allocation;
    call.values[1] = size_bytes;
    (void)bridge_invoke((RinGpuPlatformBackendAdapterV1*)context,
                        &call, &response);
}

static int bridge_bind_buffer_memory(
    void* context, uint64_t buffer_cookie, const RinGpuBufferDescV1* desc,
    void* allocation, uint64_t allocation_size, uint64_t offset_bytes,
    uint64_t* bound_cookie_out)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (bound_cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *bound_cookie_out = 0u;
    if (desc == NULL || allocation == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_BIND_BUFFER_MEMORY);
    call.values[0] = buffer_cookie;
    call.values[1] = (uint64_t)(uintptr_t)allocation;
    call.values[2] = allocation_size;
    call.values[3] = offset_bytes;
    call.records[0] = desc;
    call.record_sizes[0] = sizeof(*desc);
    result = bridge_invoke((RinGpuPlatformBackendAdapterV1*)context,
                           &call, &response);
    *bound_cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *bound_cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

static int bridge_bind_image_memory(
    void* context, uint64_t image_cookie, const RinGpuImageDescV1* desc,
    uint64_t resource_size, void* allocation, uint64_t allocation_size,
    uint64_t offset_bytes, uint64_t* bound_cookie_out)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (bound_cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *bound_cookie_out = 0u;
    if (desc == NULL || allocation == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(&call,
                     RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_BIND_IMAGE_MEMORY);
    call.values[0] = image_cookie;
    call.values[1] = resource_size;
    call.values[2] = (uint64_t)(uintptr_t)allocation;
    call.values[3] = allocation_size;
    call.values[4] = offset_bytes;
    call.records[0] = desc;
    call.record_sizes[0] = sizeof(*desc);
    result = bridge_invoke((RinGpuPlatformBackendAdapterV1*)context,
                           &call, &response);
    *bound_cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *bound_cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_CREATE(sampler,
                     RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_SAMPLER, 0u,
                     RinGpuSamplerDescV1)
BRIDGE_COOKIE_DESTROY(sampler,
                      RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_SAMPLER)

static int bridge_create_shader_module(void* context, const void* shader_ir,
                                       uint64_t shader_size,
                                       const RinShaderInfoV1* info,
                                       uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if (shader_ir == NULL || info == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_SHADER_MODULE);
    call.records[0] = info;
    call.record_sizes[0] = sizeof(*info);
    call.data[0] = shader_ir;
    call.data_sizes[0] = shader_size;
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(shader_module,
    RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_SHADER_MODULE)

static int bridge_create_compute_pipeline(
    void* context, uint64_t shader_cookie, const RinShaderInfoV1* info,
    uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if (info == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_COMPUTE_PIPELINE);
    call.values[0] = shader_cookie;
    call.records[0] = info;
    call.record_sizes[0] = sizeof(*info);
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(compute_pipeline,
    RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_COMPUTE_PIPELINE)

static int bridge_create_graphics_pipeline(
    void* context, uint64_t vertex_shader_cookie,
    const RinShaderInfoV1* vertex_shader_info,
    uint64_t fragment_shader_cookie,
    const RinShaderInfoV1* fragment_shader_info,
    const RinGpuBackendGraphicsPipelineDescV1* desc, uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if (vertex_shader_info == NULL || fragment_shader_info == NULL ||
        desc == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_GRAPHICS_PIPELINE);
    call.values[0] = vertex_shader_cookie;
    call.values[1] = fragment_shader_cookie;
    call.records[0] = desc;
    call.record_sizes[0] = sizeof(*desc);
    call.records[1] = vertex_shader_info;
    call.record_sizes[1] = sizeof(*vertex_shader_info);
    call.records[2] = fragment_shader_info;
    call.record_sizes[2] = sizeof(*fragment_shader_info);
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(graphics_pipeline,
    RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_GRAPHICS_PIPELINE)

static int bridge_create_compute_bind_group(
    void* context, uint64_t pipeline_cookie,
    const RinGpuBackendBufferBindingV1* bindings, uint32_t binding_count,
    uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if ((binding_count != 0u && bindings == NULL) ||
        binding_count > UINT32_MAX / sizeof(*bindings))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_COMPUTE_BIND_GROUP);
    call.values[0] = pipeline_cookie;
    call.values[1] = binding_count;
    call.records[0] = bindings;
    call.record_sizes[0] = binding_count * sizeof(*bindings);
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(compute_bind_group,
    RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_COMPUTE_BIND_GROUP)

static int bridge_create_graphics_bind_group(
    void* context, uint64_t pipeline_cookie,
    const RinGpuBackendGraphicsBindingV1* bindings, uint32_t binding_count,
    uint64_t* cookie_out)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    int result;
    if (cookie_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie_out = 0u;
    if ((binding_count != 0u && bindings == NULL) ||
        binding_count > UINT32_MAX / sizeof(*bindings))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_GRAPHICS_BIND_GROUP);
    call.values[0] = pipeline_cookie;
    call.values[1] = binding_count;
    call.records[0] = bindings;
    call.record_sizes[0] = binding_count * sizeof(*bindings);
    result = bridge_invoke(adapter, &call, &response);
    *cookie_out = response.values[0];
    if (result == RIN_GPU_OK && *cookie_out == 0u)
        return RIN_GPU_ERROR_BACKEND;
    return result;
}

BRIDGE_COOKIE_DESTROY(graphics_bind_group,
    RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_GRAPHICS_BIND_GROUP)

static int bridge_command_record_from_common(
    const RinGpuBackendCommandV1* command,
    RinGpuBackendCommandRecordV1* record)
{
    if (command == NULL || record == NULL || command->reserved != 0u)
        return RIN_GPU_ERROR_PROTOCOL;
    memset(record, 0, sizeof(*record));
    record->type = command->type;
#define BRIDGE_COPY_COMMAND_PAYLOAD(command_type, member)                    \
    case command_type: {                                                     \
        _Static_assert(sizeof(command->value.member) <=                     \
                           RIN_GPU_BACKEND_COMMAND_PAYLOAD_BYTES_V1,          \
                       "command payload exceeds canonical record");         \
        memcpy(record->payload, &command->value.member,                      \
               sizeof(command->value.member));                               \
        break;                                                               \
    }
    switch (command->type) {
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_COPY_BUFFER,
                                buffer_copy);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_COPY_IMAGE,
                                image_copy);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE,
                                image_transition);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DISPATCH, dispatch);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER,
                                compute_barrier);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DRAW, draw);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS, render_pass_begin);
    case RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS:
        break;
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_PRESENT, present);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES,
                                draw_vertices);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED,
                                draw_indexed);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH,
        render_pass_depth_begin);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER,
                                graphics_barrier);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX,
        draw_indexed_base_vertex);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE,
                                raster_state);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2,
                                draw_vertices_v2);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2,
                                draw_indexed_v2);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL,
        render_pass_depth_stencil_begin);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT,
                                render_pass_mrt_begin);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER,
                                buffer_clear);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE,
                                image_clear);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE,
                                image_blit);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2, compute_barrier_v2);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2, graphics_barrier_v2);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_SET_PUSH_CONSTANTS,
                                push_constants);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY, query);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_END_QUERY, query);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_RESET_QUERY, query);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP,
        image_ownership_transfer);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_RESOLVE_IMAGE,
                                image_resolve);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DRAW_INDIRECT,
                                draw_indirect);
    BRIDGE_COPY_COMMAND_PAYLOAD(
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_INDIRECT,
        draw_indexed_indirect);
    BRIDGE_COPY_COMMAND_PAYLOAD(RIN_GPU_BACKEND_COMMAND_DISPATCH_INDIRECT,
                                dispatch_indirect);
    default:
        return RIN_GPU_ERROR_PROTOCOL;
    }
#undef BRIDGE_COPY_COMMAND_PAYLOAD
    return RIN_GPU_OK;
}

static int bridge_submit_commands(void* context,
                                  const RinGpuBackendCommandV1* commands,
                                  uint32_t command_count)
{
    RinGpuPlatformBackendAdapterV1* adapter =
        (RinGpuPlatformBackendAdapterV1*)context;
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    RinGpuBackendCommandRecordV1* records;
    uint32_t index;
    int result;
    if ((command_count != 0u && commands == NULL) ||
        command_count > RIN_GPU_CORE_MAX_COMMANDS)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    records = command_count != 0u
                  ? (RinGpuBackendCommandRecordV1*)calloc(
                        command_count, sizeof(*records))
                  : NULL;
    if (command_count != 0u && records == NULL)
        return RIN_GPU_ERROR_NO_MEMORY;
    for (index = 0u; index < command_count; ++index) {
        result = bridge_command_record_from_common(&commands[index],
                                                  &records[index]);
        if (result != RIN_GPU_OK) {
            free(records);
            return result;
        }
    }
    bridge_call_init(&call,
                     RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_SUBMIT_COMMANDS);
    call.values[0] = command_count;
    call.values[1] = sizeof(RinGpuBackendCommandRecordV1);
    call.values[2] = RIN_GPU_BACKEND_COMMAND_ABI_VERSION;
    call.records[0] = records;
    call.record_sizes[0] = sizeof(RinGpuBackendCommandRecordV1);
    result = bridge_invoke(adapter, &call, &response);
    free(records);
    return result;
}

static int bridge_get_query_result(void* context, uint64_t query_cookie,
                                   uint32_t query_type,
                                   uint64_t values[8], uint32_t* available)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    if (values == NULL || available == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(values, 0, sizeof(uint64_t) * 8u);
    *available = 0u;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_GET_QUERY_RESULT);
    call.values[0] = query_cookie;
    call.values[1] = query_type;
    call.outputs[0] = values;
    call.output_sizes[0] = sizeof(uint64_t) * 8u;
    call.outputs[1] = available;
    call.output_sizes[1] = sizeof(*available);
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

static int bridge_get_timestamp_period(void* context,
                                       uint64_t* period_nanoseconds)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    if (period_nanoseconds == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *period_nanoseconds = 0u;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_GET_TIMESTAMP_PERIOD);
    call.outputs[0] = period_nanoseconds;
    call.output_sizes[0] = sizeof(*period_nanoseconds);
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

BRIDGE_COOKIE_DESTROY(query,
    RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_QUERY)

static int bridge_wait_for_completion(void* context, uint64_t timeout_ns)
{
    RinGpuPlatformBackendBridgeCallV1 call;
    RinGpuPlatformBackendBridgeResponseV1 response;
    bridge_call_init(
        &call, RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_WAIT_FOR_COMPLETION);
    call.values[0] = timeout_ns;
    return bridge_invoke((RinGpuPlatformBackendAdapterV1*)context, &call,
                         &response);
}

int ringpu_platform_backend_adapter_init(
    RinGpuPlatformBackendAdapterV1* adapter,
    const RinGpuPlatformBackendBridgeV1* bridge, void* bridge_context,
    uint32_t requested_family)
{
    uint64_t callback_mask;
    if (adapter == NULL || bridge == NULL || bridge_context == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(adapter, 0, sizeof(*adapter));
    if (bridge->struct_size != sizeof(*bridge) ||
        bridge->version != RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION ||
        bridge->backend_family != requested_family ||
        bridge->contract_version != RIN_GPU_BACKEND_CONTRACT_VERSION ||
        bridge->adapter_version != RIN_GPU_OS_CORE_BACKEND_ADAPTER_VERSION ||
        bridge->command_abi_version != RIN_GPU_BACKEND_COMMAND_ABI_VERSION ||
        bridge->command_record_size !=
            sizeof(RinGpuBackendCommandRecordV1) ||
        bridge->reserved[0] != 0u || bridge->reserved[1] != 0u ||
        bridge->invoke == NULL ||
        (bridge->callback_mask &
         ~RIN_GPU_PLATFORM_BACKEND_BRIDGE_CALLBACKS_KNOWN) != 0u)
        return RIN_GPU_ERROR_PROTOCOL;
    callback_mask = bridge->callback_mask;
    adapter->bridge = bridge;
    adapter->bridge_context = bridge_context;
    adapter->backend_ops.abi_version = RIN_GPU_ABI_VERSION;
    adapter->backend_ops.struct_size = sizeof(adapter->backend_ops);
    adapter->backend_ops.create_buffer = bridge_create_buffer;
    adapter->backend_ops.destroy_buffer = bridge_destroy_buffer;
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_UPLOAD_BUFFER) != 0u)
        adapter->backend_ops.upload_buffer = bridge_upload_buffer;
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_READBACK_BUFFER) != 0u)
        adapter->backend_ops.readback_buffer = bridge_readback_buffer;
    adapter->backend_ops.create_image = bridge_create_image;
    adapter->backend_ops.destroy_image = bridge_destroy_image;
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_UPLOAD_IMAGE) != 0u)
        adapter->backend_ops.upload_image = bridge_upload_image;
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_READBACK_IMAGE) != 0u)
        adapter->backend_ops.readback_image = bridge_readback_image;
    adapter->backend_ops.create_sampler = bridge_create_sampler;
    adapter->backend_ops.destroy_sampler = bridge_destroy_sampler;
    adapter->backend_ops.create_shader_module = bridge_create_shader_module;
    adapter->backend_ops.destroy_shader_module = bridge_destroy_shader_module;
    adapter->backend_ops.create_compute_pipeline =
        bridge_create_compute_pipeline;
    adapter->backend_ops.destroy_compute_pipeline =
        bridge_destroy_compute_pipeline;
    adapter->backend_ops.create_graphics_pipeline =
        bridge_create_graphics_pipeline;
    adapter->backend_ops.destroy_graphics_pipeline =
        bridge_destroy_graphics_pipeline;
    adapter->backend_ops.create_compute_bind_group =
        bridge_create_compute_bind_group;
    adapter->backend_ops.destroy_compute_bind_group =
        bridge_destroy_compute_bind_group;
    adapter->backend_ops.create_graphics_bind_group =
        bridge_create_graphics_bind_group;
    adapter->backend_ops.destroy_graphics_bind_group =
        bridge_destroy_graphics_bind_group;
    adapter->backend_ops.submit_commands = bridge_submit_commands;
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_QUERIES) != 0u) {
        adapter->backend_ops.get_query_result = bridge_get_query_result;
        adapter->backend_ops.get_timestamp_period = bridge_get_timestamp_period;
        adapter->backend_ops.destroy_query = bridge_destroy_query;
    }
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_WAIT) != 0u)
        adapter->backend_ops.wait_for_completion = bridge_wait_for_completion;
    if ((callback_mask & RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_MEMORY) != 0u) {
        adapter->backend_ops.create_memory = bridge_create_memory;
        adapter->backend_ops.destroy_memory = bridge_destroy_memory;
        adapter->backend_ops.bind_buffer_memory = bridge_bind_buffer_memory;
        adapter->backend_ops.bind_image_memory = bridge_bind_image_memory;
    }
    return RIN_GPU_OK;
}
