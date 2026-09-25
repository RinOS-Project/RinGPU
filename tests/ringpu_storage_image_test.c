/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackendDescV1 backend_desc = {0};
    RinGpuDisplayInfoV1 display = {0};
    RinGpuCoreConfigV1 config = {0};
    RinGpuSoftwareBackend* backend = NULL;
    backend_desc.struct_size = sizeof(backend_desc);
    backend_desc.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend_desc.max_total_bytes = 4u * 1024u * 1024u;
    if (ringpu_software_backend_create(&backend_desc, &backend) != RIN_GPU_OK)
        return 0;
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 1u;
    display.height = 1u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "storage", 8u);
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x53544f5241474531);
    config.max_buffer_size = 4096u;
    config.max_image_size = 4096u;
    config.max_total_allocation_size = 2u * 1024u * 1024u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 1u;
    config.max_image_mip_levels = 1u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS;
    memcpy(config.adapter.name, "storage", 8u);
    config.backend = *ringpu_software_backend_ops();
    config.backend_context = backend;
    config.displays = &display;
    config.display_count = 1u;
    config.backend_family = RIN_GPU_BACKEND_FAMILY_SOFTWARE;
    if (ringpu_core_init(core, &config) != RIN_GPU_OK) {
        ringpu_software_backend_destroy(backend);
        return 0;
    }
    *backend_out = backend;
    return 1;
}

static void set_instruction(RinShaderInstructionV1* instruction,
                            uint16_t opcode, uint16_t destination,
                            uint16_t source0, uint16_t source1,
                            uint16_t resource, uint32_t immediate)
{
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->destination = destination;
    instruction->source0 = source0;
    instruction->source1 = source1;
    instruction->resource = resource;
    instruction->immediate = immediate;
}

static void make_vertex_shader(uint8_t* storage)
{
    RinShaderHeaderV1* header = (RinShaderHeaderV1*)storage;
    RinShaderInstructionV1* instructions =
        (RinShaderInstructionV1*)(storage + sizeof(*header));
    static const uint32_t bits[] = {
        UINT32_C(0), UINT32_C(0), UINT32_C(0), UINT32_C(0x3f800000)};
    memset(storage, 0, sizeof(RinShaderHeaderV1) +
           9u * sizeof(RinShaderInstructionV1));
    header->magic = RIN_SHADER_MAGIC;
    header->version = RIN_SHADER_IR_VERSION;
    header->header_size = sizeof(*header);
    header->total_size = sizeof(RinShaderHeaderV1) +
                         9u * sizeof(RinShaderInstructionV1);
    header->stage = RIN_SHADER_STAGE_VERTEX;
    header->instruction_count = 9u;
    header->register_count = 4u;
    header->output_count = 4u;
    for (uint16_t index = 0u; index < 4u; ++index) {
        set_instruction(&instructions[index], RIN_SHADER_OP_CONST_F32, index,
                        RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                        RIN_SHADER_UNUSED, bits[index]);
        set_instruction(&instructions[index + 4u],
                        RIN_SHADER_OP_STORE_OUTPUT_F32, RIN_SHADER_UNUSED,
                        index, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, index);
    }
    set_instruction(&instructions[8], RIN_SHADER_OP_RETURN, RIN_SHADER_UNUSED,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u);
}

static void make_fragment_shader(uint8_t* storage)
{
    RinShaderHeaderV1* header = (RinShaderHeaderV1*)storage;
    RinShaderInstructionV1* instructions =
        (RinShaderInstructionV1*)(storage + sizeof(*header));
    static const uint32_t bits[] = {
        UINT32_C(0), UINT32_C(0), UINT32_C(123),
        UINT32_C(0x3f800000), UINT32_C(0), UINT32_C(0), UINT32_C(0x3f800000)};
    memset(storage, 0, sizeof(RinShaderHeaderV1) +
           13u * sizeof(RinShaderInstructionV1));
    header->magic = RIN_SHADER_MAGIC;
    header->version = RIN_SHADER_IR_VERSION;
    header->header_size = sizeof(*header);
    header->total_size = sizeof(RinShaderHeaderV1) +
                         13u * sizeof(RinShaderInstructionV1);
    header->stage = RIN_SHADER_STAGE_FRAGMENT;
    header->instruction_count = 13u;
    header->register_count = 7u;
    header->output_count = 4u;
    header->resource_count = 1u;
    for (uint16_t index = 0u; index < 3u; ++index)
        set_instruction(&instructions[index], RIN_SHADER_OP_CONST_I32, index,
                        RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                        RIN_SHADER_UNUSED, bits[index]);
    set_instruction(&instructions[3], RIN_SHADER_OP_STORE_IMAGE_2D_I32, 2u,
                    0u, 1u, 0u, 0u);
    for (uint16_t index = 0u; index < 4u; ++index) {
        set_instruction(&instructions[index + 4u], RIN_SHADER_OP_CONST_F32,
                        index + 3u, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                        RIN_SHADER_UNUSED, bits[index + 3u]);
        set_instruction(&instructions[index + 8u],
                        RIN_SHADER_OP_STORE_OUTPUT_F32, RIN_SHADER_UNUSED,
                        index + 3u, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, index);
    }
    set_instruction(&instructions[12], RIN_SHADER_OP_RETURN, RIN_SHADER_UNUSED,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u);
}

