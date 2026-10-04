// SPDX-License-Identifier: MIT
#include <ringpu/runtime.h>

#include "core/core.h"
#include "platform/backend_bridge.h"
#ifndef RINGPU_BUILD_SOFTWARE_BACKEND
#define RINGPU_BUILD_SOFTWARE_BACKEND 1
#endif
#if RINGPU_BUILD_SOFTWARE_BACKEND
#include "software/software_backend.h"
#endif
#include "validation/diagnostics.h"

#include <stdlib.h>
#include <string.h>

struct RinGpuRuntime {
    RinGpuCore core;
    RinGpuDiagnosticsRuntime diagnostics;
    RinGpuPlatformThreadSchedulerV1 platform_scheduler;
    RinGpuPlatformDiagnosticCallbackV1 platform_diagnostic_callback;
    void* platform_diagnostic_context;
    RinGpuPlatformBackendAdapterV1 platform_backend_adapter;
#if RINGPU_BUILD_SOFTWARE_BACKEND
    RinGpuSoftwareBackend* software_backend;
#endif
    uint32_t initialized;
};

static int runtime_display_is_zero(const RinGpuDisplayInfoV1* display)
{
    const unsigned char* bytes = (const unsigned char*)display;
    size_t index;
    if (display == NULL) return 0;
    for (index = 0u; index < sizeof(*display); ++index)
        if (bytes[index] != 0u) return 0;
    return 1;
}

static int runtime_desc_valid(const RinGpuRuntimeDescV1* desc,
                              int allow_backend_resolution)
{
    int headless;
    int backend_resolution;
    if (desc == NULL) return 0;
    headless = (desc->flags & RIN_GPU_RUNTIME_FLAG_HEADLESS) != 0u;
    backend_resolution =
        allow_backend_resolution &&
        desc->backend_family >= RIN_GPU_RUNTIME_BACKEND_FAMILY_INTEL &&
        desc->backend_family <= RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO;
    if (desc->struct_size != sizeof(*desc) ||
        desc->version != RIN_GPU_RUNTIME_VERSION ||
        desc->device_generation == 0u || desc->handle_secret == 0u ||
        desc->max_buffer_size == 0u || desc->max_image_size == 0u ||
        desc->max_total_allocation_size == 0u ||
        desc->max_image_dimension == 0u || desc->max_image_layers == 0u ||
        desc->max_image_mip_levels == 0u || desc->max_image_sample_count == 0u ||
        desc->adapter.struct_size != sizeof(desc->adapter) ||
        desc->adapter.abi_version != RIN_GPU_ABI_VERSION ||
        (desc->flags & ~RIN_GPU_RUNTIME_FLAGS_KNOWN) != 0u ||
        desc->reserved0 != 0u || desc->reserved[0] != 0u ||
        desc->reserved[1] != 0u || desc->reserved1 != 0u)
        return 0;
    if (headless) {
        if (!runtime_display_is_zero(&desc->display)) return 0;
    } else if (desc->display.struct_size != sizeof(desc->display) ||
               desc->display.abi_version != RIN_GPU_ABI_VERSION ||
               desc->display.width == 0u || desc->display.height == 0u) {
        return 0;
    }
    if (desc->backend_ops == NULL) {
        if (desc->backend_context != NULL) return 0;
        if (desc->backend_family != RIN_GPU_RUNTIME_BACKEND_FAMILY_UNKNOWN &&
            desc->backend_family != RIN_GPU_RUNTIME_BACKEND_FAMILY_SOFTWARE &&
            !backend_resolution)
            return 0;
        /* Platform-resolved backends own presentation/acquisition; these
         * callbacks configure only a runtime-created software backend. */
        if (headless) {
            if (desc->present_callback != NULL ||
                desc->present_context != NULL || desc->acquire_image != NULL ||
                desc->image_context != NULL)
                return 0;
        } else if (!backend_resolution &&
                   (desc->present_callback == NULL ||
                    desc->acquire_image == NULL)) {
            return 0;
        }
    } else if (desc->backend_context == NULL ||
               desc->backend_family < RIN_GPU_RUNTIME_BACKEND_FAMILY_INTEL ||
               desc->backend_family > RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO ||
               (desc->flags &
                RIN_GPU_RUNTIME_FLAG_MEMORY_SUBALLOCATOR) != 0u ||
               desc->present_callback != NULL || desc->present_context != NULL ||
               desc->acquire_image != NULL || desc->image_context != NULL) {
        return 0;
    }
    return 1;
}

