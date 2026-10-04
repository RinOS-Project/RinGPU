/* SPDX-License-Identifier: MIT */
#include <ringpu/runtime.h>

#include "../src/core/core.h"
#include "../src/platform/backend_bridge.h"

#include <string.h>

static const RinGpuBackendOpsV1* external_backend_delegate;
static uint32_t external_create_buffer_calls;
static uint32_t external_destroy_buffer_calls;
static uint32_t external_fail_next_create_buffer;
static uint32_t external_partial_fail_next_create_buffer;
static uint32_t external_empty_success_next_create_buffer;
static uint32_t external_create_image_calls;
static uint32_t external_destroy_image_calls;
static uint32_t external_fail_next_create_image;
static uint32_t external_partial_fail_next_create_image;
static uint32_t external_empty_success_next_create_image;
static uint32_t external_create_sampler_calls;
static uint32_t external_destroy_sampler_calls;
static uint32_t external_fail_next_create_sampler;
static uint32_t external_partial_fail_next_create_sampler;
static uint32_t external_empty_success_next_create_sampler;
static uint32_t external_create_memory_calls;
static uint32_t external_destroy_memory_calls;
static uint32_t external_fail_next_create_memory;
static uint32_t external_partial_fail_next_create_memory;
static uint32_t external_empty_success_next_create_memory;
static uint32_t external_bind_buffer_memory_calls;
static uint32_t external_fail_next_bind_buffer_memory;
static uint32_t external_partial_fail_next_bind_buffer_memory;
static uint32_t external_empty_success_next_bind_buffer_memory;
static uint32_t external_bind_image_memory_calls;
static uint32_t external_partial_fail_next_bind_image_memory;
static uint32_t external_empty_success_next_bind_image_memory;

typedef struct PlatformDiagnosticState {
    uint32_t event_count;
    uint32_t last_type;
    uint64_t last_resource_cookie;
    uint64_t last_queue_cookie;
    uint64_t last_value0;
    uint64_t last_value1;
    int32_t last_status;
    uint32_t submission_count;
    uint32_t present_count;
    uint32_t device_lost_count;
} PlatformDiagnosticState;

typedef struct PlatformBackendResolverState {
    const RinGpuBackendOpsV1* backend_ops;
    void* backend_context;
    uint32_t expected_family;
    uint32_t call_count;
    int result;
} PlatformBackendResolverState;

typedef struct PlatformBackendBridgeMock {
    uint8_t bytes[64];
    uint32_t create_count;
    uint32_t destroy_count;
    uint32_t upload_count;
    uint32_t readback_count;
    uint32_t fail_next_upload;
    const RinGpuBackendCommandV1* expected_commands;
    uint32_t expected_command_count;
    uint32_t submit_count;
} PlatformBackendBridgeMock;

typedef struct PlatformBackendBridgeResolverState {
    const RinGpuPlatformBackendBridgeV1* bridge;
    void* bridge_context;
    uint32_t expected_family;
    uint32_t call_count;
} PlatformBackendBridgeResolverState;

static uint32_t platform_backend_command_payload_size(uint32_t type)
{
#define PLATFORM_COMMAND_PAYLOAD_SIZE(command_type, member)                  \
    case command_type:                                                       \
        return (uint32_t)sizeof(                                                \
            ((RinGpuBackendCommandV1*)0)->value.member)
    switch (type) {
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_COPY_BUFFER,
                                  buffer_copy);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_COPY_IMAGE,
                                  image_copy);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE,
                                  image_transition);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DISPATCH, dispatch);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER,
                                  compute_barrier);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DRAW, draw);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS, render_pass_begin);
    case RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS:
        return 0u;
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_PRESENT, present);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES,
                                  draw_vertices);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED,
                                  draw_indexed);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH,
        render_pass_depth_begin);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER,
                                  graphics_barrier);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX,
        draw_indexed_base_vertex);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE,
                                  raster_state);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2,
                                  draw_vertices_v2);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2,
                                  draw_indexed_v2);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL,
        render_pass_depth_stencil_begin);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT,
                                  render_pass_mrt_begin);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER,
                                  buffer_clear);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE,
                                  image_clear);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE,
                                  image_blit);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2, compute_barrier_v2);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2, graphics_barrier_v2);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_SET_PUSH_CONSTANTS,
                                  push_constants);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY, query);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_END_QUERY, query);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_RESET_QUERY, query);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP,
        image_ownership_transfer);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_RESOLVE_IMAGE,
                                  image_resolve);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DRAW_INDIRECT,
                                  draw_indirect);
    PLATFORM_COMMAND_PAYLOAD_SIZE(
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_INDIRECT,
        draw_indexed_indirect);
    PLATFORM_COMMAND_PAYLOAD_SIZE(RIN_GPU_BACKEND_COMMAND_DISPATCH_INDIRECT,
                                  dispatch_indirect);
    default:
        return UINT32_MAX;
    }
#undef PLATFORM_COMMAND_PAYLOAD_SIZE
}

