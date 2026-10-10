/* SPDX-License-Identifier: MIT */
#include <ringpu/runtime.h>

#include <string.h>

static int bytes_are_zero(const void* memory, size_t size)
{
    const unsigned char* bytes = (const unsigned char*)memory;
    for (size_t index = 0u; index < size; ++index)
        if (bytes[index] != 0u) return 0;
    return 1;
}

static int make_runtime(RinGpuRuntime** runtime_out)
{
    RinGpuRuntimeDescV1 desc;

    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.version = RIN_GPU_RUNTIME_VERSION;
    desc.device_generation = 1u;
    desc.handle_secret = UINT64_C(0x72756e74696d6531);
    desc.max_buffer_size = UINT64_C(1) << 20u;
    desc.max_image_size = UINT64_C(1) << 20u;
    desc.max_total_allocation_size = UINT64_C(4) << 20u;
    desc.max_image_dimension = 4096u;
    desc.max_image_layers = 64u;
    desc.max_image_mip_levels = 13u;
    desc.max_image_sample_count = 1u;
    desc.adapter.abi_version = RIN_GPU_ABI_VERSION;
    desc.adapter.struct_size = sizeof(desc.adapter);
    desc.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY |
        RIN_GPU_QUEUE_COMPUTE | RIN_GPU_QUEUE_GRAPHICS;
    memcpy(desc.adapter.name, "binding runtime", 16u);
    desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS;
    return ringpu_runtime_create(&desc, runtime_out) == RIN_GPU_OK &&
           *runtime_out != NULL;
}

static int make_memory(RinGpuRuntime* runtime,
                       const RinGpuResourceMemoryRequirementsV1* req,
                       RinGpuHandle* memory_out, uint64_t* offset_out)
{
    RinGpuMemoryDescV1 memory_desc;
    uint64_t offset;

    if (!runtime || !req || !memory_out || !offset_out ||
        req->alignment == 0u || req->size_bytes == 0u ||
        req->size_bytes > UINT64_MAX - req->alignment)
        return 0;
    offset = req->alignment;
    memset(&memory_desc, 0, sizeof(memory_desc));
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = req->size_bytes + offset;
    memory_desc.alignment = req->alignment;
    if (ringpu_runtime_create_memory(runtime, &memory_desc, memory_out) !=
            RIN_GPU_OK || *memory_out == 0u)
        return 0;
    *offset_out = offset;
    return 1;
}

int main(void)
{
    RinGpuRuntime* runtime = NULL;
    RinGpuBufferDescV1 buffer_desc;
    RinGpuImageDescV1 image_desc;
    RinGpuResourceMemoryRequirementsV1 buffer_req;
    RinGpuResourceMemoryRequirementsV1 image_req;
    RinGpuResourceMemoryRequirementsV1 queried_requirements;
    RinGpuResourceMemoryBindingV1 binding;
    RinGpuResourceMemoryBindingV1 queried;
    RinGpuHandle buffer = 0u;
    RinGpuHandle unbound_buffer = 0u;
    RinGpuHandle image = 0u;
    RinGpuHandle buffer_memory = 0u;
    RinGpuHandle image_memory = 0u;
    uint64_t buffer_offset = 0u;
    uint64_t image_offset = 0u;
    int result = 1;

    memset(&queried, 0xa5, sizeof(queried));
    if (ringpu_runtime_get_buffer_memory_binding(NULL, 1u, &queried) !=
            RIN_GPU_ERROR_STATE || !bytes_are_zero(&queried, sizeof(queried)))
        return 10;
    memset(&queried_requirements, 0xa5, sizeof(queried_requirements));
    if (ringpu_runtime_get_buffer_memory_requirements(
            NULL, NULL, &queried_requirements) != RIN_GPU_ERROR_STATE ||
        !bytes_are_zero(&queried_requirements,
                        sizeof(queried_requirements)))
        return 11;
    result = 2;
    if (!make_runtime(&runtime)) goto cleanup;
    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = 256u;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    if (ringpu_runtime_create_buffer(runtime, &buffer_desc, &buffer) !=
            RIN_GPU_OK ||
        ringpu_runtime_create_buffer(runtime, &buffer_desc,
                                     &unbound_buffer) != RIN_GPU_OK)
        { result = 3; goto cleanup; }
    memset(&queried, 0xa5, sizeof(queried));
    if (ringpu_runtime_get_buffer_memory_binding(runtime, unbound_buffer,
                                                  &queried) == RIN_GPU_OK)
        { result = 4; goto cleanup; }
    if (!bytes_are_zero(&queried, sizeof(queried)))
        { result = 5; goto cleanup; }
    if (ringpu_runtime_get_buffer_memory_requirements(
            runtime, &buffer_desc, &buffer_req) != RIN_GPU_OK)
        { result = 61; goto cleanup; }
    if (buffer_req.resource_type != RIN_GPU_RESOURCE_MEMORY_BUFFER)
        { result = 62; goto cleanup; }
    if (make_memory(runtime, &buffer_req, &buffer_memory, &buffer_offset) == 0)
        { result = 63; goto cleanup; }
    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.memory = buffer_memory;
    binding.offset_bytes = buffer_offset;
    binding.size_bytes = buffer_req.size_bytes;
    if (ringpu_runtime_bind_buffer_memory(runtime, buffer, &binding) !=
            RIN_GPU_OK) { result = 71; goto cleanup; }
    if (ringpu_runtime_get_buffer_memory_binding(runtime, buffer, &queried) !=
            RIN_GPU_OK) { result = 72; goto cleanup; }
    if (queried.memory != buffer_memory ||
        queried.offset_bytes != buffer_offset ||
        queried.size_bytes != buffer_req.size_bytes)
        { result = 73; goto cleanup; }
    memset(&queried, 0xa5, sizeof(queried));
    if (ringpu_runtime_get_image_memory_binding(runtime, buffer, &queried) ==
            RIN_GPU_OK || !bytes_are_zero(&queried, sizeof(queried)))
        { result = 74; goto cleanup; }

    memset(&image_desc, 0, sizeof(image_desc));
    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    image_desc.width = 4u;
    image_desc.height = 4u;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COPY_SOURCE |
                       RIN_GPU_IMAGE_COPY_DESTINATION;
    if (ringpu_runtime_create_image(runtime, &image_desc, &image) !=
            RIN_GPU_OK) { result = 81; goto cleanup; }
    if (ringpu_runtime_get_image_memory_requirements(
            runtime, &image_desc, &image_req) != RIN_GPU_OK)
        { result = 82; goto cleanup; }
    if (image_req.resource_type != RIN_GPU_RESOURCE_MEMORY_IMAGE)
        { result = 83; goto cleanup; }
    if (make_memory(runtime, &image_req, &image_memory, &image_offset) == 0)
        { result = 84; goto cleanup; }
    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.memory = image_memory;
    binding.offset_bytes = image_offset;
    binding.size_bytes = image_req.size_bytes;
    if (ringpu_runtime_bind_image_memory(runtime, image, &binding) !=
            RIN_GPU_OK ||
        ringpu_runtime_get_image_memory_binding(runtime, image, &queried) !=
            RIN_GPU_OK ||
        queried.memory != image_memory || queried.offset_bytes != image_offset ||
        queried.size_bytes != image_req.size_bytes)
        { result = 85; goto cleanup; }

    result = 0;

cleanup:
    if (runtime) ringpu_runtime_destroy(runtime);
    return result;
}
