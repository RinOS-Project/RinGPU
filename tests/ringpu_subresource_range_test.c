/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackendDescV1 backend_desc;
    RinGpuDisplayInfoV1 display;
    RinGpuCoreConfigV1 config;
    RinGpuSoftwareBackend* backend = NULL;
    int api_result;

    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.struct_size = sizeof(backend_desc);
    backend_desc.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend_desc.max_total_bytes = UINT64_C(8) * 1024u * 1024u;
    api_result = ringpu_software_backend_create(&backend_desc, &backend);
    if (api_result != RIN_GPU_OK || !backend) {
        fprintf(stderr, "software backend create failed: %d\n", api_result);
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
    memcpy(display.name, "subresource-test", 16u);
    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x53554252414e4745);
    config.max_buffer_size = UINT64_C(1) << 20u;
    config.max_image_size = UINT64_C(1) << 20u;
    config.max_total_allocation_size = UINT64_C(4) << 20u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 4u;
    config.max_image_mip_levels = 4u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY |
        RIN_GPU_QUEUE_GRAPHICS;
    memcpy(config.adapter.name, "subresource-test", 16u);
    config.backend = *ringpu_software_backend_ops();
    config.backend_context = backend;
    config.displays = &display;
    config.display_count = 1u;
    config.backend_family = RIN_GPU_BACKEND_FAMILY_SOFTWARE;
    api_result = ringpu_core_init(core, &config);
    if (api_result != RIN_GPU_OK) {
        fprintf(stderr, "core init failed: %d\n", api_result);
        ringpu_software_backend_destroy(backend);
        return 0;
    }
    *backend_out = backend;
    return 1;
}

static int expect_state(const RinGpuCore* core, RinGpuHandle image,
                        uint32_t mip, uint32_t layer, uint32_t expected)
{
    uint32_t state = UINT32_MAX;
    return ringpu_get_image_state(core, image, mip, layer, &state) ==
               RIN_GPU_OK &&
           state == expected;
}

