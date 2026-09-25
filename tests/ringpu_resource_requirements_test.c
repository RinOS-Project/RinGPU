/* SPDX-License-Identifier: MIT */
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
    result = 0;

done:
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    if (result != 0) {
        fprintf(stderr, "resource requirements test failed\n");
    }
    return result;
}