static int runtime_create_internal(
    const RinGpuRuntimeDescV1* desc,
    const RinGpuPlatformThreadSchedulerV1* platform_scheduler,
    RinGpuPlatformDiagnosticCallbackV1 platform_diagnostic_callback,
    void* platform_diagnostic_context,
    RinGpuPlatformResolveBackendCallbackV1 backend_resolver,
    void* backend_resolver_context,
    const RinGpuPlatformBackendBridgeV1* backend_bridge,
    void* backend_bridge_context,
    RinGpuRuntime** runtime_out)
{
    RinGpuRuntime* runtime;
    RinGpuRuntimeDescV1 resolved_desc;
#if RINGPU_BUILD_SOFTWARE_BACKEND
    RinGpuSoftwareBackendDescV3 software_desc;
    RinGpuSoftwareBackendDescV4 headless_software_desc;
    RinGpuSoftwareBackendDescV4 pooled_software_desc;
#endif
    RinGpuCoreConfigV1 config;
    const RinGpuBackendOpsV1* backend_ops;
    const RinGpuBackendOpsV1* external_ops;
    void* resolved_backend_context = NULL;
    int result;

    if (runtime_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (!runtime_desc_valid(desc, backend_resolver != NULL ||
                                      backend_bridge != NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (desc->backend_ops == NULL &&
        desc->backend_family >= RIN_GPU_RUNTIME_BACKEND_FAMILY_INTEL &&
        desc->backend_family <= RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO) {
        resolved_desc = *desc;
        if (backend_resolver != NULL) {
            const RinGpuBackendOpsV1* resolved_ops = NULL;
            result = backend_resolver(backend_resolver_context,
                                      desc->backend_family, &resolved_ops,
                                      &resolved_backend_context);
            if (result != RIN_GPU_OK)
                return result < RIN_GPU_OK ? result : RIN_GPU_ERROR_BACKEND;
            if (resolved_ops == NULL || resolved_backend_context == NULL)
                return RIN_GPU_ERROR_BACKEND;
            resolved_desc.backend_ops = resolved_ops;
            resolved_desc.backend_context = resolved_backend_context;
            desc = &resolved_desc;
            if (!runtime_desc_valid(desc, 0))
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
        } else if (backend_bridge == NULL || backend_bridge_context == NULL) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }
    if (platform_scheduler != NULL &&
        (platform_scheduler->struct_size != sizeof(*platform_scheduler) ||
         platform_scheduler->version !=
             RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION ||
         platform_scheduler->yield_thread == NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    runtime = calloc(1u, sizeof(*runtime));
    if (runtime == NULL) return RIN_GPU_ERROR_NO_MEMORY;
    if (platform_scheduler != NULL)
        runtime->platform_scheduler = *platform_scheduler;
    runtime->platform_diagnostic_callback = platform_diagnostic_callback;
    runtime->platform_diagnostic_context = platform_diagnostic_context;

    if (backend_bridge != NULL) {
        result = ringpu_platform_backend_adapter_init(
            &runtime->platform_backend_adapter, backend_bridge,
            backend_bridge_context, desc->backend_family);
        if (result != RIN_GPU_OK) goto fail;
        resolved_desc = *desc;
        resolved_desc.backend_ops =
            &runtime->platform_backend_adapter.backend_ops;
        resolved_desc.backend_context = &runtime->platform_backend_adapter;
        desc = &resolved_desc;
        if (!runtime_desc_valid(desc, 0)) {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
    }

    external_ops = (const RinGpuBackendOpsV1*)desc->backend_ops;
    if (external_ops != NULL) {
        backend_ops = external_ops;
    } else {
#if RINGPU_BUILD_SOFTWARE_BACKEND
        if ((desc->flags & RIN_GPU_RUNTIME_FLAG_HEADLESS) != 0u) {
            memset(&headless_software_desc, 0,
                   sizeof(headless_software_desc));
            headless_software_desc.base.base.base.struct_size =
                sizeof(headless_software_desc);
            headless_software_desc.base.base.base.version =
                RIN_GPU_SOFTWARE_BACKEND_VERSION_4;
            headless_software_desc.base.base.base.max_total_bytes =
                desc->max_total_allocation_size;
            headless_software_desc.base.base.base.flags =
                RIN_GPU_SOFTWARE_BACKEND_FLAG_HEADLESS |
                (((desc->flags &
                   RIN_GPU_RUNTIME_FLAG_MEMORY_SUBALLOCATOR) != 0u)
                     ? RIN_GPU_SOFTWARE_BACKEND_FLAG_MEMORY_SUBALLOCATOR
                     : 0u);
            result = ringpu_software_backend_create(
                &headless_software_desc.base.base.base,
                &runtime->software_backend);
            if (result != RIN_GPU_OK) goto fail;
            backend_ops = ringpu_software_backend_ops();
        } else if ((desc->flags &
                    RIN_GPU_RUNTIME_FLAG_MEMORY_SUBALLOCATOR) != 0u) {
            memset(&pooled_software_desc, 0, sizeof(pooled_software_desc));
            pooled_software_desc.base.base.base.struct_size =
                sizeof(pooled_software_desc);
            pooled_software_desc.base.base.base.version =
                RIN_GPU_SOFTWARE_BACKEND_VERSION_4;
            pooled_software_desc.base.base.base.max_total_bytes =
                desc->max_total_allocation_size;
            pooled_software_desc.base.base.base.flags =
                RIN_GPU_SOFTWARE_BACKEND_FLAG_MEMORY_SUBALLOCATOR;
            pooled_software_desc.base.base.present_callback =
                desc->present_callback;
            pooled_software_desc.base.base.present_context =
                desc->present_context;
            pooled_software_desc.base.acquire_image = desc->acquire_image;
            pooled_software_desc.base.image_context = desc->image_context;
            result = ringpu_software_backend_create(
                &pooled_software_desc.base.base.base,
                &runtime->software_backend);
            if (result != RIN_GPU_OK) goto fail;
            backend_ops = ringpu_software_backend_ops();
        } else {
            memset(&software_desc, 0, sizeof(software_desc));
            software_desc.base.base.struct_size = sizeof(software_desc);
            software_desc.base.base.version =
                RIN_GPU_SOFTWARE_BACKEND_VERSION_3;
            software_desc.base.base.max_total_bytes =
                desc->max_total_allocation_size;
            software_desc.base.present_callback = desc->present_callback;
            software_desc.base.present_context = desc->present_context;
            software_desc.acquire_image = desc->acquire_image;
            software_desc.image_context = desc->image_context;
            result = ringpu_software_backend_create(
                &software_desc.base.base, &runtime->software_backend);
            if (result != RIN_GPU_OK) goto fail;
            backend_ops = ringpu_software_backend_ops();
        }
#else
        result = RIN_GPU_ERROR_UNSUPPORTED;
        goto fail;
#endif
    }
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
    config.backend_context = external_ops != NULL ? desc->backend_context
#if RINGPU_BUILD_SOFTWARE_BACKEND
                                                  : runtime->software_backend;
#else
                                                  : NULL;
#endif
    config.backend_family = external_ops != NULL
                                ? desc->backend_family
#if RINGPU_BUILD_SOFTWARE_BACKEND
                                : RIN_GPU_RUNTIME_BACKEND_FAMILY_SOFTWARE;
#else
                                : RIN_GPU_RUNTIME_BACKEND_FAMILY_UNKNOWN;
#endif
    if (rin_gpu_diagnostics_init(&runtime->diagnostics,
                                 desc->device_generation) != 0) {
        result = RIN_GPU_ERROR_BACKEND;
        goto fail;
    }
    config.diagnostics = &runtime->diagnostics;
    config.platform_yield_thread =
        runtime->platform_scheduler.yield_thread;
    config.platform_scheduler_context = runtime->platform_scheduler.context;
    config.platform_diagnostic_callback =
        runtime->platform_diagnostic_callback;
    config.platform_diagnostic_context =
        runtime->platform_diagnostic_context;
    config.displays = (desc->flags & RIN_GPU_RUNTIME_FLAG_HEADLESS) != 0u
                          ? NULL
                          : &desc->display;
    config.display_count = (desc->flags & RIN_GPU_RUNTIME_FLAG_HEADLESS) != 0u
                               ? 0u
                               : 1u;
    result = ringpu_core_init(&runtime->core, &config);
    if (result != RIN_GPU_OK) goto fail;
    runtime->initialized = RIN_GPU_RUNTIME_VERSION;
    *runtime_out = runtime;
    return RIN_GPU_OK;

fail:
    if (runtime->initialized == RIN_GPU_RUNTIME_VERSION)
        ringpu_core_shutdown(&runtime->core);
#if RINGPU_BUILD_SOFTWARE_BACKEND
    if (runtime->software_backend != NULL)
        ringpu_software_backend_destroy(runtime->software_backend);
#endif
    free(runtime);
    return result;
}

int ringpu_runtime_create(const RinGpuRuntimeDescV1* desc,
                          RinGpuRuntime** runtime_out)
{
    return runtime_create_internal(desc, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, runtime_out);
}

int ringpu_runtime_create_with_platform(
    const RinGpuRuntimeDescV1* desc,
    const RinGpuPlatformThreadSchedulerV1* scheduler,
    RinGpuRuntime** runtime_out)
{
    if (runtime_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (scheduler == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return runtime_create_internal(desc, scheduler, NULL, NULL, NULL, NULL,
                                   NULL, NULL, runtime_out);
}

int ringpu_runtime_create_with_platform_services(
    const RinGpuRuntimeDescV1* desc,
    const RinGpuPlatformServicesV1* services,
    RinGpuRuntime** runtime_out)
{
    const RinGpuPlatformThreadSchedulerV1* scheduler;
    int scheduler_is_zero;
    if (runtime_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (services == NULL ||
        services->struct_size != sizeof(*services) ||
        services->version != RIN_GPU_PLATFORM_SERVICES_VERSION ||
        (services->diagnostic_callback == NULL &&
         services->diagnostic_context != NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    scheduler_is_zero =
        services->thread_scheduler.struct_size == 0u &&
        services->thread_scheduler.version == 0u &&
        services->thread_scheduler.yield_thread == NULL &&
        services->thread_scheduler.context == NULL;
    scheduler = scheduler_is_zero ? NULL : &services->thread_scheduler;
    if (!scheduler_is_zero &&
        (services->thread_scheduler.struct_size !=
             sizeof(services->thread_scheduler) ||
         services->thread_scheduler.version !=
             RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION ||
         services->thread_scheduler.yield_thread == NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    return runtime_create_internal(desc, scheduler,
                                   services->diagnostic_callback,
                                   services->diagnostic_context, NULL, NULL,
                                   NULL, NULL, runtime_out);
}

int ringpu_runtime_create_with_platform_services_v2(
    const RinGpuRuntimeDescV1* desc,
    const RinGpuPlatformServicesV2* services,
    RinGpuRuntime** runtime_out)
{
    const RinGpuPlatformThreadSchedulerV1* scheduler;
    int scheduler_is_zero;
    if (runtime_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (services == NULL ||
        services->struct_size != sizeof(*services) ||
        services->version != RIN_GPU_PLATFORM_SERVICES_V2_VERSION ||
        (services->diagnostic_callback == NULL &&
         services->diagnostic_context != NULL) ||
        (services->resolve_backend == NULL &&
         services->backend_resolver_context != NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    scheduler_is_zero =
        services->thread_scheduler.struct_size == 0u &&
        services->thread_scheduler.version == 0u &&
        services->thread_scheduler.yield_thread == NULL &&
        services->thread_scheduler.context == NULL;
    scheduler = scheduler_is_zero ? NULL : &services->thread_scheduler;
    if (!scheduler_is_zero &&
        (services->thread_scheduler.struct_size !=
             sizeof(services->thread_scheduler) ||
         services->thread_scheduler.version !=
             RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION ||
         services->thread_scheduler.yield_thread == NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    return runtime_create_internal(desc, scheduler,
                                   services->diagnostic_callback,
                                   services->diagnostic_context,
                                   services->resolve_backend,
                                   services->backend_resolver_context,
                                   NULL, NULL, runtime_out);
}

int ringpu_runtime_create_with_platform_services_v3(
    const RinGpuRuntimeDescV1* desc,
    const RinGpuPlatformServicesV3* services,
    RinGpuRuntime** runtime_out)
{
    const RinGpuPlatformThreadSchedulerV1* scheduler;
    const RinGpuPlatformBackendBridgeV1* bridge = NULL;
    void* bridge_context = NULL;
    int scheduler_is_zero;
    int result;
    if (runtime_out == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (services == NULL ||
        services->struct_size != sizeof(*services) ||
        services->version != RIN_GPU_PLATFORM_SERVICES_V3_VERSION ||
        (services->diagnostic_callback == NULL &&
         services->diagnostic_context != NULL) ||
        (services->resolve_backend == NULL &&
         services->backend_resolver_context != NULL) ||
        (services->resolve_backend_bridge == NULL &&
         services->backend_bridge_resolver_context != NULL) ||
        (services->resolve_backend != NULL &&
         services->resolve_backend_bridge != NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    scheduler_is_zero =
        services->thread_scheduler.struct_size == 0u &&
        services->thread_scheduler.version == 0u &&
        services->thread_scheduler.yield_thread == NULL &&
        services->thread_scheduler.context == NULL;
    scheduler = scheduler_is_zero ? NULL : &services->thread_scheduler;
    if (!scheduler_is_zero &&
        (services->thread_scheduler.struct_size !=
             sizeof(services->thread_scheduler) ||
         services->thread_scheduler.version !=
             RIN_GPU_PLATFORM_THREAD_SCHEDULER_VERSION ||
         services->thread_scheduler.yield_thread == NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!runtime_desc_valid(
            desc, services->resolve_backend != NULL ||
                      services->resolve_backend_bridge != NULL))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    if (services->resolve_backend_bridge != NULL && desc->backend_ops == NULL &&
        desc->backend_family >= RIN_GPU_RUNTIME_BACKEND_FAMILY_INTEL &&
        desc->backend_family <= RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO) {
        result = services->resolve_backend_bridge(
            services->backend_bridge_resolver_context, desc->backend_family,
            &bridge, &bridge_context);
        if (result != RIN_GPU_OK)
            return result < RIN_GPU_OK ? result : RIN_GPU_ERROR_BACKEND;
        if (bridge == NULL || bridge_context == NULL)
            return RIN_GPU_ERROR_BACKEND;
    }

    return runtime_create_internal(
        desc, scheduler, services->diagnostic_callback,
        services->diagnostic_context, services->resolve_backend,
        services->backend_resolver_context, bridge, bridge_context,
        runtime_out);
}

int ringpu_runtime_software_surface_create(
    const RinGpuRuntimeSoftwareSurfaceDescV1* desc,
    RinGpuRuntime** runtime_out)
{
    return ringpu_runtime_create(desc, runtime_out);
}

void ringpu_runtime_destroy(RinGpuRuntime* runtime)
{
    if (runtime == NULL) return;
    if (runtime->initialized == RIN_GPU_RUNTIME_VERSION)
        ringpu_core_shutdown(&runtime->core);
#if RINGPU_BUILD_SOFTWARE_BACKEND
    if (runtime->software_backend != NULL)
        ringpu_software_backend_destroy(runtime->software_backend);
#endif
    memset(runtime, 0, sizeof(*runtime));
    free(runtime);
}

int ringpu_runtime_device_lost(const RinGpuRuntime* runtime)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return 1;
    return runtime->core.device_lost != 0u;
}

void ringpu_runtime_mark_device_lost(RinGpuRuntime* runtime)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return;
    ringpu_core_mark_device_lost(&runtime->core);
}

int ringpu_runtime_get_device_generation(const RinGpuRuntime* runtime,
                                         uint64_t* generation_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_get_device_generation(&runtime->core, generation_out);
}

int ringpu_runtime_get_adapter_info(const RinGpuRuntime* runtime,
                                    RinGpuAdapterInfoV1* info)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_get_adapter_info(&runtime->core, info);
}

int ringpu_runtime_get_adapter_capabilities(
    const RinGpuRuntime* runtime,
    RinGpuAdapterCapabilitiesV1* capabilities)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_get_adapter_capabilities(&runtime->core, capabilities);
}

int ringpu_runtime_create_buffer(RinGpuRuntime* runtime,
                                 const RinGpuBufferDescV1* desc,
                                 RinGpuHandle* buffer_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_buffer(&runtime->core, desc, buffer_out);
}

int ringpu_runtime_create_memory(RinGpuRuntime* runtime,
                                 const RinGpuMemoryDescV1* desc,
                                 RinGpuHandle* memory_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_memory(&runtime->core, desc, memory_out);
}

int ringpu_runtime_bind_buffer_memory(
    RinGpuRuntime* runtime, RinGpuHandle buffer,
    const RinGpuResourceMemoryBindingV1* binding)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_bind_buffer_memory(&runtime->core, buffer, binding);
}

int ringpu_runtime_upload_buffer(RinGpuRuntime* runtime,
                                 RinGpuHandle buffer, uint64_t destination_offset,
                                 const void* source, uint64_t size_bytes)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_upload_buffer(&runtime->core, buffer, destination_offset,
                                source, size_bytes);
}

int ringpu_runtime_readback_buffer(RinGpuRuntime* runtime,
                                   RinGpuHandle buffer, uint64_t source_offset,
                                   void* destination, uint64_t size_bytes)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_readback_buffer(&runtime->core, buffer, source_offset,
                                  destination, size_bytes);
}

int ringpu_runtime_get_buffer_info(const RinGpuRuntime* runtime,
                                   RinGpuHandle buffer,
                                   RinGpuBufferInfoV1* info)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_get_buffer_info(&runtime->core, buffer, info);
}

int ringpu_runtime_create_queue(RinGpuRuntime* runtime,
                                const RinGpuQueueDescV1* desc,
                                RinGpuHandle* queue_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_queue(&runtime->core, desc, queue_out);
}

int ringpu_runtime_create_queue_v2(RinGpuRuntime* runtime,
                                   const RinGpuQueueDescV2* desc,
                                   RinGpuHandle* queue_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_queue_v2(&runtime->core, desc, queue_out);
}

int ringpu_runtime_create_fence(RinGpuRuntime* runtime,
                                uint64_t initial_value,
                                RinGpuHandle* fence_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_fence(&runtime->core, initial_value, fence_out);
}

int ringpu_runtime_create_command_list(
    RinGpuRuntime* runtime, const RinGpuCommandListDescV1* desc,
    RinGpuHandle* command_list_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_command_list(&runtime->core, desc, command_list_out);
}

int ringpu_runtime_create_query(RinGpuRuntime* runtime,
                                const RinGpuQueryDescV1* desc,
                                RinGpuHandle* query_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_query(&runtime->core, desc, query_out);
}

int ringpu_runtime_get_image_info(const RinGpuRuntime* runtime,
                                  RinGpuHandle image,
                                  RinGpuImageInfoV1* info)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_get_image_info(&runtime->core, image, info);
}

int ringpu_runtime_create_image(RinGpuRuntime* runtime,
                                const RinGpuImageDescV1* desc,
                                RinGpuHandle* image_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_image(&runtime->core, desc, image_out);
}

int ringpu_runtime_bind_image_memory(
    RinGpuRuntime* runtime, RinGpuHandle image,
    const RinGpuResourceMemoryBindingV1* binding)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_bind_image_memory(&runtime->core, image, binding);
}

int ringpu_runtime_upload_image(RinGpuRuntime* runtime, RinGpuHandle image,
                                const RinGpuImageUploadV1* upload,
                                const void* source, uint64_t source_size)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_upload_image(&runtime->core, image, upload, source,
                               source_size);
}

int ringpu_runtime_create_sampler(RinGpuRuntime* runtime,
                                  const RinGpuSamplerDescV1* desc,
                                  RinGpuHandle* sampler_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_sampler(&runtime->core, desc, sampler_out);
}

int ringpu_runtime_create_shader_module(RinGpuRuntime* runtime,
                                        const void* rin_shader_ir,
                                        uint64_t shader_size,
                                        RinGpuHandle* shader_module_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_shader_module(&runtime->core, rin_shader_ir,
                                       shader_size, shader_module_out);
}

int ringpu_runtime_create_graphics_pipeline_vertex(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_pipeline_vertex(
        &runtime->core, desc, attributes, attribute_count, pipeline_out);
}

int ringpu_runtime_create_graphics_pipeline_vertex_bindings(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_pipeline_vertex_bindings(
        &runtime->core, desc, attributes, attribute_count, vertex_bindings,
        vertex_binding_count, pipeline_out);
}

int ringpu_runtime_create_graphics_pipeline_native(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_pipeline_native(
        &runtime->core, desc, attributes, attribute_count, varyings,
        varying_count, pipeline_out);
}

int ringpu_runtime_create_graphics_pipeline_native_v2(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_pipeline_native_v2(
        &runtime->core, desc, attributes, attribute_count, varyings,
        varying_count, pipeline_out);
}

int ringpu_runtime_create_graphics_pipeline_native_vertex_bindings(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_pipeline_native_vertex_bindings(
        &runtime->core, desc, attributes, attribute_count, vertex_bindings,
        vertex_binding_count, varyings, varying_count, pipeline_out);
}

int ringpu_runtime_create_graphics_pipeline_native_vertex_bindings_v2(
    RinGpuRuntime* runtime, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_pipeline_native_vertex_bindings_v2(
        &runtime->core, desc, attributes, attribute_count, vertex_bindings,
        vertex_binding_count, varyings, varying_count, pipeline_out);
}

int ringpu_runtime_create_compute_pipeline(
    RinGpuRuntime* runtime, const RinGpuComputePipelineDescV1* desc,
    RinGpuHandle* pipeline_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_compute_pipeline(&runtime->core, desc, pipeline_out);
}

int ringpu_runtime_create_graphics_bind_group_typed(
    RinGpuRuntime* runtime, RinGpuHandle pipeline,
    const RinGpuGraphicsBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_graphics_bind_group_typed(
        &runtime->core, pipeline, bindings, binding_count, bind_group_out);
}

int ringpu_runtime_create_compute_bind_group(
    RinGpuRuntime* runtime, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_create_compute_bind_group(&runtime->core, pipeline, bindings,
                                            binding_count, bind_group_out);
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

int ringpu_runtime_command_begin_query(RinGpuRuntime* runtime,
                                       RinGpuHandle command_list,
                                       RinGpuHandle query)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_begin_query(&runtime->core, command_list, query);
}

int ringpu_runtime_command_end_query(RinGpuRuntime* runtime,
                                     RinGpuHandle command_list,
                                     RinGpuHandle query)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_end_query(&runtime->core, command_list, query);
}

int ringpu_runtime_command_reset_query(RinGpuRuntime* runtime,
                                       RinGpuHandle command_list,
                                       RinGpuHandle query)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_reset_query(&runtime->core, command_list, query);
}

int ringpu_runtime_command_compute_barrier_v2(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuComputeBarrierV2* barrier)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_compute_barrier_v2(&runtime->core, command_list,
                                             barrier);
}

int ringpu_runtime_command_graphics_barrier_v2(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuGraphicsBarrierV2* barrier)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_graphics_barrier_v2(&runtime->core, command_list,
                                              barrier);
}

int ringpu_runtime_command_set_push_constants(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuPushConstantsV1* constants)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_set_push_constants(&runtime->core, command_list,
                                             constants);
}

int ringpu_runtime_command_copy_buffer(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, uint64_t destination_offset,
    RinGpuHandle source, uint64_t source_offset, uint64_t size_bytes)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_copy_buffer(&runtime->core, command_list,
                                      destination, destination_offset, source,
                                      source_offset, size_bytes);
}

int ringpu_runtime_command_clear_buffer(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, const RinGpuBufferClearV1* clear)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_clear_buffer(&runtime->core, command_list,
                                       destination, clear);
}

int ringpu_runtime_command_copy_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, RinGpuHandle source,
    const RinGpuImageCopyRegionV1* region)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_copy_image(&runtime->core, command_list, destination,
                                     source, region);
}

int ringpu_runtime_command_blit_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, RinGpuHandle source,
    const RinGpuImageBlitV1* blit)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_blit_image(&runtime->core, command_list, destination,
                                     source, blit);
}

