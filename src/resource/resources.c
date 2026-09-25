// SPDX-License-Identifier: MIT
#include "resources.h"

#include "../core/object_table.h"
#include "../software/software_backend.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"

#include <stdlib.h>
#include <string.h>

static int ringpu_software_memory_binding_supported(const RinGpuCore* core)
{
    const RinGpuBackendOpsV1* software_ops;

    if (!core || core->backend_family != RIN_GPU_BACKEND_FAMILY_SOFTWARE)
        return 0;
    software_ops = ringpu_software_backend_ops();
    return software_ops != NULL &&
        core->backend.create_buffer == software_ops->create_buffer &&
        core->backend.destroy_buffer == software_ops->destroy_buffer &&
        core->backend.create_image == software_ops->create_image &&
        core->backend.destroy_image == software_ops->destroy_image;
}

static int ringpu_memory_desc_valid(const RinGpuMemoryDescV1* desc)
{
    uint64_t alignment;

    if (!desc || !ringpu_versioned(desc->abi_version, desc->struct_size,
                                   sizeof(*desc)) || desc->size_bytes == 0u ||
        (desc->flags & ~RIN_GPU_MEMORY_BINDING_KNOWN_FLAGS) != 0u ||
        desc->reserved0 != 0u || desc->reserved[0] != 0u ||
        desc->reserved[1] != 0u) {
        return 0;
    }
    alignment = desc->alignment == 0u ? 1u : desc->alignment;
    return (alignment & (alignment - 1u)) == 0u &&
        alignment <= RIN_GPU_MEMORY_MAX_ALIGNMENT;
}

static int ringpu_memory_binding_valid(
    const RinGpuResourceMemoryBindingV1* binding)
{
    return binding != NULL &&
        ringpu_versioned(binding->abi_version, binding->struct_size,
                         sizeof(*binding)) && binding->memory != 0u &&
        binding->size_bytes != 0u && binding->reserved[0] == 0u &&
        binding->reserved[1] == 0u;
}

static int ringpu_memory_ranges_overlap(uint64_t left_offset,
                                        uint64_t left_size,
                                        uint64_t right_offset,
                                        uint64_t right_size)
{
    return left_offset < right_offset + right_size &&
        right_offset < left_offset + left_size;
}