int main(void)
{
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuImageDescV1 color_desc;
    RinGpuImageDescV1 depth_stencil_desc;
    RinGpuImageTransitionV1 transition;
    RinGpuSubmitInfoV1 submit;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle color_image = 0u;
    RinGpuHandle depth_stencil_image = 0u;
    uint32_t state = UINT32_MAX;
    int result = 1;

    memset(&core, 0, sizeof(core));
    if (!make_core(&core, &backend)) return 1;

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY | RIN_GPU_QUEUE_GRAPHICS;
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = queue_desc.capabilities;
    result = 2;
    if (ringpu_create_queue(&core, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_create_command_list(&core, &command_desc, &command_list) !=
            RIN_GPU_OK) {
        goto done;
    }

    memset(&color_desc, 0, sizeof(color_desc));
    color_desc.abi_version = RIN_GPU_ABI_VERSION;
    color_desc.struct_size = sizeof(color_desc);
    color_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    color_desc.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    color_desc.width = 8u;
    color_desc.height = 8u;
    color_desc.depth = 1u;
    color_desc.array_layers = 2u;
    color_desc.mip_levels = 2u;
    color_desc.sample_count = 1u;
    color_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
        RIN_GPU_IMAGE_COPY_DESTINATION;
    result = 3;
    if (ringpu_create_image(&core, &color_desc, &color_image) != RIN_GPU_OK ||
        !expect_state(&core, color_image, 0u, 0u,
                      RIN_GPU_IMAGE_STATE_UNDEFINED) ||
        !expect_state(&core, color_image, 1u, 1u,
                      RIN_GPU_IMAGE_STATE_UNDEFINED)) {
        goto done;
    }

    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.base_mip_level = 1u;
    transition.mip_level_count = 1u;
    transition.base_array_layer = 1u;
    transition.array_layer_count = 1u;
    transition.aspect_mask = RIN_GPU_IMAGE_ASPECT_DEPTH;
    transition.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    result = 4;
    if (ringpu_command_transition_image(&core, command_list, color_image,
                                        &transition) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }
    transition.aspect_mask = RIN_GPU_IMAGE_ASPECT_COLOR;
    transition.base_mip_level = 2u;
    result = 5;
    if (ringpu_command_transition_image(&core, command_list, color_image,
                                        &transition) != RIN_GPU_ERROR_BOUNDS) {
        goto done;
    }
    transition.base_mip_level = 1u;
    result = 6;
    if (ringpu_command_transition_image(&core, command_list, color_image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }
    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    result = 7;
    if (ringpu_queue_submit(&core, queue, &submit) != RIN_GPU_OK ||
        !expect_state(&core, color_image, 0u, 0u,
                      RIN_GPU_IMAGE_STATE_UNDEFINED) ||
        !expect_state(&core, color_image, 0u, 1u,
                      RIN_GPU_IMAGE_STATE_UNDEFINED) ||
        !expect_state(&core, color_image, 1u, 0u,
                      RIN_GPU_IMAGE_STATE_UNDEFINED) ||
        !expect_state(&core, color_image, 1u, 1u,
                      RIN_GPU_IMAGE_STATE_COPY_DESTINATION)) {
        goto done;
    }

    if (ringpu_command_list_reset(&core, command_list) != RIN_GPU_OK)
        goto done;
    transition.base_mip_level = 0u;
    transition.base_array_layer = 0u;
    transition.aspect_mask = 0u;
    result = 9;
    if (ringpu_command_transition_image(&core, command_list, color_image,
                                        &transition) != RIN_GPU_OK ||
        ringpu_command_list_close(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }
    submit.command_list = command_list;
    result = 10;
    if (ringpu_queue_submit(&core, queue, &submit) != RIN_GPU_OK ||
        !expect_state(&core, color_image, 0u, 0u,
                      RIN_GPU_IMAGE_STATE_COPY_DESTINATION)) {
        goto done;
    }

    memset(&depth_stencil_desc, 0, sizeof(depth_stencil_desc));
    depth_stencil_desc.abi_version = RIN_GPU_ABI_VERSION;
    depth_stencil_desc.struct_size = sizeof(depth_stencil_desc);
    depth_stencil_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    depth_stencil_desc.format = RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
    depth_stencil_desc.width = 8u;
    depth_stencil_desc.height = 8u;
    depth_stencil_desc.depth = 1u;
    depth_stencil_desc.array_layers = 1u;
    depth_stencil_desc.mip_levels = 1u;
    depth_stencil_desc.sample_count = 1u;
    depth_stencil_desc.usage = RIN_GPU_IMAGE_DEPTH_STENCIL |
        RIN_GPU_IMAGE_SAMPLED;
    result = 11;
    if (ringpu_create_image(&core, &depth_stencil_desc,
                            &depth_stencil_image) != RIN_GPU_OK ||
        ringpu_command_list_reset(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }

    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.aspect_mask = RIN_GPU_IMAGE_ASPECT_DEPTH;
    transition.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transition.after_state = RIN_GPU_IMAGE_STATE_DEPTH_TARGET;
    result = 12;
    if (ringpu_command_transition_image(&core, command_list,
                                        depth_stencil_image, &transition) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT) {
        goto done;
    }
    transition.aspect_mask = RIN_GPU_IMAGE_ASPECT_DEPTH |
        RIN_GPU_IMAGE_ASPECT_STENCIL;
    result = 13;
    if (ringpu_command_transition_image(&core, command_list,
                                        depth_stencil_image, &transition) !=
            RIN_GPU_OK ||
        ringpu_command_list_close(&core, command_list) != RIN_GPU_OK) {
        goto done;
    }
    submit.command_list = command_list;
    result = 14;
    if (ringpu_queue_submit(&core, queue, &submit) != RIN_GPU_OK ||
        ringpu_get_image_state(&core, depth_stencil_image, 0u, 0u, &state) !=
            RIN_GPU_OK ||
        state != RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
        goto done;
    }

    result = 0;
done:
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return result;
}