int ringpu_runtime_command_clear_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, const RinGpuImageClearV1* clear)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_clear_image(&runtime->core, command_list, destination,
                                      clear);
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

int ringpu_runtime_command_resolve_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle destination, RinGpuHandle source,
    const RinGpuImageResolveV1* resolve)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_resolve_image(&runtime->core, command_list,
                                        destination, source, resolve);
}

int ringpu_runtime_command_transfer_image_ownership(
    RinGpuRuntime* runtime, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageOwnershipTransferV1* transfer)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_transfer_image_ownership(
        &runtime->core, command_list, image, transfer);
}

int ringpu_runtime_command_begin_render_pass(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassDescV1* render_pass)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_begin_render_pass(&runtime->core, command_list,
                                            render_pass);
}

int ringpu_runtime_command_begin_render_pass_mrt(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassMrtDescV1* render_pass)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_begin_render_pass_mrt(&runtime->core, command_list,
                                                render_pass);
}

int ringpu_runtime_command_begin_render_pass_depth(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassDepthDescV1* render_pass)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_begin_render_pass_depth(&runtime->core, command_list,
                                                  render_pass);
}

int ringpu_runtime_command_begin_render_pass_depth_stencil(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRenderPassDepthStencilDescV1* render_pass)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_begin_render_pass_depth_stencil(
        &runtime->core, command_list, render_pass);
}