int main(void)
{
    uint8_t vertex_shader[sizeof(RinShaderHeaderV1) +
                         9u * sizeof(RinShaderInstructionV1)];
    uint8_t fragment_shader[sizeof(RinShaderHeaderV1) +
                           13u * sizeof(RinShaderInstructionV1)];
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuGraphicsPipelineNativeDescV2 pipeline_desc = {0};
    RinGpuImageDescV1 storage_desc = {0};
    RinGpuImageDescV1 color_desc = {0};
    RinGpuImageTransitionV1 transition = {0};
    RinGpuRenderPassDescV1 render_pass = {0};
    RinGpuGraphicsBindingV1 binding = {0};
    RinGpuDrawV1 draw = {0};
    RinGpuQueueDescV1 queue_desc = {0};
    RinGpuCommandListDescV1 command_desc = {0};
    RinGpuSubmitInfoV1 submit = {0};
    RinGpuImageReadbackV1 readback = {0};
    RinGpuHandle vertex_module = 0u, fragment_module = 0u;
    RinGpuHandle pipeline = 0u, storage_image = 0u, color_image = 0u;
    RinGpuHandle bind_group = 0u, queue = 0u, command_list = 0u, fence = 0u;
    uint8_t value = 0u;
    const char* failure_stage = "initialization";
    int result = 1;

    make_vertex_shader(vertex_shader);
    make_fragment_shader(fragment_shader);
    if (!make_core(&core, &backend)) return 1;
    failure_stage = "shader modules";
    if (ringpu_create_shader_module(&core, vertex_shader, sizeof(vertex_shader),
                                    &vertex_module) != RIN_GPU_OK ||
        ringpu_create_shader_module(&core, fragment_shader,
                                    sizeof(fragment_shader), &fragment_module) !=
            RIN_GPU_OK)
        goto done;
    pipeline_desc.base.abi_version = RIN_GPU_ABI_VERSION;
    pipeline_desc.base.struct_size = sizeof(pipeline_desc);
    pipeline_desc.base.vertex_shader = vertex_module;
    pipeline_desc.base.fragment_shader = fragment_module;
    pipeline_desc.base.color_format = RIN_GPU_FORMAT_BGRA8_UNORM;
    pipeline_desc.base.primitive_topology = RIN_GPU_PRIMITIVE_POINT_LIST;
    pipeline_desc.base.position_output_location = 0u;
    pipeline_desc.base.color_write_mask = RIN_GPU_COLOR_WRITE_ALL;
    pipeline_desc.base.cull_mode = RIN_GPU_CULL_NONE;
    pipeline_desc.base.front_face = RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE;
    failure_stage = "graphics pipeline";
    {
        int pipeline_result = ringpu_create_graphics_pipeline_native_v2(
            &core, &pipeline_desc, NULL, 0u, NULL, 0u, &pipeline);
        if (pipeline_result != RIN_GPU_OK) {
            fprintf(stderr, "graphics pipeline error %d\n", pipeline_result);
            goto done;
        }
    }
    storage_desc.abi_version = RIN_GPU_ABI_VERSION;
    storage_desc.struct_size = sizeof(storage_desc);
    storage_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    storage_desc.format = RIN_GPU_FORMAT_R8_UNORM;
    storage_desc.width = 1u;
    storage_desc.height = 1u;
    storage_desc.depth = 1u;
    storage_desc.array_layers = 1u;
    storage_desc.mip_levels = 1u;
    storage_desc.sample_count = 1u;
    storage_desc.usage = RIN_GPU_IMAGE_STORAGE | RIN_GPU_IMAGE_COPY_SOURCE;
    storage_desc.flags = RIN_GPU_IMAGE_CPU_READABLE;
    color_desc = storage_desc;
    color_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    color_desc.usage = RIN_GPU_IMAGE_COLOR_TARGET | RIN_GPU_IMAGE_COPY_SOURCE;
    failure_stage = "images";
    if (ringpu_create_image(&core, &storage_desc, &storage_image) != RIN_GPU_OK ||
        ringpu_create_image(&core, &color_desc, &color_image) != RIN_GPU_OK)
        goto done;
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.binding = 0u;
    binding.kind = RIN_SHADER_RESOURCE_STORAGE_IMAGE;
    binding.access = RIN_GPU_RESOURCE_WRITE;
    binding.resource = storage_image;
    failure_stage = "bind group";
    if (ringpu_create_graphics_bind_group_typed(&core, pipeline, &binding, 1u,
                                                &bind_group) != RIN_GPU_OK)
        goto done;
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    failure_stage = "queue objects";
    if (ringpu_create_queue(&core, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc, &command_list) !=
            RIN_GPU_OK || ringpu_create_fence(&core, 0u, &fence) != RIN_GPU_OK)
        goto done;
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transition.after_state = RIN_GPU_IMAGE_STATE_SHADER_READ;
    failure_stage = "storage transition";
    if (ringpu_command_transition_image(&core, command_list, storage_image,
                                        &transition) != RIN_GPU_OK)
        goto done;
    transition.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    failure_stage = "color transition";
    if (ringpu_command_transition_image(&core, command_list, color_image,
                                        &transition) != RIN_GPU_OK)
        goto done;
    render_pass.abi_version = RIN_GPU_ABI_VERSION;
    render_pass.struct_size = sizeof(render_pass);
    render_pass.color_target = color_image;
    render_pass.load_op = RIN_GPU_RENDER_CLEAR;
    render_pass.store_op = RIN_GPU_RENDER_STORE;
    render_pass.clear_red = 0.0f;
    render_pass.clear_green = 0.0f;
    render_pass.clear_blue = 0.0f;
    render_pass.clear_alpha = 1.0f;
    failure_stage = "render pass and bindings";
    if (ringpu_command_begin_render_pass(&core, command_list, &render_pass) !=
            RIN_GPU_OK ||
        ringpu_command_bind_graphics_resources(&core, command_list, bind_group) !=
            RIN_GPU_OK)
        goto done;
    draw.abi_version = RIN_GPU_ABI_VERSION;
    draw.struct_size = sizeof(draw);
    draw.pipeline = pipeline;
    draw.color_target = color_image;
    draw.vertex_count = 1u;
    draw.instance_count = 1u;
    failure_stage = "draw";
    if (ringpu_command_draw(&core, command_list, &draw) != RIN_GPU_OK ||
        ringpu_command_end_render_pass(&core, command_list) != RIN_GPU_OK)
        goto done;
    transition.before_state = RIN_GPU_IMAGE_STATE_SHADER_READ;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    failure_stage = "storage readback transition";
    if (ringpu_command_transition_image(&core, command_list, storage_image,
                                        &transition) != RIN_GPU_OK)
        goto done;
    transition.before_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    failure_stage = "close";
    if (ringpu_command_transition_image(&core, command_list, color_image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, command_list) != RIN_GPU_OK)
        goto done;
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    submit.signal_fence = fence;
    submit.signal_value = 1u;
    failure_stage = "submit";
    {
        int submit_result = ringpu_queue_submit(&core, queue, &submit);
        if (submit_result != RIN_GPU_OK) {
            fprintf(stderr, "submit error %d\n", submit_result);
            goto done;
        }
        submit_result = ringpu_wait_fence(&core, fence, 1u, 0u);
        if (submit_result != RIN_GPU_OK) {
            fprintf(stderr, "fence error %d\n", submit_result);
            goto done;
        }
    }
    readback.abi_version = RIN_GPU_ABI_VERSION;
    readback.struct_size = sizeof(readback);
    readback.width = 1u;
    readback.height = 1u;
    readback.depth = 1u;
    failure_stage = "storage readback";
    {
        int readback_result = ringpu_readback_image(
            &core, storage_image, &readback, &value, sizeof(value));
        if (readback_result != RIN_GPU_OK || value != 123u) {
            fprintf(stderr, "readback error %d value %u\n", readback_result,
                    (unsigned)value);
            goto done;
        }
    }
    result = 0;
done:
    if (result != 0)
        fprintf(stderr, "storage image test failed at %s\n", failure_stage);
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return result;
}
