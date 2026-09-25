// SPDX-License-Identifier: MIT
#include "draw.h"

#include "../core/object_table.h"
#include "../pipeline/pipelines.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"
#include "record.h"

#include <string.h>

void ringpu_release_graphics_bind_group_reference(
    RinGpuCore* core, RinGpuHandle bind_group) {
    RinGpuObjectSlot* slot;
    if (bind_group != 0u &&
        ringpu_slot(core, bind_group, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP,
                    NULL, &slot) == RIN_GPU_OK &&
        slot->value.graphics_bind_group.reference_count != 0u) {
        slot->value.graphics_bind_group.reference_count--;
    }
}

int ringpu_command_bind_graphics_resources(RinGpuCore* core,
                                           RinGpuHandle command_list,
                                           RinGpuHandle bind_group) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* group = NULL;
    RinGpuHandle previous;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (bind_group != 0u) {
        result = ringpu_slot(core, bind_group,
                             RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                             &group);
        if (result != RIN_GPU_OK) return result;
    }
    previous = list->value.command_list.graphics_bind_group;
    if (previous == bind_group) return RIN_GPU_OK;
    if (group) group->value.graphics_bind_group.reference_count++;
    ringpu_release_graphics_bind_group_reference(core, previous);
    list->value.command_list.graphics_bind_group = bind_group;
    return RIN_GPU_OK;
}

int ringpu_command_set_raster_state(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRasterStateV1* state) {
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!ringpu_raster_state_valid(state)) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST,
                         NULL, &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE;
    memset(&command->value.raster_state, 0,
           sizeof(command->value.raster_state));
    command->value.raster_state.base.base.base.base = *state;
    command->value.raster_state.base.base.line_width = 1.0f;
    command->value.raster_state.base.sample_coverage_value = 1.0f;
    if (state->struct_size == sizeof(RinGpuRasterStateV2) ||
        state->struct_size == sizeof(RinGpuRasterStateV3) ||
        state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV2* extended =
            (const RinGpuRasterStateV2*)(const void*)state;

        command->value.raster_state.base.base.base.polygon_offset_fill_enabled =
            extended->polygon_offset_fill_enabled;
        command->value.raster_state.base.base.base.polygon_offset_factor =
            extended->polygon_offset_factor;
        command->value.raster_state.base.base.base.polygon_offset_units =
            extended->polygon_offset_units;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV3) ||
        state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV3* extended =
            (const RinGpuRasterStateV3*)(const void*)state;

        command->value.raster_state.base.base.line_width = extended->line_width;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV4* extended =
            (const RinGpuRasterStateV4*)(const void*)state;

        command->value.raster_state.base.sample_coverage_enabled =
            extended->sample_coverage_enabled;
        command->value.raster_state.base.sample_coverage_value =
            extended->sample_coverage_value;
        command->value.raster_state.base.sample_coverage_invert =
            extended->sample_coverage_invert;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV5* extended =
            (const RinGpuRasterStateV5*)(const void*)state;

        command->value.raster_state.dither_enabled = extended->dither_enabled;
    }
    command->value.raster_state.base.base.base.base.struct_size =
        sizeof(command->value.raster_state);
    command->value.raster_state.base.base.base.base.viewport.struct_size =
        sizeof(command->value.raster_state.base.base.base.base.viewport);
    command->value.raster_state.base.base.base.base.scissor.struct_size =
        sizeof(command->value.raster_state.base.base.base.base.scissor);
    list->value.command_list.count++;
    return RIN_GPU_OK;
}

