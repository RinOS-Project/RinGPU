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
    if (ringpu_software_backend_create(&backend_desc, &backend) !=
            RIN_GPU_OK || !backend)
        return 0;
    memset(&display, 0, sizeof(display));
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 1u;
    display.height = 1u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "srgb", 5u);
    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x535247425f544553);
    config.max_buffer_size = 1024u;
    config.max_image_size = 4096u;
    config.max_total_allocation_size = 2u * 1024u * 1024u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 1u;
    config.max_image_mip_levels = 1u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY;
    memcpy(config.adapter.name, "srgb", 5u);
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

static void image_desc(RinGpuImageDescV1* desc, uint32_t format)
{
    memset(desc, 0, sizeof(*desc));
    desc->abi_version = RIN_GPU_ABI_VERSION;
    desc->struct_size = sizeof(*desc);
    desc->dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    desc->format = format;
    desc->width = 1u;
    desc->height = 1u;
    desc->depth = 1u;
    desc->array_layers = 1u;
    desc->mip_levels = 1u;
    desc->sample_count = 1u;
    desc->usage = RIN_GPU_IMAGE_COPY_SOURCE |
                  RIN_GPU_IMAGE_COPY_DESTINATION |
                  RIN_GPU_IMAGE_SAMPLED |
                  RIN_GPU_IMAGE_COLOR_TARGET;
    desc->flags = RIN_GPU_IMAGE_CPU_VISIBLE | RIN_GPU_IMAGE_CPU_READABLE;
}

static void upload_desc(RinGpuImageUploadV1* upload)
{
    memset(upload, 0, sizeof(*upload));
    upload->abi_version = RIN_GPU_ABI_VERSION;
    upload->struct_size = sizeof(*upload);
    upload->width = 1u;
    upload->height = 1u;
    upload->depth = 1u;
}

static void readback_desc(RinGpuImageReadbackV1* readback)
{
    memset(readback, 0, sizeof(*readback));
    readback->abi_version = RIN_GPU_ABI_VERSION;
    readback->struct_size = sizeof(*readback);
    readback->width = 1u;
    readback->height = 1u;
    readback->depth = 1u;
}

static void transition_desc(RinGpuImageTransitionV1* transition,
                            uint32_t before, uint32_t after)
{
    memset(transition, 0, sizeof(*transition));
    transition->abi_version = RIN_GPU_ABI_VERSION;
    transition->struct_size = sizeof(*transition);
    transition->mip_level_count = 1u;
    transition->array_layer_count = 1u;
    transition->before_state = before;
    transition->after_state = after;
}

static int submit_once(RinGpuCore* core, RinGpuHandle queue,
                       RinGpuHandle command_list, RinGpuHandle fence,
                       uint64_t value)
{
    RinGpuSubmitInfoV1 submit;

    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    submit.signal_fence = fence;
    submit.signal_value = value;
    return ringpu_queue_submit(core, queue, &submit) == RIN_GPU_OK &&
           ringpu_wait_fence(core, fence, value, 0u) == RIN_GPU_OK;
}