static int ringpu_memory_binding_common(
    RinGpuCore* core, const RinGpuResourceMemoryBindingV1* binding,
    const RinGpuResourceMemoryRequirementsV1* requirements,
    RinGpuObjectSlot** memory_slot_out)
{
    RinGpuObjectSlot* memory_slot;
    uint64_t end;
    int result;

    if (!ringpu_memory_binding_valid(binding) || !requirements ||
        binding->size_bytes != requirements->size_bytes ||
        binding->offset_bytes > UINT64_MAX - binding->size_bytes) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, binding->memory, RIN_GPU_OBJECT_MEMORY, NULL,
                         &memory_slot);
    if (result != RIN_GPU_OK) return result;
    end = binding->offset_bytes + binding->size_bytes;
    if (binding->offset_bytes % requirements->alignment != 0u ||
        binding->offset_bytes % memory_slot->value.memory.alignment != 0u ||
        end > memory_slot->value.memory.size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((memory_slot->value.memory.flags &
         RIN_GPU_MEMORY_BINDING_DEDICATED) != 0u &&
        (memory_slot->value.memory.size_bytes != requirements->size_bytes ||
         binding->offset_bytes != 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; ++index) {
        const RinGpuObjectSlot* slot = &core->objects[index];
        uint64_t occupied_offset;
        uint64_t occupied_size;

        if (!slot->occupied || slot->type == RIN_GPU_OBJECT_MEMORY)
            continue;
        if (slot->type == RIN_GPU_OBJECT_BUFFER) {
            if (slot->value.buffer.memory_handle != binding->memory)
                continue;
            occupied_offset = slot->value.buffer.memory_offset;
            occupied_size = slot->value.buffer.memory_size;
        } else if (slot->type == RIN_GPU_OBJECT_IMAGE) {
            if (slot->value.image.memory_handle != binding->memory)
                continue;
            occupied_offset = slot->value.image.memory_offset;
            occupied_size = slot->value.image.memory_size;
        } else {
            continue;
        }
        if (ringpu_memory_ranges_overlap(binding->offset_bytes,
                                          binding->size_bytes,
                                          occupied_offset,
                                          occupied_size)) {
            return RIN_GPU_ERROR_BOUNDS;
        }
    }
    *memory_slot_out = memory_slot;
    return RIN_GPU_OK;
}

static void ringpu_memory_binding_commit(RinGpuObjectSlot* resource_slot,
                                         RinGpuObjectSlot* memory_slot,
                                         RinGpuHandle memory,
                                         uint64_t offset_bytes,
                                         uint64_t size_bytes)
{
    memory_slot->value.memory.reference_count++;
    if (resource_slot->type == RIN_GPU_OBJECT_BUFFER) {
        resource_slot->value.buffer.memory_handle = memory;
        resource_slot->value.buffer.memory_offset = offset_bytes;
        resource_slot->value.buffer.memory_size = size_bytes;
    } else {
        resource_slot->value.image.memory_handle = memory;
        resource_slot->value.image.memory_offset = offset_bytes;
        resource_slot->value.image.memory_size = size_bytes;
    }
}

static int ringpu_resource_memory_align_up(uint64_t value,
                                           uint64_t alignment,
                                           uint64_t* result_out)
{
    uint64_t mask;

    if (!result_out || alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u) {
        return 0;
    }
    mask = alignment - 1u;
    if (value > UINT64_MAX - mask) return 0;
    *result_out = (value + mask) & ~mask;
    return 1;
}

static uint32_t ringpu_resource_memory_type_bits(const RinGpuCore* core)
{
    uint32_t bits = 0u;

    if (core->adapter.dedicated_memory_bytes != 0u)
        bits |= RIN_GPU_RESOURCE_MEMORY_TYPE_LOCAL;
    if (core->adapter.shared_memory_bytes != 0u)
        bits |= RIN_GPU_RESOURCE_MEMORY_TYPE_SYSTEM;
    /* A host-only core may not have physical telemetry.  The mask remains a
     * portable allocator namespace in that case; physical backends must
     * replace it with their admitted heap mask before binding. */
    return bits != 0u ? bits : RIN_GPU_RESOURCE_MEMORY_TYPE_LOCAL |
                                  RIN_GPU_RESOURCE_MEMORY_TYPE_SYSTEM;
}

static int ringpu_resource_memory_requirements_fill(
    const RinGpuCore* core, uint32_t resource_type, uint64_t size_bytes,
    uint64_t alignment, RinGpuResourceMemoryRequirementsV1* requirements)
{
    uint64_t rounded_size;

    if (!core || !requirements || size_bytes == 0u ||
        !ringpu_resource_memory_align_up(size_bytes, alignment,
                                         &rounded_size) ||
        rounded_size > core->max_total_allocation_size) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(requirements, 0, sizeof(*requirements));
    requirements->abi_version = RIN_GPU_ABI_VERSION;
    requirements->struct_size = sizeof(*requirements);
    requirements->resource_type = resource_type;
    requirements->size_bytes = rounded_size;
    requirements->alignment = alignment;
    requirements->memory_type_bits =
        ringpu_resource_memory_type_bits(core);
    return RIN_GPU_OK;
}

int ringpu_get_buffer_memory_requirements(
    const RinGpuCore* core, const RinGpuBufferDescV1* desc,
    RinGpuResourceMemoryRequirementsV1* requirements)
{
    int result;

    result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !requirements ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        desc->size_bytes == 0u ||
        desc->size_bytes > core->max_buffer_size || desc->usage == 0u ||
        (desc->usage & ~RIN_GPU_BUFFER_KNOWN_USAGE) != 0u ||
        (desc->flags & ~RIN_GPU_BUFFER_KNOWN_FLAGS) != 0u ||
        ((desc->flags & RIN_GPU_BUFFER_CPU_VISIBLE) != 0u &&
         (desc->usage & RIN_GPU_BUFFER_COPY_DESTINATION) == 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_resource_memory_requirements_fill(
        core, RIN_GPU_RESOURCE_MEMORY_BUFFER, desc->size_bytes, 256u,
        requirements);
}

int ringpu_get_image_memory_requirements(
    const RinGpuCore* core, const RinGpuImageDescV1* desc,
    RinGpuResourceMemoryRequirementsV1* requirements)
{
    uint64_t allocation_bytes = 0u;
    int result;

    result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!requirements) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_image_allocation_size(core, desc, &allocation_bytes);
    if (result != RIN_GPU_OK) return result;
    return ringpu_resource_memory_requirements_fill(
        core, RIN_GPU_RESOURCE_MEMORY_IMAGE, allocation_bytes,
        RIN_GPU_MEMORY_MIN_PAGE_SIZE, requirements);
}

int ringpu_create_memory(RinGpuCore* core, const RinGpuMemoryDescV1* desc,
                         RinGpuHandle* memory)
{
    RinGpuObjectSlot* slot;
    uint8_t* bytes = NULL;
    uint64_t alignment;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!memory) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *memory = 0u;
    if (!ringpu_software_memory_binding_supported(core))
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (!ringpu_memory_desc_valid(desc))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (desc->size_bytes > core->max_total_allocation_size -
            core->allocated_bytes) {
        return RIN_GPU_ERROR_LIMIT;
    }
    result = ringpu_software_backend_create_memory(
        core->backend_context, desc->size_bytes, &bytes);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_MEMORY, memory, &slot);
    if (result != RIN_GPU_OK) {
        ringpu_software_backend_destroy_memory(core->backend_context, bytes,
                                                desc->size_bytes);
        *memory = 0u;
        return result;
    }
    alignment = desc->alignment == 0u ? 1u : desc->alignment;
    slot->value.memory.bytes = bytes;
    slot->value.memory.size_bytes = desc->size_bytes;
    slot->value.memory.alignment = alignment;
    slot->value.memory.flags = desc->flags;
    core->allocated_bytes += desc->size_bytes;
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE,
                           (uint64_t)(uintptr_t)bytes, 0u,
                           RIN_GPU_OBJECT_MEMORY, desc->size_bytes,
                           RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_bind_buffer_memory(
    RinGpuCore* core, RinGpuHandle buffer,
    const RinGpuResourceMemoryBindingV1* binding)
{
    RinGpuObjectSlot* resource_slot;
    RinGpuObjectSlot* memory_slot;
    RinGpuResourceMemoryRequirementsV1 requirements;
    uint64_t new_cookie = 0u;
    uint64_t old_cookie;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!ringpu_software_memory_binding_supported(core))
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = ringpu_slot(core, buffer, RIN_GPU_OBJECT_BUFFER, NULL,
                         &resource_slot);
    if (result != RIN_GPU_OK) return result;
    if (resource_slot->value.buffer.reference_count != 0u ||
        resource_slot->value.buffer.memory_handle != 0u) {
        return RIN_GPU_ERROR_BUSY;
    }
    result = ringpu_get_buffer_memory_requirements(
        core, &(RinGpuBufferDescV1){
            RIN_GPU_ABI_VERSION, sizeof(RinGpuBufferDescV1),
            resource_slot->value.buffer.size_bytes,
            resource_slot->value.buffer.usage,
            resource_slot->value.buffer.flags}, &requirements);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_memory_binding_common(core, binding, &requirements,
                                          &memory_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_software_backend_bind_buffer(
        core->backend_context,
        &(RinGpuBufferDescV1){
            RIN_GPU_ABI_VERSION, sizeof(RinGpuBufferDescV1),
            resource_slot->value.buffer.size_bytes,
            resource_slot->value.buffer.usage,
            resource_slot->value.buffer.flags},
        memory_slot->value.memory.bytes, memory_slot->value.memory.size_bytes,
        binding->offset_bytes, &new_cookie);
    if (result != RIN_GPU_OK) return result;
    if (core->allocated_bytes < resource_slot->value.buffer.size_bytes) {
        core->backend.destroy_buffer(core->backend_context, new_cookie);
        return RIN_GPU_ERROR_STATE;
    }
    old_cookie = resource_slot->value.buffer.backend_cookie;
    core->backend.destroy_buffer(core->backend_context, old_cookie);
    resource_slot->value.buffer.backend_cookie = new_cookie;
    core->allocated_bytes -= resource_slot->value.buffer.size_bytes;
    ringpu_memory_binding_commit(resource_slot, memory_slot, binding->memory,
                                 binding->offset_bytes, binding->size_bytes);
    return RIN_GPU_OK;
}

