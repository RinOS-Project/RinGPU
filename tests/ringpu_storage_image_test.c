/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>
#include <ringpu/spirv_frontend.h>

static RinGpuBackendOpsV1 delegated_ops;
static uint32_t fail_next_graphics_pipeline;
static uint32_t fail_next_graphics_bind_group;
static uint32_t empty_success_next_graphics_pipeline;
static uint32_t empty_success_next_graphics_bind_group;
static uint32_t create_graphics_pipeline_calls;
static uint32_t destroy_graphics_pipeline_calls;
static uint32_t destroy_graphics_bind_group_calls;

static int injected_create_graphics_pipeline(
    void* context, uint64_t vertex_shader_cookie,
    const RinShaderInfoV1* vertex_shader_info,
    uint64_t fragment_shader_cookie,
    const RinShaderInfoV1* fragment_shader_info,
    const RinGpuBackendGraphicsPipelineDescV1* desc, uint64_t* cookie_out)
{
    int result;
    ++create_graphics_pipeline_calls;
    if (empty_success_next_graphics_pipeline != 0u) {
        --empty_success_next_graphics_pipeline;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_OK;
    }
    result = delegated_ops.create_graphics_pipeline(
        context, vertex_shader_cookie, vertex_shader_info,
        fragment_shader_cookie, fragment_shader_info, desc, cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (fail_next_graphics_pipeline != 0u) {
        --fail_next_graphics_pipeline;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static void injected_destroy_graphics_pipeline(void* context,
                                               uint64_t cookie)
{
    ++destroy_graphics_pipeline_calls;
    delegated_ops.destroy_graphics_pipeline(context, cookie);
}

static int injected_create_graphics_bind_group(
    void* context, uint64_t pipeline_cookie,
    const RinGpuBackendGraphicsBindingV1* bindings, uint32_t binding_count,
    uint64_t* cookie_out)
{
    int result;
    if (empty_success_next_graphics_bind_group != 0u) {
        --empty_success_next_graphics_bind_group;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_OK;
    }
    result = delegated_ops.create_graphics_bind_group(
        context, pipeline_cookie, bindings, binding_count, cookie_out);
    if (result != RIN_GPU_OK) return result;
    if (fail_next_graphics_bind_group != 0u) {
        --fail_next_graphics_bind_group;
        return RIN_GPU_ERROR_BACKEND;
    }
    return result;
}

static void injected_destroy_graphics_bind_group(void* context,
                                                 uint64_t cookie)
{
    ++destroy_graphics_bind_group_calls;
    delegated_ops.destroy_graphics_bind_group(context, cookie);
}

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

static void emit_spirv(uint32_t* words, size_t* count, uint16_t opcode,
                       uint16_t word_count, const uint32_t* operands)
{
    words[(*count)++] = ((uint32_t)word_count << 16u) | opcode;
    for (uint16_t index = 0u; index + 1u < word_count; ++index)
        words[(*count)++] = operands[index];
}

static int make_fragment_shader(uint8_t* storage, size_t storage_capacity,
                                size_t* shader_size_out)
{
    uint32_t words[256] = {0};
    uint32_t operands[8] = {0};
    size_t count = 5u;
    RinSpirvTranslationInfoV2 info;
    RinShaderHeaderV1* header;
    int result;

    words[0] = RIN_SPIRV_MAGIC;
    words[1] = 0x00010300u;
    words[2] = 0u;
    words[3] = 26u;
    words[4] = 0u;
    operands[0] = 1u;
    emit_spirv(words, &count, 17u, 2u, operands); /* Shader */
    operands[0] = 49u;
    emit_spirv(words, &count, 17u, 2u, operands); /* StorageImageExtendedFormats */
    operands[0] = 0u; operands[1] = 3u;
    emit_spirv(words, &count, 14u, 3u, operands); /* Vulkan memory */
    operands[0] = 4u; operands[1] = 18u;
    operands[2] = 0x6e69616du; operands[3] = 0u; operands[4] = 25u;
    emit_spirv(words, &count, 15u, 6u, operands); /* Fragment main/output */
    operands[0] = 11u; operands[1] = 34u; operands[2] = 0u;
    emit_spirv(words, &count, 71u, 4u, operands); /* R8 set */
    operands[0] = 11u; operands[1] = 33u; operands[2] = 0u;
    emit_spirv(words, &count, 71u, 4u, operands); /* R8 binding */
    operands[0] = 12u; operands[1] = 34u; operands[2] = 0u;
    emit_spirv(words, &count, 71u, 4u, operands); /* RGBA8 set */
    operands[0] = 12u; operands[1] = 33u; operands[2] = 1u;
    emit_spirv(words, &count, 71u, 4u, operands); /* RGBA8 binding */
    operands[0] = 25u; operands[1] = 30u; operands[2] = 0u;
    emit_spirv(words, &count, 71u, 4u, operands); /* color output location */

    operands[0] = 1u;
    emit_spirv(words, &count, 19u, 2u, operands); /* void */
    operands[0] = 2u; operands[1] = 32u;
    emit_spirv(words, &count, 22u, 3u, operands); /* float */
    operands[0] = 3u; operands[1] = 32u; operands[2] = 1u;
    emit_spirv(words, &count, 21u, 4u, operands); /* signed int */
    operands[0] = 4u; operands[1] = 3u; operands[2] = 2u;
    emit_spirv(words, &count, 23u, 4u, operands); /* ivec2 */
    operands[0] = 5u; operands[1] = 2u; operands[2] = 4u;
    emit_spirv(words, &count, 23u, 4u, operands); /* vec4 */
    operands[0] = 6u; operands[1] = 2u; operands[2] = 1u;
    operands[3] = 0u; operands[4] = 0u; operands[5] = 0u;
    operands[6] = 2u; operands[7] = 15u;
    emit_spirv(words, &count, 25u, 9u, operands); /* R8 storage image */
    operands[0] = 7u; operands[7] = 4u;
    emit_spirv(words, &count, 25u, 9u, operands); /* RGBA8 storage image */
    operands[0] = 8u; operands[1] = 0u; operands[2] = 6u;
    emit_spirv(words, &count, 32u, 4u, operands); /* R8 image pointer */
    operands[0] = 9u; operands[1] = 0u; operands[2] = 7u;
    emit_spirv(words, &count, 32u, 4u, operands); /* RGBA8 image pointer */
    operands[0] = 24u; operands[1] = 3u; operands[2] = 5u;
    emit_spirv(words, &count, 32u, 4u, operands); /* output pointer */
    operands[0] = 10u; operands[1] = 1u;
    emit_spirv(words, &count, 33u, 3u, operands); /* function type */
    operands[0] = 8u; operands[1] = 11u; operands[2] = 0u;
    emit_spirv(words, &count, 59u, 4u, operands); /* R8 descriptor */
    operands[0] = 9u; operands[1] = 12u; operands[2] = 0u;
    emit_spirv(words, &count, 59u, 4u, operands); /* RGBA8 descriptor */
    operands[0] = 24u; operands[1] = 25u; operands[2] = 3u;
    emit_spirv(words, &count, 59u, 4u, operands); /* fragment color output */
    operands[0] = 3u; operands[1] = 13u; operands[2] = 0u;
    emit_spirv(words, &count, 43u, 4u, operands); /* integer zero */
    operands[0] = 2u; operands[1] = 14u; operands[2] = 0x3f000000u;
    emit_spirv(words, &count, 43u, 4u, operands); /* 0.5 */
    operands[0] = 2u; operands[1] = 15u; operands[2] = 0x3e800000u;
    emit_spirv(words, &count, 43u, 4u, operands); /* 0.25 */
    operands[0] = 2u; operands[1] = 16u; operands[2] = 0x3f400000u;
    emit_spirv(words, &count, 43u, 4u, operands); /* 0.75 */
    operands[0] = 2u; operands[1] = 17u; operands[2] = 0x3f800000u;
    emit_spirv(words, &count, 43u, 4u, operands); /* 1.0 */

    operands[0] = 1u; operands[1] = 18u; operands[2] = 0u; operands[3] = 10u;
    emit_spirv(words, &count, 54u, 5u, operands); /* function */
    operands[0] = 19u;
    emit_spirv(words, &count, 248u, 2u, operands); /* label */
    operands[0] = 4u; operands[1] = 20u;
    operands[2] = 13u; operands[3] = 13u;
    emit_spirv(words, &count, 80u, 5u, operands); /* ivec2 coordinate */
    operands[0] = 5u; operands[1] = 21u;
    operands[2] = 14u; operands[3] = 15u;
    operands[4] = 16u; operands[5] = 17u;
    emit_spirv(words, &count, 80u, 7u, operands); /* vec4 value */
    operands[0] = 6u; operands[1] = 22u; operands[2] = 11u;
    emit_spirv(words, &count, 61u, 4u, operands); /* load R8 image */
    operands[0] = 7u; operands[1] = 23u; operands[2] = 12u;
    emit_spirv(words, &count, 61u, 4u, operands); /* load RGBA8 image */
    operands[0] = 22u; operands[1] = 20u; operands[2] = 21u;
    emit_spirv(words, &count, 99u, 4u, operands); /* write R8 */
    operands[0] = 23u;
    emit_spirv(words, &count, 99u, 4u, operands); /* write RGBA8 */
    operands[0] = 25u; operands[1] = 21u;
    emit_spirv(words, &count, 62u, 3u, operands); /* store color */
    emit_spirv(words, &count, 253u, 1u, operands); /* return */
    emit_spirv(words, &count, 56u, 1u, operands); /* function end */

    result = ringpu_spirv_translate_v3(
        words, count, RIN_SHADER_STAGE_FRAGMENT, NULL, 0u,
        RIN_SPIRV_TRANSLATE_FEATURE_R8_STORAGE_IMAGE,
        storage, storage_capacity, &info);
    if (result != RIN_SPIRV_OK || info.base.descriptor_count != 2u ||
        info.base.descriptors[0].binding != 0u ||
        info.base.descriptors[1].binding != 1u) {
        fprintf(stderr, "storage-image SPIR-V translation=%d descriptors=%u: %s\n",
                result, info.base.descriptor_count, info.base.diagnostic);
        return 0;
    }
    header = (RinShaderHeaderV1*)storage;
    if (header->total_size < sizeof(*header) ||
        header->total_size > storage_capacity)
        return 0;
    *shader_size_out = header->total_size;
    return 1;
}

int main(void)
{
    uint8_t vertex_shader[sizeof(RinShaderHeaderV1) +
                         9u * sizeof(RinShaderInstructionV1)];
    uint8_t fragment_shader[sizeof(RinShaderHeaderV1) +
                           20u * sizeof(RinShaderInstructionV1)];
    size_t fragment_shader_size = 0u;
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuGraphicsPipelineNativeDescV2 pipeline_desc = {0};
    RinGpuImageDescV1 storage_desc = {0};
    RinGpuImageDescV1 color_desc = {0};
    RinGpuImageTransitionV1 transition = {0};
    RinGpuRenderPassDescV1 render_pass = {0};
    RinGpuGraphicsBarrierV2 graphics_barrier_v2 = {0};
    RinGpuGraphicsBindingV1 binding = {0};
    RinGpuGraphicsBindingV1 bindings[2] = {{0}};
    RinGpuDrawV1 draw = {0};
    RinGpuQueueDescV1 queue_desc = {0};
    RinGpuCommandListDescV1 command_desc = {0};
    RinGpuSubmitInfoV1 submit = {0};
    RinGpuImageReadbackV1 readback = {0};
    RinGpuHandle vertex_module = 0u, fragment_module = 0u;
    RinGpuHandle pipeline = 0u, pipeline_alias = 0u, variant_pipeline = 0u;
    RinGpuHandle storage_image = 0u, rgba_storage_image = 0u;
    RinGpuHandle color_image = 0u;
    RinGpuHandle bind_group = 0u, queue = 0u, command_list = 0u, fence = 0u;
    uint8_t value = 0u;
    uint8_t rgba_value[4] = {0u, 0u, 0u, 0u};
    const char* failure_stage = "initialization";
    int result = 1;

    make_vertex_shader(vertex_shader);
    if (!make_fragment_shader(fragment_shader, sizeof(fragment_shader),
                              &fragment_shader_size))
        return 1;
    if (!make_core(&core, &backend)) return 1;
    delegated_ops = core.backend;
    core.backend.create_graphics_pipeline =
        injected_create_graphics_pipeline;
    core.backend.destroy_graphics_pipeline =
        injected_destroy_graphics_pipeline;
    core.backend.create_graphics_bind_group =
        injected_create_graphics_bind_group;
    core.backend.destroy_graphics_bind_group =
        injected_destroy_graphics_bind_group;
    failure_stage = "shader modules";
    if (ringpu_create_shader_module(&core, vertex_shader, sizeof(vertex_shader),
                                    &vertex_module) != RIN_GPU_OK ||
        ringpu_create_shader_module(&core, fragment_shader,
                                    fragment_shader_size, &fragment_module) !=
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
        fail_next_graphics_pipeline = 1u;
        pipeline = UINT64_MAX;
        int pipeline_result = ringpu_create_graphics_pipeline_native_v2(
            &core, &pipeline_desc, NULL, 0u, NULL, 0u, &pipeline);
        if (pipeline_result != RIN_GPU_ERROR_BACKEND || pipeline != 0u ||
            destroy_graphics_pipeline_calls != 1u)
            goto done;
    }
    {
        empty_success_next_graphics_pipeline = 1u;
        pipeline = UINT64_MAX;
        int pipeline_result = ringpu_create_graphics_pipeline_native_v2(
            &core, &pipeline_desc, NULL, 0u, NULL, 0u, &pipeline);
        if (pipeline_result != RIN_GPU_ERROR_BACKEND || pipeline != 0u ||
            destroy_graphics_pipeline_calls != 1u)
            goto done;
    }
    {
        int pipeline_result = ringpu_create_graphics_pipeline_native_v2(
            &core, &pipeline_desc, NULL, 0u, NULL, 0u, &pipeline);
        if (pipeline_result != RIN_GPU_OK) {
            fprintf(stderr, "graphics pipeline error %d\n", pipeline_result);
            goto done;
        }
    }
    if (create_graphics_pipeline_calls != 3u ||
        ringpu_create_graphics_pipeline_native_v2(
            &core, &pipeline_desc, NULL, 0u, NULL, 0u,
            &pipeline_alias) != RIN_GPU_OK ||
        pipeline_alias == pipeline || create_graphics_pipeline_calls != 3u)
        goto done;
    {
        RinGpuGraphicsPipelineNativeDescV2 variant_desc = pipeline_desc;
        variant_desc.base.primitive_topology = RIN_GPU_PRIMITIVE_LINE_LIST;
        if (ringpu_create_graphics_pipeline_native_v2(
                &core, &variant_desc, NULL, 0u, NULL, 0u,
                &variant_pipeline) != RIN_GPU_OK ||
            variant_pipeline == pipeline ||
            create_graphics_pipeline_calls != 4u)
            goto done;
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
    RinGpuImageDescV1 rgba_storage_desc = storage_desc;
    rgba_storage_desc.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    rgba_storage_desc.width = 32u;
    rgba_storage_desc.height = 4u;
    color_desc = storage_desc;
    color_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    color_desc.usage = RIN_GPU_IMAGE_COLOR_TARGET | RIN_GPU_IMAGE_COPY_SOURCE;
    failure_stage = "images";
    if (ringpu_create_image(&core, &storage_desc, &storage_image) != RIN_GPU_OK ||
        ringpu_create_image(&core, &rgba_storage_desc,
                            &rgba_storage_image) != RIN_GPU_OK ||
        ringpu_create_image(&core, &color_desc, &color_image) != RIN_GPU_OK)
        goto done;
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.binding = 0u;
    binding.kind = RIN_SHADER_RESOURCE_STORAGE_IMAGE;
    binding.access = RIN_GPU_RESOURCE_WRITE;
    binding.resource = storage_image;
    bindings[0] = binding;
    bindings[1] = binding;
    bindings[1].binding = 1u;
    bindings[1].resource = rgba_storage_image;
    failure_stage = "bind group";
    fail_next_graphics_bind_group = 1u;
    bind_group = UINT64_MAX;
    if (ringpu_create_graphics_bind_group_typed(
            &core, pipeline, bindings, 2u, &bind_group) !=
            RIN_GPU_ERROR_BACKEND ||
        bind_group != 0u || destroy_graphics_bind_group_calls != 1u)
        goto done;
    empty_success_next_graphics_bind_group = 1u;
    bind_group = UINT64_MAX;
    if (ringpu_create_graphics_bind_group_typed(
            &core, pipeline, bindings, 2u, &bind_group) !=
            RIN_GPU_ERROR_BACKEND ||
        bind_group != 0u || destroy_graphics_bind_group_calls != 1u)
        goto done;
    bind_group = 0u;
    if (ringpu_create_graphics_bind_group_typed(&core, pipeline, bindings, 2u,
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
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_transition_image(&core, command_list,
                                        rgba_storage_image,
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
    graphics_barrier_v2.abi_version = RIN_GPU_ABI_VERSION;
    graphics_barrier_v2.struct_size = sizeof(graphics_barrier_v2);
    graphics_barrier_v2.source_stage =
        RIN_GPU_PIPELINE_STAGE_COMPUTE_SHADER;
    graphics_barrier_v2.destination_stage =
        RIN_GPU_PIPELINE_STAGE_COLOR_OUTPUT;
    graphics_barrier_v2.source_access = RIN_GPU_RESOURCE_READ;
    graphics_barrier_v2.destination_access = RIN_GPU_RESOURCE_WRITE;
    if (ringpu_command_graphics_barrier_v2(
            &core, command_list, &graphics_barrier_v2) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }
    graphics_barrier_v2.source_stage =
        RIN_GPU_PIPELINE_STAGE_TOP_OF_PIPE;
    graphics_barrier_v2.destination_stage =
        RIN_GPU_PIPELINE_STAGE_COLOR_OUTPUT;
    graphics_barrier_v2.source_access = 0u;
    graphics_barrier_v2.destination_access = RIN_GPU_RESOURCE_WRITE;
    if (ringpu_command_graphics_barrier_v2(
            &core, command_list, &graphics_barrier_v2) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }
    graphics_barrier_v2.source_stage =
        RIN_GPU_PIPELINE_STAGE_COLOR_OUTPUT;
    graphics_barrier_v2.destination_stage = RIN_GPU_PIPELINE_STAGE_HOST;
    graphics_barrier_v2.source_access = RIN_GPU_RESOURCE_READ;
    graphics_barrier_v2.destination_access = RIN_GPU_RESOURCE_WRITE;
    if (ringpu_command_graphics_barrier_v2(
            &core, command_list, &graphics_barrier_v2) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }
    graphics_barrier_v2.source_stage =
        RIN_GPU_PIPELINE_STAGE_COLOR_OUTPUT;
    graphics_barrier_v2.destination_stage =
        RIN_GPU_PIPELINE_STAGE_COMPUTE_SHADER;
    if (ringpu_command_graphics_barrier_v2(
            &core, command_list, &graphics_barrier_v2) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }
    graphics_barrier_v2.source_stage =
        RIN_GPU_PIPELINE_STAGE_COLOR_OUTPUT;
    graphics_barrier_v2.destination_stage =
        RIN_GPU_PIPELINE_STAGE_FRAGMENT_SHADER;
    if (ringpu_command_graphics_barrier_v2(
            &core, command_list, &graphics_barrier_v2) != RIN_GPU_OK) {
        goto done;
    }
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
    if (ringpu_command_transition_image(&core, command_list,
                                        rgba_storage_image,
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
        if (readback_result != RIN_GPU_OK || value != 128u) {
            fprintf(stderr, "R8 readback error %d value %u\n",
                    readback_result, (unsigned)value);
            goto done;
        }
        readback_result = ringpu_readback_image(
            &core, rgba_storage_image, &readback, rgba_value,
            sizeof(rgba_value));
        if (readback_result != RIN_GPU_OK || rgba_value[0] != 128u ||
            rgba_value[1] != 64u || rgba_value[2] != 191u ||
            rgba_value[3] != 255u) {
            fprintf(stderr,
                    "RGBA8 readback error %d value %u,%u,%u,%u\n",
                    readback_result, (unsigned)rgba_value[0],
                    (unsigned)rgba_value[1], (unsigned)rgba_value[2],
                    (unsigned)rgba_value[3]);
            goto done;
        }
    }
    failure_stage = "idle graphics pipeline cache";
    if (ringpu_destroy(&core, command_list) != RIN_GPU_OK) goto done;
    command_list = 0u;
    if (ringpu_destroy(&core, bind_group) != RIN_GPU_OK) goto done;
    bind_group = 0u;
    if (ringpu_destroy(&core, pipeline_alias) != RIN_GPU_OK) goto done;
    pipeline_alias = 0u;
    if (ringpu_destroy(&core, variant_pipeline) != RIN_GPU_OK) goto done;
    variant_pipeline = 0u;
    if (ringpu_destroy(&core, pipeline) != RIN_GPU_OK) goto done;
    pipeline = 0u;
    if (destroy_graphics_pipeline_calls != 1u) goto done;
    {
        RinGpuHandle reused_pipeline = 0u;
        if (ringpu_create_graphics_pipeline_native_v2(
                &core, &pipeline_desc, NULL, 0u, NULL, 0u,
                &reused_pipeline) != RIN_GPU_OK ||
            create_graphics_pipeline_calls != 4u ||
            ringpu_destroy(&core, reused_pipeline) != RIN_GPU_OK ||
            destroy_graphics_pipeline_calls != 1u)
            goto done;
    }
    if (ringpu_destroy(&core, vertex_module) != RIN_GPU_OK) goto done;
    vertex_module = 0u;
    if (destroy_graphics_pipeline_calls != 3u ||
        ringpu_destroy(&core, fragment_module) != RIN_GPU_OK)
        goto done;
    fragment_module = 0u;
    result = 0;
done:
    if (result != 0)
        fprintf(stderr, "storage image test failed at %s\n", failure_stage);
    ringpu_core_shutdown(&core);
    if (destroy_graphics_pipeline_calls != 3u) {
        fprintf(stderr, "pipeline cache retired %u realizations, expected 3\n",
                destroy_graphics_pipeline_calls);
        result = 1;
    }
    ringpu_software_backend_destroy(backend);
    return result;
}