static int ringpu_validate_graphics_sampled_record(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    const RinGpuObjectSlot* bind_group, uint32_t command_limit) {
    if (!core || !list || !bind_group ||
        command_limit > list->value.command_list.count) {
        return RIN_GPU_ERROR_STATE;
    }
    for (uint32_t binding_index = 0u;
         binding_index < bind_group->value.graphics_bind_group.binding_count;
         binding_index++) {
        const RinGpuGraphicsBindingV1* binding =
            &bind_group->value.graphics_bind_group.bindings[binding_index];
        RinGpuObjectSlot* image;
        uint32_t state;
        int result;
        if (!ringpu_graphics_image_kind(binding->kind)) continue;
        result = ringpu_slot(core, binding->resource, RIN_GPU_OBJECT_IMAGE,
                             NULL, &image);
        if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
        for (uint32_t mip = binding->mip_level;
             mip < binding->mip_level + ringpu_graphics_binding_mip_count(
                       binding, &image->value.image.descriptor);
             ++mip) {
            uint32_t color_index;
            int samples_active_color = 0;

            for (color_index = 0u;
                 color_index < RIN_GPU_MAX_COLOR_TARGETS; ++color_index) {
                if ((list->value.command_list.active_color_mask &
                     (1u << color_index)) != 0u &&
                    list->value.command_list.render_color_targets[color_index] ==
                        binding->resource &&
                    list->value.command_list.render_color_mip_levels[color_index] ==
                        mip &&
                    list->value.command_list
                        .render_color_array_layers[color_index] ==
                        binding->array_layer) {
                    samples_active_color = 1;
                    break;
                }
            }
            if (samples_active_color ||
                (list->value.command_list.render_depth_target == binding->resource &&
                 list->value.command_list.render_depth_mip_level == mip &&
                 list->value.command_list.render_depth_array_layer == binding->array_layer) ||
                (list->value.command_list.render_stencil_target == binding->resource &&
                 list->value.command_list.render_stencil_mip_level == mip &&
                 list->value.command_list.render_stencil_array_layer == binding->array_layer)) {
                return RIN_GPU_ERROR_STATE;
            }
            state = image->value.image.subresource_states[
                binding->array_layer * image->value.image.descriptor.mip_levels + mip];
            for (uint32_t command_index = 0u; command_index < command_limit;
                 command_index++) {
                const RinGpuRecordedCommand* command =
                    &list->value.command_list.commands[command_index];
                const RinGpuImageTransitionV1* transition;
                if (command->type != RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE ||
                    command->destination != binding->resource) {
                    continue;
                }
                transition = &command->value.image_transition;
                if (mip < transition->base_mip_level ||
                    mip >= transition->base_mip_level + transition->mip_level_count ||
                    binding->array_layer < transition->base_array_layer ||
                    binding->array_layer >= transition->base_array_layer +
                        transition->array_layer_count) {
                    continue;
                }
                if (state != transition->before_state)
                    return RIN_GPU_ERROR_STATE;
                state = transition->after_state;
            }
            if (state != RIN_GPU_IMAGE_STATE_SHADER_READ)
                return RIN_GPU_ERROR_STATE;
        }
    }
    return RIN_GPU_OK;
}

static int ringpu_graphics_resources_for_draw(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    RinGpuHandle pipeline_handle, const RinGpuObjectSlot* pipeline,
    RinGpuHandle* bind_group_handle, RinGpuObjectSlot** bind_group) {
    if (!core || !list || !pipeline || !bind_group_handle || !bind_group) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *bind_group_handle = 0u;
    *bind_group = NULL;
    if (pipeline->value.graphics_pipeline.resource_count == 0u) {
        return RIN_GPU_OK;
    }
    *bind_group_handle = list->value.command_list.graphics_bind_group;
    {
        int result = ringpu_validate_graphics_bind_group(
            core, pipeline_handle, pipeline, *bind_group_handle, bind_group);
        if (result != RIN_GPU_OK) return result;
        return ringpu_validate_graphics_sampled_record(
            core, list, *bind_group, list->value.command_list.count);
    }
}

