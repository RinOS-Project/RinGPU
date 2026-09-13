// SPDX-License-Identifier: MIT
#include "present.h"

#include "../command/record.h"
#include "../core/object_table.h"
#include "../validation/resource.h"

const RinGpuDisplayInfoV1* ringpu_find_display(
    const RinGpuCore* core, uint32_t display_id)
{
    if (!core) return NULL;
    for (uint32_t index = 0u; index < core->display_count; index++) {
        if (core->displays[index].display_id == display_id) {
            return &core->displays[index];
        }
    }
    return NULL;
}

int ringpu_command_present(RinGpuCore* core, RinGpuHandle command_list,
                           const RinGpuPresentV1* present)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* image;
    RinGpuRecordedCommand* command;
    const RinGpuDisplayInfoV1* display;
    const RinGpuImageDescV1* desc;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!present ||
        !ringpu_versioned(present->abi_version, present->struct_size,
                          sizeof(*present)) ||
        present->flags != 0u || present->reserved0 != 0u ||
        present->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    display = ringpu_find_display(core, present->display_id);
    if (!display) return RIN_GPU_ERROR_UNSUPPORTED;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, present->image, RIN_GPU_OBJECT_IMAGE, NULL,
                         &image);
    if (result != RIN_GPU_OK) return result;
    desc = &image->value.image.descriptor;
    if ((desc->usage & RIN_GPU_IMAGE_PRESENT) == 0u ||
        desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        desc->array_layers != 1u || desc->mip_levels != 1u ||
        desc->sample_count != 1u || !ringpu_scanout_format(desc->format) ||
        desc->width != display->width || desc->height != display->height ||
        desc->format != display->format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_PRESENT;
    command->destination = present->image;
    command->value.present = *present;
    command->value.present.struct_size = sizeof(command->value.present);
    list->value.command_list.count++;
    image->value.image.reference_count++;
    return RIN_GPU_OK;
}