int ringpu_bind_image_memory(
    RinGpuCore* core, RinGpuHandle image,
    const RinGpuResourceMemoryBindingV1* binding)
{
    RinGpuObjectSlot* resource_slot;
    RinGpuObjectSlot* memory_slot;
    RinGpuResourceMemoryRequirementsV1 requirements;
    uint64_t new_cookie = 0u;
    uint64_t old_cookie;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!ringpu_software_memory_binding_supported(core))
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = ringpu_slot(core, image, RIN_GPU_OBJECT_IMAGE, NULL,
                         &resource_slot);
    if (result != RIN_GPU_OK) return result;
    if (resource_slot->value.image.reference_count != 0u ||
        resource_slot->value.image.memory_handle != 0u) {
        return RIN_GPU_ERROR_BUSY;
    }
    result = ringpu_get_image_memory_requirements(
        core, &resource_slot->value.image.descriptor, &requirements);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_memory_binding_common(core, binding, &requirements,
                                          &memory_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_software_backend_bind_image(
        core->backend_context, &resource_slot->value.image.descriptor,
        resource_slot->value.image.allocation_bytes,
        memory_slot->value.memory.bytes, memory_slot->value.memory.size_bytes,
        binding->offset_bytes, &new_cookie);
    if (result != RIN_GPU_OK) return result;
    if (core->allocated_bytes < resource_slot->value.image.allocation_bytes) {
        core->backend.destroy_image(core->backend_context, new_cookie);
        return RIN_GPU_ERROR_STATE;
    }
    old_cookie = resource_slot->value.image.backend_cookie;
    core->backend.destroy_image(core->backend_context, old_cookie);
    resource_slot->value.image.backend_cookie = new_cookie;
    core->allocated_bytes -= resource_slot->value.image.allocation_bytes;
    ringpu_memory_binding_commit(resource_slot, memory_slot, binding->memory,
                                 binding->offset_bytes, binding->size_bytes);
    return RIN_GPU_OK;
}