int ringpu_runtime_command_bind_graphics_resources(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    RinGpuHandle bind_group)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_bind_graphics_resources(&runtime->core, command_list,
                                                  bind_group);
}

int ringpu_runtime_command_set_raster_state(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuRasterStateV1* state)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_set_raster_state(&runtime->core, command_list,
                                           state);
}

int ringpu_runtime_command_draw(RinGpuRuntime* runtime,
                                RinGpuHandle command_list,
                                const RinGpuDrawV1* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw(&runtime->core, command_list, draw);
}

int ringpu_runtime_command_draw_vertices(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawVerticesV1* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw_vertices(&runtime->core, command_list, draw);
}

int ringpu_runtime_command_draw_vertices_v2(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawVerticesV2* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw_vertices_v2(&runtime->core, command_list, draw);
}

int ringpu_runtime_command_draw_indexed(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndexedV1* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw_indexed(&runtime->core, command_list, draw);
}

int ringpu_runtime_command_draw_indexed_v2(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndexedV2* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw_indexed_v2(&runtime->core, command_list, draw);
}

int ringpu_runtime_command_dispatch(RinGpuRuntime* runtime,
                                    RinGpuHandle command_list,
                                    const RinGpuDispatchV1* dispatch)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_dispatch(&runtime->core, command_list, dispatch);
}

