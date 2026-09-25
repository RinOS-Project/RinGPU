// SPDX-License-Identifier: MIT
#include "commands.h"

#include "record.h"
#include "../core/object_table.h"
#include "../presentation/render_pass.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"

int ringpu_command_copy_buffer(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle destination,
                               uint64_t destination_offset,
                               RinGpuHandle source, uint64_t source_offset,
                               uint64_t size_bytes)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* destination_slot;
    RinGpuObjectSlot* source_slot;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COPY) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, destination, RIN_GPU_OBJECT_BUFFER, NULL,
                         &destination_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, source, RIN_GPU_OBJECT_BUFFER, NULL, &source_slot);
    if (result != RIN_GPU_OK) return result;
    if ((destination_slot->value.buffer.usage &
         RIN_GPU_BUFFER_COPY_DESTINATION) == 0u ||
        (source_slot->value.buffer.usage & RIN_GPU_BUFFER_COPY_SOURCE) == 0u ||
        !ringpu_buffer_upload_ready(destination_slot) ||
        !ringpu_buffer_upload_ready(source_slot)) {
        return RIN_GPU_ERROR_STATE;
    }
    if (!ringpu_range(destination_offset, size_bytes,
                      destination_slot->value.buffer.size_bytes) ||
        !ringpu_range(source_offset, size_bytes,
                      source_slot->value.buffer.size_bytes)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (destination == source &&
        destination_offset < source_offset + size_bytes &&
        source_offset < destination_offset + size_bytes) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_COPY_BUFFER;
    command->destination = destination;
    command->source = source;
    command->value.buffer_copy.destination_offset = destination_offset;
    command->value.buffer_copy.source_offset = source_offset;
    command->value.buffer_copy.size_bytes = size_bytes;
    list->value.command_list.count++;
    destination_slot->value.buffer.reference_count++;
    source_slot->value.buffer.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_clear_buffer(RinGpuCore* core, RinGpuHandle command_list,
                                RinGpuHandle destination,
                                const RinGpuBufferClearV1* clear)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* destination_slot;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!clear || !ringpu_versioned(clear->abi_version, clear->struct_size,
                                    sizeof(*clear)) || clear->flags != 0u ||
        clear->reserved != 0u || clear->size_bytes == 0u ||
        (clear->offset & UINT64_C(3)) != 0u ||
        (clear->size_bytes & UINT64_C(3)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COPY) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, destination, RIN_GPU_OBJECT_BUFFER, NULL,
                         &destination_slot);
    if (result != RIN_GPU_OK) return result;
    if ((destination_slot->value.buffer.usage &
         RIN_GPU_BUFFER_COPY_DESTINATION) == 0u ||
        !ringpu_buffer_upload_ready(destination_slot) ||
        !ringpu_range(clear->offset, clear->size_bytes,
                      destination_slot->value.buffer.size_bytes)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER;
    command->destination = destination;
    command->value.buffer_clear = *clear;
    command->value.buffer_clear.struct_size = sizeof(command->value.buffer_clear);
    list->value.command_list.count++;
    destination_slot->value.buffer.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_copy_image(RinGpuCore* core, RinGpuHandle command_list,
                              RinGpuHandle destination, RinGpuHandle source,
                              const RinGpuImageCopyRegionV1* region)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* destination_slot;
    RinGpuObjectSlot* source_slot;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* destination_desc;
    const RinGpuImageDescV1* source_desc;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!region ||
        !ringpu_versioned(region->abi_version, region->struct_size,
                          sizeof(*region)) || region->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COPY) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, destination, RIN_GPU_OBJECT_IMAGE, NULL,
                         &destination_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, source, RIN_GPU_OBJECT_IMAGE, NULL,
                         &source_slot);
    if (result != RIN_GPU_OK) return result;
    destination_desc = &destination_slot->value.image.descriptor;
    source_desc = &source_slot->value.image.descriptor;
    if ((destination_desc->usage & RIN_GPU_IMAGE_COPY_DESTINATION) == 0u ||
        (source_desc->usage & RIN_GPU_IMAGE_COPY_SOURCE) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (destination_desc->format != source_desc->format ||
        destination_desc->dimension != source_desc->dimension ||
        destination_desc->sample_count != source_desc->sample_count ||
        destination_desc->sample_count != 1u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!ringpu_image_copy_side_valid(
            source_desc, region->source_mip_level,
            region->source_array_layer, region->source_x,
            region->source_y, region->source_z, region->width,
            region->height, region->depth) ||
        !ringpu_image_copy_side_valid(
            destination_desc, region->destination_mip_level,
            region->destination_array_layer, region->destination_x,
            region->destination_y, region->destination_z, region->width,
            region->height, region->depth)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (destination == source &&
        region->destination_mip_level == region->source_mip_level &&
        region->destination_array_layer == region->source_array_layer &&
        region->destination_x < region->source_x + region->width &&
        region->source_x < region->destination_x + region->width &&
        region->destination_y < region->source_y + region->height &&
        region->source_y < region->destination_y + region->height &&
        region->destination_z < region->source_z + region->depth &&
        region->source_z < region->destination_z + region->depth) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_COPY_IMAGE;
    command->destination = destination;
    command->source = source;
    command->value.image_copy = *region;
    command->value.image_copy.struct_size = sizeof(command->value.image_copy);
    list->value.command_list.count++;
    destination_slot->value.image.reference_count++;
    source_slot->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_blit_image(RinGpuCore* core, RinGpuHandle command_list,
                              RinGpuHandle destination, RinGpuHandle source,
                              const RinGpuImageBlitV1* blit)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* destination_slot;
    RinGpuObjectSlot* source_slot;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* destination_desc;
    const RinGpuImageDescV1* source_desc;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!blit || !ringpu_versioned(blit->abi_version, blit->struct_size,
                                   sizeof(*blit)) || blit->flags != 0u ||
        blit->reserved != 0u || blit->source_width == 0u ||
        blit->source_height == 0u || blit->destination_width == 0u ||
        blit->destination_height == 0u || blit->filter >
            RIN_GPU_IMAGE_BLIT_LINEAR) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COPY) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, destination, RIN_GPU_OBJECT_IMAGE, NULL,
                         &destination_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, source, RIN_GPU_OBJECT_IMAGE, NULL, &source_slot);
    if (result != RIN_GPU_OK) return result;
    destination_desc = &destination_slot->value.image.descriptor;
    source_desc = &source_slot->value.image.descriptor;
    if ((destination_desc->usage & RIN_GPU_IMAGE_COPY_DESTINATION) == 0u ||
        (source_desc->usage & RIN_GPU_IMAGE_COPY_SOURCE) == 0u ||
        !ringpu_image_upload_ready(destination_slot) ||
        !ringpu_image_upload_ready(source_slot) ||
        destination_desc->format != source_desc->format ||
        !ringpu_color_format(destination_desc->format) ||
        destination_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        source_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        destination_desc->sample_count != 1u ||
        source_desc->sample_count != 1u ||
        !ringpu_image_copy_side_valid(
            source_desc, blit->source_mip_level, blit->source_array_layer,
            blit->source_x, blit->source_y, 0u, blit->source_width,
            blit->source_height, 1u) ||
        !ringpu_image_copy_side_valid(
            destination_desc, blit->destination_mip_level,
            blit->destination_array_layer, blit->destination_x,
            blit->destination_y, 0u, blit->destination_width,
            blit->destination_height, 1u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (destination == source &&
        blit->destination_mip_level == blit->source_mip_level &&
        blit->destination_array_layer == blit->source_array_layer &&
        blit->destination_x < blit->source_x + blit->source_width &&
        blit->source_x < blit->destination_x + blit->destination_width &&
        blit->destination_y < blit->source_y + blit->source_height &&
        blit->source_y < blit->destination_y + blit->destination_height) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE;
    command->destination = destination;
    command->source = source;
    command->value.image_blit = *blit;
    command->value.image_blit.struct_size = sizeof(command->value.image_blit);
    list->value.command_list.count++;
    destination_slot->value.image.reference_count++;
    source_slot->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_clear_image(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle destination,
                               const RinGpuImageClearV1* clear)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* destination_slot;
    const RinGpuImageDescV1* desc;
    RinGpuRecordedCommand* command;
    int color;
    int depth;
    int stencil;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!clear || !ringpu_versioned(clear->abi_version, clear->struct_size,
                                    sizeof(*clear)) || clear->flags != 0u ||
        clear->reserved != 0u || clear->aspects == 0u ||
        (clear->aspects & ~RIN_GPU_IMAGE_CLEAR_KNOWN_ASPECTS) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    color = (clear->aspects & RIN_GPU_IMAGE_CLEAR_COLOR) != 0u;
    depth = (clear->aspects & RIN_GPU_IMAGE_CLEAR_DEPTH) != 0u;
    stencil = (clear->aspects & RIN_GPU_IMAGE_CLEAR_STENCIL) != 0u;
    if (!color && (clear->color_red != 0.0f || clear->color_green != 0.0f ||
                   clear->color_blue != 0.0f || clear->color_alpha != 0.0f)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!depth && clear->depth != 0.0f) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!stencil && clear->stencil != 0u) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if ((color && (!ringpu_finite_float(clear->color_red) ||
                   !ringpu_finite_float(clear->color_green) ||
                   !ringpu_finite_float(clear->color_blue) ||
                   !ringpu_finite_float(clear->color_alpha))) ||
        (depth && (!ringpu_finite_float(clear->depth) ||
                   clear->depth < 0.0f || clear->depth > 1.0f)) ||
        (stencil && clear->stencil > UINT32_C(0xff))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COPY) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, destination, RIN_GPU_OBJECT_IMAGE, NULL,
                         &destination_slot);
    if (result != RIN_GPU_OK) return result;
    desc = &destination_slot->value.image.descriptor;
    if ((desc->usage & RIN_GPU_IMAGE_COPY_DESTINATION) == 0u ||
        !ringpu_image_upload_ready(destination_slot) ||
        clear->mip_level >= desc->mip_levels ||
        clear->array_layer >= desc->array_layers ||
        (color && (!ringpu_color_format(desc->format) ||
                   !ringpu_render_color_clear_valid_for_format(
                       desc->format, RIN_GPU_RENDER_CLEAR,
                       clear->color_red, clear->color_green,
                       clear->color_blue, clear->color_alpha,
                       RIN_GPU_COLOR_WRITE_ALL))) ||
        (depth && !ringpu_depth_aspect_format(desc->format)) ||
        (stencil && !ringpu_stencil_aspect_format(desc->format))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE;
    command->destination = destination;
    command->value.image_clear = *clear;
    command->value.image_clear.struct_size = sizeof(command->value.image_clear);
    list->value.command_list.count++;
    destination_slot->value.image.reference_count++;
    return RIN_GPU_OK;
}