int ringpu_create_buffer(RinGpuCore* core, const RinGpuBufferDescV1* desc,
                         RinGpuHandle* buffer)
{
    RinGpuObjectSlot* slot;
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !buffer ||
        !ringpu_versioned(desc->abi_version, desc->struct_size, sizeof(*desc)) ||
        desc->size_bytes == 0u || desc->size_bytes > core->max_buffer_size ||
        desc->usage == 0u || (desc->usage & ~RIN_GPU_BUFFER_KNOWN_USAGE) != 0u ||
        (desc->flags & ~RIN_GPU_BUFFER_KNOWN_FLAGS) != 0u ||
        ((desc->flags & RIN_GPU_BUFFER_CPU_VISIBLE) != 0u &&
         (desc->usage & RIN_GPU_BUFFER_COPY_DESTINATION) == 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if ((desc->flags & RIN_GPU_BUFFER_CPU_VISIBLE) != 0u &&
        !core->backend.upload_buffer) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (desc->size_bytes > core->max_total_allocation_size -
            core->allocated_bytes) {
        return RIN_GPU_ERROR_LIMIT;
    }
    result = core->backend.create_buffer(core->backend_context, desc, &cookie);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_BUFFER, buffer, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_buffer(core->backend_context, cookie);
        return result;
    }
    slot->value.buffer.size_bytes = desc->size_bytes;
    slot->value.buffer.usage = desc->usage;
    slot->value.buffer.flags = desc->flags;
    slot->value.buffer.backend_cookie = cookie;
    slot->value.buffer.cpu_upload_pending =
        (desc->flags & RIN_GPU_BUFFER_CPU_VISIBLE) != 0u;
    core->allocated_bytes += desc->size_bytes;
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_BUFFER, desc->size_bytes,
                           RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_upload_buffer(RinGpuCore* core, RinGpuHandle buffer,
                         uint64_t destination_offset, const void* source,
                         uint64_t size_bytes)
{
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!source || size_bytes == 0u) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_slot(core, buffer, RIN_GPU_OBJECT_BUFFER, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if ((slot->value.buffer.flags & RIN_GPU_BUFFER_CPU_VISIBLE) == 0u ||
        !core->backend.upload_buffer) {
        return RIN_GPU_ERROR_STATE;
    }
    if (slot->value.buffer.reference_count != 0u) return RIN_GPU_ERROR_BUSY;
    if (!ringpu_range(destination_offset, size_bytes,
                      slot->value.buffer.size_bytes)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = core->backend.upload_buffer(
        core->backend_context, slot->value.buffer.backend_cookie,
        destination_offset, source, size_bytes);
    if (result != RIN_GPU_OK) {
        slot->value.buffer.cpu_upload_pending = 1u;
        return result;
    }
    if (destination_offset == 0u &&
        size_bytes == slot->value.buffer.size_bytes) {
        slot->value.buffer.cpu_upload_pending = 0u;
    }
    return RIN_GPU_OK;
}

int ringpu_readback_buffer(RinGpuCore* core, RinGpuHandle buffer,
                           uint64_t source_offset, void* destination,
                           uint64_t size_bytes)
{
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!destination || size_bytes == 0u) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_slot(core, buffer, RIN_GPU_OBJECT_BUFFER, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if ((slot->value.buffer.flags & RIN_GPU_BUFFER_CPU_VISIBLE) == 0u ||
        !core->backend.readback_buffer) {
        return RIN_GPU_ERROR_STATE;
    }
    if (slot->value.buffer.cpu_upload_pending != 0u)
        return RIN_GPU_ERROR_BUSY;
    if (!ringpu_range(source_offset, size_bytes,
                      slot->value.buffer.size_bytes))
        return RIN_GPU_ERROR_BOUNDS;
    return core->backend.readback_buffer(
        core->backend_context, slot->value.buffer.backend_cookie,
        source_offset, destination, size_bytes);
}

int ringpu_create_image(RinGpuCore* core, const RinGpuImageDescV1* desc,
                        RinGpuHandle* image)
{
    RinGpuObjectSlot* slot;
    uint32_t* subresource_states = NULL;
    uint8_t* cpu_upload_complete = NULL;
    uint32_t subresource_count;
    uint64_t allocation_bytes = 0u;
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!image) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *image = 0u;
    result = ringpu_image_allocation_size(core, desc, &allocation_bytes);
    if (result != RIN_GPU_OK) return result;
    if ((desc->flags & RIN_GPU_IMAGE_CPU_VISIBLE) != 0u &&
        !core->backend.upload_image) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if ((desc->flags & RIN_GPU_IMAGE_CPU_READABLE) != 0u &&
        !core->backend.readback_image) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (allocation_bytes > core->max_total_allocation_size -
            core->allocated_bytes) {
        return RIN_GPU_ERROR_LIMIT;
    }
    subresource_count = desc->array_layers * desc->mip_levels;
    subresource_states = (uint32_t*)calloc(
        subresource_count, sizeof(*subresource_states));
    if (!subresource_states) return RIN_GPU_ERROR_NO_MEMORY;
    if ((desc->flags & RIN_GPU_IMAGE_CPU_VISIBLE) != 0u) {
        cpu_upload_complete = (uint8_t*)calloc(
            subresource_count, sizeof(*cpu_upload_complete));
        if (!cpu_upload_complete) {
            free(subresource_states);
            return RIN_GPU_ERROR_NO_MEMORY;
        }
    }
    result = core->backend.create_image(
        core->backend_context, desc, allocation_bytes, &cookie);
    if (result != RIN_GPU_OK) {
        free(subresource_states);
        free(cpu_upload_complete);
        return result;
    }
    result = ringpu_allocate(core, RIN_GPU_OBJECT_IMAGE, image, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_image(core->backend_context, cookie);
        free(subresource_states);
        free(cpu_upload_complete);
        *image = 0u;
        return result;
    }
    slot->value.image.descriptor = *desc;
    slot->value.image.descriptor.struct_size =
        sizeof(slot->value.image.descriptor);
    slot->value.image.allocation_bytes = allocation_bytes;
    slot->value.image.backend_cookie = cookie;
    slot->value.image.subresource_states = subresource_states;
    slot->value.image.subresource_count = subresource_count;
    slot->value.image.cpu_upload_complete = cpu_upload_complete;
    slot->value.image.cpu_upload_pending = cpu_upload_complete != NULL;
    core->allocated_bytes += allocation_bytes;
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_IMAGE, allocation_bytes,
                           RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_create_sampler(RinGpuCore* core, const RinGpuSamplerDescV1* desc,
                          RinGpuHandle* sampler)
{
    RinGpuObjectSlot* slot;
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!sampler) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *sampler = 0u;
    if (!ringpu_sampler_desc_valid(desc)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = core->backend.create_sampler(core->backend_context, desc,
                                          &cookie);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_SAMPLER, sampler, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_sampler(core->backend_context, cookie);
        *sampler = 0u;
        return result;
    }
    slot->value.sampler.descriptor = *desc;
    slot->value.sampler.descriptor.struct_size =
        sizeof(slot->value.sampler.descriptor);
    slot->value.sampler.backend_cookie = cookie;
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_SAMPLER, 0u, RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_get_sampler_info(const RinGpuCore* core, RinGpuHandle sampler,
                            RinGpuSamplerDescV1* info)
{
    const RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!info) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    result = ringpu_slot_const(core, sampler, RIN_GPU_OBJECT_SAMPLER, NULL,
                               &slot);
    if (result != RIN_GPU_OK) return result;
    *info = slot->value.sampler.descriptor;
    return RIN_GPU_OK;
}

int ringpu_get_image_info(const RinGpuCore* core, RinGpuHandle image,
                          RinGpuImageInfoV1* info)
{
    const RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!info) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    result = ringpu_slot_const(core, image, RIN_GPU_OBJECT_IMAGE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    info->abi_version = RIN_GPU_ABI_VERSION;
    info->struct_size = sizeof(*info);
    info->descriptor = slot->value.image.descriptor;
    info->allocation_bytes = slot->value.image.allocation_bytes;
    return RIN_GPU_OK;
}

int ringpu_get_image_state(const RinGpuCore* core, RinGpuHandle image,
                           uint32_t mip_level, uint32_t array_layer,
                           uint32_t* state)
{
    const RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!state) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_slot_const(core, image, RIN_GPU_OBJECT_IMAGE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if (mip_level >= slot->value.image.descriptor.mip_levels ||
        array_layer >= slot->value.image.descriptor.array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    *state = slot->value.image.subresource_states[
        array_layer * slot->value.image.descriptor.mip_levels + mip_level];
    return RIN_GPU_OK;
}

int ringpu_upload_image(RinGpuCore* core, RinGpuHandle image,
                        const RinGpuImageUploadV1* upload,
                        const void* source, uint64_t source_size)
{
    RinGpuObjectSlot* slot;
    RinGpuImageUploadV1 canonical_upload;
    const RinGpuImageDescV1* desc;
    uint32_t mip_width;
    uint32_t mip_height;
    uint32_t mip_depth;
    int covers_complete_subresource;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!upload || !source || source_size == 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, image, RIN_GPU_OBJECT_IMAGE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if ((slot->value.image.descriptor.flags & RIN_GPU_IMAGE_CPU_VISIBLE) ==
            0u ||
        !core->backend.upload_image) {
        return RIN_GPU_ERROR_STATE;
    }
    if (slot->value.image.reference_count != 0u) return RIN_GPU_ERROR_BUSY;
    canonical_upload = *upload;
    canonical_upload.struct_size = sizeof(canonical_upload);
    desc = &slot->value.image.descriptor;
    result = ringpu_image_upload_source_valid(desc, &canonical_upload,
                                              source_size);
    if (result != RIN_GPU_OK) return result;
    result = core->backend.upload_image(
        core->backend_context, slot->value.image.backend_cookie,
        &canonical_upload, source, source_size);
    if (result != RIN_GPU_OK) {
        if (slot->value.image.cpu_upload_complete != NULL) {
            memset(slot->value.image.cpu_upload_complete, 0,
                   slot->value.image.subresource_count);
        }
        slot->value.image.cpu_upload_pending = 1u;
        return result;
    }
    mip_width = ringpu_image_mip_extent(desc->width,
                                        canonical_upload.mip_level);
    mip_height = ringpu_image_mip_extent(desc->height,
                                         canonical_upload.mip_level);
    mip_depth = ringpu_image_mip_extent(desc->depth,
                                        canonical_upload.mip_level);
    covers_complete_subresource =
        canonical_upload.x == 0u && canonical_upload.y == 0u &&
        canonical_upload.z == 0u && canonical_upload.width == mip_width &&
        canonical_upload.height == mip_height &&
        canonical_upload.depth == mip_depth;
    if (covers_complete_subresource) {
        uint32_t subresource_index = canonical_upload.array_layer *
            desc->mip_levels + canonical_upload.mip_level;
        slot->value.image.subresource_states[subresource_index] =
            RIN_GPU_IMAGE_STATE_COPY_DESTINATION;
        if (slot->value.image.cpu_upload_complete != NULL) {
            uint32_t index;
            slot->value.image.cpu_upload_complete[subresource_index] = 1u;
            for (index = 0u; index < slot->value.image.subresource_count;
                 ++index) {
                if (slot->value.image.cpu_upload_complete[index] == 0u)
                    return RIN_GPU_OK;
            }
            slot->value.image.cpu_upload_pending = 0u;
        } else if (canonical_upload.mip_level == 0u &&
                   canonical_upload.array_layer == 0u) {
            slot->value.image.cpu_upload_pending = 0u;
        }
    }
    return RIN_GPU_OK;
}

int ringpu_readback_image(RinGpuCore* core, RinGpuHandle image,
                          const RinGpuImageReadbackV1* readback,
                          void* destination, uint64_t destination_size)
{
    RinGpuObjectSlot* slot;
    RinGpuImageReadbackV1 canonical_readback;
    uint32_t state;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!readback || !destination || destination_size == 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, image, RIN_GPU_OBJECT_IMAGE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if ((slot->value.image.descriptor.flags & RIN_GPU_IMAGE_CPU_READABLE) ==
            0u ||
        !core->backend.readback_image || !ringpu_image_upload_ready(slot)) {
        return RIN_GPU_ERROR_STATE;
    }
    canonical_readback = *readback;
    canonical_readback.struct_size = sizeof(canonical_readback);
    result = ringpu_image_readback_destination_valid(
        &slot->value.image.descriptor, &canonical_readback, destination_size);
    if (result != RIN_GPU_OK) return result;
    state = slot->value.image.subresource_states[
        canonical_readback.array_layer *
            slot->value.image.descriptor.mip_levels +
        canonical_readback.mip_level];
    if (state != RIN_GPU_IMAGE_STATE_COPY_SOURCE) return RIN_GPU_ERROR_STATE;
    return core->backend.readback_image(
        core->backend_context, slot->value.image.backend_cookie,
        &canonical_readback, destination, destination_size);
}
