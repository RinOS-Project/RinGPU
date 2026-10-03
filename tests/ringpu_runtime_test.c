/* SPDX-License-Identifier: MIT */
#include <ringpu/runtime.h>

#include "../src/core/core.h"

#include <string.h>

static const RinGpuBackendOpsV1* external_backend_delegate;
static uint32_t external_create_buffer_calls;
static uint32_t external_destroy_buffer_calls;
static uint32_t external_fail_next_create_buffer;
static uint32_t external_create_image_calls;
static uint32_t external_destroy_image_calls;
static uint32_t external_fail_next_create_image;

static int external_create_buffer(
    void* context, const RinGpuBufferDescV1* desc, uint64_t* cookie_out)
{
    ++external_create_buffer_calls;
    if (external_fail_next_create_buffer != 0u) {
        --external_fail_next_create_buffer;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_ERROR_BACKEND;
    }
    return external_backend_delegate->create_buffer(context, desc,
                                                     cookie_out);
}

static void external_destroy_buffer(void* context, uint64_t cookie)
{
    ++external_destroy_buffer_calls;
    external_backend_delegate->destroy_buffer(context, cookie);
}

static int external_create_image(
    void* context, const RinGpuImageDescV1* desc, uint64_t allocation_bytes,
    uint64_t* cookie_out)
{
    ++external_create_image_calls;
    if (external_fail_next_create_image != 0u) {
        --external_fail_next_create_image;
        if (cookie_out != NULL) *cookie_out = 0u;
        return RIN_GPU_ERROR_BACKEND;
    }
    return external_backend_delegate->create_image(
        context, desc, allocation_bytes, cookie_out);
}

static void external_destroy_image(void* context, uint64_t cookie)
{
    ++external_destroy_image_calls;
    external_backend_delegate->destroy_image(context, cookie);
}

