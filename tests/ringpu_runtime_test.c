/* SPDX-License-Identifier: MIT */
#include <ringpu/runtime.h>

#include <string.h>

static int present(void* context, const RinGpuSoftwarePresentedImageV1* image)
{
    (void)context;
    return image != NULL && image->pixels != NULL ? RIN_GPU_OK
                                                   : RIN_GPU_ERROR_INVALID_ARGUMENT;
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
    RinGpuImageTransitionV1 transition = {0};
    RinGpuSubmitInfoV1 submit = {0};
    RinGpuHandle queue = 0u;
    RinGpuHandle fence = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle image = 0u;
    uint64_t generation = 0u;

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
    desc.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS;
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
    desc.acquire_image = acquire;
    if (ringpu_runtime_software_surface_create(&desc, &runtime) != RIN_GPU_OK)
        return 1;
    if (ringpu_runtime_get_device_generation(runtime, &generation) !=
            RIN_GPU_OK ||
        generation != desc.device_generation ||
        ringpu_runtime_device_lost(runtime))
        return 6;

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
    ringpu_runtime_mark_device_lost(runtime);
    if (!ringpu_runtime_device_lost(runtime))
        return 7;
    ringpu_runtime_destroy(runtime);
    return 0;
}