int main(void)
{
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuImageDescV1 srgb_desc;
    RinGpuImageUploadV1 upload;
    RinGpuImageReadbackV1 readback;
    RinGpuImageClearV1 clear;
    RinGpuImageBlitV1 blit;
    RinGpuImageTransitionV1 transition;
    RinGpuHandle queue = 0u;
    RinGpuHandle clear_list = 0u;
    RinGpuHandle blit_list = 0u;
    RinGpuHandle fence = 0u;
    RinGpuHandle clear_image = 0u;
    RinGpuHandle source_image = 0u;
    RinGpuHandle destination_image = 0u;
    uint8_t clear_readback[4u];
    uint8_t source_pixels[4u] = {128u, 64u, 32u, 255u};
    uint8_t destination_pixels[4u] = {0u};
    int result = 1;

    memset(&core, 0, sizeof(core));
    if (!make_core(&core, &backend))
        return 1;
    image_desc(&srgb_desc, RIN_GPU_FORMAT_RGBA8_SRGB);
    if (ringpu_create_image(&core, &srgb_desc, &clear_image) != RIN_GPU_OK ||
        ringpu_create_image(&core, &srgb_desc, &source_image) != RIN_GPU_OK ||
        ringpu_create_image(&core, &srgb_desc, &destination_image) !=
            RIN_GPU_OK) {
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
        ringpu_create_command_list(&core, &command_desc, &clear_list) !=
            RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc, &blit_list) !=
            RIN_GPU_OK || ringpu_create_fence(&core, 0u, &fence) !=
            RIN_GPU_OK)
        goto done;

    memset(&upload, 0, sizeof(upload));
    upload_desc(&upload);
    if (ringpu_upload_image(&core, clear_image, &upload, source_pixels,
                            sizeof(source_pixels)) != RIN_GPU_OK ||
        ringpu_upload_image(&core, source_image, &upload, source_pixels,
                            sizeof(source_pixels)) != RIN_GPU_OK ||
        ringpu_upload_image(&core, destination_image, &upload,
                            destination_pixels, sizeof(destination_pixels)) !=
            RIN_GPU_OK)
        goto done;

    memset(&clear, 0, sizeof(clear));
    clear.abi_version = RIN_GPU_ABI_VERSION;
    clear.struct_size = sizeof(clear);
    clear.aspects = RIN_GPU_IMAGE_CLEAR_COLOR;
    clear.color_red = 0.5f;
    clear.color_green = 0.25f;
    clear.color_blue = 0.0f;
    clear.color_alpha = 1.0f;
    if (ringpu_command_clear_image(&core, clear_list, clear_image, &clear) !=
            RIN_GPU_OK)
        goto done;
    transition_desc(&transition, RIN_GPU_IMAGE_STATE_COPY_DESTINATION,
                    RIN_GPU_IMAGE_STATE_COPY_SOURCE);
    if (ringpu_command_transition_image(&core, clear_list, clear_image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, clear_list) != RIN_GPU_OK ||
        !submit_once(&core, queue, clear_list, fence, 1u))
        goto done;
    readback_desc(&readback);
    if (ringpu_readback_image(&core, clear_image, &readback, clear_readback,
                              sizeof(clear_readback)) != RIN_GPU_OK ||
        clear_readback[0] != 188u || clear_readback[1] != 137u ||
        clear_readback[2] != 0u || clear_readback[3] != 255u) {
        goto done;
    }

    transition_desc(&transition, RIN_GPU_IMAGE_STATE_COPY_DESTINATION,
                    RIN_GPU_IMAGE_STATE_COPY_SOURCE);
    if (ringpu_command_transition_image(&core, blit_list, source_image,
                                        &transition) != RIN_GPU_OK)
        goto done;
    memset(&blit, 0, sizeof(blit));
    blit.abi_version = RIN_GPU_ABI_VERSION;
    blit.struct_size = sizeof(blit);
    blit.source_width = 1u;
    blit.source_height = 1u;
    blit.destination_width = 1u;
    blit.destination_height = 1u;
    blit.filter = RIN_GPU_IMAGE_BLIT_NEAREST;
    if (ringpu_command_blit_image(&core, blit_list, destination_image,
                                  source_image, &blit) != RIN_GPU_OK)
        goto done;
    transition_desc(&transition, RIN_GPU_IMAGE_STATE_COPY_DESTINATION,
                    RIN_GPU_IMAGE_STATE_COPY_SOURCE);
    if (ringpu_command_transition_image(&core, blit_list, destination_image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, blit_list) != RIN_GPU_OK ||
        !submit_once(&core, queue, blit_list, fence, 2u))
        goto done;
    if (ringpu_readback_image(&core, destination_image, &readback,
                              destination_pixels,
                              sizeof(destination_pixels)) != RIN_GPU_OK ||
        destination_pixels[0] != 128u || destination_pixels[1] != 64u ||
        destination_pixels[2] != 32u || destination_pixels[3] != 255u) {
        goto done;
    }
    result = 0;

done:
    ringpu_destroy(&core, destination_image);
    ringpu_destroy(&core, source_image);
    ringpu_destroy(&core, clear_image);
    ringpu_destroy(&core, fence);
    ringpu_destroy(&core, blit_list);
    ringpu_destroy(&core, clear_list);
    ringpu_destroy(&core, queue);
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return result;
}
