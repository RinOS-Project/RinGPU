// SPDX-License-Identifier: MIT
#include "render_pass.h"

#include "../command/record.h"
#include "../core/object_table.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"

#include <string.h>

int ringpu_render_color_clear_valid(uint32_t load_op, float red,
                                            float green, float blue,
                                            float alpha,
                                            uint32_t color_write_mask) {
    if ((color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u)
        return 0;
    if (load_op == RIN_GPU_RENDER_LOAD) {
        return red == 0.0f && green == 0.0f && blue == 0.0f &&
               alpha == 0.0f && color_write_mask == 0u;
    }
    /* Float color targets retain finite values outside [0, 1]. Fixed-point
     * backends quantize/clamp when they store the pass clear; keeping the
     * command value intact avoids losing Float32 render-target semantics. */
    return load_op == RIN_GPU_RENDER_CLEAR && ringpu_finite_float(red) &&
           ringpu_finite_float(green) && ringpu_finite_float(blue) &&
           ringpu_finite_float(alpha);
}

int ringpu_render_color_clear_valid_for_format(
    uint32_t format, uint32_t load_op, float red, float green, float blue,
    float alpha, uint32_t color_write_mask) {
    if (!ringpu_render_color_clear_valid(load_op, red, green, blue, alpha,
                                         color_write_mask)) {
        return 0;
    }
    return load_op != RIN_GPU_RENDER_CLEAR ||
        format == RIN_GPU_FORMAT_RGBA16_FLOAT ||
        format == RIN_GPU_FORMAT_RGBA32_FLOAT ||
        (red >= 0.0f && red <= 1.0f && green >= 0.0f && green <= 1.0f &&
         blue >= 0.0f && blue <= 1.0f && alpha >= 0.0f && alpha <= 1.0f);
}

int ringpu_clear_region_valid(const RinGpuClearRegionV1* region,
                                     uint32_t width, uint32_t height) {
    if (!region || region->enabled > 1u || region->reserved != 0u)
        return 0;
    if (region->enabled == 0u) {
        return region->x == 0 && region->y == 0 && region->width == 0u &&
               region->height == 0u;
    }
    return region->x >= 0 && region->y >= 0 &&
           (uint32_t)region->x <= width && region->width <= width -
               (uint32_t)region->x &&
           (uint32_t)region->y <= height && region->height <= height -
               (uint32_t)region->y;
}

int ringpu_render_clear_valid(const RinGpuRenderPassDescV1* pass) {
    return pass && ringpu_render_color_clear_valid(
        pass->load_op, pass->clear_red, pass->clear_green,
        pass->clear_blue, pass->clear_alpha, pass->color_write_mask);
}

int ringpu_render_depth_clear_valid(uint32_t load_op, float depth) {
    if (load_op == RIN_GPU_RENDER_LOAD) return depth == 0.0f;
    return load_op == RIN_GPU_RENDER_CLEAR && depth >= 0.0f && depth <= 1.0f;
}

int ringpu_render_stencil_clear_valid(uint32_t format,
                                             uint32_t load_op,
                                             uint32_t store_op,
                                             uint32_t clear_stencil,
                                             uint32_t write_mask) {
    if (format == RIN_GPU_FORMAT_D32_FLOAT) {
        return load_op == 0u && store_op == 0u && clear_stencil == 0u &&
               write_mask == 0u;
    }
    if ((format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
         format != RIN_GPU_FORMAT_S8_UINT) ||
        store_op != RIN_GPU_RENDER_STORE || clear_stencil > 0xffu ||
        write_mask > 0xffu) {
        return 0;
    }
    return load_op == RIN_GPU_RENDER_CLEAR ||
           (load_op == RIN_GPU_RENDER_LOAD && clear_stencil == 0u &&
            write_mask == 0u);
}

uint32_t ringpu_mip_dimension(uint32_t dimension,
                                     uint32_t mip_level) {
    dimension >>= mip_level;
    return dimension == 0u ? 1u : dimension;
}

int ringpu_command_begin_render_pass(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDescV1* render_pass) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* color_target;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!render_pass ||
        !ringpu_versioned(render_pass->abi_version,
                          render_pass->struct_size, sizeof(*render_pass)) ||
        render_pass->store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_clear_valid(render_pass) ||
        render_pass->flags != 0u || render_pass->reserved != 0u ||
        render_pass->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, render_pass->color_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (render_pass->mip_level >= target_desc->mip_levels ||
        render_pass->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        !ringpu_color_format(target_desc->format) ||
        !ringpu_render_color_clear_valid_for_format(
            target_desc->format, render_pass->load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(target_desc->width, render_pass->mip_level),
            ringpu_mip_dimension(target_desc->height, render_pass->mip_level))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS;
    command->destination = render_pass->color_target;
    command->value.render_pass = *render_pass;
    command->value.render_pass.struct_size =
        sizeof(command->value.render_pass);
    list->value.command_list.count++;
    list->value.command_list.render_target = render_pass->color_target;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_color_targets[0] = render_pass->color_target;
    list->value.command_list.render_depth_target = 0u;
    list->value.command_list.render_stencil_target = 0u;
    list->value.command_list.render_mip_level = render_pass->mip_level;
    list->value.command_list.render_array_layer = render_pass->array_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.render_color_mip_levels[0] = render_pass->mip_level;
    list->value.command_list.render_color_array_layers[0] = render_pass->array_layer;
    list->value.command_list.active_color_mask = 1u;
    list->value.command_list.render_depth_mip_level = 0u;
    list->value.command_list.render_depth_array_layer = 0u;
    list->value.command_list.render_stencil_mip_level = 0u;
    list->value.command_list.render_stencil_array_layer = 0u;
    list->value.command_list.render_pass_active = 1u;
    color_target->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_begin_render_pass_mrt(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassMrtDescV1* render_pass)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* first_color = NULL;
    RinGpuObjectSlot* depth_target = NULL;
    RinGpuObjectSlot* stencil_target = NULL;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* first_desc = NULL;
    RinGpuHandle first_handle = 0u;
    uint32_t first_mip = 0u;
    uint32_t first_layer = 0u;
    uint32_t color_index;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK)
        return result;
    if (render_pass == NULL ||
        !ringpu_versioned(render_pass->abi_version, render_pass->struct_size,
                          sizeof(*render_pass)) ||
        render_pass->active_color_mask == 0u ||
        (render_pass->active_color_mask & ~((1u << RIN_GPU_MAX_COLOR_TARGETS) - 1u)) != 0u ||
        render_pass->color_store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_color_clear_valid(
            render_pass->color_load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        render_pass->flags != 0u || render_pass->reserved0 != 0u ||
        render_pass->reserved1 != 0u || render_pass->reserved2 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST,
                         NULL, &list);
    if (result != RIN_GPU_OK)
        return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }

    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        const RinGpuColorAttachmentV1* attachment =
            &render_pass->color_attachments[color_index];
        RinGpuObjectSlot* color_target;
        const RinGpuImageDescV1* color_desc;

        if ((render_pass->active_color_mask & (1u << color_index)) == 0u) {
            if (attachment->target != 0u || attachment->mip_level != 0u ||
                attachment->array_layer != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            continue;
        }
        result = ringpu_slot(core, attachment->target, RIN_GPU_OBJECT_IMAGE,
                             NULL, &color_target);
        if (result != RIN_GPU_OK)
            return result;
        color_desc = &color_target->value.image.descriptor;
        if (attachment->mip_level >= color_desc->mip_levels ||
            attachment->array_layer >= color_desc->array_layers ||
            (color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
            color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
            color_desc->sample_count != 1u || !ringpu_color_format(color_desc->format) ||
            !ringpu_render_color_clear_valid_for_format(
                color_desc->format, render_pass->color_load_op,
                render_pass->clear_red, render_pass->clear_green,
                render_pass->clear_blue, render_pass->clear_alpha,
                render_pass->color_write_mask)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        if (first_color == NULL) {
            first_color = color_target;
            first_desc = color_desc;
            first_handle = attachment->target;
            first_mip = attachment->mip_level;
            first_layer = attachment->array_layer;
        } else if (color_desc->format != first_desc->format ||
                   ringpu_mip_dimension(color_desc->width,
                                        attachment->mip_level) !=
                       ringpu_mip_dimension(first_desc->width, first_mip) ||
                   ringpu_mip_dimension(color_desc->height,
                                        attachment->mip_level) !=
                       ringpu_mip_dimension(first_desc->height, first_mip)) {
            /* The current graphics-pipeline ABI has one color format. Do not
             * pretend mixed-format MRT is executable until that ABI grows the
             * corresponding per-target pipeline description. */
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
        for (uint32_t previous = 0u; previous < color_index; ++previous) {
            const RinGpuColorAttachmentV1* prior =
                &render_pass->color_attachments[previous];

            if ((render_pass->active_color_mask & (1u << previous)) != 0u &&
                prior->target == attachment->target &&
                prior->mip_level == attachment->mip_level &&
                prior->array_layer == attachment->array_layer) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (first_color == NULL || first_desc == NULL ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(first_desc->width, first_mip),
            ringpu_mip_dimension(first_desc->height, first_mip))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }

    if (render_pass->depth_target == 0u) {
        if (render_pass->depth_mip_level != 0u ||
            render_pass->depth_array_layer != 0u ||
            render_pass->depth_load_op != 0u || render_pass->depth_store_op != 0u ||
            render_pass->clear_depth != 0.0f) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else {
        const RinGpuImageDescV1* depth_desc;

        result = ringpu_slot(core, render_pass->depth_target,
                             RIN_GPU_OBJECT_IMAGE, NULL, &depth_target);
        if (result != RIN_GPU_OK)
            return result;
        depth_desc = &depth_target->value.image.descriptor;
        if (render_pass->depth_store_op != RIN_GPU_RENDER_STORE ||
            !ringpu_render_depth_clear_valid(render_pass->depth_load_op,
                                             render_pass->clear_depth) ||
            render_pass->depth_mip_level >= depth_desc->mip_levels ||
            render_pass->depth_array_layer >= depth_desc->array_layers ||
            (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
            depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
            depth_desc->sample_count != 1u ||
            !ringpu_depth_stencil_format(depth_desc->format) ||
            (depth_desc->format == RIN_GPU_FORMAT_S8_UINT &&
             (render_pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
              render_pass->clear_depth != 0.0f)) ||
            ringpu_mip_dimension(depth_desc->width, render_pass->depth_mip_level) !=
                ringpu_mip_dimension(first_desc->width, first_mip) ||
            ringpu_mip_dimension(depth_desc->height, render_pass->depth_mip_level) !=
                ringpu_mip_dimension(first_desc->height, first_mip)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }
    if (render_pass->stencil_target == 0u) {
        if (render_pass->stencil_mip_level != 0u ||
            render_pass->stencil_array_layer != 0u ||
            render_pass->stencil_load_op != 0u ||
            render_pass->stencil_store_op != 0u || render_pass->clear_stencil != 0u ||
            render_pass->stencil_write_mask != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else {
        const RinGpuImageDescV1* stencil_desc;

        result = ringpu_slot(core, render_pass->stencil_target,
                             RIN_GPU_OBJECT_IMAGE, NULL, &stencil_target);
        if (result != RIN_GPU_OK)
            return result;
        stencil_desc = &stencil_target->value.image.descriptor;
        if (!ringpu_render_stencil_clear_valid(
                stencil_desc->format, render_pass->stencil_load_op,
                render_pass->stencil_store_op, render_pass->clear_stencil,
                render_pass->stencil_write_mask) ||
            render_pass->stencil_mip_level >= stencil_desc->mip_levels ||
            render_pass->stencil_array_layer >= stencil_desc->array_layers ||
            (stencil_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
            stencil_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
            stencil_desc->sample_count != 1u ||
            !ringpu_depth_stencil_format(stencil_desc->format) ||
            ringpu_mip_dimension(stencil_desc->width,
                                 render_pass->stencil_mip_level) !=
                ringpu_mip_dimension(first_desc->width, first_mip) ||
            ringpu_mip_dimension(stencil_desc->height,
                                 render_pass->stencil_mip_level) !=
                ringpu_mip_dimension(first_desc->height, first_mip)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }

    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK)
        return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT;
    command->destination = first_handle;
    command->source = render_pass->depth_target;
    command->auxiliary = render_pass->stencil_target;
    command->value.render_pass_mrt = *render_pass;
    command->value.render_pass_mrt.struct_size =
        sizeof(command->value.render_pass_mrt);
    list->value.command_list.count++;
    list->value.command_list.render_target = first_handle;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_depth_target = render_pass->depth_target;
    list->value.command_list.render_stencil_target = render_pass->stencil_target;
    list->value.command_list.render_mip_level = first_mip;
    list->value.command_list.render_array_layer = first_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.active_color_mask = render_pass->active_color_mask;
    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        if ((render_pass->active_color_mask & (1u << color_index)) == 0u)
            continue;
        list->value.command_list.render_color_targets[color_index] =
            render_pass->color_attachments[color_index].target;
        list->value.command_list.render_color_mip_levels[color_index] =
            render_pass->color_attachments[color_index].mip_level;
        list->value.command_list.render_color_array_layers[color_index] =
            render_pass->color_attachments[color_index].array_layer;
    }
    list->value.command_list.render_depth_mip_level = render_pass->depth_mip_level;
    list->value.command_list.render_depth_array_layer = render_pass->depth_array_layer;
    list->value.command_list.render_stencil_mip_level = render_pass->stencil_mip_level;
    list->value.command_list.render_stencil_array_layer = render_pass->stencil_array_layer;
    list->value.command_list.render_pass_active = 1u;
    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        if ((render_pass->active_color_mask & (1u << color_index)) == 0u)
            continue;
        result = ringpu_slot(core,
                             render_pass->color_attachments[color_index].target,
                             RIN_GPU_OBJECT_IMAGE, NULL, &first_color);
        if (result != RIN_GPU_OK)
            return RIN_GPU_ERROR_STATE;
        first_color->value.image.reference_count++;
    }
    if (depth_target != NULL)
        depth_target->value.image.reference_count++;
    if (stencil_target != NULL)
        stencil_target->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_begin_render_pass_depth(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDepthDescV1* render_pass) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* depth_target;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* color_desc;
    const RinGpuImageDescV1* depth_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!render_pass ||
        !ringpu_versioned(render_pass->abi_version,
                          render_pass->struct_size, sizeof(*render_pass)) ||
        render_pass->color_store_op != RIN_GPU_RENDER_STORE ||
        render_pass->depth_store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_color_clear_valid(
            render_pass->color_load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        !ringpu_render_depth_clear_valid(
            render_pass->depth_load_op, render_pass->clear_depth) ||
        render_pass->flags != 0u || render_pass->reserved0 != 0u ||
        render_pass->reserved1 != 0u || render_pass->reserved2 != 0u ||
        render_pass->reserved3 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, render_pass->color_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, render_pass->depth_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &depth_target);
    if (result != RIN_GPU_OK) return result;
    color_desc = &color_target->value.image.descriptor;
    depth_desc = &depth_target->value.image.descriptor;
    if (render_pass->color_mip_level >= color_desc->mip_levels ||
        render_pass->color_array_layer >= color_desc->array_layers ||
        render_pass->depth_mip_level >= depth_desc->mip_levels ||
        render_pass->depth_array_layer >= depth_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        color_desc->sample_count != 1u ||
        !ringpu_color_format(color_desc->format) ||
        !ringpu_render_color_clear_valid_for_format(
            color_desc->format, render_pass->color_load_op,
            render_pass->clear_red, render_pass->clear_green,
            render_pass->clear_blue, render_pass->clear_alpha,
            render_pass->color_write_mask) ||
        (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
        depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        depth_desc->sample_count != 1u ||
        !ringpu_depth_stencil_format(depth_desc->format) ||
        (depth_desc->format == RIN_GPU_FORMAT_S8_UINT &&
         (render_pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
          render_pass->clear_depth != 0.0f)) ||
        !ringpu_render_stencil_clear_valid(
            depth_desc->format, render_pass->stencil_load_op,
            render_pass->stencil_store_op, render_pass->clear_stencil,
            render_pass->stencil_write_mask) ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(color_desc->width,
                                 render_pass->color_mip_level),
            ringpu_mip_dimension(color_desc->height,
                                 render_pass->color_mip_level)) ||
        ringpu_mip_dimension(color_desc->width,
                             render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->width,
                                 render_pass->depth_mip_level) ||
        ringpu_mip_dimension(color_desc->height,
                             render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->height,
                                 render_pass->depth_mip_level)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH;
    command->destination = render_pass->color_target;
    command->source = render_pass->depth_target;
    command->value.render_pass_depth = *render_pass;
    command->value.render_pass_depth.struct_size =
        sizeof(command->value.render_pass_depth);
    list->value.command_list.count++;
    list->value.command_list.render_target = render_pass->color_target;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_color_targets[0] = render_pass->color_target;
    list->value.command_list.render_depth_target = render_pass->depth_target;
    list->value.command_list.render_stencil_target = 0u;
    list->value.command_list.render_mip_level =
        render_pass->color_mip_level;
    list->value.command_list.render_array_layer =
        render_pass->color_array_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.render_color_mip_levels[0] =
        render_pass->color_mip_level;
    list->value.command_list.render_color_array_layers[0] =
        render_pass->color_array_layer;
    list->value.command_list.active_color_mask = 1u;
    list->value.command_list.render_depth_mip_level =
        render_pass->depth_mip_level;
    list->value.command_list.render_depth_array_layer =
        render_pass->depth_array_layer;
    list->value.command_list.render_stencil_mip_level = 0u;
    list->value.command_list.render_stencil_array_layer = 0u;
    list->value.command_list.render_pass_active = 1u;
    color_target->value.image.reference_count++;
    depth_target->value.image.reference_count++;
    return RIN_GPU_OK;
}


int ringpu_command_begin_render_pass_depth_stencil(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDepthStencilDescV1* render_pass) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* depth_target;
    RinGpuObjectSlot* stencil_target;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* color_desc;
    const RinGpuImageDescV1* depth_desc;
    const RinGpuImageDescV1* stencil_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!render_pass ||
        !ringpu_versioned(render_pass->abi_version, render_pass->struct_size,
                          sizeof(*render_pass)) ||
        render_pass->color_store_op != RIN_GPU_RENDER_STORE ||
        render_pass->depth_store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_color_clear_valid(
            render_pass->color_load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        !ringpu_render_depth_clear_valid(render_pass->depth_load_op,
                                         render_pass->clear_depth) ||
        !ringpu_render_stencil_clear_valid(
            RIN_GPU_FORMAT_S8_UINT, render_pass->stencil_load_op,
            render_pass->stencil_store_op, render_pass->clear_stencil,
            render_pass->stencil_write_mask) ||
        render_pass->flags != 0u || render_pass->reserved0 != 0u ||
        render_pass->reserved1 != 0u || render_pass->reserved2 != 0u ||
        render_pass->reserved3 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (render_pass->color_target == render_pass->depth_target ||
        render_pass->color_target == render_pass->stencil_target ||
        render_pass->depth_target == render_pass->stencil_target) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, render_pass->color_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, render_pass->depth_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &depth_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, render_pass->stencil_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &stencil_target);
    if (result != RIN_GPU_OK) return result;
    color_desc = &color_target->value.image.descriptor;
    depth_desc = &depth_target->value.image.descriptor;
    stencil_desc = &stencil_target->value.image.descriptor;
    if (render_pass->color_mip_level >= color_desc->mip_levels ||
        render_pass->color_array_layer >= color_desc->array_layers ||
        render_pass->depth_mip_level >= depth_desc->mip_levels ||
        render_pass->depth_array_layer >= depth_desc->array_layers ||
        render_pass->stencil_mip_level >= stencil_desc->mip_levels ||
        render_pass->stencil_array_layer >= stencil_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        color_desc->sample_count != 1u || !ringpu_color_format(color_desc->format) ||
        !ringpu_render_color_clear_valid_for_format(
            color_desc->format, render_pass->color_load_op,
            render_pass->clear_red, render_pass->clear_green,
            render_pass->clear_blue, render_pass->clear_alpha,
            render_pass->color_write_mask) ||
        (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
        depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        depth_desc->sample_count != 1u ||
        !ringpu_depth_aspect_format(depth_desc->format) ||
        (stencil_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
        stencil_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        stencil_desc->sample_count != 1u ||
        !ringpu_stencil_aspect_format(stencil_desc->format) ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(color_desc->width,
                                 render_pass->color_mip_level),
            ringpu_mip_dimension(color_desc->height,
                                 render_pass->color_mip_level)) ||
        ringpu_mip_dimension(color_desc->width, render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->width, render_pass->depth_mip_level) ||
        ringpu_mip_dimension(color_desc->height, render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->height, render_pass->depth_mip_level) ||
        ringpu_mip_dimension(color_desc->width, render_pass->color_mip_level) !=
            ringpu_mip_dimension(stencil_desc->width, render_pass->stencil_mip_level) ||
        ringpu_mip_dimension(color_desc->height, render_pass->color_mip_level) !=
            ringpu_mip_dimension(stencil_desc->height, render_pass->stencil_mip_level)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL;
    command->destination = render_pass->color_target;
    command->source = render_pass->depth_target;
    command->auxiliary = render_pass->stencil_target;
    command->value.render_pass_depth_stencil = *render_pass;
    command->value.render_pass_depth_stencil.struct_size =
        sizeof(command->value.render_pass_depth_stencil);
    list->value.command_list.count++;
    list->value.command_list.render_target = render_pass->color_target;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_color_targets[0] = render_pass->color_target;
    list->value.command_list.render_depth_target = render_pass->depth_target;
    list->value.command_list.render_stencil_target = render_pass->stencil_target;
    list->value.command_list.render_mip_level = render_pass->color_mip_level;
    list->value.command_list.render_array_layer = render_pass->color_array_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.render_color_mip_levels[0] =
        render_pass->color_mip_level;
    list->value.command_list.render_color_array_layers[0] =
        render_pass->color_array_layer;
    list->value.command_list.active_color_mask = 1u;
    list->value.command_list.render_depth_mip_level = render_pass->depth_mip_level;
    list->value.command_list.render_depth_array_layer = render_pass->depth_array_layer;
    list->value.command_list.render_stencil_mip_level = render_pass->stencil_mip_level;
    list->value.command_list.render_stencil_array_layer = render_pass->stencil_array_layer;
    list->value.command_list.render_pass_active = 1u;
    color_target->value.image.reference_count++;
    depth_target->value.image.reference_count++;
    stencil_target->value.image.reference_count++;
    return RIN_GPU_OK;
}
