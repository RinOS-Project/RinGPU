/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackendDescV1 backend_desc = {0};
    RinGpuDisplayInfoV1 display = {0};
    RinGpuCoreConfigV1 config = {0};
    RinGpuSoftwareBackend* backend = NULL;

    backend_desc.struct_size = sizeof(backend_desc);
    backend_desc.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend_desc.max_total_bytes = UINT64_C(4) * 1024u * 1024u;
    CHECK(ringpu_software_backend_create(&backend_desc, &backend) ==
          RIN_GPU_OK && backend != NULL);
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 16u;
    display.height = 16u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "ownership", 9u);
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x4f574e4552534849);
    config.max_buffer_size = UINT64_C(1) << 20u;
    config.max_image_size = UINT64_C(1) << 20u;
    config.max_total_allocation_size = UINT64_C(2) << 20u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 2u;
    config.max_image_mip_levels = 2u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY;
    memcpy(config.adapter.name, "ownership", 9u);
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

static int submit(RinGpuCore* core, RinGpuHandle queue,
                  RinGpuHandle list)
{
    RinGpuSubmitInfoV1 info = {0};
    info.abi_version = RIN_GPU_ABI_VERSION;
    info.struct_size = sizeof(info);
    info.command_list = list;
    return ringpu_queue_submit(core, queue, &info);
}

int main(void)
{
    RinGpuCore core = {0};
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuQueueDescV1 queue_v1 = {0};
    RinGpuQueueDescV2 queue_v2 = {0};
    RinGpuCommandListDescV1 list_desc = {0};
    RinGpuImageDescV1 image_desc = {0};
    RinGpuImageOwnershipTransferV1 transfer = {0};
    RinGpuImageTransitionV1 transition = {0};
    RinGpuHandle queue0 = 0u;
    RinGpuHandle queue12 = 0u;
    RinGpuHandle list = 0u;
    RinGpuHandle image = 0u;

    CHECK(make_core(&core, &backend));
    queue_v1.abi_version = RIN_GPU_ABI_VERSION;
    queue_v1.struct_size = sizeof(queue_v1);
    queue_v1.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_queue(&core, &queue_v1, &queue0) == RIN_GPU_OK);
    queue_v2.base.abi_version = RIN_GPU_ABI_VERSION;
    queue_v2.base.struct_size = sizeof(queue_v2.base);
    queue_v2.base.capabilities = RIN_GPU_QUEUE_COPY;
    queue_v2.family_index = 1u;
    queue_v2.engine_index = 2u;
    CHECK(ringpu_create_queue_v2(&core, &queue_v2, &queue12) == RIN_GPU_OK);
    list_desc.abi_version = RIN_GPU_ABI_VERSION;
    list_desc.struct_size = sizeof(list_desc);
    list_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_command_list(&core, &list_desc, &list) == RIN_GPU_OK);
    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    image_desc.width = 2u;
    image_desc.height = 2u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE | RIN_GPU_IMAGE_COPY_DESTINATION;
    CHECK(ringpu_create_image(&core, &image_desc, &image) == RIN_GPU_OK);

    transfer.abi_version = RIN_GPU_ABI_VERSION;
    transfer.struct_size = sizeof(transfer);
    transfer.mip_level_count = 1u;
    transfer.array_layer_count = 1u;
    transfer.source_family_index = 0u;
    transfer.source_engine_index = 0u;
    transfer.destination_family_index = 1u;
    transfer.destination_engine_index = 2u;
    transfer.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transfer.after_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    CHECK(ringpu_command_transfer_image_ownership(&core, list, image,
                                                  &transfer) == RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, list) == RIN_GPU_OK);
    CHECK(submit(&core, queue12, list) == RIN_GPU_OK);

    CHECK(ringpu_command_list_reset(&core, list) == RIN_GPU_OK);
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    CHECK(ringpu_command_transition_image(&core, list, image, &transition) ==
          RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, list) == RIN_GPU_OK);
    CHECK(submit(&core, queue0, list) == RIN_GPU_ERROR_STATE);
    CHECK(submit(&core, queue12, list) == RIN_GPU_OK);

    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return 0;
}
