// SPDX-License-Identifier: MIT
#include <ringpu/runtime.h>

#include "core/core.h"
#include "software/software_backend.h"
#include "validation/diagnostics.h"

#include <stdlib.h>
#include <string.h>

struct RinGpuRuntime {
    RinGpuCore core;
    RinGpuDiagnosticsRuntime diagnostics;
    RinGpuSoftwareBackend* software_backend;
    uint32_t initialized;
};

static int runtime_desc_valid(const RinGpuRuntimeSoftwareSurfaceDescV1* desc)
{
    if (desc == NULL || desc->struct_size != sizeof(*desc) ||
        desc->version != RIN_GPU_RUNTIME_VERSION ||
        desc->device_generation == 0u || desc->handle_secret == 0u ||
        desc->max_buffer_size == 0u || desc->max_image_size == 0u ||
        desc->max_total_allocation_size == 0u ||
        desc->max_image_dimension == 0u || desc->max_image_layers == 0u ||
        desc->max_image_mip_levels == 0u || desc->max_image_sample_count == 0u ||
        desc->adapter.struct_size != sizeof(desc->adapter) ||
        desc->adapter.abi_version != RIN_GPU_ABI_VERSION ||
        desc->display.struct_size != sizeof(desc->display) ||
        desc->display.abi_version != RIN_GPU_ABI_VERSION ||
        desc->display.width == 0u || desc->display.height == 0u ||
        desc->flags != 0u || desc->reserved0 != 0u ||
        desc->reserved[0] != 0u || desc->reserved[1] != 0u)
        return 0;
    if (desc->present_callback == NULL || desc->acquire_image == NULL)
        return 0;
    return 1;
}

int ringpu_runtime_software_surface_create(
    const RinGpuRuntimeSoftwareSurfaceDescV1* desc,
    RinGpuRuntime** runtime_out)
{
    RinGpuRuntime* runtime;
    RinGpuSoftwareBackendDescV3 software_desc;
    RinGpuCoreConfigV1 config;
    const RinGpuBackendOpsV1* backend_ops;
    int result;

    if (runtime_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (!runtime_desc_valid(desc)) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    runtime = calloc(1u, sizeof(*runtime));
    if (runtime == NULL) return RIN_GPU_ERROR_NO_MEMORY;

    memset(&software_desc, 0, sizeof(software_desc));
    software_desc.base.base.struct_size = sizeof(software_desc);
    software_desc.base.base.version = RIN_GPU_SOFTWARE_BACKEND_VERSION_3;
    software_desc.base.base.max_total_bytes = desc->max_total_allocation_size;
    software_desc.base.present_callback = desc->present_callback;
    software_desc.base.present_context = desc->present_context;
    software_desc.acquire_image = desc->acquire_image;
    software_desc.image_context = desc->image_context;
    result = ringpu_software_backend_create(&software_desc.base.base,
                                            &runtime->software_backend);
    if (result != RIN_GPU_OK) goto fail;
    backend_ops = ringpu_software_backend_ops();
    if (backend_ops == NULL) {
        result = RIN_GPU_ERROR_BACKEND;
        goto fail;
    }

    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = desc->handle_secret;
    config.max_buffer_size = desc->max_buffer_size;
    config.max_image_size = desc->max_image_size;
    config.max_total_allocation_size = desc->max_total_allocation_size;
    config.max_image_dimension = desc->max_image_dimension;
    config.max_image_layers = desc->max_image_layers;
    config.max_image_mip_levels = desc->max_image_mip_levels;
    config.max_image_sample_count = desc->max_image_sample_count;
    config.adapter = desc->adapter;
    config.backend = *backend_ops;
    config.backend_context = runtime->software_backend;
    if (rin_gpu_diagnostics_init(&runtime->diagnostics,
                                 desc->device_generation) != 0) {
        result = RIN_GPU_ERROR_BACKEND;
        goto fail;
    }
    config.diagnostics = &runtime->diagnostics;
    config.displays = &desc->display;
    config.display_count = 1u;
    result = ringpu_core_init(&runtime->core, &config);
    if (result != RIN_GPU_OK) goto fail;
    runtime->initialized = RIN_GPU_RUNTIME_VERSION;
    *runtime_out = runtime;
    return RIN_GPU_OK;

fail:
    if (runtime->initialized == RIN_GPU_RUNTIME_VERSION)
        ringpu_core_shutdown(&runtime->core);
    ringpu_software_backend_destroy(runtime->software_backend);
    free(runtime);
    return result;
}

void ringpu_runtime_destroy(RinGpuRuntime* runtime)
{
    if (runtime == NULL) return;
    if (runtime->initialized == RIN_GPU_RUNTIME_VERSION)
        ringpu_core_shutdown(&runtime->core);
    ringpu_software_backend_destroy(runtime->software_backend);
    memset(runtime, 0, sizeof(*runtime));
    free(runtime);
}

int ringpu_runtime_device_lost(const RinGpuRuntime* runtime)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return 1;
    return runtime->core.device_lost != 0u;
}

int ringpu_runtime_create_queue(RinGpuRuntime* runtime,
                                const RinGpuQueueDescV1* desc,
                                RinGpuHandle* queue_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_queue(&runtime->core, desc, queue_out);
}

int ringpu_runtime_create_command_list(
    RinGpuRuntime* runtime, const RinGpuCommandListDescV1* desc,
    RinGpuHandle* command_list_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_command_list(&runtime->core, desc, command_list_out);
}

int ringpu_runtime_create_image(RinGpuRuntime* runtime,
                                const RinGpuImageDescV1* desc,
                                RinGpuHandle* image_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_image(&runtime->core, desc, image_out);
}

int ringpu_runtime_command_list_reset(RinGpuRuntime* runtime,
                                      RinGpuHandle command_list)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_list_reset(&runtime->core, command_list);
}

int ringpu_runtime_command_list_close(RinGpuRuntime* runtime,
                                      RinGpuHandle command_list)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_list_close(&runtime->core, command_list);
}

int ringpu_runtime_command_transition_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageTransitionV1* transition)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_transition_image(&runtime->core, command_list, image,
                                           transition);
}

int ringpu_runtime_queue_submit(RinGpuRuntime* runtime, RinGpuHandle queue,
                                const RinGpuSubmitInfoV1* submit)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_queue_submit(&runtime->core, queue, submit);
}
