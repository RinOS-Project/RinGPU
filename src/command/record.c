// SPDX-License-Identifier: MIT
#include "record.h"
#include "../core/object_table.h"

#include <stdlib.h>
#include <string.h>

int ringpu_record_command(RinGpuObjectSlot* list,
                          RinGpuRecordedCommand** command_out)
{
    RinGpuRecordedCommand* commands;
    uint32_t next_capacity;

    if (!list || !command_out) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *command_out = NULL;
    if (list->value.command_list.count == RIN_GPU_CORE_MAX_COMMANDS)
        return RIN_GPU_ERROR_LIMIT;
    if (list->value.command_list.count == list->value.command_list.capacity) {
        next_capacity = list->value.command_list.capacity == 0u ? 8u :
                        list->value.command_list.capacity * 2u;
        if (next_capacity > RIN_GPU_CORE_MAX_COMMANDS)
            next_capacity = RIN_GPU_CORE_MAX_COMMANDS;
        commands = (RinGpuRecordedCommand*)realloc(
            list->value.command_list.commands,
            (size_t)next_capacity * sizeof(*commands));
        if (!commands) return RIN_GPU_ERROR_NO_MEMORY;
        list->value.command_list.commands = commands;
        list->value.command_list.capacity = next_capacity;
    }
    *command_out = &list->value.command_list.commands[
        list->value.command_list.count];
    memset(*command_out, 0, sizeof(**command_out));
    return RIN_GPU_OK;
}

static void release_reference(RinGpuCore* core, RinGpuHandle handle,
                              uint16_t type)
{
    RinGpuObjectSlot* slot;
    uint32_t* references = NULL;

    if (!core || handle == 0u ||
        ringpu_slot(core, handle, type, NULL, &slot) != RIN_GPU_OK) {
        return;
    }
    switch (type) {
    case RIN_GPU_OBJECT_BUFFER:
        references = &slot->value.buffer.reference_count;
        break;
    case RIN_GPU_OBJECT_IMAGE:
        references = &slot->value.image.reference_count;
        break;
    case RIN_GPU_OBJECT_COMPUTE_PIPELINE:
        references = &slot->value.compute_pipeline.reference_count;
        break;
    case RIN_GPU_OBJECT_GRAPHICS_PIPELINE:
        references = &slot->value.graphics_pipeline.reference_count;
        break;
    case RIN_GPU_OBJECT_COMPUTE_BIND_GROUP:
        references = &slot->value.compute_bind_group.reference_count;
        break;
    case RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP:
        references = &slot->value.graphics_bind_group.reference_count;
        break;
    default:
        break;
    }
    if (references != NULL && *references != 0u) (*references)--;
}

static void release_graphics_bind_group_reference(RinGpuCore* core,
                                                   RinGpuHandle bind_group)
{
    release_reference(core, bind_group, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP);
}

void ringpu_release_command_references(RinGpuCore* core,
                                        RinGpuObjectSlot* list)
{
    if (!core || !list) return;
    for (uint32_t index = 0u; index < list->value.command_list.count; ++index) {
        RinGpuRecordedCommand* command =
            &list->value.command_list.commands[index];
        switch (command->type) {
        case RIN_GPU_BACKEND_COMMAND_DISPATCH:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_COMPUTE_PIPELINE);
            release_reference(core, command->source,
                              RIN_GPU_OBJECT_COMPUTE_BIND_GROUP);
            break;
        case RIN_GPU_BACKEND_COMMAND_DISPATCH_INDIRECT:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_COMPUTE_PIPELINE);
            release_reference(core, command->source,
                              RIN_GPU_OBJECT_COMPUTE_BIND_GROUP);
            release_reference(core, command->auxiliary, RIN_GPU_OBJECT_BUFFER);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->value.draw_vertices.vertex_buffer,
                              RIN_GPU_OBJECT_BUFFER);
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            for (uint32_t binding = 0u;
                 binding < command->value.draw_vertices_v2.binding_count;
                 ++binding) {
                release_reference(
                    core,
                    command->value.draw_vertices_v2.vertex_buffers[binding].buffer,
                    RIN_GPU_OBJECT_BUFFER);
            }
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED:
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->value.draw_indexed.vertex_buffer,
                              RIN_GPU_OBJECT_BUFFER);
            release_reference(core, command->value.draw_indexed.index_buffer,
                              RIN_GPU_OBJECT_BUFFER);
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_reference(
                core, command->value.draw_indexed_v2.index_buffer,
                RIN_GPU_OBJECT_BUFFER);
            for (uint32_t binding = 0u;
                 binding < command->value.draw_indexed_v2.binding_count;
                 ++binding) {
                release_reference(
                    core,
                    command->value.draw_indexed_v2.vertex_buffers[binding].buffer,
                    RIN_GPU_OBJECT_BUFFER);
            }
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDIRECT:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->auxiliary, RIN_GPU_OBJECT_BUFFER);
            for (uint32_t binding = 0u;
                 binding < command->value.draw_indirect.binding_count;
                 ++binding) {
                release_reference(
                    core,
                    command->value.draw_indirect.vertex_buffers[binding].buffer,
                    RIN_GPU_OBJECT_BUFFER);
            }
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_INDIRECT:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_GRAPHICS_PIPELINE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->auxiliary, RIN_GPU_OBJECT_BUFFER);
            release_reference(core, command->value.draw_indexed_indirect.index_buffer,
                              RIN_GPU_OBJECT_BUFFER);
            for (uint32_t binding = 0u;
                 binding < command->value.draw_indexed_indirect.binding_count;
                 ++binding) {
                release_reference(
                    core,
                    command->value.draw_indexed_indirect.vertex_buffers[binding].buffer,
                    RIN_GPU_OBJECT_BUFFER);
            }
            release_graphics_bind_group_reference(core, command->resources);
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT: {
            const RinGpuRenderPassMrtDescV1* pass =
                &command->value.render_pass_mrt;
            for (uint32_t color = 0u; color < RIN_GPU_MAX_COLOR_TARGETS;
                 ++color) {
                if ((pass->active_color_mask & (1u << color)) != 0u)
                    release_reference(core,
                                      pass->color_attachments[color].target,
                                      RIN_GPU_OBJECT_IMAGE);
            }
            release_reference(core, pass->depth_target, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, pass->stencil_target, RIN_GPU_OBJECT_IMAGE);
            break;
        }
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->auxiliary, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_PRESENT:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_COPY_BUFFER:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_BUFFER);
            release_reference(core, command->source, RIN_GPU_OBJECT_BUFFER);
            break;
        case RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER:
            release_reference(core, command->destination,
                              RIN_GPU_OBJECT_BUFFER);
            break;
        case RIN_GPU_BACKEND_COMMAND_COPY_IMAGE:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_RESOLVE_IMAGE:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            release_reference(core, command->source, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            break;
        case RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP:
            release_reference(core, command->destination, RIN_GPU_OBJECT_IMAGE);
            break;
        default:
            break;
        }
    }
    release_graphics_bind_group_reference(
        core, list->value.command_list.graphics_bind_group);
    list->value.command_list.count = 0u;
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
    list->value.command_list.graphics_bind_group = 0u;
}
