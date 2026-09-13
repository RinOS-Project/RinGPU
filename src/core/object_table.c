// SPDX-License-Identifier: MIT
#include "object_table.h"

#include <string.h>

static RinGpuHandle ringpu_encode(const RinGpuCore* core, uint32_t index,
                                  uint16_t type, uint32_t generation)
{
    uint64_t raw = ((uint64_t)generation << 32) |
                   ((uint64_t)type << 16) | (uint64_t)(index + 1u);
    return raw ^ core->handle_secret;
}

int ringpu_slot_const(const RinGpuCore* core, RinGpuHandle handle,
                      uint16_t expected_type, uint32_t* index_out,
                      const RinGpuObjectSlot** slot_out)
{
    uint64_t raw;
    uint32_t encoded_index;
    uint16_t encoded_type;
    uint32_t generation;
    const RinGpuObjectSlot* slot;

    if (!core || !handle) return RIN_GPU_ERROR_INVALID_HANDLE;
    raw = handle ^ core->handle_secret;
    encoded_index = (uint32_t)(raw & 0xffffu);
    encoded_type = (uint16_t)((raw >> 16) & 0xffu);
    generation = (uint32_t)(raw >> 32);
    if (encoded_index == 0u || encoded_index > RIN_GPU_CORE_MAX_OBJECTS ||
        ((raw >> 24) & 0xffu) != 0u) {
        return RIN_GPU_ERROR_INVALID_HANDLE;
    }
    slot = &core->objects[encoded_index - 1u];
    if (!slot->occupied || slot->generation != generation ||
        slot->type != encoded_type) {
        return RIN_GPU_ERROR_INVALID_HANDLE;
    }
    if (expected_type != RIN_GPU_OBJECT_NONE && encoded_type != expected_type)
        return RIN_GPU_ERROR_WRONG_TYPE;
    if (index_out) *index_out = encoded_index - 1u;
    if (slot_out) *slot_out = slot;
    return RIN_GPU_OK;
}

int ringpu_slot(RinGpuCore* core, RinGpuHandle handle, uint16_t expected_type,
                uint32_t* index_out, RinGpuObjectSlot** slot_out)
{
    uint32_t index;
    int result = ringpu_slot_const(core, handle, expected_type, &index, NULL);
    if (result != RIN_GPU_OK) return result;
    if (index_out) *index_out = index;
    if (slot_out) *slot_out = &core->objects[index];
    return RIN_GPU_OK;
}

int ringpu_allocate(RinGpuCore* core, uint16_t type, RinGpuHandle* handle,
                    RinGpuObjectSlot** slot_out)
{
    if (!core || !handle) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        if (slot->occupied) continue;
        if (slot->generation == 0u) slot->generation = 1u;
        slot->type = type;
        slot->occupied = 1u;
        memset(&slot->value, 0, sizeof(slot->value));
        *handle = ringpu_encode(core, index, type, slot->generation);
        if (*handle == 0u) {
            slot->generation++;
            if (slot->generation == 0u) slot->generation = 1u;
            *handle = ringpu_encode(core, index, type, slot->generation);
        }
        if (slot_out) *slot_out = slot;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_LIMIT;
}

void ringpu_release_slot(RinGpuObjectSlot* slot)
{
    if (!slot) return;
    slot->occupied = 0u;
    slot->type = RIN_GPU_OBJECT_NONE;
    slot->generation++;
    if (slot->generation == 0u) slot->generation = 1u;
    memset(&slot->value, 0, sizeof(slot->value));
}
