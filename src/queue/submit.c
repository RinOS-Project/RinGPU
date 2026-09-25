// SPDX-License-Identifier: MIT
#include "../core/core.h"

#include "../command/draw.h"
#include "../core/object_table.h"
#include "../pipeline/pipelines.h"
#include "../presentation/present.h"
#include "../presentation/render_pass.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"

#include <stdlib.h>
#include <string.h>

static int ringpu_backend_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline_handle,
    const RinGpuObjectSlot* pipeline, RinGpuHandle bind_group_handle,
    uint64_t* cookie) {
    RinGpuObjectSlot* bind_group;
    int result;
    if (!core || !pipeline || !cookie) return RIN_GPU_ERROR_STATE;
    *cookie = 0u;
    if (pipeline->value.graphics_pipeline.resource_count == 0u) {
        return bind_group_handle == 0u ? RIN_GPU_OK : RIN_GPU_ERROR_STATE;
    }
    result = ringpu_validate_graphics_bind_group(
        core, pipeline_handle, pipeline, bind_group_handle, &bind_group);
    if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
    *cookie = bind_group->value.graphics_bind_group.backend_cookie;
    return RIN_GPU_OK;
}

static int ringpu_stage_image_states(RinGpuObjectSlot* image,
                                     uint32_t object_index,
                                     uint32_t** staged_states) {
    uint32_t* states;
    if (!image || !staged_states ||
        object_index >= RIN_GPU_CORE_MAX_OBJECTS ||
        image->type != RIN_GPU_OBJECT_IMAGE ||
        !ringpu_image_upload_ready(image) ||
        image->value.image.subresource_count == 0u ||
        !image->value.image.subresource_states) {
        return RIN_GPU_ERROR_STATE;
    }
    states = staged_states[object_index];
    if (!states) {
        states = (uint32_t*)malloc(
            (size_t)image->value.image.subresource_count * sizeof(*states));
        if (!states) return RIN_GPU_ERROR_NO_MEMORY;
        memcpy(states, image->value.image.subresource_states,
               (size_t)image->value.image.subresource_count * sizeof(*states));
        staged_states[object_index] = states;
    }
    return RIN_GPU_OK;
}

static int ringpu_validate_graphics_sampled_submit(
    RinGpuCore* core, RinGpuHandle bind_group_handle,
    uint32_t** staged_states, RinGpuHandle color_target,
    uint32_t color_mip_level, uint32_t color_array_layer,
    RinGpuHandle depth_target, uint32_t depth_mip_level,
    uint32_t depth_array_layer, RinGpuHandle stencil_target,
    uint32_t stencil_mip_level, uint32_t stencil_array_layer) {
    RinGpuObjectSlot* bind_group;
    int result;
    if (bind_group_handle == 0u) return RIN_GPU_OK;
    result = ringpu_slot(core, bind_group_handle,
                         RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                         &bind_group);
    if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
    for (uint32_t binding_index = 0u;
         binding_index < bind_group->value.graphics_bind_group.binding_count;
         binding_index++) {
        const RinGpuGraphicsBindingV1* binding =
            &bind_group->value.graphics_bind_group.bindings[binding_index];
        RinGpuObjectSlot* image;
        uint32_t object_index;
        uint32_t* states;
        if (!ringpu_graphics_image_kind(binding->kind)) continue;
        result = ringpu_slot(core, binding->resource, RIN_GPU_OBJECT_IMAGE,
                             &object_index, &image);
        if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
        result = ringpu_stage_image_states(image, object_index,
                                           staged_states);
        if (result != RIN_GPU_OK) return result;
        states = staged_states[object_index];
        for (uint32_t mip = binding->mip_level;
             mip < binding->mip_level + ringpu_graphics_binding_mip_count(
                       binding, &image->value.image.descriptor);
             ++mip) {
            if ((color_target == binding->resource &&
                 color_mip_level == mip && color_array_layer == binding->array_layer) ||
                (depth_target == binding->resource &&
                 depth_mip_level == mip && depth_array_layer == binding->array_layer) ||
                (stencil_target == binding->resource &&
                 stencil_mip_level == mip && stencil_array_layer == binding->array_layer) ||
                states[binding->array_layer *
                           image->value.image.descriptor.mip_levels + mip] !=
                    RIN_GPU_IMAGE_STATE_SHADER_READ) {
                return RIN_GPU_ERROR_STATE;
            }
        }
    }
    return RIN_GPU_OK;
}

