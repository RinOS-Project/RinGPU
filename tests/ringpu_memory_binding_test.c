/* SPDX-License-Identifier: MIT */
#include "../src/core/core.h"
#include "../src/core/object_table.h"
#include <ringpu/software.h>

#include <string.h>

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackendDescV1 backend_desc;
    RinGpuDisplayInfoV1 display;
    RinGpuCoreConfigV1 config;
    RinGpuSoftwareBackend* backend = NULL;

    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.struct_size = sizeof(backend_desc);
    backend_desc.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend_desc.max_total_bytes = UINT64_C(8) * 1024u * 1024u;
    if (ringpu_software_backend_create(&backend_desc, &backend) !=
            RIN_GPU_OK || !backend) {
        return 0;
    }
    memset(&display, 0, sizeof(display));
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 64u;
    display.height = 32u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "memory binding", 14u);

    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x4d454d42494e4431);
    config.max_buffer_size = UINT64_C(1) << 20u;
    config.max_image_size = UINT64_C(1) << 20u;
    config.max_total_allocation_size = UINT64_C(4) << 20u;
    config.max_image_dimension = 4096u;
    config.max_image_layers = 64u;
    config.max_image_mip_levels = 13u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY |
        RIN_GPU_QUEUE_COMPUTE | RIN_GPU_QUEUE_GRAPHICS;
    memcpy(config.adapter.name, "memory binding", 14u);
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

static void init_image_desc(RinGpuImageDescV1* desc, uint32_t flags)
{
    memset(desc, 0, sizeof(*desc));
    desc->abi_version = RIN_GPU_ABI_VERSION;
    desc->struct_size = sizeof(*desc);
    desc->dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    desc->format = RIN_GPU_FORMAT_BGRA8_UNORM;
    desc->width = 2u;
    desc->height = 2u;
    desc->depth = 1u;
    desc->array_layers = 1u;
    desc->mip_levels = 1u;
    desc->sample_count = 1u;
    desc->usage = RIN_GPU_IMAGE_COPY_SOURCE |
        RIN_GPU_IMAGE_COPY_DESTINATION;
    desc->flags = flags;
}

