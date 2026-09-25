/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackendDescV1 backend_desc;
    RinGpuDisplayInfoV1 display;
    RinGpuCoreConfigV1 config;
    RinGpuSoftwareBackend* backend = NULL;

    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.struct_size = sizeof(backend_desc);
    backend_desc.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend_desc.max_total_bytes = UINT64_C(4) * 1024u * 1024u;
    if (ringpu_software_backend_create(&backend_desc, &backend) != RIN_GPU_OK ||
        !backend) {
        return 0;
    }
    memset(&display, 0, sizeof(display));
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 2u;
    display.height = 2u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "restart", 7u);

    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x5052494d52455354);
    config.max_buffer_size = 1024u;
    config.max_image_size = 1024u;
    config.max_total_allocation_size = 2u * 1024u * 1024u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 1u;
    config.max_image_mip_levels = 1u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS;
    memcpy(config.adapter.name, "restart", 7u);
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
                            uint16_t source0, uint32_t immediate)
{
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->destination = destination;
    instruction->source0 = source0;
    instruction->source1 = RIN_SHADER_UNUSED;
    instruction->resource = RIN_SHADER_UNUSED;
    instruction->immediate = immediate;
}

static void make_constant_shader(uint8_t* storage, uint32_t stage,
                                 int use_push_constant)
{
    RinShaderHeaderV1* header = (RinShaderHeaderV1*)storage;
    RinShaderInstructionV1* instructions =
        (RinShaderInstructionV1*)(storage + sizeof(*header));
    static const uint32_t colors[4] = {
        UINT32_C(0x00000000), UINT32_C(0x00000000),
        UINT32_C(0x3f800000), UINT32_C(0x3f800000)};

    memset(storage, 0, sizeof(RinShaderHeaderV1) + 9u *
           sizeof(RinShaderInstructionV1));
    header->magic = RIN_SHADER_MAGIC;
    header->version = RIN_SHADER_IR_VERSION;
    header->header_size = sizeof(*header);
    header->total_size = sizeof(RinShaderHeaderV1) +
                         9u * sizeof(RinShaderInstructionV1);
    header->stage = stage;
    header->flags = use_push_constant ? RIN_SHADER_FLAG_PUSH_CONSTANTS : 0u;
    header->instruction_count = 9u;
    header->register_count = 4u;
    header->input_count = stage == RIN_SHADER_STAGE_VERTEX ? 1u : 0u;
    header->output_count = 4u;
    header->entry_instruction = 0u;
    for (uint16_t component = 0u; component < 4u; ++component) {
        if (use_push_constant && component == 2u) {
            set_instruction(&instructions[component],
                            RIN_SHADER_OP_LOAD_PUSH_CONSTANT_F32, component,
                            RIN_SHADER_UNUSED, 0u);
        } else {
            set_instruction(&instructions[component], RIN_SHADER_OP_CONST_F32,
                            component, RIN_SHADER_UNUSED, colors[component]);
        }
        set_instruction(&instructions[4u + component],
                        RIN_SHADER_OP_STORE_OUTPUT_F32, RIN_SHADER_UNUSED,
                        component, component);
    }
    set_instruction(&instructions[8u], RIN_SHADER_OP_RETURN,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u);
}

static void make_mrt_fragment_shader(uint8_t* storage)
{
    RinShaderHeaderV1* header = (RinShaderHeaderV1*)storage;
    RinShaderInstructionV1* instructions =
        (RinShaderInstructionV1*)(storage + sizeof(*header));
    static const uint32_t colors[8] = {
        UINT32_C(0x3f800000), UINT32_C(0x00000000),
        UINT32_C(0x00000000), UINT32_C(0x3f800000),
        UINT32_C(0x00000000), UINT32_C(0x3f800000),
        UINT32_C(0x00000000), UINT32_C(0x3f800000)};

    memset(storage, 0, sizeof(RinShaderHeaderV1) + 17u *
           sizeof(RinShaderInstructionV1));
    header->magic = RIN_SHADER_MAGIC;
    header->version = RIN_SHADER_IR_VERSION;
    header->header_size = sizeof(*header);
    header->total_size = sizeof(RinShaderHeaderV1) +
                         17u * sizeof(RinShaderInstructionV1);
    header->stage = RIN_SHADER_STAGE_FRAGMENT;
    header->flags = RIN_SHADER_FLAG_PUSH_CONSTANTS;
    header->instruction_count = 17u;
    header->register_count = 8u;
    header->input_count = 0u;
    header->output_count = 8u;
    header->entry_instruction = 0u;
    for (uint16_t component = 0u; component < 8u; ++component) {
        if (component == 0u) {
            set_instruction(&instructions[component],
                            RIN_SHADER_OP_LOAD_PUSH_CONSTANT_F32, component,
                            RIN_SHADER_UNUSED, 0u);
        } else {
            set_instruction(&instructions[component], RIN_SHADER_OP_CONST_F32,
                            component, RIN_SHADER_UNUSED, colors[component]);
        }
        set_instruction(&instructions[8u + component],
                        RIN_SHADER_OP_STORE_OUTPUT_F32, RIN_SHADER_UNUSED,
                        component, component);
    }
    set_instruction(&instructions[16u], RIN_SHADER_OP_RETURN,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u);
}

