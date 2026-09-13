// SPDX-License-Identifier: MIT
#include "resources.h"

#include "../core/object_table.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"

#include <stdlib.h>
#include <string.h>

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
