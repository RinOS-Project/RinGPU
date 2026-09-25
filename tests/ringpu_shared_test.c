/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

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
    CHECK(ringpu_software_backend_create(&backend_desc, &backend) ==
          RIN_GPU_OK && backend != NULL);

    memset(&display, 0, sizeof(display));
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 1u;
    display.height = 1u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "shared", 7u);

    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x5348415245445445);
    config.max_buffer_size = 1024u;
    config.max_image_size = 1024u;
    config.max_total_allocation_size = 2u * 1024u * 1024u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 1u;
    config.max_image_mip_levels = 1u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COMPUTE;
    memcpy(config.adapter.name, "shared", 7u);
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

static void make_shared_shader(uint8_t* storage)
{
    RinShaderHeaderV1* header = (RinShaderHeaderV1*)storage;
    RinShaderInstructionV1* instructions =
        (RinShaderInstructionV1*)(storage + sizeof(*header));

    memset(storage, 0, sizeof(RinShaderHeaderV1) +
           7u * sizeof(RinShaderInstructionV1));
    header->magic = RIN_SHADER_MAGIC;
    header->version = RIN_SHADER_IR_VERSION;
    header->header_size = sizeof(*header);
    header->total_size = sizeof(RinShaderHeaderV1) +
                         7u * sizeof(RinShaderInstructionV1);
    header->stage = RIN_SHADER_STAGE_COMPUTE;
    header->flags = RIN_SHADER_FLAG_WORKGROUP_SHARED;
    header->instruction_count = 7u;
    header->register_count = 3u;
    header->resource_count = 1u;
    header->workgroup_x = 1u;
    header->workgroup_y = 1u;
    header->workgroup_z = 1u;
    header->reserved0 = 16u;
    set_instruction(&instructions[0], RIN_SHADER_OP_CONST_I32, 0u,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                    RIN_SHADER_UNUSED, 0u);
    set_instruction(&instructions[1], RIN_SHADER_OP_CONST_I32, 1u,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                    RIN_SHADER_UNUSED, 42u);
    set_instruction(&instructions[2], RIN_SHADER_OP_STORE_SHARED_I32,
                    RIN_SHADER_UNUSED, 0u, 1u, RIN_SHADER_UNUSED, 0u);
    set_instruction(&instructions[3], RIN_SHADER_OP_WORKGROUP_BARRIER,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                    RIN_SHADER_UNUSED, 0u);
    set_instruction(&instructions[4], RIN_SHADER_OP_LOAD_SHARED_I32, 2u, 0u,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u);
    set_instruction(&instructions[5], RIN_SHADER_OP_STORE_RESOURCE_I32,
                    RIN_SHADER_UNUSED, 0u, 2u, 0u, 0u);
    set_instruction(&instructions[6], RIN_SHADER_OP_RETURN,
                    RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                    RIN_SHADER_UNUSED, 0u);
}

int main(void)
{
    uint8_t shader[sizeof(RinShaderHeaderV1) +
                   7u * sizeof(RinShaderInstructionV1)];
    const uint32_t initial_value = 0u;
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuBufferDescV1 buffer_desc;
    RinGpuBufferBindingV1 binding;
    RinGpuComputePipelineDescV1 pipeline_desc;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuDispatchV1 dispatch;
    RinGpuHandle buffer = 0u;
    RinGpuHandle shader_module = 0u;
    RinGpuHandle pipeline = 0u;
    RinGpuHandle bind_group = 0u;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuSubmitInfoV1 submit;

    make_shared_shader(shader);
    memset(&core, 0, sizeof(core));
    CHECK(make_core(&core, &backend));

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = sizeof(initial_value);
    buffer_desc.usage = RIN_GPU_BUFFER_STORAGE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    CHECK(ringpu_create_buffer(&core, &buffer_desc, &buffer) == RIN_GPU_OK);
    CHECK(ringpu_upload_buffer(&core, buffer, 0u, &initial_value,
                               sizeof(initial_value)) == RIN_GPU_OK);

    CHECK(ringpu_create_shader_module(&core, shader, sizeof(shader),
                                     &shader_module) == RIN_GPU_OK);
    memset(&pipeline_desc, 0, sizeof(pipeline_desc));
    pipeline_desc.abi_version = RIN_GPU_ABI_VERSION;
    pipeline_desc.struct_size = sizeof(pipeline_desc);
    pipeline_desc.shader_module = shader_module;
    CHECK(ringpu_create_compute_pipeline(&core, &pipeline_desc, &pipeline) ==
          RIN_GPU_OK);

    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.binding = 0u;
    binding.access = RIN_GPU_RESOURCE_WRITE;
    binding.buffer = buffer;
    binding.size_bytes = sizeof(initial_value);
    CHECK(ringpu_create_compute_bind_group(&core, pipeline, &binding, 1u,
                                           &bind_group) == RIN_GPU_OK);

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COMPUTE;
    CHECK(ringpu_create_queue(&core, &queue_desc, &queue) == RIN_GPU_OK);
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_COMPUTE;
    CHECK(ringpu_create_command_list(&core, &command_desc, &command_list) ==
          RIN_GPU_OK);
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.abi_version = RIN_GPU_ABI_VERSION;
    dispatch.struct_size = sizeof(dispatch);
    dispatch.pipeline = pipeline;
    dispatch.bind_group = bind_group;
    dispatch.group_count_x = 1u;
    dispatch.group_count_y = 1u;
    dispatch.group_count_z = 1u;
    CHECK(ringpu_command_dispatch(&core, command_list, &dispatch) ==
          RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, command_list) == RIN_GPU_OK);
    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    CHECK(ringpu_queue_submit(&core, queue, &submit) == RIN_GPU_OK);

    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return 0;
}
