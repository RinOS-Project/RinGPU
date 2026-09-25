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
    display.width = 4u;
    display.height = 4u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "bc1", 4u);
    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x4243315f54455354);
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
    memcpy(config.adapter.name, "bc1", 4u);
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
    static const uint8_t red_bc1_block[8] = {
        0x00u, 0xf8u, 0x00u, 0xf8u, 0x00u, 0x00u, 0x00u, 0x00u};
    uint8_t pixels[4u * 4u * 4u];
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuImageDescV1 image_desc;
    RinGpuImageUploadV1 upload;
    RinGpuImageReadbackV1 readback;
    RinGpuImageTransitionV1 transition;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuSubmitInfoV1 submit;
    RinGpuHandle image = 0u;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle fence = 0u;

    memset(&core, 0, sizeof(core));
    CHECK(make_core(&core, &backend));
    memset(&image_desc, 0, sizeof(image_desc));
    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BC1_RGBA_UNORM;
    image_desc.width = 4u;
    image_desc.height = 4u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
                       RIN_GPU_IMAGE_COPY_DESTINATION |
                       RIN_GPU_IMAGE_SAMPLED;
    image_desc.flags = RIN_GPU_IMAGE_CPU_VISIBLE |
                       RIN_GPU_IMAGE_CPU_READABLE;
    CHECK(ringpu_create_image(&core, &image_desc, &image) == RIN_GPU_OK);
    memset(&upload, 0, sizeof(upload));
    upload.abi_version = RIN_GPU_ABI_VERSION;
    upload.struct_size = sizeof(upload);
    upload.width = 4u;
    upload.height = 4u;
    upload.depth = 1u;
    CHECK(ringpu_upload_image(&core, image, &upload, red_bc1_block,
                              sizeof(red_bc1_block)) == RIN_GPU_OK);

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_queue(&core, &queue_desc, &queue) == RIN_GPU_OK);
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_command_list(&core, &command_desc, &command_list) ==
          RIN_GPU_OK);
    CHECK(ringpu_create_fence(&core, 0u, &fence) == RIN_GPU_OK);
    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    CHECK(ringpu_command_transition_image(&core, command_list, image,
                                          &transition) == RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, command_list) == RIN_GPU_OK);
    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    submit.signal_fence = fence;
    submit.signal_value = 1u;
    CHECK(ringpu_queue_submit(&core, queue, &submit) == RIN_GPU_OK);
    CHECK(ringpu_wait_fence(&core, fence, 1u, 0u) == RIN_GPU_OK);

    memset(&readback, 0, sizeof(readback));
    readback.abi_version = RIN_GPU_ABI_VERSION;
    readback.struct_size = sizeof(readback);
    readback.width = 4u;
    readback.height = 4u;
    readback.depth = 1u;
    memset(pixels, 0, sizeof(pixels));
    CHECK(ringpu_readback_image(&core, image, &readback, pixels,
                                sizeof(pixels)) == RIN_GPU_OK);
    for (uint32_t index = 0u; index < 16u; ++index) {
        CHECK(pixels[index * 4u + 0u] == 255u);
        CHECK(pixels[index * 4u + 1u] == 0u);
        CHECK(pixels[index * 4u + 2u] == 0u);
        CHECK(pixels[index * 4u + 3u] == 255u);
    }

    ringpu_destroy(&core, fence);
    ringpu_destroy(&core, command_list);
    ringpu_destroy(&core, queue);
    ringpu_destroy(&core, image);
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return 0;
}