static int ringpu_queue_submit_internal(
    RinGpuCore* core, RinGpuHandle queue, const RinGpuSubmitInfoV1* submit,
    uint32_t wait_count, const RinGpuSubmitWaitV1* waits) {
    RinGpuObjectSlot* queue_slot;
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* fence = NULL;
    RinGpuBackendCommandV1* commands = NULL;
    uint32_t* staged_states[RIN_GPU_CORE_MAX_OBJECTS] = {0};
    RinGpuHandle active_render_target = 0u;
    RinGpuHandle active_depth_target = 0u;
    RinGpuHandle active_stencil_target = 0u;
    uint32_t active_render_mip_level = 0u;
    uint32_t active_render_array_layer = 0u;
    uint32_t active_depth_mip_level = 0u;
    uint32_t active_depth_array_layer = 0u;
    uint32_t active_stencil_mip_level = 0u;
    uint32_t active_stencil_array_layer = 0u;
    uint32_t active_depth_format = 0u;
    uint32_t present_count = 0u;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!submit ||
        !ringpu_versioned(submit->abi_version, submit->struct_size,
                          sizeof(*submit))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, queue, RIN_GPU_OBJECT_QUEUE, NULL, &queue_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, submit->command_list,
                         RIN_GPU_OBJECT_COMMAND_LIST, NULL, &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_EXECUTABLE ||
        (list->value.command_list.capabilities &
         ~queue_slot->value.queue.capabilities) != 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (submit->signal_fence != 0u) {
        result = ringpu_slot(core, submit->signal_fence, RIN_GPU_OBJECT_FENCE,
                             NULL, &fence);
        if (result != RIN_GPU_OK) return result;
        if (submit->signal_value <= fence->value.fence.value) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else if (submit->signal_value != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (wait_count > RIN_GPU_MAX_SUBMIT_WAITS ||
        (wait_count != 0u && !waits)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t wait_index = 0u; wait_index < wait_count; ++wait_index) {
        RinGpuObjectSlot* wait_fence;

        if (!ringpu_versioned(waits[wait_index].abi_version,
                              waits[wait_index].struct_size,
                              sizeof(waits[wait_index])) ||
            waits[wait_index].value == 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        result = ringpu_slot(core, waits[wait_index].fence,
                             RIN_GPU_OBJECT_FENCE, NULL, &wait_fence);
        if (result != RIN_GPU_OK) return result;
        /* The core and the portable software backend submit synchronously.
         * A value that is not complete cannot be silently ignored or turned
         * into a fake wait; physical asynchronous owners may block at their
         * own queue boundary before calling this admission path. */
        if (waits[wait_index].value > wait_fence->value.fence.value)
            return RIN_GPU_ERROR_BUSY;
    }
    if (list->value.command_list.count != 0u) {
        commands = (RinGpuBackendCommandV1*)calloc(
            list->value.command_list.count, sizeof(*commands));
        if (!commands) return RIN_GPU_ERROR_NO_MEMORY;
    }
    for (uint32_t index = 0; index < list->value.command_list.count; index++) {
        RinGpuRecordedCommand* command =
            &list->value.command_list.commands[index];
        RinGpuObjectSlot* destination;
        RinGpuObjectSlot* source;
        RinGpuObjectSlot* auxiliary;
        uint32_t destination_index;
        uint32_t source_index;
        uint32_t auxiliary_index;
        uint32_t* destination_states;
        uint32_t* source_states;
        uint32_t* auxiliary_states;
        commands[index].type = command->type;
        if (command->type == RIN_GPU_BACKEND_COMMAND_COPY_BUFFER) {
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &source);
            if (result != RIN_GPU_OK) break;
            commands[index].value.buffer_copy.destination_cookie =
                destination->value.buffer.backend_cookie;
            commands[index].value.buffer_copy.destination_offset =
                command->value.buffer_copy.destination_offset;
            commands[index].value.buffer_copy.source_cookie =
                source->value.buffer.backend_cookie;
            commands[index].value.buffer_copy.source_offset =
                command->value.buffer_copy.source_offset;
            commands[index].value.buffer_copy.size_bytes =
                command->value.buffer_copy.size_bytes;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER) {
            const RinGpuBufferClearV1* clear = &command->value.buffer_clear;

            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &destination);
            if (result != RIN_GPU_OK) break;
            if ((destination->value.buffer.usage &
                 RIN_GPU_BUFFER_COPY_DESTINATION) == 0u ||
                !ringpu_buffer_upload_ready(destination) ||
                !ringpu_versioned(clear->abi_version, clear->struct_size,
                                  sizeof(*clear)) || clear->flags != 0u ||
                clear->reserved != 0u || clear->size_bytes == 0u ||
                (clear->offset & UINT64_C(3)) != 0u ||
                (clear->size_bytes & UINT64_C(3)) != 0u ||
                !ringpu_range(clear->offset, clear->size_bytes,
                              destination->value.buffer.size_bytes)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.buffer_clear.destination_cookie =
                destination->value.buffer.backend_cookie;
            commands[index].value.buffer_clear.offset = clear->offset;
            commands[index].value.buffer_clear.size_bytes = clear->size_bytes;
            commands[index].value.buffer_clear.pattern = clear->pattern;
            commands[index].value.buffer_clear.reserved = clear->reserved;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_COPY_IMAGE) {
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (source_index == destination_index) {
                source_states = destination_states;
            } else {
                result = ringpu_stage_image_states(
                    source, source_index, staged_states);
                if (result != RIN_GPU_OK) break;
                source_states = staged_states[source_index];
            }
            if (destination_states[
                    command->value.image_copy.destination_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    command->value.image_copy.destination_mip_level] !=
                RIN_GPU_IMAGE_STATE_COPY_DESTINATION) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            if (source_states[
                    command->value.image_copy.source_array_layer *
                        source->value.image.descriptor.mip_levels +
                    command->value.image_copy.source_mip_level] !=
                    RIN_GPU_IMAGE_STATE_COPY_SOURCE) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.image_copy.destination_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.image_copy.source_cookie =
                source->value.image.backend_cookie;
            commands[index].value.image_copy.region =
                command->value.image_copy;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE) {
            const RinGpuImageBlitV1* blit = &command->value.image_blit;
            RinGpuBackendImageBlitV1* backend_blit =
                &commands[index].value.image_blit;

            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (source_index == destination_index) {
                source_states = destination_states;
            } else {
                result = ringpu_stage_image_states(
                    source, source_index, staged_states);
                if (result != RIN_GPU_OK) break;
                source_states = staged_states[source_index];
            }
            if (destination_states[
                    blit->destination_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    blit->destination_mip_level] !=
                    RIN_GPU_IMAGE_STATE_COPY_DESTINATION ||
                source_states[
                    blit->source_array_layer *
                        source->value.image.descriptor.mip_levels +
                    blit->source_mip_level] != RIN_GPU_IMAGE_STATE_COPY_SOURCE) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            backend_blit->destination_cookie =
                destination->value.image.backend_cookie;
            backend_blit->source_cookie = source->value.image.backend_cookie;
            backend_blit->blit = *blit;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE) {
            const RinGpuImageClearV1* clear = &command->value.image_clear;
            RinGpuBackendImageClearV1* backend_clear =
                &commands[index].value.image_clear;
            uint32_t subresource;

            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (!ringpu_versioned(clear->abi_version, clear->struct_size,
                                  sizeof(*clear)) || clear->flags != 0u ||
                clear->reserved != 0u || clear->aspects == 0u ||
                (clear->aspects & ~RIN_GPU_IMAGE_CLEAR_KNOWN_ASPECTS) != 0u ||
                clear->mip_level >= destination->value.image.descriptor.mip_levels ||
                clear->array_layer >= destination->value.image.descriptor.array_layers ||
                !ringpu_range(
                    clear->array_layer * destination->value.image.descriptor.mip_levels +
                        clear->mip_level,
                    1u, destination->value.image.subresource_count)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            subresource = clear->array_layer *
                destination->value.image.descriptor.mip_levels +
                clear->mip_level;
            if (destination_states[subresource] !=
                RIN_GPU_IMAGE_STATE_COPY_DESTINATION) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            backend_clear->destination_cookie =
                destination->value.image.backend_cookie;
            backend_clear->mip_level = clear->mip_level;
            backend_clear->array_layer = clear->array_layer;
            backend_clear->aspects = clear->aspects;
            backend_clear->flags = clear->flags;
            backend_clear->color_red = clear->color_red;
            backend_clear->color_green = clear->color_green;
            backend_clear->color_blue = clear->color_blue;
            backend_clear->color_alpha = clear->color_alpha;
            backend_clear->depth = clear->depth;
            backend_clear->stencil = clear->stencil;
            backend_clear->reserved = clear->reserved;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE) {
            const RinGpuImageTransitionV1* transition =
                &command->value.image_transition;
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            for (uint32_t layer = 0u;
                 layer < transition->array_layer_count; layer++) {
                for (uint32_t mip = 0u;
                     mip < transition->mip_level_count; mip++) {
                    uint32_t subresource =
                        (transition->base_array_layer + layer) *
                            destination->value.image.descriptor.mip_levels +
                        transition->base_mip_level + mip;
                    if (destination_states[subresource] !=
                        transition->before_state) {
                        result = RIN_GPU_ERROR_STATE;
                        break;
                    }
                }
                if (result != RIN_GPU_OK) break;
            }
            if (result != RIN_GPU_OK) break;
            for (uint32_t layer = 0u;
                 layer < transition->array_layer_count; layer++) {
                for (uint32_t mip = 0u;
                     mip < transition->mip_level_count; mip++) {
                    uint32_t subresource =
                        (transition->base_array_layer + layer) *
                            destination->value.image.descriptor.mip_levels +
                        transition->base_mip_level + mip;
                    destination_states[subresource] = transition->after_state;
                }
            }
            commands[index].value.image_transition.image_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.image_transition.transition = *transition;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_DISPATCH) {
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_COMPUTE_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_COMPUTE_BIND_GROUP, NULL,
                                 &source);
            if (result != RIN_GPU_OK) break;
            if (source->value.compute_bind_group.pipeline !=
                command->destination) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.dispatch.pipeline_cookie =
                destination->value.compute_pipeline.backend_cookie;
            commands[index].value.dispatch.bind_group_cookie =
                source->value.compute_bind_group.backend_cookie;
            commands[index].value.dispatch.group_count_x =
                command->value.dispatch.group_count_x;
            commands[index].value.dispatch.group_count_y =
                command->value.dispatch.group_count_y;
            commands[index].value.dispatch.group_count_z =
                command->value.dispatch.group_count_z;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER) {
            commands[index].value.compute_barrier.source_access =
                command->value.compute_barrier.source_access;
            commands[index].value.compute_barrier.destination_access =
                command->value.compute_barrier.destination_access;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2) {
            const RinGpuComputeBarrierV2* barrier =
                &command->value.compute_barrier_v2;
            commands[index].value.compute_barrier_v2.source_stage =
                barrier->source_stage;
            commands[index].value.compute_barrier_v2.destination_stage =
                barrier->destination_stage;
            commands[index].value.compute_barrier_v2.source_access =
                barrier->source_access;
            commands[index].value.compute_barrier_v2.destination_access =
                barrier->destination_access;
            commands[index].value.compute_barrier_v2.flags = barrier->flags;
            commands[index].value.compute_barrier_v2.reserved =
                barrier->reserved;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER) {
            const RinGpuGraphicsBarrierV1* barrier =
                &command->value.graphics_barrier;
            if (active_render_target == 0u ||
                barrier->source_access != RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                barrier->destination_access !=
                    RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                barrier->flags != 0u || barrier->reserved != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.graphics_barrier.source_access =
                barrier->source_access;
            commands[index].value.graphics_barrier.destination_access =
                barrier->destination_access;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2) {
            const RinGpuGraphicsBarrierV2* barrier =
                &command->value.graphics_barrier_v2;
            if (active_render_target == 0u || barrier->source_stage == 0u ||
                barrier->destination_stage == 0u ||
                (barrier->source_stage & ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
                (barrier->destination_stage &
                 ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
                barrier->source_access == 0u ||
                barrier->destination_access == 0u ||
                (barrier->source_access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) !=
                    0u ||
                (barrier->destination_access &
                 ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                barrier->flags != 0u || barrier->reserved != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.graphics_barrier_v2.source_stage =
                barrier->source_stage;
            commands[index].value.graphics_barrier_v2.destination_stage =
                barrier->destination_stage;
            commands[index].value.graphics_barrier_v2.source_access =
                barrier->source_access;
            commands[index].value.graphics_barrier_v2.destination_access =
                barrier->destination_access;
            commands[index].value.graphics_barrier_v2.flags = barrier->flags;
            commands[index].value.graphics_barrier_v2.reserved =
                barrier->reserved;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT) {
            const RinGpuRenderPassMrtDescV1* pass =
                &command->value.render_pass_mrt;
            RinGpuBackendRenderPassMrtBeginV1* backend_pass =
                &commands[index].value.render_pass_mrt_begin;
            const RinGpuImageDescV1* first_desc = NULL;
            RinGpuHandle first_target = 0u;
            uint32_t first_mip = 0u;
            uint32_t first_layer = 0u;
            uint32_t color_index;

            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u ||
                !ringpu_versioned(pass->abi_version, pass->struct_size,
                                  sizeof(*pass)) ||
                pass->active_color_mask == 0u ||
                (pass->active_color_mask &
                 ~((1u << RIN_GPU_MAX_COLOR_TARGETS) - 1u)) != 0u ||
                pass->color_store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_color_clear_valid(
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                pass->flags != 0u || pass->reserved0 != 0u ||
                pass->reserved1 != 0u || pass->reserved2 != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
                 ++color_index) {
                const RinGpuColorAttachmentV1* attachment =
                    &pass->color_attachments[color_index];
                RinGpuObjectSlot* color;
                const RinGpuImageDescV1* color_desc;
                uint32_t color_slot_index;
                uint32_t* color_states;

                if ((pass->active_color_mask & (1u << color_index)) == 0u) {
                    if (attachment->target != 0u || attachment->mip_level != 0u ||
                        attachment->array_layer != 0u) {
                        result = RIN_GPU_ERROR_STATE;
                        break;
                    }
                    continue;
                }
                result = ringpu_slot(core, attachment->target,
                                     RIN_GPU_OBJECT_IMAGE, &color_slot_index,
                                     &color);
                if (result != RIN_GPU_OK)
                    break;
                result = ringpu_stage_image_states(color, color_slot_index,
                                                   staged_states);
                if (result != RIN_GPU_OK)
                    break;
                color_states = staged_states[color_slot_index];
                color_desc = &color->value.image.descriptor;
                if (attachment->mip_level >= color_desc->mip_levels ||
                    attachment->array_layer >= color_desc->array_layers ||
                    (color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                    color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                    color_desc->sample_count != 1u ||
                    !ringpu_color_format(color_desc->format) ||
                    !ringpu_render_color_clear_valid_for_format(
                        color_desc->format, pass->color_load_op,
                        pass->clear_red, pass->clear_green, pass->clear_blue,
                        pass->clear_alpha, pass->color_write_mask) ||
                    color_states[attachment->array_layer * color_desc->mip_levels +
                                 attachment->mip_level] !=
                        RIN_GPU_IMAGE_STATE_COLOR_TARGET) {
                    result = RIN_GPU_ERROR_STATE;
                    break;
                }
                if (first_desc == NULL) {
                    first_desc = color_desc;
                    first_target = attachment->target;
                    first_mip = attachment->mip_level;
                    first_layer = attachment->array_layer;
                } else if (color_desc->format != first_desc->format ||
                           ringpu_mip_dimension(color_desc->width,
                                                attachment->mip_level) !=
                               ringpu_mip_dimension(first_desc->width,
                                                    first_mip) ||
                           ringpu_mip_dimension(color_desc->height,
                                                attachment->mip_level) !=
                               ringpu_mip_dimension(first_desc->height,
                                                    first_mip)) {
                    result = RIN_GPU_ERROR_UNSUPPORTED;
                    break;
                }
                for (uint32_t prior_index = 0u; prior_index < color_index;
                     ++prior_index) {
                    const RinGpuColorAttachmentV1* prior =
                        &pass->color_attachments[prior_index];

                    if ((pass->active_color_mask & (1u << prior_index)) != 0u &&
                        prior->target == attachment->target &&
                        prior->mip_level == attachment->mip_level &&
                        prior->array_layer == attachment->array_layer) {
                        result = RIN_GPU_ERROR_STATE;
                        break;
                    }
                }
                if (result != RIN_GPU_OK)
                    break;
                backend_pass->color_target_cookies[color_index] =
                    color->value.image.backend_cookie;
                backend_pass->color_mip_levels[color_index] = attachment->mip_level;
                backend_pass->color_array_layers[color_index] =
                    attachment->array_layer;
            }
            if (result != RIN_GPU_OK)
                break;
            if (first_desc == NULL || command->destination != first_target ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(first_desc->width, first_mip),
                    ringpu_mip_dimension(first_desc->height, first_mip))) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            if (pass->depth_target != 0u) {
                const RinGpuImageDescV1* depth_desc;

                result = ringpu_slot(core, pass->depth_target,
                                     RIN_GPU_OBJECT_IMAGE, &source_index,
                                     &source);
                if (result != RIN_GPU_OK)
                    break;
                result = ringpu_stage_image_states(source, source_index,
                                                   staged_states);
                if (result != RIN_GPU_OK)
                    break;
                source_states = staged_states[source_index];
                depth_desc = &source->value.image.descriptor;
                if (command->source != pass->depth_target ||
                    pass->depth_store_op != RIN_GPU_RENDER_STORE ||
                    !ringpu_render_depth_clear_valid(pass->depth_load_op,
                                                     pass->clear_depth) ||
                    pass->depth_mip_level >= depth_desc->mip_levels ||
                    pass->depth_array_layer >= depth_desc->array_layers ||
                    (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                    depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                    depth_desc->sample_count != 1u ||
                    !ringpu_depth_stencil_format(depth_desc->format) ||
                    (depth_desc->format == RIN_GPU_FORMAT_S8_UINT &&
                     (pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
                      pass->clear_depth != 0.0f)) ||
                    ringpu_mip_dimension(depth_desc->width,
                                         pass->depth_mip_level) !=
                        ringpu_mip_dimension(first_desc->width, first_mip) ||
                    ringpu_mip_dimension(depth_desc->height,
                                         pass->depth_mip_level) !=
                        ringpu_mip_dimension(first_desc->height, first_mip) ||
                    source_states[pass->depth_array_layer * depth_desc->mip_levels +
                                  pass->depth_mip_level] !=
                        RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                    result = RIN_GPU_ERROR_STATE;
                    break;
                }
                backend_pass->depth_target_cookie =
                    source->value.image.backend_cookie;
            } else if (command->source != 0u || pass->depth_mip_level != 0u ||
                       pass->depth_array_layer != 0u || pass->depth_load_op != 0u ||
                       pass->depth_store_op != 0u || pass->clear_depth != 0.0f) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            if (pass->stencil_target != 0u) {
                const RinGpuImageDescV1* stencil_desc;

                result = ringpu_slot(core, pass->stencil_target,
                                     RIN_GPU_OBJECT_IMAGE, &auxiliary_index,
                                     &auxiliary);
                if (result != RIN_GPU_OK)
                    break;
                result = ringpu_stage_image_states(auxiliary, auxiliary_index,
                                                   staged_states);
                if (result != RIN_GPU_OK)
                    break;
                auxiliary_states = staged_states[auxiliary_index];
                stencil_desc = &auxiliary->value.image.descriptor;
                if (command->auxiliary != pass->stencil_target ||
                    !ringpu_render_stencil_clear_valid(
                        stencil_desc->format, pass->stencil_load_op,
                        pass->stencil_store_op, pass->clear_stencil,
                        pass->stencil_write_mask) ||
                    pass->stencil_mip_level >= stencil_desc->mip_levels ||
                    pass->stencil_array_layer >= stencil_desc->array_layers ||
                    (stencil_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                    stencil_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                    stencil_desc->sample_count != 1u ||
                    !ringpu_depth_stencil_format(stencil_desc->format) ||
                    ringpu_mip_dimension(stencil_desc->width,
                                         pass->stencil_mip_level) !=
                        ringpu_mip_dimension(first_desc->width, first_mip) ||
                    ringpu_mip_dimension(stencil_desc->height,
                                         pass->stencil_mip_level) !=
                        ringpu_mip_dimension(first_desc->height, first_mip) ||
                    auxiliary_states[
                        pass->stencil_array_layer * stencil_desc->mip_levels +
                        pass->stencil_mip_level] != RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                    result = RIN_GPU_ERROR_STATE;
                    break;
                }
                backend_pass->stencil_target_cookie =
                    auxiliary->value.image.backend_cookie;
            } else if (command->auxiliary != 0u ||
                       pass->stencil_mip_level != 0u ||
                       pass->stencil_array_layer != 0u ||
                       pass->stencil_load_op != 0u ||
                       pass->stencil_store_op != 0u ||
                       pass->clear_stencil != 0u ||
                       pass->stencil_write_mask != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            backend_pass->active_color_mask = pass->active_color_mask;
            backend_pass->depth_mip_level = pass->depth_mip_level;
            backend_pass->depth_array_layer = pass->depth_array_layer;
            backend_pass->stencil_mip_level = pass->stencil_mip_level;
            backend_pass->stencil_array_layer = pass->stencil_array_layer;
            backend_pass->color_load_op = pass->color_load_op;
            backend_pass->color_store_op = pass->color_store_op;
            backend_pass->depth_load_op = pass->depth_load_op;
            backend_pass->depth_store_op = pass->depth_store_op;
            backend_pass->stencil_load_op = pass->stencil_load_op;
            backend_pass->stencil_store_op = pass->stencil_store_op;
            backend_pass->clear_red = pass->clear_red;
            backend_pass->clear_green = pass->clear_green;
            backend_pass->clear_blue = pass->clear_blue;
            backend_pass->clear_alpha = pass->clear_alpha;
            backend_pass->clear_depth = pass->clear_depth;
            backend_pass->clear_stencil = pass->clear_stencil;
            backend_pass->stencil_write_mask = pass->stencil_write_mask;
            backend_pass->color_write_mask = pass->color_write_mask;
            backend_pass->clear_region = pass->clear_region;
            active_render_target = first_target;
            active_render_mip_level = first_mip;
            active_render_array_layer = first_layer;
            active_depth_target = pass->depth_target;
            active_depth_mip_level = pass->depth_mip_level;
            active_depth_array_layer = pass->depth_array_layer;
            active_stencil_target = pass->stencil_target;
            active_stencil_mip_level = pass->stencil_mip_level;
            active_stencil_array_layer = pass->stencil_array_layer;
            active_depth_format = pass->depth_target != 0u
                ? source->value.image.descriptor.format
                : pass->stencil_target != 0u
                    ? auxiliary->value.image.descriptor.format : 0u;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS) {
            const RinGpuRenderPassDescV1* pass =
                &command->value.render_pass;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (pass->color_target != command->destination ||
                pass->store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_clear_valid(pass) || pass->flags != 0u ||
                !ringpu_render_color_clear_valid_for_format(
                    destination->value.image.descriptor.format, pass->load_op,
                    pass->clear_red, pass->clear_green, pass->clear_blue,
                    pass->clear_alpha, pass->color_write_mask) ||
                pass->reserved != 0u || pass->reserved1 != 0u ||
                pass->mip_level >=
                    destination->value.image.descriptor.mip_levels ||
                pass->array_layer >=
                    destination->value.image.descriptor.array_layers ||
                (destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.sample_count != 1u ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.width,
                        pass->mip_level),
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.height,
                        pass->mip_level)) ||
                destination_states[
                    pass->array_layer *
                        destination->value.image.descriptor.mip_levels +
                    pass->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.render_pass_begin.color_target_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.render_pass_begin.mip_level =
                pass->mip_level;
            commands[index].value.render_pass_begin.array_layer =
                pass->array_layer;
            commands[index].value.render_pass_begin.load_op = pass->load_op;
            commands[index].value.render_pass_begin.store_op = pass->store_op;
            commands[index].value.render_pass_begin.clear_red =
                pass->clear_red;
            commands[index].value.render_pass_begin.clear_green =
                pass->clear_green;
            commands[index].value.render_pass_begin.clear_blue =
                pass->clear_blue;
            commands[index].value.render_pass_begin.clear_alpha =
                pass->clear_alpha;
            commands[index].value.render_pass_begin.color_write_mask =
                pass->color_write_mask;
            commands[index].value.render_pass_begin.clear_region =
                pass->clear_region;
            active_render_target = command->destination;
            active_render_mip_level = pass->mip_level;
            active_render_array_layer = pass->array_layer;
            active_depth_target = 0u;
            active_stencil_target = 0u;
            active_stencil_mip_level = 0u;
            active_stencil_array_layer = 0u;
            active_depth_format = 0u;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH) {
            const RinGpuRenderPassDepthDescV1* pass =
                &command->value.render_pass_depth;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (source_index == destination_index) {
                source_states = destination_states;
            } else {
                result = ringpu_stage_image_states(
                    source, source_index, staged_states);
                if (result != RIN_GPU_OK) break;
                source_states = staged_states[source_index];
            }
            if (pass->color_target != command->destination ||
                pass->depth_target != command->source ||
                pass->color_store_op != RIN_GPU_RENDER_STORE ||
                pass->depth_store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_color_clear_valid(
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_color_clear_valid_for_format(
                    destination->value.image.descriptor.format,
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_depth_clear_valid(
                    pass->depth_load_op, pass->clear_depth) ||
                (source->value.image.descriptor.format ==
                     RIN_GPU_FORMAT_S8_UINT &&
                 (pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
                  pass->clear_depth != 0.0f)) ||
                !ringpu_render_stencil_clear_valid(
                    source->value.image.descriptor.format,
                    pass->stencil_load_op, pass->stencil_store_op,
                    pass->clear_stencil, pass->stencil_write_mask) ||
                pass->flags != 0u || pass->reserved0 != 0u ||
                pass->reserved1 != 0u || pass->reserved2 != 0u ||
                pass->reserved3 != 0u ||
                pass->color_mip_level >=
                    destination->value.image.descriptor.mip_levels ||
                pass->color_array_layer >=
                    destination->value.image.descriptor.array_layers ||
                pass->depth_mip_level >=
                    source->value.image.descriptor.mip_levels ||
                pass->depth_array_layer >=
                    source->value.image.descriptor.array_layers ||
                (destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.sample_count != 1u ||
                !ringpu_color_format(
                    destination->value.image.descriptor.format) ||
                (source->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                source->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                source->value.image.descriptor.sample_count != 1u ||
                !ringpu_depth_stencil_format(
                    source->value.image.descriptor.format) ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.width,
                        pass->color_mip_level),
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.height,
                        pass->color_mip_level)) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.width,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.width,
                        pass->depth_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.height,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.height,
                        pass->depth_mip_level) ||
                destination_states[
                    pass->color_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    pass->color_mip_level] !=
                    RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source_states[
                    pass->depth_array_layer *
                        source->value.image.descriptor.mip_levels +
                    pass->depth_mip_level] !=
                    RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.render_pass_depth_begin.color_target_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.render_pass_depth_begin.depth_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.render_pass_depth_begin.color_mip_level =
                pass->color_mip_level;
            commands[index].value.render_pass_depth_begin.color_array_layer =
                pass->color_array_layer;
            commands[index].value.render_pass_depth_begin.depth_mip_level =
                pass->depth_mip_level;
            commands[index].value.render_pass_depth_begin.depth_array_layer =
                pass->depth_array_layer;
            commands[index].value.render_pass_depth_begin.color_load_op =
                pass->color_load_op;
            commands[index].value.render_pass_depth_begin.color_store_op =
                pass->color_store_op;
            commands[index].value.render_pass_depth_begin.depth_load_op =
                pass->depth_load_op;
            commands[index].value.render_pass_depth_begin.depth_store_op =
                pass->depth_store_op;
            commands[index].value.render_pass_depth_begin.clear_red =
                pass->clear_red;
            commands[index].value.render_pass_depth_begin.clear_green =
                pass->clear_green;
            commands[index].value.render_pass_depth_begin.clear_blue =
                pass->clear_blue;
            commands[index].value.render_pass_depth_begin.clear_alpha =
                pass->clear_alpha;
            commands[index].value.render_pass_depth_begin.clear_depth =
                pass->clear_depth;
            commands[index].value.render_pass_depth_begin.stencil_load_op =
                pass->stencil_load_op;
            commands[index].value.render_pass_depth_begin.stencil_store_op =
                pass->stencil_store_op;
            commands[index].value.render_pass_depth_begin.clear_stencil =
                pass->clear_stencil;
            commands[index].value.render_pass_depth_begin.stencil_write_mask =
                pass->stencil_write_mask;
            commands[index].value.render_pass_depth_begin.reserved2 =
                pass->reserved2;
            commands[index].value.render_pass_depth_begin.color_write_mask =
                pass->color_write_mask;
            commands[index].value.render_pass_depth_begin.clear_region =
                pass->clear_region;
            active_render_target = command->destination;
            active_depth_target = command->source;
            active_render_mip_level = pass->color_mip_level;
            active_render_array_layer = pass->color_array_layer;
            active_depth_mip_level = pass->depth_mip_level;
            active_depth_array_layer = pass->depth_array_layer;
            active_depth_format = source->value.image.descriptor.format;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL) {
            const RinGpuRenderPassDepthStencilDescV1* pass =
                &command->value.render_pass_depth_stencil;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source, RIN_GPU_OBJECT_IMAGE,
                                 &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->auxiliary,
                                 RIN_GPU_OBJECT_IMAGE, &auxiliary_index,
                                 &auxiliary);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(destination, destination_index,
                                               staged_states);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                               staged_states);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(auxiliary, auxiliary_index,
                                               staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            source_states = staged_states[source_index];
            auxiliary_states = staged_states[auxiliary_index];
            if (pass->color_target != command->destination ||
                pass->depth_target != command->source ||
                pass->stencil_target != command->auxiliary ||
                command->destination == command->source ||
                command->destination == command->auxiliary ||
                command->source == command->auxiliary ||
                pass->color_store_op != RIN_GPU_RENDER_STORE ||
                pass->depth_store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_color_clear_valid(
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_color_clear_valid_for_format(
                    destination->value.image.descriptor.format,
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_depth_clear_valid(pass->depth_load_op,
                                                 pass->clear_depth) ||
                !ringpu_render_stencil_clear_valid(
                    auxiliary->value.image.descriptor.format,
                    pass->stencil_load_op,
                    pass->stencil_store_op, pass->clear_stencil,
                    pass->stencil_write_mask) ||
                pass->flags != 0u || pass->reserved0 != 0u ||
                pass->reserved1 != 0u || pass->reserved2 != 0u ||
                pass->reserved3 != 0u ||
                pass->color_mip_level >=
                    destination->value.image.descriptor.mip_levels ||
                pass->color_array_layer >=
                    destination->value.image.descriptor.array_layers ||
                pass->depth_mip_level >=
                    source->value.image.descriptor.mip_levels ||
                pass->depth_array_layer >=
                    source->value.image.descriptor.array_layers ||
                pass->stencil_mip_level >=
                    auxiliary->value.image.descriptor.mip_levels ||
                pass->stencil_array_layer >=
                    auxiliary->value.image.descriptor.array_layers ||
                (destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.sample_count != 1u ||
                !ringpu_color_format(
                    destination->value.image.descriptor.format) ||
                (source->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                source->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                source->value.image.descriptor.sample_count != 1u ||
                !ringpu_depth_aspect_format(
                    source->value.image.descriptor.format) ||
                (auxiliary->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                auxiliary->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                auxiliary->value.image.descriptor.sample_count != 1u ||
                !ringpu_stencil_aspect_format(
                    auxiliary->value.image.descriptor.format) ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.width,
                        pass->color_mip_level),
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.height,
                        pass->color_mip_level)) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.width,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.width,
                        pass->depth_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.height,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.height,
                        pass->depth_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.width,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        auxiliary->value.image.descriptor.width,
                        pass->stencil_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.height,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        auxiliary->value.image.descriptor.height,
                        pass->stencil_mip_level) ||
                destination_states[
                    pass->color_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    pass->color_mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source_states[
                    pass->depth_array_layer *
                        source->value.image.descriptor.mip_levels +
                    pass->depth_mip_level] != RIN_GPU_IMAGE_STATE_DEPTH_TARGET ||
                auxiliary_states[
                    pass->stencil_array_layer *
                        auxiliary->value.image.descriptor.mip_levels +
                    pass->stencil_mip_level] != RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.render_pass_depth_stencil_begin
                .color_target_cookie = destination->value.image.backend_cookie;
            commands[index].value.render_pass_depth_stencil_begin
                .depth_target_cookie = source->value.image.backend_cookie;
            commands[index].value.render_pass_depth_stencil_begin
                .stencil_target_cookie = auxiliary->value.image.backend_cookie;
            commands[index].value.render_pass_depth_stencil_begin.color_mip_level =
                pass->color_mip_level;
            commands[index].value.render_pass_depth_stencil_begin.color_array_layer =
                pass->color_array_layer;
            commands[index].value.render_pass_depth_stencil_begin.depth_mip_level =
                pass->depth_mip_level;
            commands[index].value.render_pass_depth_stencil_begin.depth_array_layer =
                pass->depth_array_layer;
            commands[index].value.render_pass_depth_stencil_begin.stencil_mip_level =
                pass->stencil_mip_level;
            commands[index].value.render_pass_depth_stencil_begin.stencil_array_layer =
                pass->stencil_array_layer;
            commands[index].value.render_pass_depth_stencil_begin.color_load_op =
                pass->color_load_op;
            commands[index].value.render_pass_depth_stencil_begin.color_store_op =
                pass->color_store_op;
            commands[index].value.render_pass_depth_stencil_begin.depth_load_op =
                pass->depth_load_op;
            commands[index].value.render_pass_depth_stencil_begin.depth_store_op =
                pass->depth_store_op;
            commands[index].value.render_pass_depth_stencil_begin.clear_red =
                pass->clear_red;
            commands[index].value.render_pass_depth_stencil_begin.clear_green =
                pass->clear_green;
            commands[index].value.render_pass_depth_stencil_begin.clear_blue =
                pass->clear_blue;
            commands[index].value.render_pass_depth_stencil_begin.clear_alpha =
                pass->clear_alpha;
            commands[index].value.render_pass_depth_stencil_begin.clear_depth =
                pass->clear_depth;
            commands[index].value.render_pass_depth_stencil_begin.stencil_load_op =
                pass->stencil_load_op;
            commands[index].value.render_pass_depth_stencil_begin.stencil_store_op =
                pass->stencil_store_op;
            commands[index].value.render_pass_depth_stencil_begin.clear_stencil =
                pass->clear_stencil;
            commands[index].value.render_pass_depth_stencil_begin.stencil_write_mask =
                pass->stencil_write_mask;
            commands[index].value.render_pass_depth_stencil_begin.color_write_mask =
                pass->color_write_mask;
            commands[index].value.render_pass_depth_stencil_begin.clear_region =
                pass->clear_region;
            active_render_target = command->destination;
            active_depth_target = command->source;
            active_stencil_target = command->auxiliary;
            active_render_mip_level = pass->color_mip_level;
            active_render_array_layer = pass->color_array_layer;
            active_depth_mip_level = pass->depth_mip_level;
            active_depth_array_layer = pass->depth_array_layer;
            active_stencil_mip_level = 0u;
            active_stencil_array_layer = 0u;
            active_stencil_mip_level = pass->stencil_mip_level;
            active_stencil_array_layer = pass->stencil_array_layer;
            active_depth_format = RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE) {
            const RinGpuRasterStateV5* state = &command->value.raster_state;
            uint32_t target_width;
            uint32_t target_height;
            if (active_render_target == 0u ||
                !ringpu_raster_state_valid(&state->base.base.base.base)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, active_render_target,
                                 RIN_GPU_OBJECT_IMAGE, NULL, &destination);
            if (result != RIN_GPU_OK) break;
            target_width = ringpu_mip_dimension(
                destination->value.image.descriptor.width,
                active_render_mip_level);
            target_height = ringpu_mip_dimension(
                destination->value.image.descriptor.height,
                active_render_mip_level);
            /* Viewports may extend beyond an attachment, including from a
             * negative lower-left origin. Backends clip coverage to the
             * attachment; only the integer scissor retains bounded target
             * coordinates in this profile. */
            if (state->base.base.base.base.scissor.enabled != 0u &&
                 ((uint32_t)state->base.base.base.base.scissor.x > target_width ||
                  state->base.base.base.base.scissor.width >
                      target_width - (uint32_t)state->base.base.base.base.scissor.x ||
                  (uint32_t)state->base.base.base.base.scissor.y > target_height ||
                  state->base.base.base.base.scissor.height >
                      target_height - (uint32_t)state->base.base.base.base.scissor.y)) {
                result = RIN_GPU_ERROR_BOUNDS;
                break;
            }
            commands[index].value.raster_state.viewport_x = state->base.base.base.base.viewport.x;
            commands[index].value.raster_state.viewport_y = state->base.base.base.base.viewport.y;
            commands[index].value.raster_state.viewport_width =
                state->base.base.base.base.viewport.width;
            commands[index].value.raster_state.viewport_height =
                state->base.base.base.base.viewport.height;
            commands[index].value.raster_state.min_depth =
                state->base.base.base.base.viewport.min_depth;
            commands[index].value.raster_state.max_depth =
                state->base.base.base.base.viewport.max_depth;
            commands[index].value.raster_state.scissor_x = state->base.base.base.base.scissor.x;
            commands[index].value.raster_state.scissor_y = state->base.base.base.base.scissor.y;
            commands[index].value.raster_state.scissor_width =
                state->base.base.base.base.scissor.width;
            commands[index].value.raster_state.scissor_height =
                state->base.base.base.base.scissor.height;
            commands[index].value.raster_state.scissor_enabled =
                state->base.base.base.base.scissor.enabled;
            commands[index].value.raster_state.polygon_offset_fill_enabled =
                state->base.base.base.polygon_offset_fill_enabled;
            commands[index].value.raster_state.polygon_offset_factor =
                state->base.base.base.polygon_offset_factor;
            commands[index].value.raster_state.polygon_offset_units =
                state->base.base.base.polygon_offset_units;
            commands[index].value.raster_state.line_width = state->base.base.line_width;
            commands[index].value.raster_state.sample_coverage_enabled =
                state->base.sample_coverage_enabled;
            commands[index].value.raster_state.sample_coverage_value =
                state->base.sample_coverage_value;
            commands[index].value.raster_state.sample_coverage_invert =
                state->base.sample_coverage_invert;
            commands[index].value.raster_state.dither_enabled =
                state->dither_enabled;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_DRAW) {
            uint64_t bind_group_cookie;
            if (active_render_target != command->source ||
                active_render_mip_level != command->value.draw.mip_level ||
                active_render_array_layer !=
                    command->value.draw.array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            if (source_states[
                    command->value.draw.array_layer *
                        source->value.image.descriptor.mip_levels +
                    command->value.draw.mip_level] !=
                    RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                destination->value.graphics_pipeline.vertex_input_count != 0u ||
                destination->value.graphics_pipeline.vertex_stride != 0u ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.draw.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw.mip_level =
                command->value.draw.mip_level;
            commands[index].value.draw.array_layer =
                command->value.draw.array_layer;
            commands[index].value.draw.vertex_count =
                command->value.draw.vertex_count;
            commands[index].value.draw.instance_count =
                command->value.draw.instance_count;
            commands[index].value.draw.first_vertex =
                command->value.draw.first_vertex;
            commands[index].value.draw.first_instance =
                command->value.draw.first_instance;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES) {
            const RinGpuDrawVerticesV1* draw =
                &command->value.draw_vertices;
            RinGpuObjectSlot* vertex_buffer = NULL;
            uint64_t bind_group_cookie;
            uint64_t vertex_end;
            uint64_t required_bytes;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            if (destination->value.graphics_pipeline.vertex_stride != 0u) {
                result = ringpu_slot(core, draw->vertex_buffer,
                                     RIN_GPU_OBJECT_BUFFER, NULL,
                                     &vertex_buffer);
                if (result != RIN_GPU_OK) break;
            }
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            vertex_end = (uint64_t)draw->first_vertex + draw->vertex_count;
            if (destination->value.graphics_pipeline.vertex_input_count == 0u ||
                (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u ||
                (destination->value.graphics_pipeline.vertex_stride == 0u
                     ? (draw->vertex_buffer != 0u ||
                        draw->vertex_offset != 0u)
                     : (vertex_buffer == NULL ||
                        (vertex_buffer->value.buffer.usage &
                         RIN_GPU_BUFFER_VERTEX) == 0u ||
                        !ringpu_buffer_upload_ready(vertex_buffer) ||
                        !ringpu_multiply_u64(
                            vertex_end,
                            destination->value.graphics_pipeline.vertex_stride,
                            &required_bytes) ||
                        draw->vertex_offset >
                            vertex_buffer->value.buffer.size_bytes ||
                        required_bytes >
                            vertex_buffer->value.buffer.size_bytes -
                                draw->vertex_offset)) ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.draw_vertices.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_vertices.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_vertices.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_vertices.vertex_buffer_cookie =
                vertex_buffer != NULL
                    ? vertex_buffer->value.buffer.backend_cookie : 0u;
            commands[index].value.draw_vertices.vertex_offset =
                draw->vertex_offset;
            commands[index].value.draw_vertices.vertex_stride =
                destination->value.graphics_pipeline.vertex_stride;
            commands[index].value.draw_vertices.mip_level = draw->mip_level;
            commands[index].value.draw_vertices.array_layer =
                draw->array_layer;
            commands[index].value.draw_vertices.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_vertices.instance_count =
                draw->instance_count;
            commands[index].value.draw_vertices.first_vertex =
                draw->first_vertex;
            commands[index].value.draw_vertices.first_instance =
                draw->first_instance;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2) {
            const RinGpuDrawVerticesV2* draw =
                &command->value.draw_vertices_v2;
            RinGpuObjectSlot* vertex_buffers[
                RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
            uint64_t bind_group_cookie;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            if (destination->value.graphics_pipeline.vertex_input_count == 0u ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_vertex_bindings_for_draw(
                core, destination, draw->vertex_buffers, draw->binding_count,
                draw->first_vertex, draw->vertex_count, draw->first_instance,
                draw->instance_count, vertex_buffers);
            if (result != RIN_GPU_OK) break;
            commands[index].value.draw_vertices_v2.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_vertices_v2.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_vertices_v2.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_vertices_v2.mip_level = draw->mip_level;
            commands[index].value.draw_vertices_v2.array_layer =
                draw->array_layer;
            commands[index].value.draw_vertices_v2.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_vertices_v2.instance_count =
                draw->instance_count;
            commands[index].value.draw_vertices_v2.first_vertex =
                draw->first_vertex;
            commands[index].value.draw_vertices_v2.first_instance =
                draw->first_instance;
            commands[index].value.draw_vertices_v2.vertex_binding_count =
                draw->binding_count;
            for (uint32_t binding = 0u; binding < draw->binding_count;
                 ++binding) {
                commands[index].value.draw_vertices_v2.vertex_buffers[binding]
                    .binding = draw->vertex_buffers[binding].binding;
                commands[index].value.draw_vertices_v2.vertex_buffers[binding]
                    .buffer_cookie =
                        vertex_buffers[binding]->value.buffer.backend_cookie;
                commands[index].value.draw_vertices_v2.vertex_buffers[binding]
                    .offset = draw->vertex_buffers[binding].offset;
            }
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED ||
                   command->type ==
                       RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
            const RinGpuDrawIndexedV1* draw =
                &command->value.draw_indexed;
            RinGpuObjectSlot* vertex_buffer = NULL;
            RinGpuObjectSlot* index_buffer;
            uint64_t bind_group_cookie;
            uint64_t vertex_bytes;
            uint64_t index_end;
            uint64_t index_bytes;
            uint32_t index_stride;
            int32_t base_vertex = command->base_vertex;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            if (destination->value.graphics_pipeline.vertex_stride != 0u) {
                result = ringpu_slot(core, draw->vertex_buffer,
                                     RIN_GPU_OBJECT_BUFFER, NULL,
                                     &vertex_buffer);
                if (result != RIN_GPU_OK) break;
            }
            result = ringpu_slot(core, draw->index_buffer,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &index_buffer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            index_stride = ringpu_index_format_bytes(draw->index_format);
            index_end = (uint64_t)draw->first_index + draw->index_count;
            if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
                destination->value.graphics_pipeline.vertex_input_count == 0u ||
                index_stride == 0u || draw->index_count == 0u ||
                draw->vertex_count == 0u || draw->instance_count == 0u ||
                !ringpu_base_vertex_has_valid_index(
                    base_vertex, draw->vertex_count, index_stride) ||
                (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u ||
                (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u ||
                (index_buffer->value.buffer.usage &
                 RIN_GPU_BUFFER_INDEX) == 0u ||
                !ringpu_buffer_upload_ready(index_buffer) ||
                draw->index_offset > index_buffer->value.buffer.size_bytes ||
                index_bytes >
                    index_buffer->value.buffer.size_bytes -
                        draw->index_offset ||
                (destination->value.graphics_pipeline.vertex_stride == 0u
                     ? (draw->vertex_buffer != 0u ||
                        draw->vertex_offset != 0u)
                     : (vertex_buffer == NULL ||
                        (vertex_buffer->value.buffer.usage &
                         RIN_GPU_BUFFER_VERTEX) == 0u ||
                        !ringpu_buffer_upload_ready(vertex_buffer) ||
                        !ringpu_multiply_u64(
                            draw->vertex_count,
                            destination->value.graphics_pipeline.vertex_stride,
                            &vertex_bytes) ||
                        draw->vertex_offset >
                            vertex_buffer->value.buffer.size_bytes ||
                        vertex_bytes >
                            vertex_buffer->value.buffer.size_bytes -
                                draw->vertex_offset)) ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.draw_indexed.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_indexed.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_indexed.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_indexed.vertex_buffer_cookie =
                vertex_buffer != NULL
                    ? vertex_buffer->value.buffer.backend_cookie : 0u;
            commands[index].value.draw_indexed.index_buffer_cookie =
                index_buffer->value.buffer.backend_cookie;
            commands[index].value.draw_indexed.vertex_offset =
                draw->vertex_offset;
            commands[index].value.draw_indexed.index_offset =
                draw->index_offset;
            commands[index].value.draw_indexed.vertex_stride =
                destination->value.graphics_pipeline.vertex_stride;
            commands[index].value.draw_indexed.index_format =
                draw->index_format;
            commands[index].value.draw_indexed.mip_level = draw->mip_level;
            commands[index].value.draw_indexed.array_layer =
                draw->array_layer;
            commands[index].value.draw_indexed.index_count =
                draw->index_count;
            commands[index].value.draw_indexed.instance_count =
                draw->instance_count;
            commands[index].value.draw_indexed.first_index =
                draw->first_index;
            commands[index].value.draw_indexed.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_indexed.first_instance =
                draw->first_instance;
            if (command->type ==
                RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
                commands[index].value.draw_indexed_base_vertex.base_vertex =
                    base_vertex;
            } else {
                commands[index].value.draw_indexed.reserved = 0u;
            }
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2) {
            const RinGpuDrawIndexedV2* draw =
                &command->value.draw_indexed_v2;
            RinGpuObjectSlot* vertex_buffers[
                RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
            RinGpuObjectSlot* index_buffer;
            uint64_t bind_group_cookie;
            uint64_t index_end;
            uint64_t index_bytes;
            uint32_t index_stride;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, draw->index_buffer,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &index_buffer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            index_stride = ringpu_index_format_bytes(draw->index_format);
            index_end = (uint64_t)draw->first_index + draw->index_count;
            if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
                destination->value.graphics_pipeline.vertex_input_count == 0u ||
                index_stride == 0u || draw->index_count == 0u ||
                draw->vertex_count == 0u || draw->instance_count == 0u ||
                (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u ||
                (index_buffer->value.buffer.usage &
                 RIN_GPU_BUFFER_INDEX) == 0u ||
                !ringpu_buffer_upload_ready(index_buffer) ||
                draw->index_offset > index_buffer->value.buffer.size_bytes ||
                index_bytes >
                    index_buffer->value.buffer.size_bytes - draw->index_offset ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_vertex_bindings_for_draw(
                core, destination, draw->vertex_buffers, draw->binding_count,
                0u, draw->vertex_count, draw->first_instance,
                draw->instance_count, vertex_buffers);
            if (result != RIN_GPU_OK) break;
            commands[index].value.draw_indexed_v2.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_indexed_v2.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_indexed_v2.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_indexed_v2.index_buffer_cookie =
                index_buffer->value.buffer.backend_cookie;
            commands[index].value.draw_indexed_v2.index_offset =
                draw->index_offset;
            commands[index].value.draw_indexed_v2.index_format =
                draw->index_format;
            commands[index].value.draw_indexed_v2.mip_level = draw->mip_level;
            commands[index].value.draw_indexed_v2.array_layer =
                draw->array_layer;
            commands[index].value.draw_indexed_v2.index_count =
                draw->index_count;
            commands[index].value.draw_indexed_v2.instance_count =
                draw->instance_count;
            commands[index].value.draw_indexed_v2.first_index =
                draw->first_index;
            commands[index].value.draw_indexed_v2.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_indexed_v2.first_instance =
                draw->first_instance;
            commands[index].value.draw_indexed_v2.vertex_binding_count =
                draw->binding_count;
            for (uint32_t binding = 0u; binding < draw->binding_count;
                 ++binding) {
                commands[index].value.draw_indexed_v2.vertex_buffers[binding]
                    .binding = draw->vertex_buffers[binding].binding;
                commands[index].value.draw_indexed_v2.vertex_buffers[binding]
                    .buffer_cookie =
                        vertex_buffers[binding]->value.buffer.backend_cookie;
                commands[index].value.draw_indexed_v2.vertex_buffers[binding]
                    .offset = draw->vertex_buffers[binding].offset;
            }
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS) {
            if (active_render_target == 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            active_render_target = 0u;
            active_depth_target = 0u;
            active_stencil_target = 0u;
            active_render_mip_level = 0u;
            active_render_array_layer = 0u;
            active_depth_mip_level = 0u;
            active_depth_array_layer = 0u;
            active_stencil_mip_level = 0u;
            active_stencil_array_layer = 0u;
            active_depth_format = 0u;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_PRESENT) {
            const RinGpuPresentV1* present = &command->value.present;
            const RinGpuDisplayInfoV1* display;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u ||
                present->image != command->destination ||
                present->flags != 0u || present->reserved0 != 0u ||
                present->reserved1 != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            display = ringpu_find_display(core, present->display_id);
            if (!display) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if ((destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_PRESENT) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.array_layers != 1u ||
                destination->value.image.descriptor.mip_levels != 1u ||
                destination->value.image.descriptor.sample_count != 1u ||
                destination->value.image.descriptor.width != display->width ||
                destination->value.image.descriptor.height !=
                    display->height ||
                destination->value.image.descriptor.format !=
                    display->format ||
                destination_states[0] != RIN_GPU_IMAGE_STATE_PRESENT) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.present.image_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.present.display_id = present->display_id;
        } else {
            result = RIN_GPU_ERROR_STATE;
            break;
        }
    }
    if (result == RIN_GPU_OK &&
        (active_render_target != 0u || active_depth_target != 0u ||
         active_stencil_target != 0u)) {
        result = RIN_GPU_ERROR_STATE;
    }
    if (result == RIN_GPU_OK) {
        for (uint32_t index = 0u;
             index < list->value.command_list.count; ++index) {
            if (commands[index].type == RIN_GPU_BACKEND_COMMAND_PRESENT)
                ++present_count;
        }
    }
    if (result == RIN_GPU_OK) {
        result = core->backend.submit_commands(
            core->backend_context, commands,
            list->value.command_list.count);
    }
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_SUBMISSION,
                           submit->command_list, queue,
                           list->value.command_list.count,
                           submit->signal_value, result);
    for (uint32_t index = 0u; index < present_count; ++index)
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_PRESENT, 0u, queue,
                               submit->signal_value, index, result);
    if (result == RIN_GPU_OK) {
        for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
            if (!staged_states[index]) continue;
            memcpy(core->objects[index].value.image.subresource_states,
                   staged_states[index],
                   (size_t)core->objects[index].value.image.subresource_count *
                       sizeof(*staged_states[index]));
        }
    }
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        free(staged_states[index]);
    }
    free(commands);
    if (result != RIN_GPU_OK) return result;
    if (fence) fence->value.fence.value = submit->signal_value;
    return RIN_GPU_OK;
}

int ringpu_queue_submit(RinGpuCore* core, RinGpuHandle queue,
                        const RinGpuSubmitInfoV1* submit)
{
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!submit || !ringpu_versioned(submit->abi_version,
                                     submit->struct_size,
                                     sizeof(*submit))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_queue_submit_internal(core, queue, submit, 0u, NULL);
}

int ringpu_queue_submit_v2(RinGpuCore* core, RinGpuHandle queue,
                           const RinGpuSubmitInfoV2* submit)
{
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!submit || submit->base.abi_version != RIN_GPU_ABI_VERSION ||
        submit->base.struct_size != sizeof(submit->base) ||
        submit->wait_count > RIN_GPU_MAX_SUBMIT_WAITS || submit->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_queue_submit_internal(core, queue, &submit->base,
                                        submit->wait_count, submit->waits);
}