int ringpu_graphics_draw_has_hazard(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    RinGpuHandle new_bind_group_handle, uint32_t command_limit) {
    RinGpuObjectSlot* new_bind_group;
    if (!core || !list || command_limit > list->value.command_list.count) {
        return 1;
    }
    if (new_bind_group_handle == 0u) return 0;
    if (ringpu_slot(core, new_bind_group_handle,
                    RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                    &new_bind_group) != RIN_GPU_OK) {
        return 1;
    }
    for (uint32_t reverse = command_limit; reverse != 0u; reverse--) {
        const RinGpuRecordedCommand* previous =
            &list->value.command_list.commands[reverse - 1u];
        RinGpuObjectSlot* previous_group;
        if (previous->type == RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER ||
            previous->type == RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2) {
            return 0;
        }
        if (previous->type == RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS ||
            previous->type ==
                RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH ||
            previous->type ==
                RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL) {
            return 0;
        }
        if (previous->type != RIN_GPU_BACKEND_COMMAND_DRAW &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2 &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2 &&
            previous->type !=
                RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
            continue;
        }
        if (previous->resources == 0u) continue;
        if (ringpu_slot(core, previous->resources,
                        RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                        &previous_group) != RIN_GPU_OK) {
            return 1;
        }
        for (uint32_t current = 0u;
             current <
                 new_bind_group->value.graphics_bind_group.binding_count;
             current++) {
            const RinGpuGraphicsBindingV1* current_binding =
                &new_bind_group->value.graphics_bind_group.bindings[current];
            for (uint32_t prior = 0u;
                 prior <
                     previous_group->value.graphics_bind_group.binding_count;
                 prior++) {
                const RinGpuGraphicsBindingV1* prior_binding =
                    &previous_group->value.graphics_bind_group.bindings[prior];
                if (current_binding->kind ==
                        RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                    prior_binding->kind ==
                        RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                    current_binding->resource == prior_binding->resource &&
                    current_binding->offset <
                        prior_binding->offset + prior_binding->size_bytes &&
                    prior_binding->offset <
                        current_binding->offset +
                            current_binding->size_bytes &&
                    ((current_binding->access | prior_binding->access) &
                     RIN_GPU_RESOURCE_WRITE) != 0u) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int ringpu_command_draw(RinGpuCore* core, RinGpuHandle command_list,
                        const RinGpuDrawV1* draw) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    RinGpuHandle bind_group_handle;
    const RinGpuImageDescV1* target_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->vertex_count == 0u || draw->instance_count == 0u ||
        draw->flags != 0u || draw->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count != 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw = *draw;
    command->value.draw.struct_size = sizeof(command->value.draw);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_vertex_bindings_for_draw(
    RinGpuCore* core, const RinGpuObjectSlot* pipeline,
    const RinGpuVertexBufferBindingV1* bindings, uint32_t binding_count,
    uint32_t first_vertex, uint32_t vertex_count, uint32_t first_instance,
    uint32_t instance_count,
    RinGpuObjectSlot* resolved[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS])
{
    uint64_t vertex_end;

    if (!core || !pipeline || !resolved ||
        pipeline->type != RIN_GPU_OBJECT_GRAPHICS_PIPELINE ||
        binding_count != pipeline->value.graphics_pipeline.vertex_binding_count ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0u || instance_count == 0u ||
        first_vertex > UINT32_MAX - vertex_count ||
        first_instance > UINT32_MAX - instance_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    vertex_end = (uint64_t)first_vertex + vertex_count;
    for (uint32_t index = 0u; index < binding_count; ++index) {
        const RinGpuVertexBufferBindingV1* binding = &bindings[index];
        const RinGpuVertexBufferLayoutV1* layout =
            &pipeline->value.graphics_pipeline.vertex_bindings[index];
        RinGpuObjectSlot* buffer;
        uint64_t required_bytes;
        int result;

        if (binding->binding != index || binding->reserved != 0u ||
            (binding->offset & (sizeof(uint32_t) - 1u)) != 0u ||
            layout->binding != index || layout->stride == 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        result = ringpu_slot(core, binding->buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &buffer);
        if (result != RIN_GPU_OK) return result;
        if ((buffer->value.buffer.usage & RIN_GPU_BUFFER_VERTEX) == 0u ||
            !ringpu_buffer_upload_ready(buffer)) {
            return RIN_GPU_ERROR_STATE;
        }
        uint64_t element_end = layout->flags == 0u
            ? vertex_end
            : ((uint64_t)first_instance + instance_count - 1u) /
                    layout->flags +
                1u;

        if (!ringpu_multiply_u64(element_end, layout->stride, &required_bytes) ||
            binding->offset > buffer->value.buffer.size_bytes ||
            required_bytes >
                buffer->value.buffer.size_bytes - binding->offset) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        resolved[index] = buffer;
    }
    return RIN_GPU_OK;
}

int ringpu_command_draw_vertices(RinGpuCore* core,
                                 RinGpuHandle command_list,
                                 const RinGpuDrawVerticesV1* draw) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* vertex_buffer = NULL;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    uint64_t vertex_end;
    uint64_t required_bytes;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->vertex_count == 0u || draw->instance_count == 0u ||
        draw->flags != 0u || draw->reserved != 0u ||
        (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (pipeline->value.graphics_pipeline.vertex_binding_count > 1u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->value.graphics_pipeline.vertex_stride == 0u) {
        if (draw->vertex_buffer != 0u || draw->vertex_offset != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    } else {
        result = ringpu_slot(core, draw->vertex_buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &vertex_buffer);
        if (result != RIN_GPU_OK) return result;
        if ((vertex_buffer->value.buffer.usage & RIN_GPU_BUFFER_VERTEX) == 0u ||
            !ringpu_buffer_upload_ready(vertex_buffer)) {
            return RIN_GPU_ERROR_STATE;
        }
        vertex_end = (uint64_t)draw->first_vertex + draw->vertex_count;
        if (!ringpu_multiply_u64(
                vertex_end, pipeline->value.graphics_pipeline.vertex_stride,
                &required_bytes) ||
            draw->vertex_offset > vertex_buffer->value.buffer.size_bytes ||
            required_bytes >
                vertex_buffer->value.buffer.size_bytes - draw->vertex_offset) {
            return RIN_GPU_ERROR_BOUNDS;
        }
    }
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_vertices = *draw;
    command->value.draw_vertices.struct_size =
        sizeof(command->value.draw_vertices);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    if (vertex_buffer != NULL)
        vertex_buffer->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_vertices_v2(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawVerticesV2* draw)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!draw || !ringpu_versioned(draw->abi_version, draw->struct_size,
                                   sizeof(*draw)) ||
        draw->vertex_count == 0u || draw->instance_count == 0u ||
        draw->flags != 0u || draw->reserved0 != 0u ||
        draw->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u ||
        list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_vertex_bindings_for_draw(
        core, pipeline, draw->vertex_buffers, draw->binding_count,
        draw->first_vertex, draw->vertex_count, draw->first_instance,
        draw->instance_count, vertex_buffers);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_vertices_v2 = *draw;
    command->value.draw_vertices_v2.struct_size =
        sizeof(command->value.draw_vertices_v2);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    for (uint32_t index = 0u; index < draw->binding_count; ++index)
        vertex_buffers[index]->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

static int ringpu_command_draw_indexed_internal(
        RinGpuCore* core, RinGpuHandle command_list,
        const RinGpuDrawIndexedV1* draw, int32_t base_vertex,
        uint32_t backend_command_type) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* vertex_buffer = NULL;
    RinGpuObjectSlot* index_buffer;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    uint64_t vertex_bytes;
    uint64_t index_end;
    uint64_t index_bytes;
    uint32_t index_stride;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->index_count == 0u || draw->instance_count == 0u ||
        draw->vertex_count == 0u ||
        draw->flags != 0u || draw->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    index_stride = ringpu_index_format_bytes(draw->index_format);
    if (index_stride == 0u ||
        (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u ||
        (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!ringpu_base_vertex_has_valid_index(
            base_vertex, draw->vertex_count, index_stride)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (backend_command_type != RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED &&
        backend_command_type !=
            RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->index_count > RIN_GPU_MAX_DRAW_INDICES ||
        draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_index > UINT32_MAX - draw->index_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (pipeline->value.graphics_pipeline.vertex_binding_count > 1u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, draw->index_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &index_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((index_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDEX) == 0u ||
        !ringpu_buffer_upload_ready(index_buffer)) {
        return RIN_GPU_ERROR_STATE;
    }
    index_end = (uint64_t)draw->first_index + draw->index_count;
    if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
        draw->index_offset > index_buffer->value.buffer.size_bytes ||
        index_bytes >
            index_buffer->value.buffer.size_bytes - draw->index_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (pipeline->value.graphics_pipeline.vertex_stride == 0u) {
        if (draw->vertex_buffer != 0u || draw->vertex_offset != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    } else {
        result = ringpu_slot(core, draw->vertex_buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &vertex_buffer);
        if (result != RIN_GPU_OK) return result;
        if ((vertex_buffer->value.buffer.usage & RIN_GPU_BUFFER_VERTEX) == 0u ||
            !ringpu_buffer_upload_ready(vertex_buffer)) {
            return RIN_GPU_ERROR_STATE;
        }
        if (!ringpu_multiply_u64(
                draw->vertex_count,
                pipeline->value.graphics_pipeline.vertex_stride,
                &vertex_bytes) ||
            draw->vertex_offset > vertex_buffer->value.buffer.size_bytes ||
            vertex_bytes >
                vertex_buffer->value.buffer.size_bytes - draw->vertex_offset) {
            return RIN_GPU_ERROR_BOUNDS;
        }
    }
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = backend_command_type;
    command->base_vertex = base_vertex;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_indexed = *draw;
    command->value.draw_indexed.struct_size =
        sizeof(command->value.draw_indexed);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    if (vertex_buffer != NULL)
        vertex_buffer->value.buffer.reference_count++;
    index_buffer->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_indexed(RinGpuCore* core,
                                RinGpuHandle command_list,
                                const RinGpuDrawIndexedV1* draw) {
    return ringpu_command_draw_indexed_internal(
        core, command_list, draw, 0,
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED);
}

int ringpu_command_draw_indexed_v2(RinGpuCore* core,
                                   RinGpuHandle command_list,
                                   const RinGpuDrawIndexedV2* draw)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* index_buffer;
    RinGpuObjectSlot* vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    uint64_t index_end;
    uint64_t index_bytes;
    uint32_t index_stride;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!draw || !ringpu_versioned(draw->abi_version, draw->struct_size,
                                   sizeof(*draw)) ||
        draw->index_count == 0u || draw->vertex_count == 0u ||
        draw->instance_count == 0u || draw->flags != 0u ||
        draw->reserved0 != 0u || draw->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    index_stride = ringpu_index_format_bytes(draw->index_format);
    if (index_stride == 0u ||
        (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->index_count > RIN_GPU_MAX_DRAW_INDICES ||
        draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_index > UINT32_MAX - draw->index_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u ||
        list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, draw->index_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &index_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((index_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDEX) == 0u ||
        !ringpu_buffer_upload_ready(index_buffer)) {
        return RIN_GPU_ERROR_STATE;
    }
    index_end = (uint64_t)draw->first_index + draw->index_count;
    if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
        draw->index_offset > index_buffer->value.buffer.size_bytes ||
        index_bytes > index_buffer->value.buffer.size_bytes - draw->index_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_vertex_bindings_for_draw(
        core, pipeline, draw->vertex_buffers, draw->binding_count,
        0u, draw->vertex_count, draw->first_instance, draw->instance_count,
        vertex_buffers);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_indexed_v2 = *draw;
    command->value.draw_indexed_v2.struct_size =
        sizeof(command->value.draw_indexed_v2);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    index_buffer->value.buffer.reference_count++;
    for (uint32_t index = 0u; index < draw->binding_count; ++index)
        vertex_buffers[index]->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_indexed_base_vertex(
        RinGpuCore* core, RinGpuHandle command_list,
        const RinGpuDrawIndexedBaseVertexV1* draw) {
    RinGpuDrawIndexedV1 canonical;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(&canonical, 0, sizeof(canonical));
    canonical.abi_version = draw->abi_version;
    canonical.struct_size = sizeof(canonical);
    canonical.pipeline = draw->pipeline;
    canonical.color_target = draw->color_target;
    canonical.vertex_buffer = draw->vertex_buffer;
    canonical.index_buffer = draw->index_buffer;
    canonical.vertex_offset = draw->vertex_offset;
    canonical.index_offset = draw->index_offset;
    canonical.index_format = draw->index_format;
    canonical.mip_level = draw->mip_level;
    canonical.array_layer = draw->array_layer;
    canonical.index_count = draw->index_count;
    canonical.instance_count = draw->instance_count;
    canonical.first_index = draw->first_index;
    canonical.vertex_count = draw->vertex_count;
    canonical.first_instance = draw->first_instance;
    canonical.flags = draw->flags;
    return ringpu_command_draw_indexed_internal(
        core, command_list, &canonical, draw->base_vertex,
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX);
}

static int ringpu_indirect_range_valid(const RinGpuObjectSlot* buffer,
                                       uint64_t offset, uint32_t draw_count,
                                       uint32_t stride, uint32_t packet_size)
{
    uint64_t span;
    if (!buffer || draw_count == 0u || draw_count > RIN_GPU_MAX_INDIRECT_COMMANDS ||
        stride < packet_size || (stride & UINT32_C(3)) != 0u ||
        (offset & UINT64_C(3)) != 0u ||
        !ringpu_multiply_u64((uint64_t)(draw_count - 1u), stride, &span) ||
        span > UINT64_MAX - packet_size ||
        offset > buffer->value.buffer.size_bytes ||
        span + packet_size > buffer->value.buffer.size_bytes - offset) {
        return 0;
    }
    return 1;
}

static int ringpu_indirect_graphics_target(
    RinGpuCore* core, RinGpuHandle command_list, RinGpuHandle pipeline_handle,
    RinGpuHandle color_target_handle, uint32_t mip_level,
    uint32_t array_layer, RinGpuObjectSlot** list_out,
    RinGpuObjectSlot** pipeline_out, RinGpuObjectSlot** color_target_out)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    const RinGpuImageDescV1* target_desc;
    int result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST,
                             NULL, &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u ||
        list->value.command_list.render_target != color_target_handle ||
        list->value.command_list.render_mip_level != mip_level ||
        list->value.command_list.render_array_layer != array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, pipeline_handle,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, color_target_handle, RIN_GPU_OBJECT_IMAGE,
                         NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (mip_level >= target_desc->mip_levels ||
        array_layer >= target_desc->array_layers ||
        (target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *list_out = list;
    *pipeline_out = pipeline;
    *color_target_out = color_target;
    return RIN_GPU_OK;
}

int ringpu_command_draw_indirect(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawIndirectV1* draw)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* indirect_buffer;
    RinGpuObjectSlot* vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    RinGpuHandle bind_group_handle;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw || !ringpu_versioned(draw->abi_version, draw->struct_size,
                                   sizeof(*draw)) || draw->flags != 0u ||
        draw->reserved0 != 0u || draw->reserved1 != 0u ||
        draw->binding_count > RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_indirect_graphics_target(
        core, command_list, draw->pipeline, draw->color_target,
        draw->mip_level, draw->array_layer, &list, &pipeline, &color_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, draw->indirect_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &indirect_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((indirect_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDIRECT) == 0u ||
        !ringpu_buffer_upload_ready(indirect_buffer) ||
        !ringpu_indirect_range_valid(indirect_buffer, draw->indirect_offset,
                                     draw->draw_count, draw->stride, 16u)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_vertex_bindings_for_draw(
        core, pipeline, draw->vertex_buffers, draw->binding_count,
        0u, 1u, 0u, 1u, vertex_buffers);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle, &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle, list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_INDIRECT;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->auxiliary = draw->indirect_buffer;
    command->resources = bind_group_handle;
    command->value.draw_indirect = *draw;
    command->value.draw_indirect.struct_size = sizeof(command->value.draw_indirect);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    indirect_buffer->value.buffer.reference_count++;
    for (uint32_t binding = 0u; binding < draw->binding_count; ++binding)
        vertex_buffers[binding]->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_indexed_indirect(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawIndexedIndirectV1* draw)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* index_buffer;
    RinGpuObjectSlot* indirect_buffer;
    RinGpuObjectSlot* vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    RinGpuHandle bind_group_handle;
    uint32_t index_stride;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw || !ringpu_versioned(draw->abi_version, draw->struct_size,
                                   sizeof(*draw)) || draw->flags != 0u ||
        draw->reserved0 != 0u || draw->reserved1 != 0u ||
        draw->binding_count > RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS ||
        draw->vertex_count == 0u || draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    index_stride = ringpu_index_format_bytes(draw->index_format);
    if (index_stride == 0u ||
        (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_indirect_graphics_target(
        core, command_list, draw->pipeline, draw->color_target,
        draw->mip_level, draw->array_layer, &list, &pipeline, &color_target);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = ringpu_slot(core, draw->index_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &index_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((index_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDEX) == 0u ||
        !ringpu_buffer_upload_ready(index_buffer) ||
        draw->index_offset > index_buffer->value.buffer.size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, draw->indirect_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &indirect_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((indirect_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDIRECT) == 0u ||
        !ringpu_buffer_upload_ready(indirect_buffer) ||
        !ringpu_indirect_range_valid(indirect_buffer, draw->indirect_offset,
                                     draw->draw_count, draw->stride, 20u)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_vertex_bindings_for_draw(
        core, pipeline, draw->vertex_buffers, draw->binding_count,
        0u, 1u, 0u, 1u, vertex_buffers);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle, &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle, list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_INDIRECT;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->auxiliary = draw->indirect_buffer;
    command->resources = bind_group_handle;
    command->value.draw_indexed_indirect = *draw;
    command->value.draw_indexed_indirect.struct_size =
        sizeof(command->value.draw_indexed_indirect);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    index_buffer->value.buffer.reference_count++;
    indirect_buffer->value.buffer.reference_count++;
    for (uint32_t binding = 0u; binding < draw->binding_count; ++binding)
        vertex_buffers[binding]->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_end_render_pass(RinGpuCore* core,
                                   RinGpuHandle command_list) {
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
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
    command->type = RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS;
    list->value.command_list.count++;
    ringpu_release_graphics_bind_group_reference(
        core, list->value.command_list.graphics_bind_group);
    list->value.command_list.graphics_bind_group = 0u;
    list->value.command_list.render_target = 0u;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_depth_target = 0u;
    list->value.command_list.render_stencil_target = 0u;
    list->value.command_list.render_mip_level = 0u;
    list->value.command_list.render_array_layer = 0u;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.active_color_mask = 0u;
    list->value.command_list.render_depth_mip_level = 0u;
    list->value.command_list.render_depth_array_layer = 0u;
    list->value.command_list.render_stencil_mip_level = 0u;
    list->value.command_list.render_stencil_array_layer = 0u;
    list->value.command_list.render_pass_active = 0u;
    return RIN_GPU_OK;
}