int main(void)
{
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuImageDescV1 image_desc;
    RinGpuBufferDescV1 buffer_desc;
    RinGpuResourceMemoryRequirementsV1 requirements;
    RinGpuResourceMemoryRequirementsV1 buffer_requirements;
    RinGpuMemoryDescV1 memory_desc;
    RinGpuResourceMemoryBindingV1 binding;
    RinGpuImageUploadV1 upload;
    RinGpuImageReadbackV1 readback;
    RinGpuImageTransitionV1 transition;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuSubmitInfoV1 submit;
    RinGpuHandle image = 0u;
    RinGpuHandle image2 = 0u;
    RinGpuHandle image3 = 0u;
    RinGpuHandle image4 = 0u;
    RinGpuHandle buffer = 0u;
    RinGpuHandle memory = 0u;
    RinGpuHandle buffer_memory = 0u;
    RinGpuHandle memory2 = 0u;
    RinGpuHandle bad_memory = 0u;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle fence = 0u;
    uint8_t source[16u] = {
        1u, 2u, 3u, 255u, 4u, 5u, 6u, 255u,
        7u, 8u, 9u, 255u, 10u, 11u, 12u, 255u};
    uint8_t destination[16u] = {0};
    int result = 1;

    memset(&core, 0, sizeof(core));
    if (!make_core(&core, &backend)) return 1;
    init_image_desc(&image_desc, RIN_GPU_IMAGE_CPU_VISIBLE |
                    RIN_GPU_IMAGE_CPU_READABLE);
    if (ringpu_get_image_memory_requirements(&core, &image_desc,
                                             &requirements) != RIN_GPU_OK ||
        requirements.size_bytes != RIN_GPU_MEMORY_MIN_PAGE_SIZE ||
        requirements.alignment != RIN_GPU_MEMORY_MIN_PAGE_SIZE) {
        goto done;
    }

    memset(&memory_desc, 0, sizeof(memory_desc));
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = requirements.size_bytes;
    memory_desc.alignment = requirements.alignment;
    memory_desc.flags = RIN_GPU_MEMORY_BINDING_DEDICATED;
    if (ringpu_create_memory(&core, &memory_desc, &memory) != RIN_GPU_OK ||
        ringpu_create_image(&core, &image_desc, &image) != RIN_GPU_OK) {
        goto done;
    }
    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.memory = memory;
    binding.size_bytes = requirements.size_bytes;
    if (ringpu_bind_image_memory(&core, image, &binding) != RIN_GPU_OK ||
        ringpu_bind_image_memory(&core, image, &binding) !=
            RIN_GPU_ERROR_BUSY ||
        ringpu_destroy(&core, memory) != RIN_GPU_ERROR_BUSY) {
        goto done;
    }
    memset(&upload, 0, sizeof(upload));
    upload.abi_version = RIN_GPU_ABI_VERSION;
    upload.struct_size = sizeof(upload);
    upload.width = 2u;
    upload.height = 2u;
    upload.depth = 1u;
    if (ringpu_upload_image(&core, image, &upload, source,
                            sizeof(source)) != RIN_GPU_OK) {
        goto done;
    }

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_COPY;
    if (ringpu_create_queue(&core, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc, &command_list) !=
            RIN_GPU_OK || ringpu_create_fence(&core, 0u, &fence) !=
            RIN_GPU_OK) {
        goto done;
    }
    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    if (ringpu_command_transition_image(&core, command_list, image,
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
    readback.width = 2u;
    readback.height = 2u;
    readback.depth = 1u;
    if (ringpu_readback_image(&core, image, &readback, destination,
                              sizeof(destination)) != RIN_GPU_OK ||
        memcmp(source, destination, sizeof(source)) != 0) {
        goto done;
    }

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = 300u;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    if (ringpu_get_buffer_memory_requirements(
            &core, &buffer_desc, &buffer_requirements) != RIN_GPU_OK) {
        goto done;
    }
    memset(&memory_desc, 0, sizeof(memory_desc));
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = buffer_requirements.size_bytes;
    memory_desc.alignment = buffer_requirements.alignment;
    if (ringpu_create_memory(&core, &memory_desc, &buffer_memory) !=
            RIN_GPU_OK ||
        ringpu_create_buffer(&core, &buffer_desc, &buffer) != RIN_GPU_OK) {
        goto done;
    }
    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.memory = buffer_memory;
    binding.size_bytes = buffer_requirements.size_bytes;
    if (ringpu_bind_buffer_memory(&core, buffer, &binding) != RIN_GPU_OK ||
        ringpu_upload_buffer(&core, buffer, 295u, source,
                             sizeof(source)) != RIN_GPU_ERROR_BOUNDS) {
        goto done;
    }
    if (ringpu_upload_buffer(&core, buffer, 0u, source,
                             sizeof(source)) != RIN_GPU_OK) {
        goto done;
    }
    {
        RinGpuObjectSlot* buffer_slot;
        RinGpuObjectSlot* buffer_memory_slot;
        if (ringpu_slot(&core, buffer, RIN_GPU_OBJECT_BUFFER, NULL,
                        &buffer_slot) != RIN_GPU_OK ||
            ringpu_slot(&core, buffer_memory, RIN_GPU_OBJECT_MEMORY, NULL,
                        &buffer_memory_slot) != RIN_GPU_OK ||
            memcmp(buffer_memory_slot->value.memory.bytes +
                       buffer_slot->value.buffer.memory_offset,
                   source, sizeof(source)) != 0) {
            goto done;
        }
    }
    if (ringpu_destroy(&core, buffer) != RIN_GPU_OK ||
        ringpu_destroy(&core, buffer_memory) != RIN_GPU_OK) {
        goto done;
    }

    memset(&memory_desc, 0, sizeof(memory_desc));
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = requirements.size_bytes * 2u;
    memory_desc.alignment = requirements.alignment;
    memory_desc.flags = 0u;
    if (ringpu_create_memory(&core, &memory_desc, &memory2) != RIN_GPU_OK) {
        goto done;
    }
    if (ringpu_create_image(&core, &image_desc, &image2) != RIN_GPU_OK ||
        ringpu_create_image(&core, &image_desc, &image3) != RIN_GPU_OK ||
        ringpu_create_image(&core, &image_desc, &image4) != RIN_GPU_OK) {
        goto done;
    }
    binding.memory = memory2;
    binding.size_bytes = requirements.size_bytes;
    binding.offset_bytes = 0u;
    if (ringpu_bind_image_memory(&core, image2, &binding) != RIN_GPU_OK) {
        goto done;
    }
    binding.offset_bytes = requirements.size_bytes;
    if (ringpu_bind_image_memory(&core, image3, &binding) != RIN_GPU_OK ||
        ringpu_destroy(&core, memory2) != RIN_GPU_ERROR_BUSY) {
        goto done;
    }
    binding.offset_bytes = requirements.size_bytes / 2u;
    if (ringpu_bind_image_memory(&core, image4, &binding) !=
        RIN_GPU_ERROR_BOUNDS) {
        goto done;
    }
    if (ringpu_destroy(&core, image2) != RIN_GPU_OK ||
        ringpu_destroy(&core, image3) != RIN_GPU_OK ||
        ringpu_destroy(&core, memory2) != RIN_GPU_OK) {
        goto done;
    }

    memory_desc.size_bytes = requirements.size_bytes * 2u;
    memory_desc.flags = RIN_GPU_MEMORY_BINDING_DEDICATED;
    if (ringpu_create_memory(&core, &memory_desc, &bad_memory) != RIN_GPU_OK ||
        ringpu_bind_image_memory(&core, image4, &(RinGpuResourceMemoryBindingV1){
            RIN_GPU_ABI_VERSION, sizeof(RinGpuResourceMemoryBindingV1),
            bad_memory, 0u, requirements.size_bytes, {0u, 0u}}) !=
            RIN_GPU_ERROR_INVALID_ARGUMENT ||
        ringpu_destroy(&core, bad_memory) != RIN_GPU_OK) {
        goto done;
    }
    result = 0;

done:
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return result;
}