static int platform_backend_bridge_invoke(
    void* context, const RinGpuPlatformBackendBridgeCallV1* call,
    RinGpuPlatformBackendBridgeResponseV1* response)
{
    PlatformBackendBridgeMock* mock = (PlatformBackendBridgeMock*)context;
    if (mock == NULL || call == NULL || response == NULL ||
        call->struct_size != sizeof(*call) ||
        call->version != RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION ||
        call->reserved != 0u)
        return RIN_GPU_ERROR_PROTOCOL;
    switch (call->operation) {
    case RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_SUBMIT_COMMANDS: {
        const RinGpuBackendCommandV1* commands =
            (const RinGpuBackendCommandV1*)call->records[0];
        uint32_t count = mock->expected_command_count;
        if ((count == 0u &&
             (mock->expected_commands != NULL || commands != NULL)) ||
            (count != 0u &&
             (mock->expected_commands == NULL || commands == NULL)) ||
            call->values[0] != count ||
            call->values[1] != sizeof(RinGpuBackendCommandV1) ||
            call->values[2] != RIN_GPU_BACKEND_COMMAND_ABI_VERSION ||
            call->record_sizes[0] !=
                sizeof(RinGpuBackendCommandV1))
            return RIN_GPU_ERROR_PROTOCOL;
        for (uint32_t index = 0u; index < count; ++index) {
            uint32_t payload_size = platform_backend_command_payload_size(
                mock->expected_commands[index].type);
            const uint8_t* payload =
                (const uint8_t*)&commands[index].value;
            if (payload_size == UINT32_MAX ||
                commands[index].type != mock->expected_commands[index].type ||
                commands[index].reserved != 0u ||
                payload_size > sizeof(commands[index].value) ||
                memcmp(payload,
                       &mock->expected_commands[index].value,
                       payload_size) != 0)
                return RIN_GPU_ERROR_PROTOCOL;
            for (uint32_t byte = payload_size;
                 byte < sizeof(commands[index].value); ++byte) {
                if (payload[byte] != 0u)
                    return RIN_GPU_ERROR_PROTOCOL;
            }
        }
        ++mock->submit_count;
        return RIN_GPU_OK;
    }
    case RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_BUFFER: {
        const RinGpuBufferDescV1* desc =
            (const RinGpuBufferDescV1*)call->records[0];
        if (desc == NULL || call->record_sizes[0] != sizeof(*desc) ||
            desc->struct_size != sizeof(*desc) ||
            desc->abi_version != RIN_GPU_ABI_VERSION ||
            desc->size_bytes > sizeof(mock->bytes))
            return RIN_GPU_ERROR_PROTOCOL;
        ++mock->create_count;
        response->values[0] = UINT64_C(0x4252494447450001);
        return RIN_GPU_OK;
    }
    case RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_BUFFER:
        if (call->values[0] != UINT64_C(0x4252494447450001))
            return RIN_GPU_ERROR_INVALID_HANDLE;
        ++mock->destroy_count;
        return RIN_GPU_OK;
    case RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_UPLOAD_BUFFER:
        if (call->values[0] != UINT64_C(0x4252494447450001) ||
            call->values[1] > sizeof(mock->bytes) ||
            call->values[2] > sizeof(mock->bytes) - call->values[1] ||
            call->data[0] == NULL || call->data_sizes[0] != call->values[2])
            return RIN_GPU_ERROR_BOUNDS;
        ++mock->upload_count;
        if (mock->fail_next_upload != 0u) {
            --mock->fail_next_upload;
            return RIN_GPU_ERROR_BACKEND;
        }
        memcpy(mock->bytes + call->values[1], call->data[0],
               (size_t)call->values[2]);
        return RIN_GPU_OK;
    case RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_READBACK_BUFFER:
        if (call->values[0] != UINT64_C(0x4252494447450001) ||
            call->values[1] > sizeof(mock->bytes) ||
            call->values[2] > sizeof(mock->bytes) - call->values[1] ||
            call->outputs[0] == NULL ||
            call->output_sizes[0] != call->values[2])
            return RIN_GPU_ERROR_BOUNDS;
        ++mock->readback_count;
        memcpy(call->outputs[0], mock->bytes + call->values[1],
               (size_t)call->values[2]);
        return RIN_GPU_OK;
    default:
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
}

static int platform_resolve_backend_bridge(
    void* context, uint32_t backend_family,
    const RinGpuPlatformBackendBridgeV1** bridge_out,
    void** bridge_context_out)
{
    PlatformBackendBridgeResolverState* resolver =
        (PlatformBackendBridgeResolverState*)context;
    if (bridge_out == NULL || bridge_context_out == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *bridge_out = NULL;
    *bridge_context_out = NULL;
    if (resolver == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    ++resolver->call_count;
    if (backend_family != resolver->expected_family)
        return RIN_GPU_ERROR_UNSUPPORTED;
    *bridge_out = resolver->bridge;
    *bridge_context_out = resolver->bridge_context;
    return RIN_GPU_OK;
}

static int test_platform_backend_command_canonicalization(void)
{
    RinGpuPlatformBackendBridgeV1 bridge = {0};
    RinGpuPlatformBackendAdapterV1 adapter = {0};
    PlatformBackendBridgeMock mock = {0};
    RinGpuBackendCommandV1 commands[RIN_GPU_BACKEND_COMMAND_LAST_V1];

    bridge.struct_size = sizeof(bridge);
    bridge.version = RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION;
    bridge.backend_family = RIN_GPU_BACKEND_FAMILY_INTEL;
    bridge.contract_version = RIN_GPU_BACKEND_CONTRACT_VERSION;
    bridge.adapter_version = RIN_GPU_OS_CORE_BACKEND_ADAPTER_VERSION;
    bridge.command_abi_version = RIN_GPU_BACKEND_COMMAND_ABI_VERSION;
    bridge.command_record_size = sizeof(RinGpuBackendCommandV1);
    bridge.invoke = platform_backend_bridge_invoke;
    mock.expected_commands = commands;
    mock.expected_command_count =
        RIN_GPU_BACKEND_COMMAND_LAST_V1;
    memset(commands, 0, sizeof(commands));
    for (uint32_t index = 0u;
         index < RIN_GPU_BACKEND_COMMAND_LAST_V1; ++index) {
        uint8_t* payload = (uint8_t*)&commands[index].value;
        commands[index].type = index + 1u;
        for (uint32_t byte = 0u; byte < sizeof(commands[index].value);
             ++byte) {
            payload[byte] =
                (uint8_t)(0x37u + index * 19u + byte * 23u);
        }
    }
    if (ringpu_platform_backend_adapter_init(
            &adapter, &bridge, &mock, RIN_GPU_BACKEND_FAMILY_INTEL) !=
            RIN_GPU_OK ||
        adapter.backend_ops.submit_commands == NULL ||
        adapter.backend_ops.submit_commands(
            &adapter, commands, RIN_GPU_BACKEND_COMMAND_LAST_V1) !=
            RIN_GPU_OK ||
        mock.submit_count != 1u)
        return 67;

    commands[0].type = 0u;
    if (adapter.backend_ops.submit_commands(
            &adapter, commands, RIN_GPU_BACKEND_COMMAND_LAST_V1) !=
            RIN_GPU_ERROR_PROTOCOL ||
        mock.submit_count != 1u)
        return 68;
    commands[0].type = RIN_GPU_BACKEND_COMMAND_COPY_BUFFER;
    commands[0].reserved = 1u;
    if (adapter.backend_ops.submit_commands(
            &adapter, commands, RIN_GPU_BACKEND_COMMAND_LAST_V1) !=
            RIN_GPU_ERROR_PROTOCOL ||
        mock.submit_count != 1u)
        return 69;
    mock.expected_commands = NULL;
    mock.expected_command_count = 0u;
    if (adapter.backend_ops.submit_commands(&adapter, NULL, 0u) !=
            RIN_GPU_OK ||
        mock.submit_count != 2u)
        return 70;
    return 0;
}

static int test_platform_backend_bridge(
    const RinGpuRuntimeDescV1* template_desc)
{
    RinGpuRuntimeDescV1 desc;
    RinGpuPlatformServicesV3 services = {0};
    RinGpuPlatformBackendBridgeV1 bridge = {0};
    PlatformBackendBridgeMock mock = {0};
    PlatformBackendBridgeResolverState resolver = {0};
    RinGpuRuntime* runtime = NULL;
    RinGpuBufferDescV1 buffer_desc = {0};
    RinGpuHandle buffer = 0u;
    uint8_t source[8] = {0x52u, 0x69u, 0x6eu, 0x47u,
                         0x50u, 0x55u, 0x31u, 0xa5u};
    uint8_t destination[sizeof(source)] = {0};

    if (template_desc == NULL) return 60;
    desc = *template_desc;
    desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS;
    desc.backend_ops = NULL;
    desc.backend_context = NULL;
    desc.backend_family = RIN_GPU_RUNTIME_BACKEND_FAMILY_INTEL;
    desc.present_callback = NULL;
    desc.present_context = NULL;
    desc.acquire_image = NULL;
    desc.image_context = NULL;
    memset(&desc.display, 0, sizeof(desc.display));

    bridge.struct_size = sizeof(bridge);
    bridge.version = RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION;
    bridge.backend_family = desc.backend_family;
    bridge.contract_version = RIN_GPU_BACKEND_CONTRACT_VERSION;
    bridge.adapter_version = RIN_GPU_OS_CORE_BACKEND_ADAPTER_VERSION;
    bridge.command_abi_version = RIN_GPU_BACKEND_COMMAND_ABI_VERSION;
    bridge.command_record_size = sizeof(RinGpuBackendCommandV1);
    bridge.callback_mask =
        RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_UPLOAD_BUFFER |
        RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_READBACK_BUFFER;
    bridge.invoke = platform_backend_bridge_invoke;
    resolver.bridge = &bridge;
    resolver.bridge_context = &mock;
    resolver.expected_family = desc.backend_family;
    services.struct_size = sizeof(services);
    services.version = RIN_GPU_PLATFORM_SERVICES_V3_VERSION;
    services.resolve_backend_bridge = platform_resolve_backend_bridge;
    services.backend_bridge_resolver_context = &resolver;

    bridge.command_record_size--;
    if (ringpu_runtime_create_with_platform_services_v3(
            &desc, &services, &runtime) != RIN_GPU_ERROR_PROTOCOL ||
        runtime != NULL || resolver.call_count != 1u ||
        mock.create_count != 0u)
        return 61;
    bridge.command_record_size++;

    if (ringpu_runtime_create_with_platform_services_v3(
            &desc, &services, &runtime) != RIN_GPU_OK || runtime == NULL ||
        resolver.call_count != 2u)
        return 62;
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = sizeof(source);
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    if (ringpu_runtime_create_buffer(runtime, &buffer_desc, &buffer) !=
            RIN_GPU_OK || buffer == 0u || mock.create_count != 1u)
        return 63;
    mock.fail_next_upload = 1u;
    if (ringpu_runtime_upload_buffer(runtime, buffer, 0u, source,
                                     sizeof(source)) !=
            RIN_GPU_ERROR_BACKEND || mock.upload_count != 1u)
        return 64;
    if (ringpu_runtime_upload_buffer(runtime, buffer, 0u, source,
                                     sizeof(source)) != RIN_GPU_OK ||
        ringpu_runtime_readback_buffer(runtime, buffer, 0u, destination,
                                       sizeof(destination)) != RIN_GPU_OK ||
        memcmp(source, destination, sizeof(source)) != 0 ||
        mock.upload_count != 2u || mock.readback_count != 1u)
        return 65;
    ringpu_runtime_destroy(runtime);
    if (mock.destroy_count != 1u) return 66;
    return 0;
}

static int external_create_buffer(
    void* context, const RinGpuBufferDescV1* desc, uint64_t* cookie_out)
{
    int result;
    ++external_create_buffer_calls;
    if (external_fail_next_create_buffer != 0u) {
        --external_fail_next_create_buffer;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_ERROR_BACKEND;
    }
    if (external_empty_success_next_create_buffer != 0u) {
        --external_empty_success_next_create_buffer;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_OK;
    }
    result = external_backend_delegate->create_buffer(context, desc,
                                                       cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (external_partial_fail_next_create_buffer != 0u) {
        --external_partial_fail_next_create_buffer;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static void external_destroy_buffer(void* context, uint64_t cookie)
{
    ++external_destroy_buffer_calls;
    external_backend_delegate->destroy_buffer(context, cookie);
}

static int external_create_image(
    void* context, const RinGpuImageDescV1* desc, uint64_t allocation_bytes,
    uint64_t* cookie_out)
{
    int result;
    ++external_create_image_calls;
    if (external_fail_next_create_image != 0u) {
        --external_fail_next_create_image;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_ERROR_BACKEND;
    }
    if (external_empty_success_next_create_image != 0u) {
        --external_empty_success_next_create_image;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_OK;
    }
    result = external_backend_delegate->create_image(
        context, desc, allocation_bytes, cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (external_partial_fail_next_create_image != 0u) {
        --external_partial_fail_next_create_image;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static void external_destroy_image(void* context, uint64_t cookie)
{
    ++external_destroy_image_calls;
    external_backend_delegate->destroy_image(context, cookie);
}

static int external_create_sampler(
    void* context, const RinGpuSamplerDescV1* desc, uint64_t* cookie_out)
{
    int result;
    ++external_create_sampler_calls;
    if (external_fail_next_create_sampler != 0u) {
        --external_fail_next_create_sampler;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_ERROR_BACKEND;
    }
    if (external_empty_success_next_create_sampler != 0u) {
        --external_empty_success_next_create_sampler;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_OK;
    }
    result = external_backend_delegate->create_sampler(context, desc,
                                                        cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (external_partial_fail_next_create_sampler != 0u) {
        --external_partial_fail_next_create_sampler;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static void external_destroy_sampler(void* context, uint64_t cookie)
{
    ++external_destroy_sampler_calls;
    external_backend_delegate->destroy_sampler(context, cookie);
}

static int external_create_memory(
    void* context, const RinGpuMemoryDescV1* desc, void** allocation_out)
{
    int result;
    ++external_create_memory_calls;
    if (external_fail_next_create_memory != 0u) {
        --external_fail_next_create_memory;
        if (allocation_out != NULL) *allocation_out = NULL;
        return RIN_GPU_ERROR_BACKEND;
    }
    if (external_empty_success_next_create_memory != 0u) {
        --external_empty_success_next_create_memory;
        if (allocation_out != NULL) *allocation_out = NULL;
        return RIN_GPU_OK;
    }
    result = external_backend_delegate->create_memory(context, desc,
                                                       allocation_out);
    if (result != RIN_GPU_OK) return result;
    if (external_partial_fail_next_create_memory != 0u) {
        --external_partial_fail_next_create_memory;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static void external_destroy_memory(void* context, void* allocation,
                                    uint64_t size_bytes)
{
    ++external_destroy_memory_calls;
    external_backend_delegate->destroy_memory(context, allocation,
                                               size_bytes);
}

static int external_bind_buffer_memory(
    void* context, uint64_t buffer_cookie, const RinGpuBufferDescV1* desc,
    void* allocation, uint64_t allocation_size, uint64_t offset_bytes,
    uint64_t* bound_cookie_out)
{
    int result;
    ++external_bind_buffer_memory_calls;
    if (external_empty_success_next_bind_buffer_memory != 0u) {
        --external_empty_success_next_bind_buffer_memory;
        if (bound_cookie_out != NULL) *bound_cookie_out = 0u;
        return RIN_GPU_OK;
    }
    if (external_fail_next_bind_buffer_memory != 0u) {
        --external_fail_next_bind_buffer_memory;
        if (bound_cookie_out != NULL) *bound_cookie_out = 0u;
        return RIN_GPU_ERROR_BACKEND;
    }
    result = external_backend_delegate->bind_buffer_memory(
        context, buffer_cookie, desc, allocation, allocation_size,
        offset_bytes, bound_cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (external_partial_fail_next_bind_buffer_memory != 0u) {
        --external_partial_fail_next_bind_buffer_memory;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static int external_bind_image_memory(
    void* context, uint64_t image_cookie, const RinGpuImageDescV1* desc,
    uint64_t resource_size, void* allocation, uint64_t allocation_size,
    uint64_t offset_bytes, uint64_t* bound_cookie_out)
{
    int result;
    ++external_bind_image_memory_calls;
    if (external_empty_success_next_bind_image_memory != 0u) {
        --external_empty_success_next_bind_image_memory;
        if (bound_cookie_out != NULL) *bound_cookie_out = 0u;
        return RIN_GPU_OK;
    }
    result = external_backend_delegate->bind_image_memory(
        context, image_cookie, desc, resource_size, allocation,
        allocation_size, offset_bytes, bound_cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (external_partial_fail_next_bind_image_memory != 0u) {
        --external_partial_fail_next_bind_image_memory;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static int present(void* context, const RinGpuSoftwarePresentedImageV1* image)
{
    if (image == NULL || image->pixels == NULL || context == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    (*(uint32_t*)context)++;
    return RIN_GPU_OK;
}

static int acquire(void* context, const RinGpuImageDescV1* descriptor,
                   uint64_t allocation_bytes,
                   RinGpuSoftwareExternalImageV1* storage)
{
    (void)context;
    (void)descriptor;
    (void)allocation_bytes;
    if (storage == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(storage, 0, sizeof(*storage));
    return RIN_GPU_OK;
}

static void platform_yield(void* context)
{
    if (context != NULL) ++*(uint32_t*)context;
}

static void platform_diagnostic(
    void* context, uint32_t type, uint64_t resource_cookie,
    uint64_t queue_cookie, uint64_t value0, uint64_t value1, int32_t status)
{
    PlatformDiagnosticState* state = (PlatformDiagnosticState*)context;
    if (state == NULL) return;
    ++state->event_count;
    state->last_type = type;
    state->last_resource_cookie = resource_cookie;
    state->last_queue_cookie = queue_cookie;
    state->last_value0 = value0;
    state->last_value1 = value1;
    state->last_status = status;
    if (type == RIN_GPU_PLATFORM_DIAGNOSTIC_SUBMISSION)
        ++state->submission_count;
    else if (type == RIN_GPU_PLATFORM_DIAGNOSTIC_PRESENT)
        ++state->present_count;
    else if (type == RIN_GPU_PLATFORM_DIAGNOSTIC_DEVICE_LOST)
        ++state->device_lost_count;
}

static int platform_resolve_backend(
    void* context, uint32_t backend_family,
    const struct RinGpuBackendOpsV1** backend_ops_out,
    void** backend_context_out)
{
    PlatformBackendResolverState* state =
        (PlatformBackendResolverState*)context;
    if (state == NULL || backend_ops_out == NULL ||
        backend_context_out == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    ++state->call_count;
    *backend_ops_out = NULL;
    *backend_context_out = NULL;
    if (backend_family != state->expected_family)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (state->result != RIN_GPU_OK) return state->result;
    *backend_ops_out = state->backend_ops;
    *backend_context_out = state->backend_context;
    return RIN_GPU_OK;
}

int main(void)
{
    RinGpuRuntimeSoftwareSurfaceDescV1 desc = {0};
    RinGpuRuntime* runtime = NULL;
    RinGpuQueueDescV1 queue_desc = {0};
    RinGpuCommandListDescV1 command_desc = {0};
    RinGpuImageDescV1 image_desc = {0};
    RinGpuImageDescV1 external_image_desc = {0};
    RinGpuSamplerDescV1 external_sampler_desc = {0};
    RinGpuImageTransitionV1 transition = {0};
    RinGpuImageOwnershipTransferV1 ownership_transfer = {0};
    RinGpuPresentV1 present_desc = {0};
    RinGpuSubmitInfoV1 submit = {0};
    RinGpuAdapterCapabilitiesV1 capabilities = {0};
    RinGpuRuntimeDescV1 headless_desc = {0};
    RinGpuRuntimeDescV1 external_desc = {0};
    RinGpuBufferDescV1 buffer_desc = {0};
    RinGpuMemoryDescV1 external_memory_desc = {0};
    RinGpuResourceMemoryBindingV1 external_buffer_binding = {0};
    RinGpuResourceMemoryBindingV1 external_image_binding = {0};
    RinGpuSoftwareBackendDescV4 external_backend_desc = {0};
    RinGpuSoftwareBackend* external_backend = NULL;
    RinGpuBackendOpsV1 external_ops;
    RinGpuBackendOpsV1 missing_ops;
    RinGpuPlatformServicesV2 external_services = {0};
    PlatformBackendResolverState backend_resolver = {0};
    RinGpuRuntime* external_runtime = NULL;
    RinGpuRuntime* rejected_runtime = NULL;
    RinGpuPlatformServicesV1 invalid_platform_services = {0};
    RinGpuMemoryDescV1 memory_desc = {0};
    RinGpuResourceMemoryBindingV1 memory_binding = {0};
    RinGpuQueueDescV1 headless_queue_desc = {0};
    RinGpuCommandListDescV1 headless_command_desc = {0};
    RinGpuSubmitInfoV1 headless_submit = {0};
    RinGpuPresentV1 headless_present = {0};
    RinGpuHandle queue = 0u;
    RinGpuHandle fence = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle present_queue = 0u;
    RinGpuHandle present_list = 0u;
    RinGpuHandle image = 0u;
    RinGpuHandle headless_source = 0u;
    RinGpuHandle headless_destination = 0u;
    RinGpuHandle headless_source_memory = 0u;
    RinGpuHandle headless_destination_memory = 0u;
    RinGpuHandle headless_queue = 0u;
    RinGpuHandle headless_command_list = 0u;
    RinGpuHandle headless_fence = 0u;
    RinGpuHandle external_buffer = 0u;
    RinGpuHandle failed_external_buffer = UINT64_C(1);
    RinGpuHandle recovered_external_buffer = 0u;
    RinGpuHandle external_image = 0u;
    RinGpuHandle failed_external_image = UINT64_C(1);
    RinGpuHandle external_sampler = 0u;
    RinGpuHandle failed_external_sampler = UINT64_C(1);
    RinGpuHandle external_memory = 0u;
    RinGpuHandle failed_external_memory = UINT64_C(1);
    uint64_t generation = 0u;
    uint32_t present_count = 0u;
    uint32_t platform_yield_count = 0u;
    RinGpuPlatformThreadSchedulerV1 platform_scheduler = {0};
    RinGpuPlatformServicesV1 platform_services = {0};
    PlatformDiagnosticState platform_diagnostics = {0};
    uint8_t upload_bytes[8] = {0x52u, 0x69u, 0x6eu, 0x47u,
                               0x50u, 0x55u, 0x01u, 0xa5u};
    uint8_t readback_bytes[sizeof(upload_bytes)] = {0u};

    desc.struct_size = sizeof(desc);
    desc.version = RIN_GPU_RUNTIME_VERSION;
    desc.device_generation = 1u;
    desc.handle_secret = UINT64_C(0x52554e54494d4531);
    desc.max_buffer_size = 1024u * 1024u;
    desc.max_image_size = 1024u * 1024u;
    desc.max_total_allocation_size = 4u * 1024u * 1024u;
    desc.max_image_dimension = 64u;
    desc.max_image_layers = 1u;
    desc.max_image_mip_levels = 1u;
    desc.max_image_sample_count = 1u;
    desc.flags = RIN_GPU_RUNTIME_FLAG_MEMORY_SUBALLOCATOR;
    desc.adapter.abi_version = RIN_GPU_ABI_VERSION;
    desc.adapter.struct_size = sizeof(desc.adapter);
    desc.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS |
                                      RIN_GPU_QUEUE_PRESENT;
    memcpy(desc.adapter.name, "runtime-test", 12u);
    desc.display.abi_version = RIN_GPU_ABI_VERSION;
    desc.display.struct_size = sizeof(desc.display);
    desc.display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    desc.display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    desc.display.width = 16u;
    desc.display.height = 16u;
    desc.display.refresh_millihertz = 60000u;
    desc.display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    desc.display.physical_width_mm = 1u;
    desc.display.physical_height_mm = 1u;
    desc.display.scale_milli = 1000u;
    memcpy(desc.display.name, "runtime-test", 12u);
    desc.present_callback = present;
    desc.present_context = &present_count;
    desc.acquire_image = acquire;
    platform_scheduler.struct_size = sizeof(platform_scheduler);
    platform_scheduler.version =
        RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION;
    platform_scheduler.yield_thread = platform_yield;
    platform_scheduler.context = &platform_yield_count;
    platform_services.struct_size = sizeof(platform_services);
    platform_services.version = RIN_GPU_PLATFORM_SERVICES_VERSION;
    platform_services.thread_scheduler = platform_scheduler;
    platform_services.diagnostic_callback = platform_diagnostic;
    platform_services.diagnostic_context = &platform_diagnostics;
    invalid_platform_services = platform_services;
    ++invalid_platform_services.version;
    if (ringpu_runtime_create_with_platform_services(
            &desc, &invalid_platform_services, &rejected_runtime) !=
            RIN_GPU_ERROR_INVALID_ARGUMENT ||
        rejected_runtime != NULL)
        return 43;
    if (ringpu_runtime_create_with_platform_services(
            &desc, &platform_services, &runtime) != RIN_GPU_OK)
        return 1;
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = UINT64_C(4096);
    memory_desc.alignment = UINT64_C(4096);
    if (ringpu_runtime_create_memory(runtime, &memory_desc,
                                     &headless_source_memory) != RIN_GPU_OK)
        return 41;
    if (platform_diagnostics.event_count != 1u ||
        platform_diagnostics.last_type !=
            RIN_GPU_PLATFORM_DIAGNOSTIC_RESOURCE_CREATE ||
        platform_diagnostics.last_resource_cookie == 0u ||
        platform_diagnostics.last_queue_cookie != 0u ||
        platform_diagnostics.last_value0 != RIN_GPU_OBJECT_MEMORY ||
        platform_diagnostics.last_value1 != memory_desc.size_bytes ||
        platform_diagnostics.last_status != RIN_GPU_OK)
        return 42;
    if (ringpu_runtime_destroy_object(runtime, headless_source_memory) !=
        RIN_GPU_OK)
        return 41;
    headless_source_memory = 0u;
    if (ringpu_runtime_get_device_generation(runtime, &generation) !=
            RIN_GPU_OK ||
        generation != desc.device_generation ||
        ringpu_runtime_device_lost(runtime))
        return 6;
    if (ringpu_runtime_get_adapter_capabilities(runtime, &capabilities) !=
            RIN_GPU_OK ||
        capabilities.abi_version != RIN_GPU_ABI_VERSION ||
        capabilities.struct_size != sizeof(capabilities) ||
        capabilities.queue_capabilities != desc.adapter.queue_capabilities ||
        capabilities.feature_flags != RIN_GPU_ADAPTER_KNOWN_FEATURES ||
        capabilities.max_buffer_size != desc.max_buffer_size ||
        capabilities.max_image_size != desc.max_image_size ||
        capabilities.max_total_allocation_size !=
            desc.max_total_allocation_size ||
        capabilities.max_image_dimension != desc.max_image_dimension ||
        capabilities.max_image_layers != desc.max_image_layers ||
        capabilities.max_image_mip_levels != desc.max_image_mip_levels ||
        capabilities.max_image_sample_count != desc.max_image_sample_count)
        return 10;

    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    if (ringpu_runtime_create_queue(runtime, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_runtime_create_command_list(runtime, &command_desc,
                                           &command_list) != RIN_GPU_OK)
        return 2;
    if (ringpu_runtime_create_fence(runtime, 1u, &fence) != RIN_GPU_OK ||
        ringpu_runtime_wait_fence(runtime, fence, 1u, 0u) != RIN_GPU_OK)
        return 8;

    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    image_desc.width = 16u;
    image_desc.height = 16u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COLOR_TARGET | RIN_GPU_IMAGE_PRESENT;
    if (ringpu_runtime_create_image(runtime, &image_desc, &image) != RIN_GPU_OK)
        return 3;
    if (ringpu_runtime_readback_image(runtime, image, NULL, NULL, 0u) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT)
        return 9;

    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transition.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    if (ringpu_runtime_command_transition_image(runtime, command_list, image,
                                                &transition) != RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime, command_list) != RIN_GPU_OK)
        return 4;
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    if (ringpu_runtime_queue_submit(runtime, queue, &submit) != RIN_GPU_OK)
        return 5;

    if (ringpu_runtime_command_list_reset(runtime, command_list) != RIN_GPU_OK)
        return 11;
    transition.before_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    transition.after_state = RIN_GPU_IMAGE_STATE_PRESENT;
    if (ringpu_runtime_command_transition_image(runtime, command_list, image,
                                                &transition) != RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime, command_list) != RIN_GPU_OK ||
        ringpu_runtime_queue_submit(runtime, queue, &submit) != RIN_GPU_OK)
        return 12;

    queue_desc.capabilities = RIN_GPU_QUEUE_PRESENT;
    command_desc.capabilities = RIN_GPU_QUEUE_PRESENT;
    if (ringpu_runtime_create_queue(runtime, &queue_desc, &present_queue) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_command_list(runtime, &command_desc,
                                           &present_list) != RIN_GPU_OK)
        return 13;
    ownership_transfer.abi_version = RIN_GPU_ABI_VERSION;
    ownership_transfer.struct_size = sizeof(ownership_transfer);
    ownership_transfer.mip_level_count = 1u;
    ownership_transfer.array_layer_count = 1u;
    ownership_transfer.source_family_index = 0u;
    ownership_transfer.destination_family_index = 1u;
    ownership_transfer.before_state = RIN_GPU_IMAGE_STATE_PRESENT;
    ownership_transfer.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    if (ringpu_runtime_command_transfer_image_ownership(
            runtime, present_list, image, &ownership_transfer) !=
        RIN_GPU_ERROR_STATE)
        return 15;
    present_desc.abi_version = RIN_GPU_ABI_VERSION;
    present_desc.struct_size = sizeof(present_desc);
    present_desc.image = image;
    present_desc.display_id = RIN_GPU_PRIMARY_DISPLAY;
    submit.command_list = present_list;
    if (ringpu_runtime_command_present(runtime, present_list, &present_desc) !=
            RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime, present_list) != RIN_GPU_OK ||
        ringpu_runtime_queue_submit(runtime, present_queue, &submit) !=
            RIN_GPU_OK ||
        present_count != 1u || platform_diagnostics.submission_count == 0u ||
        platform_diagnostics.present_count == 0u)
        return 14;

    ringpu_runtime_mark_device_lost(runtime);
    if (!ringpu_runtime_device_lost(runtime) ||
        platform_diagnostics.device_lost_count != 1u ||
        platform_diagnostics.last_type !=
            RIN_GPU_PLATFORM_DIAGNOSTIC_DEVICE_LOST ||
        platform_diagnostics.last_status != RIN_GPU_ERROR_DEVICE_LOST)
        return 7;
    ringpu_runtime_destroy(runtime);

    headless_desc = desc;
    headless_desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS |
        RIN_GPU_RUNTIME_FLAG_MEMORY_SUBALLOCATOR;
    memset(&headless_desc.display, 0, sizeof(headless_desc.display));
    headless_desc.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS |
                                               RIN_GPU_QUEUE_COPY;
    headless_desc.present_callback = NULL;
    headless_desc.present_context = NULL;
    headless_desc.acquire_image = NULL;
    headless_desc.image_context = NULL;
    if (ringpu_runtime_create(&headless_desc, &runtime) != RIN_GPU_OK)
        return 16;
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = sizeof(upload_bytes);
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = UINT64_C(256);
    memory_desc.alignment = UINT64_C(256);
    if (ringpu_runtime_create_memory(runtime, &memory_desc,
                                     &headless_source_memory) != RIN_GPU_OK ||
        ringpu_runtime_create_buffer(runtime, &buffer_desc,
                                     &headless_source) != RIN_GPU_OK)
        return 17;
    memory_binding.abi_version = RIN_GPU_ABI_VERSION;
    memory_binding.struct_size = sizeof(memory_binding);
    memory_binding.memory = headless_source_memory;
    memory_binding.size_bytes = UINT64_C(256);
    if (ringpu_runtime_bind_buffer_memory(runtime, headless_source,
                                          &memory_binding) != RIN_GPU_OK)
        return 19;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_DESTINATION;
    if (ringpu_runtime_create_memory(runtime, &memory_desc,
                                     &headless_destination_memory) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_buffer(runtime, &buffer_desc,
                                     &headless_destination) != RIN_GPU_OK)
        return 20;
    memory_binding.memory = headless_destination_memory;
    if (ringpu_runtime_bind_buffer_memory(runtime, headless_destination,
                                          &memory_binding) != RIN_GPU_OK ||
        ringpu_runtime_upload_buffer(runtime, headless_source, 0u,
                                     upload_bytes, sizeof(upload_bytes)) !=
            RIN_GPU_OK ||
        ringpu_runtime_upload_buffer(runtime, headless_destination, 0u,
                                     readback_bytes,
                                     sizeof(readback_bytes)) != RIN_GPU_OK)
        return 21;
    headless_queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    headless_queue_desc.struct_size = sizeof(headless_queue_desc);
    headless_queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    headless_command_desc.abi_version = RIN_GPU_ABI_VERSION;
    headless_command_desc.struct_size = sizeof(headless_command_desc);
    headless_command_desc.capabilities = RIN_GPU_QUEUE_COPY;
    if (ringpu_runtime_create_queue(runtime, &headless_queue_desc,
                                    &headless_queue) != RIN_GPU_OK ||
        ringpu_runtime_create_command_list(runtime, &headless_command_desc,
                                           &headless_command_list) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_fence(runtime, 0u, &headless_fence) !=
            RIN_GPU_OK ||
        ringpu_runtime_command_copy_buffer(
            runtime, headless_command_list, headless_destination, 0u,
            headless_source, 0u, sizeof(upload_bytes)) != RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime,
                                          headless_command_list) !=
            RIN_GPU_OK)
        return 22;
    headless_submit.abi_version = RIN_GPU_ABI_VERSION;
    headless_submit.struct_size = sizeof(headless_submit);
    headless_submit.command_list = headless_command_list;
    headless_submit.signal_fence = headless_fence;
    headless_submit.signal_value = 1u;
    if (ringpu_runtime_queue_submit(runtime, headless_queue,
                                    &headless_submit) != RIN_GPU_OK ||
        ringpu_runtime_wait_fence(runtime, headless_fence, 1u, 0u) !=
            RIN_GPU_OK ||
        ringpu_runtime_readback_buffer(runtime, headless_destination, 0u,
                                       readback_bytes,
                                       sizeof(readback_bytes)) != RIN_GPU_OK ||
        memcmp(upload_bytes, readback_bytes, sizeof(upload_bytes)) != 0)
        return 23;
    headless_present.abi_version = RIN_GPU_ABI_VERSION;
    headless_present.struct_size = sizeof(headless_present);
    headless_present.display_id = RIN_GPU_PRIMARY_DISPLAY;
    if (ringpu_runtime_command_present(runtime, 0u, &headless_present) !=
        RIN_GPU_ERROR_UNSUPPORTED)
        return 18;
    ringpu_runtime_destroy(runtime);

    external_backend_desc.base.base.base.struct_size =
        sizeof(external_backend_desc);
    external_backend_desc.base.base.base.version =
        RIN_GPU_SOFTWARE_BACKEND_VERSION_4;
    external_backend_desc.base.base.base.max_total_bytes =
        headless_desc.max_total_allocation_size;
    external_backend_desc.base.base.base.flags =
        RIN_GPU_SOFTWARE_BACKEND_FLAG_HEADLESS;
    if (ringpu_software_backend_create(
            &external_backend_desc.base.base.base, &external_backend) !=
        RIN_GPU_OK)
        return 24;
    external_backend_delegate = ringpu_software_backend_ops();
    external_ops = *external_backend_delegate;
    external_ops.create_buffer = external_create_buffer;
    external_ops.destroy_buffer = external_destroy_buffer;
    external_ops.create_image = external_create_image;
    external_ops.destroy_image = external_destroy_image;
    external_ops.create_sampler = external_create_sampler;
    external_ops.destroy_sampler = external_destroy_sampler;
    external_ops.create_memory = external_create_memory;
    external_ops.destroy_memory = external_destroy_memory;
    external_ops.bind_buffer_memory = external_bind_buffer_memory;
    external_ops.bind_image_memory = external_bind_image_memory;
    external_desc = headless_desc;
    external_desc.backend_ops = &external_ops;
    external_desc.backend_context = external_backend;
    external_desc.backend_family = RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO;
    external_desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS |
                          RIN_GPU_RUNTIME_FLAG_MEMORY_SUBALLOCATOR;
    if (ringpu_runtime_create(&external_desc, &rejected_runtime) !=
            RIN_GPU_ERROR_INVALID_ARGUMENT || rejected_runtime != NULL)
        return 42;
    external_desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS;
    external_desc.backend_ops = NULL;
    external_desc.backend_context = NULL;
    external_services.struct_size = sizeof(external_services);
    external_services.version = RIN_GPU_PLATFORM_SERVICES_V2_VERSION;
    external_services.resolve_backend = platform_resolve_backend;
    external_services.backend_resolver_context = &backend_resolver;
    backend_resolver.backend_ops = &external_ops;
    backend_resolver.backend_context = external_backend;
    backend_resolver.expected_family = RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO;
    backend_resolver.backend_ops = NULL;
    backend_resolver.backend_context = external_backend;
    backend_resolver.result = RIN_GPU_OK;
    if (ringpu_runtime_create_with_platform_services_v2(
            &external_desc, &external_services, &rejected_runtime) !=
            RIN_GPU_ERROR_BACKEND || rejected_runtime != NULL ||
        external_create_buffer_calls != 0u)
        return 45;
    backend_resolver.backend_ops = &external_ops;
    backend_resolver.result = RIN_GPU_ERROR_BACKEND;
    if (ringpu_runtime_create_with_platform_services_v2(
            &external_desc, &external_services, &rejected_runtime) !=
            RIN_GPU_ERROR_BACKEND || rejected_runtime != NULL ||
        external_create_buffer_calls != 0u)
        return 44;
    backend_resolver.result = RIN_GPU_OK;
    if (ringpu_runtime_create_with_platform_services_v2(
            &external_desc, &external_services, &external_runtime) !=
            RIN_GPU_OK || backend_resolver.call_count != 3u)
        return 25;
    external_ops.create_buffer = NULL;
    if (ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                     &external_buffer) != RIN_GPU_OK ||
        external_create_buffer_calls != 1u || external_buffer == 0u)
        return 25;
    external_ops.create_buffer = external_create_buffer;

    external_empty_success_next_create_buffer = 1u;
    if (ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                    &failed_external_buffer) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_buffer != 0u || external_create_buffer_calls != 2u)
        return 53;
    external_fail_next_create_buffer = 1u;
    if (ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                     &failed_external_buffer) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_buffer != 0u || external_create_buffer_calls != 3u)
        return 26;
    external_partial_fail_next_create_buffer = 1u;
    if (ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                     &failed_external_buffer) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_buffer != 0u || external_create_buffer_calls != 4u ||
        external_destroy_buffer_calls != 1u)
        return 46;
    if (ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                     &recovered_external_buffer) !=
            RIN_GPU_OK ||
        recovered_external_buffer == 0u ||
        external_create_buffer_calls != 5u)
        return 33;

    external_image_desc = image_desc;
    external_image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
                                RIN_GPU_IMAGE_COPY_DESTINATION;
    external_empty_success_next_create_image = 1u;
    if (ringpu_runtime_create_image(external_runtime, &external_image_desc,
                                   &failed_external_image) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_image != 0u || external_create_image_calls != 1u)
        return 54;
    external_fail_next_create_image = 1u;
    if (ringpu_runtime_create_image(external_runtime, &external_image_desc,
                                   &failed_external_image) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_image != 0u || external_create_image_calls != 2u)
        return 29;
    external_partial_fail_next_create_image = 1u;
    if (ringpu_runtime_create_image(external_runtime, &external_image_desc,
                                   &failed_external_image) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_image != 0u || external_create_image_calls != 3u ||
        external_destroy_image_calls != 1u)
        return 47;
    if (ringpu_runtime_create_image(external_runtime, &external_image_desc,
                                    &external_image) != RIN_GPU_OK ||
        external_image == 0u || external_create_image_calls != 4u)
        return 30;

    external_sampler_desc.abi_version = RIN_GPU_ABI_VERSION;
    external_sampler_desc.struct_size = sizeof(external_sampler_desc);
    external_sampler_desc.min_filter = RIN_GPU_SAMPLER_FILTER_NEAREST;
    external_sampler_desc.mag_filter = RIN_GPU_SAMPLER_FILTER_NEAREST;
    external_sampler_desc.mip_filter = RIN_GPU_SAMPLER_MIP_FILTER_NONE;
    external_sampler_desc.address_u = RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
    external_sampler_desc.address_v = RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
    external_sampler_desc.address_w = RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
    external_sampler_desc.max_anisotropy = 1u;
    external_empty_success_next_create_sampler = 1u;
    if (ringpu_runtime_create_sampler(external_runtime,
                                     &external_sampler_desc,
                                     &failed_external_sampler) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_sampler != 0u || external_create_sampler_calls != 1u)
        return 55;
    external_fail_next_create_sampler = 1u;
    if (ringpu_runtime_create_sampler(external_runtime,
                                     &external_sampler_desc,
                                     &failed_external_sampler) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_sampler != 0u || external_create_sampler_calls != 2u)
        return 34;
    external_partial_fail_next_create_sampler = 1u;
    if (ringpu_runtime_create_sampler(external_runtime,
                                     &external_sampler_desc,
                                     &failed_external_sampler) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_sampler != 0u || external_create_sampler_calls != 3u ||
        external_destroy_sampler_calls != 1u)
        return 48;
    if (ringpu_runtime_create_sampler(external_runtime,
                                     &external_sampler_desc,
                                     &external_sampler) != RIN_GPU_OK ||
        external_sampler == 0u || external_create_sampler_calls != 4u)
        return 35;

    external_memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    external_memory_desc.struct_size = sizeof(external_memory_desc);
    external_memory_desc.size_bytes = 8192u;
    external_memory_desc.alignment = 4096u;
    external_empty_success_next_create_memory = 1u;
    if (ringpu_runtime_create_memory(external_runtime, &external_memory_desc,
                                    &failed_external_memory) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_memory != 0u || external_create_memory_calls != 1u)
        return 56;
    external_fail_next_create_memory = 1u;
    if (ringpu_runtime_create_memory(external_runtime, &external_memory_desc,
                                    &failed_external_memory) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_memory != 0u || external_create_memory_calls != 2u)
        return 36;
    external_partial_fail_next_create_memory = 1u;
    if (ringpu_runtime_create_memory(external_runtime, &external_memory_desc,
                                    &failed_external_memory) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_memory != 0u || external_create_memory_calls != 3u ||
        external_destroy_memory_calls != 1u)
        return 49;
    if (ringpu_runtime_create_memory(external_runtime, &external_memory_desc,
                                    &external_memory) != RIN_GPU_OK ||
        external_memory == 0u || external_create_memory_calls != 4u)
        return 37;
    external_buffer_binding.abi_version = RIN_GPU_ABI_VERSION;
    external_buffer_binding.struct_size = sizeof(external_buffer_binding);
    external_buffer_binding.memory = external_memory;
    external_buffer_binding.size_bytes = 256u;
    external_empty_success_next_bind_buffer_memory = 1u;
    if (ringpu_runtime_bind_buffer_memory(external_runtime, external_buffer,
                                         &external_buffer_binding) !=
            RIN_GPU_ERROR_BACKEND ||
        external_bind_buffer_memory_calls != 1u ||
        external_destroy_buffer_calls != 1u)
        return 57;
    external_fail_next_bind_buffer_memory = 1u;
    if (ringpu_runtime_bind_buffer_memory(external_runtime, external_buffer,
                                         &external_buffer_binding) !=
            RIN_GPU_ERROR_BACKEND ||
        external_bind_buffer_memory_calls != 2u ||
        external_destroy_buffer_calls != 1u)
        return 39;
    external_partial_fail_next_bind_buffer_memory = 1u;
    if (ringpu_runtime_bind_buffer_memory(external_runtime, external_buffer,
                                         &external_buffer_binding) !=
            RIN_GPU_ERROR_BACKEND ||
        external_bind_buffer_memory_calls != 3u ||
        external_destroy_buffer_calls != 2u)
        return 50;
    if (ringpu_runtime_bind_buffer_memory(external_runtime, external_buffer,
                                         &external_buffer_binding) !=
            RIN_GPU_OK ||
        external_bind_buffer_memory_calls != 4u ||
        external_destroy_buffer_calls != 3u)
        return 40;

    external_image_binding.abi_version = RIN_GPU_ABI_VERSION;
    external_image_binding.struct_size = sizeof(external_image_binding);
    external_image_binding.memory = external_memory;
    external_image_binding.offset_bytes = 4096u;
    external_image_binding.size_bytes = 4096u;
    external_empty_success_next_bind_image_memory = 1u;
    if (ringpu_runtime_bind_image_memory(external_runtime, external_image,
                                         &external_image_binding) !=
            RIN_GPU_ERROR_BACKEND ||
        external_bind_image_memory_calls != 1u ||
        external_destroy_image_calls != 1u)
        return 58;
    external_partial_fail_next_bind_image_memory = 1u;
    if (ringpu_runtime_bind_image_memory(external_runtime, external_image,
                                         &external_image_binding) !=
            RIN_GPU_ERROR_BACKEND ||
        external_bind_image_memory_calls != 2u ||
        external_destroy_image_calls != 2u)
        return 51;
    if (ringpu_runtime_bind_image_memory(external_runtime, external_image,
                                         &external_image_binding) !=
            RIN_GPU_OK ||
        external_bind_image_memory_calls != 3u ||
        external_destroy_image_calls != 3u)
        return 52;

    missing_ops = external_ops;
    missing_ops.create_buffer = NULL;
    external_desc.backend_ops = &missing_ops;
    if (ringpu_runtime_create(&external_desc, &rejected_runtime) !=
            RIN_GPU_ERROR_INVALID_ARGUMENT ||
        rejected_runtime != NULL || external_create_buffer_calls != 5u)
        return 31;

    ringpu_runtime_destroy(external_runtime);
    if (external_destroy_buffer_calls != 5u ||
        external_destroy_image_calls != 4u ||
        external_destroy_sampler_calls != 2u ||
        external_destroy_memory_calls != 2u) {
        ringpu_software_backend_destroy(external_backend);
        return 32;
    }
    ringpu_software_backend_destroy(external_backend);
    {
        const int bridge_result =
            test_platform_backend_bridge(&headless_desc);
        if (bridge_result != 0) return bridge_result;
    }
    {
        const int command_translation_result =
            test_platform_backend_command_canonicalization();
        if (command_translation_result != 0)
            return command_translation_result;
    }
    return 0;
}