static int present(void* context, const RinGpuSoftwarePresentedImageV1* image)
{
    if (image == NULL || image->pixels == NULL || context == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    (*(uint32_t*)context)++;
    return RIN_GPU_OK;
}

static int acquire(void* context, const RinGpuImageDescV1* descriptor,
                   uint64_t allocation_bytes,
                   RinGpuSoftwareExternalImageV1* storage)
{
    (void)context;
    (void)descriptor;
    (void)allocation_bytes;
    if (storage == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(storage, 0, sizeof(*storage));
    return RIN_GPU_OK;
}

int main(void)
{
    RinGpuRuntimeSoftwareSurfaceDescV1 desc = {0};
    RinGpuRuntime* runtime = NULL;
    RinGpuQueueDescV1 queue_desc = {0};
    RinGpuCommandListDescV1 command_desc = {0};
    RinGpuImageDescV1 image_desc = {0};
    RinGpuImageDescV1 external_image_desc = {0};
    RinGpuImageTransitionV1 transition = {0};
    RinGpuImageOwnershipTransferV1 ownership_transfer = {0};
    RinGpuPresentV1 present_desc = {0};
    RinGpuSubmitInfoV1 submit = {0};
    RinGpuAdapterCapabilitiesV1 capabilities = {0};
    RinGpuRuntimeDescV1 headless_desc = {0};
    RinGpuRuntimeDescV1 external_desc = {0};
    RinGpuBufferDescV1 buffer_desc = {0};
    RinGpuSoftwareBackendDescV4 external_backend_desc = {0};
    RinGpuSoftwareBackend* external_backend = NULL;
    RinGpuBackendOpsV1 external_ops;
    RinGpuBackendOpsV1 missing_ops;
    RinGpuRuntime* external_runtime = NULL;
    RinGpuRuntime* rejected_runtime = NULL;
    RinGpuMemoryDescV1 memory_desc = {0};
    RinGpuResourceMemoryBindingV1 memory_binding = {0};
    RinGpuQueueDescV1 headless_queue_desc = {0};
    RinGpuCommandListDescV1 headless_command_desc = {0};
    RinGpuSubmitInfoV1 headless_submit = {0};
    RinGpuPresentV1 headless_present = {0};
    RinGpuHandle queue = 0u;
    RinGpuHandle fence = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle present_queue = 0u;
    RinGpuHandle present_list = 0u;
    RinGpuHandle image = 0u;
    RinGpuHandle headless_source = 0u;
    RinGpuHandle headless_destination = 0u;
    RinGpuHandle headless_source_memory = 0u;
    RinGpuHandle headless_destination_memory = 0u;
    RinGpuHandle headless_queue = 0u;
    RinGpuHandle headless_command_list = 0u;
    RinGpuHandle headless_fence = 0u;
    RinGpuHandle external_buffer = 0u;
    RinGpuHandle failed_external_buffer = UINT64_C(1);
    RinGpuHandle external_image = 0u;
    RinGpuHandle failed_external_image = UINT64_C(1);
    uint64_t generation = 0u;
    uint32_t present_count = 0u;
    uint8_t upload_bytes[8] = {0x52u, 0x69u, 0x6eu, 0x47u,
                               0x50u, 0x55u, 0x01u, 0xa5u};
    uint8_t readback_bytes[sizeof(upload_bytes)] = {0u};

    desc.struct_size = sizeof(desc);
    desc.version = RIN_GPU_RUNTIME_VERSION;
    desc.device_generation = 1u;
    desc.handle_secret = UINT64_C(0x52554e54494d4531);
    desc.max_buffer_size = 1024u * 1024u;
    desc.max_image_size = 1024u * 1024u;
    desc.max_total_allocation_size = 4u * 1024u * 1024u;
    desc.max_image_dimension = 64u;
    desc.max_image_layers = 1u;
    desc.max_image_mip_levels = 1u;
    desc.max_image_sample_count = 1u;
    desc.adapter.abi_version = RIN_GPU_ABI_VERSION;
    desc.adapter.struct_size = sizeof(desc.adapter);
    desc.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS |
                                      RIN_GPU_QUEUE_PRESENT;
    memcpy(desc.adapter.name, "runtime-test", 12u);
    desc.display.abi_version = RIN_GPU_ABI_VERSION;
    desc.display.struct_size = sizeof(desc.display);
    desc.display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    desc.display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    desc.display.width = 16u;
    desc.display.height = 16u;
    desc.display.refresh_millihertz = 60000u;
    desc.display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    desc.display.physical_width_mm = 1u;
    desc.display.physical_height_mm = 1u;
    desc.display.scale_milli = 1000u;
    memcpy(desc.display.name, "runtime-test", 12u);
    desc.present_callback = present;
    desc.present_context = &present_count;
    desc.acquire_image = acquire;
    if (ringpu_runtime_software_surface_create(&desc, &runtime) != RIN_GPU_OK)
        return 1;
    if (ringpu_runtime_get_device_generation(runtime, &generation) !=
            RIN_GPU_OK ||
        generation != desc.device_generation ||
        ringpu_runtime_device_lost(runtime))
        return 6;
    if (ringpu_runtime_get_adapter_capabilities(runtime, &capabilities) !=
            RIN_GPU_OK ||
        capabilities.abi_version != RIN_GPU_ABI_VERSION ||
        capabilities.struct_size != sizeof(capabilities) ||
        capabilities.queue_capabilities != desc.adapter.queue_capabilities ||
        capabilities.feature_flags != RIN_GPU_ADAPTER_KNOWN_FEATURES ||
        capabilities.max_buffer_size != desc.max_buffer_size ||
        capabilities.max_image_size != desc.max_image_size ||
        capabilities.max_total_allocation_size !=
            desc.max_total_allocation_size ||
        capabilities.max_image_dimension != desc.max_image_dimension ||
        capabilities.max_image_layers != desc.max_image_layers ||
        capabilities.max_image_mip_levels != desc.max_image_mip_levels ||
        capabilities.max_image_sample_count != desc.max_image_sample_count)
        return 10;

    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS;
    if (ringpu_runtime_create_queue(runtime, &queue_desc, &queue) != RIN_GPU_OK ||
        ringpu_runtime_create_command_list(runtime, &command_desc,
                                           &command_list) != RIN_GPU_OK)
        return 2;
    if (ringpu_runtime_create_fence(runtime, 1u, &fence) != RIN_GPU_OK ||
        ringpu_runtime_wait_fence(runtime, fence, 1u, 0u) != RIN_GPU_OK)
        return 8;

    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    image_desc.width = 16u;
    image_desc.height = 16u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COLOR_TARGET | RIN_GPU_IMAGE_PRESENT;
    if (ringpu_runtime_create_image(runtime, &image_desc, &image) != RIN_GPU_OK)
        return 3;
    if (ringpu_runtime_readback_image(runtime, image, NULL, NULL, 0u) !=
        RIN_GPU_ERROR_INVALID_ARGUMENT)
        return 9;

    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = RIN_GPU_IMAGE_STATE_UNDEFINED;
    transition.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    if (ringpu_runtime_command_transition_image(runtime, command_list, image,
                                                &transition) != RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime, command_list) != RIN_GPU_OK)
        return 4;
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    if (ringpu_runtime_queue_submit(runtime, queue, &submit) != RIN_GPU_OK)
        return 5;

    if (ringpu_runtime_command_list_reset(runtime, command_list) != RIN_GPU_OK)
        return 11;
    transition.before_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    transition.after_state = RIN_GPU_IMAGE_STATE_PRESENT;
    if (ringpu_runtime_command_transition_image(runtime, command_list, image,
                                                &transition) != RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime, command_list) != RIN_GPU_OK ||
        ringpu_runtime_queue_submit(runtime, queue, &submit) != RIN_GPU_OK)
        return 12;

    queue_desc.capabilities = RIN_GPU_QUEUE_PRESENT;
    command_desc.capabilities = RIN_GPU_QUEUE_PRESENT;
    if (ringpu_runtime_create_queue(runtime, &queue_desc, &present_queue) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_command_list(runtime, &command_desc,
                                           &present_list) != RIN_GPU_OK)
        return 13;
    ownership_transfer.abi_version = RIN_GPU_ABI_VERSION;
    ownership_transfer.struct_size = sizeof(ownership_transfer);
    ownership_transfer.mip_level_count = 1u;
    ownership_transfer.array_layer_count = 1u;
    ownership_transfer.source_family_index = 0u;
    ownership_transfer.destination_family_index = 1u;
    ownership_transfer.before_state = RIN_GPU_IMAGE_STATE_PRESENT;
    ownership_transfer.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    if (ringpu_runtime_command_transfer_image_ownership(
            runtime, present_list, image, &ownership_transfer) !=
        RIN_GPU_ERROR_STATE)
        return 15;
    present_desc.abi_version = RIN_GPU_ABI_VERSION;
    present_desc.struct_size = sizeof(present_desc);
    present_desc.image = image;
    present_desc.display_id = RIN_GPU_PRIMARY_DISPLAY;
    submit.command_list = present_list;
    if (ringpu_runtime_command_present(runtime, present_list, &present_desc) !=
            RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime, present_list) != RIN_GPU_OK ||
        ringpu_runtime_queue_submit(runtime, present_queue, &submit) !=
            RIN_GPU_OK ||
        present_count != 1u)
        return 14;

    ringpu_runtime_mark_device_lost(runtime);
    if (!ringpu_runtime_device_lost(runtime))
        return 7;
    ringpu_runtime_destroy(runtime);

    headless_desc = desc;
    headless_desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS;
    memset(&headless_desc.display, 0, sizeof(headless_desc.display));
    headless_desc.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS |
                                               RIN_GPU_QUEUE_COPY;
    headless_desc.present_callback = NULL;
    headless_desc.present_context = NULL;
    headless_desc.acquire_image = NULL;
    headless_desc.image_context = NULL;
    if (ringpu_runtime_create(&headless_desc, &runtime) != RIN_GPU_OK)
        return 16;
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = sizeof(upload_bytes);
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = UINT64_C(256);
    memory_desc.alignment = UINT64_C(256);
    if (ringpu_runtime_create_memory(runtime, &memory_desc,
                                     &headless_source_memory) != RIN_GPU_OK ||
        ringpu_runtime_create_buffer(runtime, &buffer_desc,
                                     &headless_source) != RIN_GPU_OK)
        return 17;
    memory_binding.abi_version = RIN_GPU_ABI_VERSION;
    memory_binding.struct_size = sizeof(memory_binding);
    memory_binding.memory = headless_source_memory;
    memory_binding.size_bytes = UINT64_C(256);
    if (ringpu_runtime_bind_buffer_memory(runtime, headless_source,
                                          &memory_binding) != RIN_GPU_OK)
        return 19;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_DESTINATION;
    if (ringpu_runtime_create_memory(runtime, &memory_desc,
                                     &headless_destination_memory) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_buffer(runtime, &buffer_desc,
                                     &headless_destination) != RIN_GPU_OK)
        return 20;
    memory_binding.memory = headless_destination_memory;
    if (ringpu_runtime_bind_buffer_memory(runtime, headless_destination,
                                          &memory_binding) != RIN_GPU_OK ||
        ringpu_runtime_upload_buffer(runtime, headless_source, 0u,
                                     upload_bytes, sizeof(upload_bytes)) !=
            RIN_GPU_OK ||
        ringpu_runtime_upload_buffer(runtime, headless_destination, 0u,
                                     readback_bytes,
                                     sizeof(readback_bytes)) != RIN_GPU_OK)
        return 21;
    headless_queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    headless_queue_desc.struct_size = sizeof(headless_queue_desc);
    headless_queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    headless_command_desc.abi_version = RIN_GPU_ABI_VERSION;
    headless_command_desc.struct_size = sizeof(headless_command_desc);
    headless_command_desc.capabilities = RIN_GPU_QUEUE_COPY;
    if (ringpu_runtime_create_queue(runtime, &headless_queue_desc,
                                    &headless_queue) != RIN_GPU_OK ||
        ringpu_runtime_create_command_list(runtime, &headless_command_desc,
                                           &headless_command_list) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_fence(runtime, 0u, &headless_fence) !=
            RIN_GPU_OK ||
        ringpu_runtime_command_copy_buffer(
            runtime, headless_command_list, headless_destination, 0u,
            headless_source, 0u, sizeof(upload_bytes)) != RIN_GPU_OK ||
        ringpu_runtime_command_list_close(runtime,
                                          headless_command_list) !=
            RIN_GPU_OK)
        return 22;
    headless_submit.abi_version = RIN_GPU_ABI_VERSION;
    headless_submit.struct_size = sizeof(headless_submit);
    headless_submit.command_list = headless_command_list;
    headless_submit.signal_fence = headless_fence;
    headless_submit.signal_value = 1u;
    if (ringpu_runtime_queue_submit(runtime, headless_queue,
                                    &headless_submit) != RIN_GPU_OK ||
        ringpu_runtime_wait_fence(runtime, headless_fence, 1u, 0u) !=
            RIN_GPU_OK ||
        ringpu_runtime_readback_buffer(runtime, headless_destination, 0u,
                                       readback_bytes,
                                       sizeof(readback_bytes)) != RIN_GPU_OK ||
        memcmp(upload_bytes, readback_bytes, sizeof(upload_bytes)) != 0)
        return 23;
    headless_present.abi_version = RIN_GPU_ABI_VERSION;
    headless_present.struct_size = sizeof(headless_present);
    headless_present.display_id = RIN_GPU_PRIMARY_DISPLAY;
    if (ringpu_runtime_command_present(runtime, 0u, &headless_present) !=
        RIN_GPU_ERROR_UNSUPPORTED)
        return 18;
    ringpu_runtime_destroy(runtime);

    external_backend_desc.base.base.base.struct_size =
        sizeof(external_backend_desc);
    external_backend_desc.base.base.base.version =
        RIN_GPU_SOFTWARE_BACKEND_VERSION_4;
    external_backend_desc.base.base.base.max_total_bytes =
        headless_desc.max_total_allocation_size;
    external_backend_desc.base.base.base.flags =
        RIN_GPU_SOFTWARE_BACKEND_FLAG_HEADLESS;
    if (ringpu_software_backend_create(
            &external_backend_desc.base.base.base, &external_backend) !=
        RIN_GPU_OK)
        return 24;
    external_backend_delegate = ringpu_software_backend_ops();
    external_ops = *external_backend_delegate;
    external_ops.create_buffer = external_create_buffer;
    external_ops.destroy_buffer = external_destroy_buffer;
    external_ops.create_image = external_create_image;
    external_ops.destroy_image = external_destroy_image;
    external_desc = headless_desc;
    external_desc.backend_ops = &external_ops;
    external_desc.backend_context = external_backend;
    external_desc.backend_family = RIN_GPU_RUNTIME_BACKEND_FAMILY_VIRTIO;
    if (ringpu_runtime_create(&external_desc, &external_runtime) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                     &external_buffer) != RIN_GPU_OK ||
        external_create_buffer_calls != 1u || external_buffer == 0u)
        return 25;

    external_fail_next_create_buffer = 1u;
    if (ringpu_runtime_create_buffer(external_runtime, &buffer_desc,
                                     &failed_external_buffer) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_buffer != 0u || external_create_buffer_calls != 2u)
        return 26;

    external_image_desc = image_desc;
    external_image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
                                RIN_GPU_IMAGE_COPY_DESTINATION;
    external_fail_next_create_image = 1u;
    if (ringpu_runtime_create_image(external_runtime, &external_image_desc,
                                   &failed_external_image) !=
            RIN_GPU_ERROR_BACKEND ||
        failed_external_image != 0u || external_create_image_calls != 1u)
        return 29;
    if (ringpu_runtime_create_image(external_runtime, &external_image_desc,
                                    &external_image) != RIN_GPU_OK ||
        external_image == 0u || external_create_image_calls != 2u)
        return 30;

    missing_ops = external_ops;
    missing_ops.create_buffer = NULL;
    external_desc.backend_ops = &missing_ops;
    if (ringpu_runtime_create(&external_desc, &rejected_runtime) !=
            RIN_GPU_ERROR_INVALID_ARGUMENT ||
        rejected_runtime != NULL || external_create_buffer_calls != 2u)
        return 31;

    ringpu_runtime_destroy(external_runtime);
    if (external_destroy_buffer_calls != 1u ||
        external_destroy_image_calls != 1u) {
        ringpu_software_backend_destroy(external_backend);
        return 32;
    }
    ringpu_software_backend_destroy(external_backend);
    return 0;
}