int main(void)
{
    uint8_t vertex_shader[sizeof(RinShaderHeaderV1) +
                          9u * sizeof(RinShaderInstructionV1)];
    uint8_t fragment_shader[sizeof(RinShaderHeaderV1) +
                            17u * sizeof(RinShaderInstructionV1)];
    const uint8_t indices[3] = {0u, UINT8_MAX, 0u};
    uint8_t initial_image[2u * 2u * 4u] = {
        255u, 0u, 0u, 255u, 255u, 0u, 0u, 255u,
        255u, 0u, 0u, 255u, 255u, 0u, 0u, 255u};
    uint8_t output_image[2u * 2u * 4u] = {0};
    uint8_t output_image1[2u * 2u * 4u] = {0};
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuBufferDescV1 index_desc;
    RinGpuImageDescV1 image_desc;
    RinGpuImageUploadV1 upload;
    RinGpuImageReadbackV1 readback;
    RinGpuImageTransitionV1 transition;
    RinGpuGraphicsPipelineNativeDescV3 pipeline_desc;
    RinGpuVertexAttributeV1 attribute;
    RinGpuRenderPassMrtDescV1 render_pass;
    RinGpuDrawIndexedV1 draw;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuSubmitInfoV1 submit;
    RinGpuPushConstantsV1 push_constants;
    RinGpuHandle vertex_module = 0u;
    RinGpuHandle fragment_module = 0u;
    RinGpuHandle pipeline = 0u;
    RinGpuHandle image = 0u;
    RinGpuHandle image1 = 0u;
    RinGpuHandle index_buffer = 0u;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle fence = 0u;
    int result = 1;

    make_constant_shader(vertex_shader, RIN_SHADER_STAGE_VERTEX, 0);
    make_mrt_fragment_shader(fragment_shader);
    memset(&core, 0, sizeof(core));
    if (!make_core(&core, &backend)) return 1;

    {
        int vertex_result = ringpu_create_shader_module(
            &core, vertex_shader, sizeof(vertex_shader), &vertex_module);
        int fragment_result = ringpu_create_shader_module(
            &core, fragment_shader, sizeof(fragment_shader), &fragment_module);
        if (vertex_result != RIN_GPU_OK || fragment_result != RIN_GPU_OK) {
            goto done;
        }
    }
    memset(&pipeline_desc, 0, sizeof(pipeline_desc));
    pipeline_desc.base.base.abi_version = RIN_GPU_ABI_VERSION;
    pipeline_desc.base.base.struct_size = sizeof(pipeline_desc);
    pipeline_desc.base.base.vertex_shader = vertex_module;
    pipeline_desc.base.base.fragment_shader = fragment_module;
    pipeline_desc.base.base.color_format = RIN_GPU_FORMAT_BGRA8_UNORM;
    pipeline_desc.base.base.primitive_topology = RIN_GPU_PRIMITIVE_POINT_LIST;
    pipeline_desc.base.base.position_output_location = 0u;
    pipeline_desc.base.base.color_write_mask = RIN_GPU_COLOR_WRITE_ALL;
    pipeline_desc.base.base.cull_mode = RIN_GPU_CULL_NONE;
    pipeline_desc.base.base.front_face = RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE;
    pipeline_desc.base.base.flags =
        RIN_GPU_GRAPHICS_PIPELINE_NATIVE_PRIMITIVE_RESTART;
    pipeline_desc.blend_target_mask =
        (UINT32_C(1) << RIN_GPU_MAX_COLOR_TARGETS) - 1u;
    pipeline_desc.blend_targets[0].color_write_mask =
        RIN_GPU_COLOR_WRITE_ALL;
    pipeline_desc.blend_targets[1].blend_enabled = 1u;
    pipeline_desc.blend_targets[1].source_color_factor = RIN_GPU_BLEND_ONE;
    pipeline_desc.blend_targets[1].destination_color_factor = RIN_GPU_BLEND_ONE;
    pipeline_desc.blend_targets[1].color_operation = RIN_GPU_BLEND_ADD;
    pipeline_desc.blend_targets[1].source_alpha_factor = RIN_GPU_BLEND_ONE;
    pipeline_desc.blend_targets[1].destination_alpha_factor = RIN_GPU_BLEND_ONE;
    pipeline_desc.blend_targets[1].alpha_operation = RIN_GPU_BLEND_ADD;
    pipeline_desc.blend_targets[1].color_write_mask = RIN_GPU_COLOR_WRITE_ALL;
    pipeline_desc.blend_targets[2].color_write_mask = RIN_GPU_COLOR_WRITE_ALL;
    pipeline_desc.blend_targets[3].color_write_mask = RIN_GPU_COLOR_WRITE_ALL;
    memset(&attribute, 0, sizeof(attribute));
    attribute.abi_version = RIN_GPU_ABI_VERSION;
    attribute.struct_size = sizeof(attribute);
    attribute.location = 0u;
    attribute.format = RIN_GPU_VERTEX_FLOAT32;
    attribute.flags = RIN_GPU_VERTEX_ATTRIBUTE_CONSTANT_FLOAT32;
    if (ringpu_create_graphics_pipeline_native_v3(
            &core, &pipeline_desc, &attribute, 1u, NULL, 0u,
            &pipeline) != RIN_GPU_OK) {
        goto done;
    }

    memset(&index_desc, 0, sizeof(index_desc));
    index_desc.abi_version = RIN_GPU_ABI_VERSION;
    index_desc.struct_size = sizeof(index_desc);
    index_desc.size_bytes = sizeof(indices);
    index_desc.usage = RIN_GPU_BUFFER_INDEX | RIN_GPU_BUFFER_COPY_DESTINATION;
    index_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    if (ringpu_create_buffer(&core, &index_desc, &index_buffer) != RIN_GPU_OK ||
        ringpu_upload_buffer(&core, index_buffer, 0u, indices,
                             sizeof(indices)) != RIN_GPU_OK) {
        goto done;
    }

    memset(&image_desc, 0, sizeof(image_desc));
    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    image_desc.width = 2u;
    image_desc.height = 2u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
                       RIN_GPU_IMAGE_COPY_DESTINATION |
                       RIN_GPU_IMAGE_COLOR_TARGET;
    image_desc.flags = RIN_GPU_IMAGE_CPU_VISIBLE | RIN_GPU_IMAGE_CPU_READABLE;
    if (ringpu_create_image(&core, &image_desc, &image) != RIN_GPU_OK ||
        ringpu_create_image(&core, &image_desc, &image1) != RIN_GPU_OK) {
        goto done;
    }
    memset(&upload, 0, sizeof(upload));
    upload.abi_version = RIN_GPU_ABI_VERSION;
    upload.struct_size = sizeof(upload);
    upload.width = image_desc.width;
    upload.height = image_desc.height;
    upload.depth = 1u;
    if (ringpu_upload_image(&core, image, &upload, initial_image,
                            sizeof(initial_image)) != RIN_GPU_OK ||
        ringpu_upload_image(&core, image1, &upload, initial_image,
                            sizeof(initial_image)) != RIN_GPU_OK) {
        goto done;
    }

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    if (ringpu_create_queue(&core, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc, &command_list) !=
            RIN_GPU_OK ||
        ringpu_create_fence(&core, 0u, &fence) != RIN_GPU_OK) {
        goto done;
    }
    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    transition.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    if (ringpu_command_transition_image(&core, command_list, image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_transition_image(&core, command_list, image1,
                                        &transition) != RIN_GPU_OK) {
        goto done;
    }
    memset(&push_constants, 0, sizeof(push_constants));
    push_constants.abi_version = RIN_GPU_ABI_VERSION;
    push_constants.struct_size = sizeof(push_constants);
    {
        float red = 1.0f;
        memcpy(push_constants.data, &red, sizeof(red));
    }
    if (ringpu_command_set_push_constants(&core, command_list,
                                          &push_constants) != RIN_GPU_OK) {
        goto done;
    }
    memset(&render_pass, 0, sizeof(render_pass));
    render_pass.abi_version = RIN_GPU_ABI_VERSION;
    render_pass.struct_size = sizeof(render_pass);
    render_pass.active_color_mask = 3u;
    render_pass.color_attachments[0].target = image;
    render_pass.color_attachments[1].target = image1;
    render_pass.color_load_op = RIN_GPU_RENDER_LOAD;
    render_pass.color_store_op = RIN_GPU_RENDER_STORE;
    render_pass.color_write_mask = 0u;
    if (ringpu_command_begin_render_pass_mrt(&core, command_list, &render_pass) !=
            RIN_GPU_OK) {
        goto done;
    }
    memset(&draw, 0, sizeof(draw));
    draw.abi_version = RIN_GPU_ABI_VERSION;
    draw.struct_size = sizeof(draw);
    draw.pipeline = pipeline;
    draw.color_target = image;
    draw.index_buffer = index_buffer;
    draw.index_format = RIN_GPU_INDEX_UINT8;
    draw.index_count = 3u;
    draw.instance_count = 1u;
    draw.vertex_count = 1u;
    if (ringpu_command_draw_indexed(&core, command_list, &draw) != RIN_GPU_OK ||
        ringpu_command_end_render_pass(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }
    transition.before_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    if (ringpu_command_transition_image(&core, command_list, image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_transition_image(&core, command_list, image1,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }
    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    submit.signal_fence = fence;
    submit.signal_value = 1u;
    if (ringpu_queue_submit(&core, queue, &submit) != RIN_GPU_OK ||
        ringpu_wait_fence(&core, fence, 1u, 0u) != RIN_GPU_OK) {
        goto done;
    }
    memset(&readback, 0, sizeof(readback));
    readback.abi_version = RIN_GPU_ABI_VERSION;
    readback.struct_size = sizeof(readback);
    readback.width = image_desc.width;
    readback.height = image_desc.height;
    readback.depth = 1u;
    {
        int readback_result = ringpu_readback_image(
            &core, image, &readback, output_image, sizeof(output_image));
        int second_readback_result = ringpu_readback_image(
            &core, image1, &readback, output_image1, sizeof(output_image1));
        if (readback_result != RIN_GPU_OK ||
            second_readback_result != RIN_GPU_OK) {
            goto done;
        }
    }
    {
        int saw_target0_red = 0;
        int saw_target1_cyan = 0;

        for (uint32_t index = 0u; index < sizeof(output_image); index += 4u) {
            int target0_red = output_image[index] == 0u &&
                output_image[index + 1u] == 0u &&
                output_image[index + 2u] == 255u &&
                output_image[index + 3u] == 255u;
            int target0_blue = output_image[index] == 255u &&
                output_image[index + 1u] == 0u &&
                output_image[index + 2u] == 0u &&
                output_image[index + 3u] == 255u;
            int target1_cyan = output_image1[index] == 255u &&
                output_image1[index + 1u] == 255u &&
                output_image1[index + 2u] == 0u &&
                output_image1[index + 3u] == 255u;
            int target1_blue = output_image1[index] == 255u &&
                output_image1[index + 1u] == 0u &&
                output_image1[index + 2u] == 0u &&
                output_image1[index + 3u] == 255u;

            if ((!target0_red && !target0_blue) ||
                (!target1_cyan && !target1_blue))
                goto done;
            saw_target0_red |= target0_red;
            saw_target1_cyan |= target1_cyan;
        }
        if (!saw_target0_red || !saw_target1_cyan)
            goto done;
    }
    result = 0;

done:
    ringpu_destroy(&core, command_list);
    ringpu_destroy(&core, fence);
    ringpu_destroy(&core, queue);
    ringpu_destroy(&core, index_buffer);
    ringpu_destroy(&core, image);
    ringpu_destroy(&core, image1);
    ringpu_destroy(&core, pipeline);
    ringpu_destroy(&core, fragment_module);
    ringpu_destroy(&core, vertex_module);
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return result;
}
