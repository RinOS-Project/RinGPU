/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

typedef struct ResolveStorage {
    uint8_t source[64];
    uint8_t destination[16];
} ResolveStorage;

static int present(void* context,
                   const RinGpuSoftwarePresentedImageV1* image)
{
    (void)context;
    return image != NULL ? RIN_GPU_OK : RIN_GPU_ERROR_INVALID_ARGUMENT;
}

static int acquire(void* context, const RinGpuImageDescV1* descriptor,
                   uint64_t allocation_bytes,
                   RinGpuSoftwareExternalImageV1* storage_out)
{
    ResolveStorage* storage = (ResolveStorage*)context;
    uint8_t* pixels;
    uint32_t sample;

    if (!storage || !descriptor || !storage_out ||
        allocation_bytes > sizeof(storage->source)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(storage_out, 0, sizeof(*storage_out));
    pixels = descriptor->sample_count > 1u ? storage->source
                                           : storage->destination;
    memset(pixels, 0, (size_t)allocation_bytes);
    if (descriptor->sample_count > 1u) {
        const uint8_t colors[4][4] = {
            {255u, 0u, 0u, 255u}, {0u, 255u, 0u, 255u},
            {0u, 0u, 255u, 255u}, {255u, 255u, 255u, 255u}
        };
        uint32_t plane_size = descriptor->width * descriptor->height * 4u;
        for (sample = 0u; sample < descriptor->sample_count; ++sample) {
            for (uint32_t pixel = 0u; pixel < plane_size; pixel += 4u)
                memcpy(pixels + sample * plane_size + pixel, colors[sample], 4u);
        }
    }
    storage_out->struct_size = sizeof(*storage_out);
    storage_out->version = RIN_GPU_SOFTWARE_EXTERNAL_IMAGE_VERSION;
    storage_out->pixels = pixels;
    storage_out->size_bytes = allocation_bytes;
    storage_out->row_pitch_bytes = descriptor->width * 4u;
    return RIN_GPU_OK;
}

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out,
                     ResolveStorage* storage)
{
    RinGpuSoftwareBackendDescV3 backend_desc = {0};
    RinGpuDisplayInfoV1 display = {0};
    RinGpuCoreConfigV1 config = {0};
    RinGpuSoftwareBackend* backend = NULL;

    backend_desc.base.base.struct_size = sizeof(backend_desc);
    backend_desc.base.base.version = RIN_GPU_SOFTWARE_BACKEND_VERSION_3;
    backend_desc.base.base.max_total_bytes = UINT64_C(1) << 20u;
    backend_desc.base.present_callback = present;
    backend_desc.base.present_context = storage;
    backend_desc.acquire_image = acquire;
    backend_desc.image_context = storage;
    CHECK(ringpu_software_backend_create(
              (const RinGpuSoftwareBackendDescV1*)&backend_desc, &backend) ==
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
    memcpy(display.name, "resolve", 8u);
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x5245534f4c564531);
    config.max_buffer_size = UINT64_C(1) << 20u;
    config.max_image_size = UINT64_C(1) << 20u;
    config.max_total_allocation_size = UINT64_C(1) << 20u;
    config.max_image_dimension = 64u;
    config.max_image_layers = 1u;
    config.max_image_mip_levels = 1u;
    config.max_image_sample_count = 4u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY;
    memcpy(config.adapter.name, "resolve", 8u);
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
    RinGpuCore core = {0};
    RinGpuSoftwareBackend* backend = NULL;
    ResolveStorage storage = {0};
    RinGpuQueueDescV1 queue_desc = {0};
    RinGpuCommandListDescV1 list_desc = {0};
    RinGpuImageDescV1 source_desc = {0};
    RinGpuImageDescV1 destination_desc = {0};
    RinGpuImageTransitionV1 transition = {0};
    RinGpuImageResolveV1 resolve = {0};
    RinGpuImageReadbackV1 readback = {0};
    RinGpuSubmitInfoV1 submit = {0};
    RinGpuHandle queue = 0u;
    RinGpuHandle list = 0u;
    RinGpuHandle source = 0u;
    RinGpuHandle destination = 0u;
    uint8_t output[16] = {0};

    CHECK(make_core(&core, &backend, &storage));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_queue(&core, &queue_desc, &queue) == RIN_GPU_OK);
    list_desc.abi_version = RIN_GPU_ABI_VERSION;
    list_desc.struct_size = sizeof(list_desc);
    list_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_command_list(&core, &list_desc, &list) == RIN_GPU_OK);

    source_desc.abi_version = RIN_GPU_ABI_VERSION;
    source_desc.struct_size = sizeof(source_desc);
    source_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    source_desc.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    source_desc.width = 2u;
    source_desc.height = 2u;
    source_desc.depth = 1u;
    source_desc.array_layers = 1u;
    source_desc.mip_levels = 1u;
    source_desc.sample_count = 4u;
    source_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE;
    destination_desc = source_desc;
    destination_desc.sample_count = 1u;
    destination_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
        RIN_GPU_IMAGE_COPY_DESTINATION;
    destination_desc.flags = RIN_GPU_IMAGE_CPU_READABLE;
    CHECK(ringpu_create_image(&core, &source_desc, &source) == RIN_GPU_OK);
    CHECK(ringpu_create_image(&core, &destination_desc, &destination) ==
          RIN_GPU_OK);

    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    CHECK(ringpu_command_transition_image(&core, list, source, &transition) ==
          RIN_GPU_OK);
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    CHECK(ringpu_command_transition_image(&core, list, destination,
                                          &transition) == RIN_GPU_OK);
    resolve.abi_version = RIN_GPU_ABI_VERSION;
    resolve.struct_size = sizeof(resolve);
    resolve.width = 2u;
    resolve.height = 2u;
    CHECK(ringpu_command_resolve_image(&core, list, destination, source,
                                       &resolve) == RIN_GPU_OK);
    transition.before_state = RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
    transition.after_state = RIN_GPU_IMAGE_STATE_COPY_SOURCE;
    CHECK(ringpu_command_transition_image(&core, list, destination,
                                          &transition) == RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, list) == RIN_GPU_OK);
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = list;
    CHECK(ringpu_queue_submit(&core, queue, &submit) == RIN_GPU_OK);

    readback.abi_version = RIN_GPU_ABI_VERSION;
    readback.struct_size = sizeof(readback);
    readback.width = 2u;
    readback.height = 2u;
    readback.depth = 1u;
    CHECK(ringpu_readback_image(&core, destination, &readback, output,
                                sizeof(output)) == RIN_GPU_OK);
    for (uint32_t pixel = 0u; pixel < sizeof(output); pixel += 4u) {
        CHECK(output[pixel] == 128u && output[pixel + 1u] == 128u &&
              output[pixel + 2u] == 128u && output[pixel + 3u] == 255u);
    }
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return 0;
}
