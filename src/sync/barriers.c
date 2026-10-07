// SPDX-License-Identifier: MIT
#include "barriers.h"

#include "../command/record.h"
#include "../core/object_table.h"
#include "../validation/resource.h"

int ringpu_command_transition_image(
    RinGpuCore* core, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageTransitionV1* transition)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* image_slot;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* desc;
    uint32_t format_aspects;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!transition ||
        !ringpu_versioned(transition->abi_version, transition->struct_size,
                          sizeof(*transition)) ||
        transition->mip_level_count == 0u ||
        transition->array_layer_count == 0u ||
        transition->before_state == transition->after_state ||
        transition->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities &
         (RIN_GPU_QUEUE_COPY | RIN_GPU_QUEUE_GRAPHICS)) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, image, RIN_GPU_OBJECT_IMAGE, NULL, &image_slot);
    if (result != RIN_GPU_OK) return result;
    desc = &image_slot->value.image.descriptor;
    format_aspects = ringpu_image_format_aspects(desc->format);
    if (format_aspects == 0u ||
        (transition->aspect_mask != 0u &&
         transition->aspect_mask != format_aspects)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (transition->base_mip_level >= desc->mip_levels ||
        transition->mip_level_count >
            desc->mip_levels - transition->base_mip_level ||
        transition->base_array_layer >= desc->array_layers ||
        transition->array_layer_count >
            desc->array_layers - transition->base_array_layer) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (!ringpu_image_state_allowed(desc, transition->before_state) ||
        !ringpu_image_state_allowed(desc, transition->after_state)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE;
    command->destination = image;
    command->value.image_transition = *transition;
    command->value.image_transition.struct_size =
        sizeof(command->value.image_transition);
    command->value.image_transition.aspect_mask = format_aspects;
    list->value.command_list.count++;
    image_slot->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_transfer_image_ownership(
    RinGpuCore* core, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageOwnershipTransferV1* transfer)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* image_slot;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* desc;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!transfer ||
        !ringpu_versioned(transfer->abi_version, transfer->struct_size,
                          sizeof(*transfer)) ||
        transfer->flags != 0u || transfer->reserved != 0u ||
        (transfer->source_family_index == transfer->destination_family_index &&
         transfer->source_engine_index == transfer->destination_engine_index) ||
        transfer->before_state == transfer->after_state) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities &
         (RIN_GPU_QUEUE_COPY | RIN_GPU_QUEUE_COMPUTE |
          RIN_GPU_QUEUE_GRAPHICS)) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, image, RIN_GPU_OBJECT_IMAGE, NULL, &image_slot);
    if (result != RIN_GPU_OK) return result;
    desc = &image_slot->value.image.descriptor;
    /* The first profile is deliberately image-wide.  It cannot claim
     * subresource ownership while the core stores one owner per image. */
    if (transfer->base_mip_level != 0u ||
        transfer->mip_level_count != desc->mip_levels ||
        transfer->base_array_layer != 0u ||
        transfer->array_layer_count != desc->array_layers ||
        !ringpu_image_state_allowed(desc, transfer->before_state) ||
        !ringpu_image_state_allowed(desc, transfer->after_state)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP;
    command->destination = image;
    command->value.image_ownership_transfer = *transfer;
    command->value.image_ownership_transfer.struct_size =
        sizeof(command->value.image_ownership_transfer);
    list->value.command_list.count++;
    image_slot->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_compute_barrier(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuComputeBarrierV1* barrier)
{
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!barrier ||
        !ringpu_versioned(barrier->abi_version, barrier->struct_size,
                          sizeof(*barrier)) ||
        barrier->source_access != RIN_GPU_RESOURCE_KNOWN_ACCESS ||
        barrier->destination_access != RIN_GPU_RESOURCE_KNOWN_ACCESS ||
        barrier->flags != 0u || barrier->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COMPUTE) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER;
    command->value.compute_barrier = *barrier;
    command->value.compute_barrier.struct_size =
        sizeof(command->value.compute_barrier);
    list->value.command_list.count++;
    return RIN_GPU_OK;
}

int ringpu_command_compute_barrier_v2(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuComputeBarrierV2* barrier)
{
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!barrier ||
        !ringpu_versioned(barrier->abi_version, barrier->struct_size,
                          sizeof(*barrier)) ||
        !ringpu_sync2_barrier_scopes_valid(
            barrier->source_stage, barrier->destination_stage,
            barrier->source_access, barrier->destination_access,
            barrier->flags, barrier->reserved)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COMPUTE) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2;
    command->value.compute_barrier_v2 = *barrier;
    command->value.compute_barrier_v2.struct_size =
        sizeof(command->value.compute_barrier_v2);
    list->value.command_list.count++;
    return RIN_GPU_OK;
}

int ringpu_command_graphics_barrier(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuGraphicsBarrierV1* barrier)
{
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!barrier ||
        !ringpu_versioned(barrier->abi_version, barrier->struct_size,
                          sizeof(*barrier)) ||
        barrier->source_access != RIN_GPU_RESOURCE_KNOWN_ACCESS ||
        barrier->destination_access != RIN_GPU_RESOURCE_KNOWN_ACCESS ||
        barrier->flags != 0u || barrier->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER;
    command->value.graphics_barrier = *barrier;
    command->value.graphics_barrier.struct_size =
        sizeof(command->value.graphics_barrier);
    list->value.command_list.count++;
    return RIN_GPU_OK;
}

int ringpu_command_graphics_barrier_v2(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuGraphicsBarrierV2* barrier)
{
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!barrier ||
        !ringpu_versioned(barrier->abi_version, barrier->struct_size,
                          sizeof(*barrier)) ||
        !ringpu_sync2_barrier_scopes_valid(
            barrier->source_stage, barrier->destination_stage,
            barrier->source_access, barrier->destination_access,
            barrier->flags, barrier->reserved)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2;
    command->value.graphics_barrier_v2 = *barrier;
    command->value.graphics_barrier_v2.struct_size =
        sizeof(command->value.graphics_barrier_v2);
    list->value.command_list.count++;
    return RIN_GPU_OK;
}