static int ringpu_dispatch_has_hazard(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    const RinGpuObjectSlot* new_bind_group)
{
    if (!core || !list || !new_bind_group) return 1;
    for (uint32_t reverse = list->value.command_list.count;
         reverse != 0u; reverse--) {
        const RinGpuRecordedCommand* previous =
            &list->value.command_list.commands[reverse - 1u];
        RinGpuObjectSlot* previous_group;
        if (previous->type == RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER ||
            previous->type == RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2) {
            return 0;
        }
        if (previous->type != RIN_GPU_BACKEND_COMMAND_DISPATCH) continue;
        if (ringpu_slot(core, previous->source,
                        RIN_GPU_OBJECT_COMPUTE_BIND_GROUP, NULL,
                        &previous_group) != RIN_GPU_OK) {
            return 1;
        }
        for (uint32_t current = 0u;
             current < new_bind_group->value.compute_bind_group.binding_count;
             current++) {
            const RinGpuBufferBindingV1* current_binding =
                &new_bind_group->value.compute_bind_group.bindings[current];
            for (uint32_t old = 0u;
                 old < previous_group->value.compute_bind_group.binding_count;
                 old++) {
                const RinGpuBufferBindingV1* old_binding =
                    &previous_group->value.compute_bind_group.bindings[old];
                if (current_binding->buffer == old_binding->buffer &&
                    current_binding->offset <
                        old_binding->offset + old_binding->size_bytes &&
                    old_binding->offset <
                        current_binding->offset +
                            current_binding->size_bytes &&
                    ((current_binding->access | old_binding->access) &
                        RIN_GPU_RESOURCE_WRITE) != 0u) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int ringpu_command_dispatch(RinGpuCore* core, RinGpuHandle command_list,
                            const RinGpuDispatchV1* dispatch)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!dispatch ||
        !ringpu_versioned(dispatch->abi_version, dispatch->struct_size,
                          sizeof(*dispatch)) ||
        dispatch->group_count_x == 0u || dispatch->group_count_y == 0u ||
        dispatch->group_count_z == 0u || dispatch->flags != 0u ||
        dispatch->reserved0 != 0u || dispatch->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (dispatch->group_count_x > RIN_GPU_MAX_DISPATCH_GROUPS ||
        dispatch->group_count_y > RIN_GPU_MAX_DISPATCH_GROUPS ||
        dispatch->group_count_z > RIN_GPU_MAX_DISPATCH_GROUPS) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_COMPUTE) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, dispatch->pipeline,
                         RIN_GPU_OBJECT_COMPUTE_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, dispatch->bind_group,
                         RIN_GPU_OBJECT_COMPUTE_BIND_GROUP, NULL, &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (bind_group->value.compute_bind_group.pipeline != dispatch->pipeline) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (ringpu_dispatch_has_hazard(core, list, bind_group)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DISPATCH;
    command->destination = dispatch->pipeline;
    command->source = dispatch->bind_group;
    command->value.dispatch = *dispatch;
    command->value.dispatch.struct_size = sizeof(command->value.dispatch);
    list->value.command_list.count++;
    pipeline->value.compute_pipeline.reference_count++;
    bind_group->value.compute_bind_group.reference_count++;
    return RIN_GPU_OK;
}
