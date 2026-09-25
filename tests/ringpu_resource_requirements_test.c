/* SPDX-License-Identifier: MIT */
#include <stdio.h>
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
    backend_desc.max_total_bytes = UINT64_C(8) * 1024u * 1024u;
    if (ringpu_software_backend_create(&backend_desc, &backend) !=
            RIN_GPU_OK ||
        !backend) {
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
    memcpy(display.name, "requirements", 12u);

    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x5245534f55524345);
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
    memcpy(config.adapter.name, "requirements", 12u);
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

int main(void)
{
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuBufferDescV1 buffer_desc;
    RinGpuImageDescV1 image_desc;
    RinGpuResourceMemoryRequirementsV1 requirements;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuSubmitInfoV1 submit;
    RinGpuSubmitInfoV2 submit_v2;
    RinGpuBufferClearV1 buffer_clear;
    RinGpuImageClearV1 image_clear;
    RinGpuImageBlitV1 image_blit;
    RinGpuImageUploadV1 upload;
    RinGpuImageReadbackV1 readback;
    RinGpuImageTransitionV1 transition;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle clear_command_list = 0u;
    RinGpuHandle blit_command_list = 0u;
    RinGpuHandle buffer = 0u;
    RinGpuHandle image = 0u;
    RinGpuHandle blit_image = 0u;
    RinGpuHandle fence = 0u;
    uint8_t image_source[64u * 32u * 4u] = {0};
    uint8_t image_readback[64u * 32u * 4u] = {0};
    int result = 1;

    memset(&core, 0, sizeof(core));
    if (!make_core(&core, &backend)) return 1;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = 4097u;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    if (ringpu_get_buffer_memory_requirements(
            &core, &buffer_desc, &requirements) != RIN_GPU_OK ||
        requirements.resource_type != RIN_GPU_RESOURCE_MEMORY_BUFFER ||
        requirements.size_bytes != 4352u || requirements.alignment != 256u ||
        requirements.memory_type_bits !=
            (RIN_GPU_RESOURCE_MEMORY_TYPE_LOCAL |
             RIN_GPU_RESOURCE_MEMORY_TYPE_SYSTEM) ||
        requirements.reserved0 != 0u || requirements.reserved[0] != 0u ||
        requirements.reserved[1] != 0u) {
        goto done;
    }
    buffer_desc.usage = 0u;
    if (ringpu_get_buffer_memory_requirements(
            &core, &buffer_desc, &requirements) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }

    memset(&image_desc, 0, sizeof(image_desc));
    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    image_desc.width = 64u;
    image_desc.height = 32u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
                       RIN_GPU_IMAGE_COPY_DESTINATION |
                       RIN_GPU_IMAGE_COLOR_TARGET;
    if (ringpu_get_image_memory_requirements(
            &core, &image_desc, &requirements) != RIN_GPU_OK ||
        requirements.resource_type != RIN_GPU_RESOURCE_MEMORY_IMAGE ||
        requirements.size_bytes != RIN_GPU_MEMORY_MIN_PAGE_SIZE * 2u ||
        requirements.alignment != RIN_GPU_MEMORY_MIN_PAGE_SIZE) {
        goto done;
    }
    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    buffer_desc.size_bytes = 16u;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = 0u;
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_COPY;
    if (ringpu_create_queue(&core, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_create_buffer(&core, &buffer_desc, &buffer) != RIN_GPU_OK) {
        goto done;
    }
    image_desc.flags = RIN_GPU_IMAGE_CPU_VISIBLE |
                       RIN_GPU_IMAGE_CPU_READABLE;
    if (ringpu_create_image(&core, &image_desc, &image) != RIN_GPU_OK) {
        goto done;
    }
    memset(&upload, 0, sizeof(upload));
    upload.abi_version = RIN_GPU_ABI_VERSION;
    upload.struct_size = sizeof(upload);
    upload.width = image_desc.width;
    upload.height = image_desc.height;
    upload.depth = 1u;
    if (ringpu_upload_image(&core, image, &upload, image_source,
                            sizeof(image_source)) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc,
                                   &clear_command_list) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc,
                                   &blit_command_list) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc, &command_list) !=
            RIN_GPU_OK ||
        ringpu_create_fence(&core, 0u, &fence) != RIN_GPU_OK) {
        goto done;
    }
    memset(&buffer_clear, 0, sizeof(buffer_clear));
    buffer_clear.abi_version = RIN_GPU_ABI_VERSION;
    buffer_clear.struct_size = sizeof(buffer_clear);
    buffer_clear.size_bytes = buffer_desc.size_bytes;
    buffer_clear.pattern = UINT32_C(0xa5c33c5a);
    if (ringpu_command_clear_buffer(&core, clear_command_list, buffer,
                                    &buffer_clear) != RIN_GPU_OK) {
        goto done;
    }
    memset(&image_clear, 0, sizeof(image_clear));
    image_clear.abi_version = RIN_GPU_ABI_VERSION;
    image_clear.struct_size = sizeof(image_clear);
    image_clear.aspects = RIN_GPU_IMAGE_CLEAR_COLOR;
    image_clear.color_red = 0.25f;
    image_clear.color_green = 0.5f;
    image_clear.color_blue = 0.75f;
    image_clear.color_alpha = 1.0f;
    if (ringpu_command_clear_image(&core, clear_command_list, image,
                                   &image_clear) != RIN_GPU_OK) {
        goto done;
    }
    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    if (ringpu_command_transition_image(&core, clear_command_list, image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, clear_command_list) != RIN_GPU_OK) {
        goto done;
    }
    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = clear_command_list;
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
    if (ringpu_readback_image(&core, image, &readback, image_readback,
                              sizeof(image_readback)) != RIN_GPU_OK ||
        image_readback[0] != 191u || image_readback[1] != 128u ||
        image_readback[2] != 64u || image_readback[3] != 255u ||
        ringpu_create_image(&core, &image_desc, &blit_image) != RIN_GPU_OK) {
        goto done;
    }
    memset(&upload, 0, sizeof(upload));
    upload.abi_version = RIN_GPU_ABI_VERSION;
    upload.struct_size = sizeof(upload);
    upload.width = image_desc.width;
    upload.height = image_desc.height;
    upload.depth = 1u;
    if (ringpu_upload_image(&core, blit_image, &upload, image_source,
                            sizeof(image_source)) != RIN_GPU_OK) {
        goto done;
    }
    memset(&image_blit, 0, sizeof(image_blit));
    image_blit.abi_version = RIN_GPU_ABI_VERSION;
    image_blit.struct_size = sizeof(image_blit);
    image_blit.source_width = image_desc.width;
    image_blit.source_height = image_desc.height;
    image_blit.destination_width = image_desc.width;
    image_blit.destination_height = image_desc.height;
    image_blit.filter = RIN_GPU_IMAGE_BLIT_LINEAR;
    if (ringpu_command_blit_image(&core, blit_command_list, blit_image, image,
                                  &image_blit) != RIN_GPU_OK ||
        ringpu_command_transition_image(&core, blit_command_list, blit_image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, blit_command_list) != RIN_GPU_OK) {
        goto done;
    }
    submit.command_list = blit_command_list;
    submit.signal_value = 2u;
    {
        int blit_submit_result = ringpu_queue_submit(&core, queue, &submit);
        int blit_wait_result = blit_submit_result == RIN_GPU_OK
            ? ringpu_wait_fence(&core, fence, 2u, 0u)
            : blit_submit_result;

        if (blit_submit_result != RIN_GPU_OK ||
            blit_wait_result != RIN_GPU_OK) {
            goto done;
        }
    }
    if (ringpu_readback_image(&core, blit_image, &readback, image_readback,
                              sizeof(image_readback)) != RIN_GPU_OK ||
        image_readback[0] != 191u || image_readback[1] != 128u ||
        image_readback[2] != 64u || image_readback[3] != 255u ||
        ringpu_command_list_close(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }
    memset(&submit_v2, 0, sizeof(submit_v2));
    submit_v2.base.abi_version = RIN_GPU_ABI_VERSION;
    submit_v2.base.struct_size = sizeof(submit_v2.base);
    submit_v2.base.command_list = command_list;
    submit_v2.base.signal_fence = fence;
    submit_v2.base.signal_value = 3u;
    submit_v2.wait_count = 1u;
    submit_v2.waits[0].abi_version = RIN_GPU_ABI_VERSION;
    submit_v2.waits[0].struct_size = sizeof(submit_v2.waits[0]);
    submit_v2.waits[0].fence = fence;
    submit_v2.waits[0].value = 1u;
    if (ringpu_queue_submit_v2(&core, queue, &submit_v2) != RIN_GPU_OK) {
        goto done;
    }
    submit_v2.base.signal_value = 4u;
    submit_v2.waits[0].value = 4u;
    if (ringpu_queue_submit_v2(&core, queue, &submit_v2) !=
        RIN_GPU_ERROR_BUSY) {
        goto done;
    }
    result = 0;

done:
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    if (result != 0) {
        fprintf(stderr, "resource requirements test failed\n");
    }
    return result;
}