int ringpu_runtime_command_dispatch_indirect(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDispatchIndirectV1* dispatch)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_dispatch_indirect(&runtime->core, command_list,
                                            dispatch);
}

int ringpu_runtime_command_draw_indirect(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndirectV1* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw_indirect(&runtime->core, command_list, draw);
}

int ringpu_runtime_command_draw_indexed_indirect(
    RinGpuRuntime* runtime, RinGpuHandle command_list,
    const RinGpuDrawIndexedIndirectV1* draw)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_draw_indexed_indirect(&runtime->core, command_list,
                                                draw);
}

int ringpu_runtime_command_end_render_pass(RinGpuRuntime* runtime,
                                           RinGpuHandle command_list)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_end_render_pass(&runtime->core, command_list);
}

int ringpu_runtime_command_present(RinGpuRuntime* runtime,
                                   RinGpuHandle command_list,
                                   const RinGpuPresentV1* present)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_command_present(&runtime->core, command_list, present);
}

int ringpu_runtime_wait_fence(RinGpuRuntime* runtime, RinGpuHandle fence,
                              uint64_t value, uint64_t timeout_ns)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_wait_fence(&runtime->core, fence, value, timeout_ns);
}

int ringpu_runtime_get_fence_value(const RinGpuRuntime* runtime,
                                   RinGpuHandle fence, uint64_t* value_out)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_fence_value(&runtime->core, fence, value_out);
}

int ringpu_runtime_get_query_result(RinGpuRuntime* runtime,
                                    RinGpuHandle query, uint32_t flags,
                                    RinGpuQueryResultV1* result)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_get_query_result(&runtime->core, query, flags, result);
}

int ringpu_runtime_readback_image(
    RinGpuRuntime* runtime, RinGpuHandle image,
    const RinGpuImageReadbackV1* readback, void* destination,
    uint64_t destination_size)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_readback_image(&runtime->core, image, readback, destination,
                                 destination_size);
}

int ringpu_runtime_destroy_object(RinGpuRuntime* runtime,
                                  RinGpuHandle object)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_destroy(&runtime->core, object);
}

int ringpu_runtime_queue_submit(RinGpuRuntime* runtime, RinGpuHandle queue,
                                const RinGpuSubmitInfoV1* submit)
{
    if (runtime == NULL || runtime->initialized != RIN_GPU_RUNTIME_VERSION)
        return RIN_GPU_ERROR_STATE;
    return ringpu_queue_submit(&runtime->core, queue, submit);
}
