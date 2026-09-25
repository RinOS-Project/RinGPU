// SPDX-License-Identifier: MIT
#include "pipelines.h"

#include "../core/object_table.h"
#include "../validation/graphics_layout.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"
#include "../validation/shader.h"

#include <stdlib.h>
#include <string.h>

static int ringpu_independent_blend_target_valid(
    const RinGpuBlendTargetV1* target)
{
    int enabled;

    if (!target || target->blend_enabled > 1u ||
        (target->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        target->reserved != 0u)
        return 0;
    enabled = target->blend_enabled != 0u;
    if (!enabled)
        return target->source_color_factor == 0u &&
               target->destination_color_factor == 0u &&
               target->color_operation == 0u &&
               target->source_alpha_factor == 0u &&
               target->destination_alpha_factor == 0u &&
               target->alpha_operation == 0u;
    return ringpu_blend_source_factor_v2_valid(target->source_color_factor) &&
           ringpu_blend_factor_v2_valid(target->destination_color_factor) &&
           ringpu_blend_operation_valid(target->color_operation) &&
           ringpu_blend_source_factor_v2_valid(target->source_alpha_factor) &&
           ringpu_blend_factor_v2_valid(target->destination_alpha_factor) &&
           ringpu_blend_operation_valid(target->alpha_operation);
}

static int ringpu_create_graphics_pipeline_internal(
    RinGpuCore* core, RinGpuHandle vertex_shader_handle,
    RinGpuHandle fragment_shader_handle, uint32_t color_format,
    uint32_t primitive_topology, uint32_t depth_format,
    uint32_t depth_compare, uint32_t depth_write_enabled,
    uint32_t stencil_test_enabled, uint32_t stencil_compare,
    uint32_t stencil_reference, uint32_t stencil_read_mask,
    uint32_t stencil_write_mask, uint32_t stencil_fail_operation,
    uint32_t stencil_depth_fail_operation, uint32_t stencil_pass_operation,
    uint32_t blend_enabled, uint32_t source_color_factor,
    uint32_t destination_color_factor, uint32_t color_operation,
    uint32_t source_alpha_factor, uint32_t destination_alpha_factor,
    uint32_t alpha_operation, uint32_t color_write_mask, int explicit_layout,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    uint32_t vertex_stride, const RinGpuVertexAttributeV2* binding_attributes,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, int explicit_varyings,
    uint32_t native_flags,
    uint32_t position_output_location, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, uint32_t cull_mode, uint32_t front_face,
    uint32_t separate_stencil_enabled, uint32_t back_stencil_compare,
    uint32_t back_stencil_reference, uint32_t back_stencil_read_mask,
    uint32_t back_stencil_write_mask, uint32_t back_stencil_fail_operation,
    uint32_t back_stencil_depth_fail_operation,
    uint32_t back_stencil_pass_operation,
    RinGpuHandle* pipeline, const float* blend_constants,
    const RinGpuBlendTargetV1* blend_targets,
    uint32_t blend_target_mask) {
    RinGpuObjectSlot* vertex_shader;
    RinGpuObjectSlot* fragment_shader;
    RinGpuObjectSlot* slot;
    RinGpuBackendGraphicsPipelineDescV1 backend_desc;
    uint32_t vertex_resource_access[RIN_SHADER_MAX_RESOURCES];
    uint32_t fragment_resource_access[RIN_SHADER_MAX_RESOURCES];
    uint32_t resource_access[RIN_SHADER_MAX_RESOURCES];
    uint32_t vertex_resource_kinds[RIN_SHADER_MAX_RESOURCES];
    uint32_t fragment_resource_kinds[RIN_SHADER_MAX_RESOURCES];
    uint32_t resource_kinds[RIN_SHADER_MAX_RESOURCES];
    uint32_t resource_count;
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!pipeline) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *pipeline = 0u;
    result = ringpu_slot(core, vertex_shader_handle,
                         RIN_GPU_OBJECT_SHADER_MODULE, NULL, &vertex_shader);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, fragment_shader_handle,
                         RIN_GPU_OBJECT_SHADER_MODULE, NULL, &fragment_shader);
    if (result != RIN_GPU_OK) return result;
    if (vertex_shader->value.shader_module.info.stage !=
            RIN_SHADER_STAGE_VERTEX ||
        fragment_shader->value.shader_module.info.stage !=
            RIN_SHADER_STAGE_FRAGMENT) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    /* The original graphics profile links positional scalar IO one-for-one
     * and has one fragment color value.  The additive scalar execution
     * profile reserves vertex outputs 0..3 for clip xyzw and forwards 4..7
     * as RGBA to one through four fragment color outputs.  Backends opt in by
     * accepting the supplied immutable shader reflection at pipeline create.
     */
    if ((native_flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        (!explicit_varyings && native_flags != 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!explicit_varyings &&
        !((vertex_shader->value.shader_module.info.output_count ==
              fragment_shader->value.shader_module.info.input_count &&
          fragment_shader->value.shader_module.info.output_count == 1u) ||
          (vertex_shader->value.shader_module.info.output_count == 8u &&
           fragment_shader->value.shader_module.info.input_count == 4u &&
           fragment_shader->value.shader_module.info.output_count >= 4u &&
           fragment_shader->value.shader_module.info.output_count <=
               RIN_GPU_MAX_COLOR_TARGETS * 4u &&
           (fragment_shader->value.shader_module.info.output_count & 3u) ==
               0u))) {
        return RIN_GPU_ERROR_SHADER_INVALID;
    }
    result = ringpu_shader_resource_layout(vertex_shader,
                                           vertex_resource_access,
                                           vertex_resource_kinds);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_shader_resource_layout(fragment_shader,
                                           fragment_resource_access,
                                           fragment_resource_kinds);
    if (result != RIN_GPU_OK) return result;
    resource_count = vertex_shader->value.shader_module.info.resource_count;
    if (fragment_shader->value.shader_module.info.resource_count >
        resource_count) {
        resource_count =
            fragment_shader->value.shader_module.info.resource_count;
    }
    memset(resource_access, 0, sizeof(resource_access));
    memset(resource_kinds, 0, sizeof(resource_kinds));
    for (uint32_t resource = 0u; resource < resource_count; resource++) {
        if (vertex_resource_kinds[resource] != RIN_SHADER_RESOURCE_NONE &&
            fragment_resource_kinds[resource] != RIN_SHADER_RESOURCE_NONE &&
            vertex_resource_kinds[resource] !=
                fragment_resource_kinds[resource]) {
            return RIN_GPU_ERROR_SHADER_INVALID;
        }
        resource_kinds[resource] =
            vertex_resource_kinds[resource] != RIN_SHADER_RESOURCE_NONE
                ? vertex_resource_kinds[resource]
                : fragment_resource_kinds[resource];
        resource_access[resource] = vertex_resource_access[resource] |
                                    fragment_resource_access[resource];
    }
    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.color_format = color_format;
    backend_desc.primitive_topology = primitive_topology;
    backend_desc.flags = native_flags;
    result = binding_attributes != NULL
        ? ringpu_build_vertex_layout_bindings(
              vertex_shader->value.shader_module.info.input_count,
              binding_attributes, attribute_count, vertex_bindings,
              vertex_binding_count, &backend_desc)
        : ringpu_build_vertex_layout(
              vertex_shader->value.shader_module.info.input_count,
              explicit_layout, attributes, attribute_count, vertex_stride,
              &backend_desc);
    if (result != RIN_GPU_OK) return result;
    if (explicit_varyings) {
        result = ringpu_build_varying_layout(
            vertex_shader, fragment_shader, position_output_location,
            native_flags,
            varyings, varying_count, &backend_desc);
        if (result != RIN_GPU_OK) return result;
    }
    backend_desc.depth_format = depth_format;
    backend_desc.depth_compare = depth_compare;
    backend_desc.depth_write_enabled = depth_write_enabled;
    backend_desc.stencil_test_enabled = stencil_test_enabled;
    backend_desc.stencil_compare = stencil_compare;
    backend_desc.stencil_reference = stencil_reference;
    backend_desc.stencil_read_mask = stencil_read_mask;
    backend_desc.stencil_write_mask = stencil_write_mask;
    backend_desc.stencil_fail_operation = stencil_fail_operation;
    backend_desc.stencil_depth_fail_operation = stencil_depth_fail_operation;
    backend_desc.stencil_pass_operation = stencil_pass_operation;
    backend_desc.separate_stencil_enabled = separate_stencil_enabled;
    backend_desc.back_stencil_compare = back_stencil_compare;
    backend_desc.back_stencil_reference = back_stencil_reference;
    backend_desc.back_stencil_read_mask = back_stencil_read_mask;
    backend_desc.back_stencil_write_mask = back_stencil_write_mask;
    backend_desc.back_stencil_fail_operation = back_stencil_fail_operation;
    backend_desc.back_stencil_depth_fail_operation =
        back_stencil_depth_fail_operation;
    backend_desc.back_stencil_pass_operation = back_stencil_pass_operation;
    backend_desc.resource_count = resource_count;
    memcpy(backend_desc.resource_kinds, resource_kinds,
           sizeof(resource_kinds));
    backend_desc.blend_enabled = blend_enabled;
    backend_desc.source_color_factor = source_color_factor;
    backend_desc.destination_color_factor = destination_color_factor;
    backend_desc.color_operation = color_operation;
    backend_desc.source_alpha_factor = source_alpha_factor;
    backend_desc.destination_alpha_factor = destination_alpha_factor;
    backend_desc.alpha_operation = alpha_operation;
    if (blend_constants != NULL) {
        backend_desc.blend_constant_red = blend_constants[0];
        backend_desc.blend_constant_green = blend_constants[1];
        backend_desc.blend_constant_blue = blend_constants[2];
        backend_desc.blend_constant_alpha = blend_constants[3];
    }
    backend_desc.color_write_mask = color_write_mask;
    backend_desc.cull_mode = cull_mode;
    backend_desc.front_face = front_face;
    if (blend_targets != NULL) {
        backend_desc.independent_blend_enabled = 1u;
        backend_desc.independent_blend_mask = blend_target_mask;
        memcpy(backend_desc.blend_targets, blend_targets,
               sizeof(backend_desc.blend_targets));
    }
    result = core->backend.create_graphics_pipeline(
        core->backend_context,
        vertex_shader->value.shader_module.backend_cookie,
        &vertex_shader->value.shader_module.info,
        fragment_shader->value.shader_module.backend_cookie,
        &fragment_shader->value.shader_module.info, &backend_desc, &cookie);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_GRAPHICS_PIPELINE,
                             pipeline, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_graphics_pipeline(core->backend_context, cookie);
        *pipeline = 0u;
        return result;
    }
    slot->value.graphics_pipeline.vertex_shader = vertex_shader_handle;
    slot->value.graphics_pipeline.fragment_shader = fragment_shader_handle;
    slot->value.graphics_pipeline.backend_cookie = cookie;
    slot->value.graphics_pipeline.color_format = color_format;
    slot->value.graphics_pipeline.primitive_topology =
        primitive_topology;
    slot->value.graphics_pipeline.vertex_input_count =
        backend_desc.vertex_input_count;
    slot->value.graphics_pipeline.vertex_stride = backend_desc.vertex_stride;
    slot->value.graphics_pipeline.vertex_binding_count =
        backend_desc.vertex_binding_count;
    memcpy(slot->value.graphics_pipeline.vertex_bindings,
           backend_desc.vertex_bindings,
           sizeof(slot->value.graphics_pipeline.vertex_bindings));
    slot->value.graphics_pipeline.depth_format = depth_format;
    slot->value.graphics_pipeline.depth_compare = depth_compare;
    slot->value.graphics_pipeline.depth_write_enabled = depth_write_enabled;
    slot->value.graphics_pipeline.stencil_test_enabled = stencil_test_enabled;
    slot->value.graphics_pipeline.stencil_compare = stencil_compare;
    slot->value.graphics_pipeline.stencil_reference = stencil_reference;
    slot->value.graphics_pipeline.stencil_read_mask = stencil_read_mask;
    slot->value.graphics_pipeline.stencil_write_mask = stencil_write_mask;
    slot->value.graphics_pipeline.stencil_fail_operation =
        stencil_fail_operation;
    slot->value.graphics_pipeline.stencil_depth_fail_operation =
        stencil_depth_fail_operation;
    slot->value.graphics_pipeline.stencil_pass_operation =
        stencil_pass_operation;
    slot->value.graphics_pipeline.separate_stencil_enabled =
        separate_stencil_enabled;
    slot->value.graphics_pipeline.back_stencil_compare = back_stencil_compare;
    slot->value.graphics_pipeline.back_stencil_reference =
        back_stencil_reference;
    slot->value.graphics_pipeline.back_stencil_read_mask =
        back_stencil_read_mask;
    slot->value.graphics_pipeline.back_stencil_write_mask =
        back_stencil_write_mask;
    slot->value.graphics_pipeline.back_stencil_fail_operation =
        back_stencil_fail_operation;
    slot->value.graphics_pipeline.back_stencil_depth_fail_operation =
        back_stencil_depth_fail_operation;
    slot->value.graphics_pipeline.back_stencil_pass_operation =
        back_stencil_pass_operation;
    slot->value.graphics_pipeline.blend_enabled = blend_enabled;
    slot->value.graphics_pipeline.source_color_factor = source_color_factor;
    slot->value.graphics_pipeline.destination_color_factor =
        destination_color_factor;
    slot->value.graphics_pipeline.color_operation = color_operation;
    slot->value.graphics_pipeline.source_alpha_factor = source_alpha_factor;
    slot->value.graphics_pipeline.destination_alpha_factor =
        destination_alpha_factor;
    slot->value.graphics_pipeline.alpha_operation = alpha_operation;
    if (blend_constants != NULL) {
        slot->value.graphics_pipeline.blend_constant_red = blend_constants[0];
        slot->value.graphics_pipeline.blend_constant_green = blend_constants[1];
        slot->value.graphics_pipeline.blend_constant_blue = blend_constants[2];
        slot->value.graphics_pipeline.blend_constant_alpha = blend_constants[3];
    }
    slot->value.graphics_pipeline.color_write_mask = color_write_mask;
    slot->value.graphics_pipeline.cull_mode = cull_mode;
    slot->value.graphics_pipeline.front_face = front_face;
    slot->value.graphics_pipeline.position_output_location =
        backend_desc.position_output_location;
    slot->value.graphics_pipeline.varying_count = backend_desc.varying_count;
    slot->value.graphics_pipeline.resource_count = resource_count;
    memcpy(slot->value.graphics_pipeline.resource_access, resource_access,
           sizeof(resource_access));
    memcpy(slot->value.graphics_pipeline.resource_kinds, resource_kinds,
           sizeof(resource_kinds));
    vertex_shader->value.shader_module.reference_count++;
    fragment_shader->value.shader_module.reference_count++;
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_GRAPHICS_PIPELINE,
                           resource_count, RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_create_graphics_pipeline(
    RinGpuCore* core, const RinGpuGraphicsPipelineDescV1* desc,
    RinGpuHandle* pipeline) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        desc->flags != 0u || desc->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader, desc->color_format,
        desc->primitive_topology, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u,
        RIN_GPU_COLOR_WRITE_ALL, 0, NULL, 0u, 0u, NULL, NULL, 0u,
        0, 0u, 0u, NULL, 0u,
        RIN_GPU_CULL_NONE, RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_vertex(
    RinGpuCore* core, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    RinGpuHandle* pipeline) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        desc->flags != 0u || desc->reserved0 != 0u ||
        desc->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader,
        desc->color_format, desc->primitive_topology, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u,
        RIN_GPU_COLOR_WRITE_ALL, 1, attributes,
        attribute_count, desc->vertex_stride, NULL, NULL, 0u,
        0, 0u, 0u, NULL, 0u,
        RIN_GPU_CULL_NONE, RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_vertex_bindings(
    RinGpuCore* core, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, RinGpuHandle* pipeline)
{
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        desc->vertex_stride != 0u || desc->flags != 0u ||
        desc->reserved0 != 0u || desc->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader,
        desc->color_format, desc->primitive_topology, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u,
        RIN_GPU_COLOR_WRITE_ALL, 1, NULL, attribute_count, 0u,
        attributes, vertex_bindings, vertex_binding_count,
        0, 0u, 0u, NULL, 0u,
        RIN_GPU_CULL_NONE, RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_depth(
    RinGpuCore* core, const RinGpuGraphicsPipelineDepthDescV1* desc,
    RinGpuHandle* pipeline) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        !ringpu_depth_pipeline_valid(
            desc->depth_format, desc->depth_compare,
            desc->depth_write_enabled, 0) ||
        desc->flags != 0u ||
        desc->reserved0 != 0u || desc->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader, desc->color_format,
        desc->primitive_topology, desc->depth_format, desc->depth_compare,
        desc->depth_write_enabled,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u,
        RIN_GPU_COLOR_WRITE_ALL, 0, NULL, 0u, 0u, NULL, NULL, 0u,
        0, 0u, 0u, NULL, 0u,
        RIN_GPU_CULL_NONE, RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_blend(
    RinGpuCore* core, const RinGpuGraphicsPipelineBlendDescV1* desc,
    RinGpuHandle* pipeline) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        !ringpu_depth_pipeline_valid(
            desc->depth_format, desc->depth_compare,
            desc->depth_write_enabled, 1) ||
        !ringpu_blend_source_factor_valid(desc->source_color_factor) ||
        !ringpu_blend_factor_valid(desc->destination_color_factor) ||
        !ringpu_blend_operation_valid(desc->color_operation) ||
        !ringpu_blend_source_factor_valid(desc->source_alpha_factor) ||
        !ringpu_blend_factor_valid(desc->destination_alpha_factor) ||
        !ringpu_blend_operation_valid(desc->alpha_operation) ||
        (desc->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        desc->flags != 0u || desc->reserved0 != 0u ||
        desc->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader, desc->color_format,
        desc->primitive_topology, desc->depth_format, desc->depth_compare,
        desc->depth_write_enabled,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        1u, desc->source_color_factor,
        desc->destination_color_factor, desc->color_operation,
        desc->source_alpha_factor, desc->destination_alpha_factor,
        desc->alpha_operation, desc->color_write_mask, 0, NULL, 0u, 0u,
        NULL, NULL, 0u, 0, 0u, 0u, NULL, 0u, RIN_GPU_CULL_NONE,
        RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_native(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline) {
    int blend_valid;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        !ringpu_depth_pipeline_valid(
            desc->depth_format, desc->depth_compare,
            desc->depth_write_enabled, 1) ||
        !ringpu_stencil_pipeline_valid(
            desc->depth_format, desc->stencil_test_enabled,
            desc->stencil_compare, desc->stencil_reference,
            desc->stencil_read_mask, desc->stencil_write_mask,
            desc->stencil_fail_operation,
            desc->stencil_depth_fail_operation,
            desc->stencil_pass_operation, desc->separate_stencil_enabled,
            desc->back_stencil_compare, desc->back_stencil_reference,
            desc->back_stencil_read_mask, desc->back_stencil_write_mask,
            desc->back_stencil_fail_operation,
            desc->back_stencil_depth_fail_operation,
            desc->back_stencil_pass_operation) ||
        desc->blend_enabled > 1u ||
        (desc->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        !ringpu_cull_mode_valid(desc->cull_mode) ||
        !ringpu_front_face_valid(desc->front_face) ||
        (desc->flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        desc->reserved0 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    blend_valid = desc->blend_enabled != 0u
        ? ringpu_blend_source_factor_valid(desc->source_color_factor) &&
          ringpu_blend_factor_valid(desc->destination_color_factor) &&
          ringpu_blend_operation_valid(desc->color_operation) &&
          ringpu_blend_source_factor_valid(desc->source_alpha_factor) &&
          ringpu_blend_factor_valid(desc->destination_alpha_factor) &&
          ringpu_blend_operation_valid(desc->alpha_operation)
        : desc->source_color_factor == 0u &&
          desc->destination_color_factor == 0u &&
          desc->color_operation == 0u &&
          desc->source_alpha_factor == 0u &&
          desc->destination_alpha_factor == 0u &&
          desc->alpha_operation == 0u;
    if (!blend_valid) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader, desc->color_format,
        desc->primitive_topology, desc->depth_format, desc->depth_compare,
        desc->depth_write_enabled, desc->stencil_test_enabled,
        desc->stencil_compare, desc->stencil_reference,
        desc->stencil_read_mask, desc->stencil_write_mask,
        desc->stencil_fail_operation,
        desc->stencil_depth_fail_operation,
        desc->stencil_pass_operation, desc->blend_enabled,
        desc->source_color_factor, desc->destination_color_factor,
        desc->color_operation, desc->source_alpha_factor,
        desc->destination_alpha_factor, desc->alpha_operation,
        desc->color_write_mask, 1, attributes, attribute_count,
        desc->vertex_stride, NULL, NULL, 0u,
        1, desc->flags, desc->position_output_location, varyings,
        varying_count, desc->cull_mode, desc->front_face,
        desc->separate_stencil_enabled, desc->back_stencil_compare,
        desc->back_stencil_reference, desc->back_stencil_read_mask,
        desc->back_stencil_write_mask, desc->back_stencil_fail_operation,
        desc->back_stencil_depth_fail_operation,
        desc->back_stencil_pass_operation, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_native_vertex_bindings(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline)
{
    int blend_valid;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_color_format(desc->color_format) ||
        !ringpu_primitive_topology_valid(desc->primitive_topology) ||
        desc->vertex_stride != 0u ||
        !ringpu_depth_pipeline_valid(
            desc->depth_format, desc->depth_compare,
            desc->depth_write_enabled, 1) ||
        !ringpu_stencil_pipeline_valid(
            desc->depth_format, desc->stencil_test_enabled,
            desc->stencil_compare, desc->stencil_reference,
            desc->stencil_read_mask, desc->stencil_write_mask,
            desc->stencil_fail_operation,
            desc->stencil_depth_fail_operation,
            desc->stencil_pass_operation, desc->separate_stencil_enabled,
            desc->back_stencil_compare, desc->back_stencil_reference,
            desc->back_stencil_read_mask, desc->back_stencil_write_mask,
            desc->back_stencil_fail_operation,
            desc->back_stencil_depth_fail_operation,
            desc->back_stencil_pass_operation) ||
        desc->blend_enabled > 1u ||
        (desc->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        !ringpu_cull_mode_valid(desc->cull_mode) ||
        !ringpu_front_face_valid(desc->front_face) ||
        (desc->flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        desc->reserved0 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    blend_valid = desc->blend_enabled != 0u
        ? ringpu_blend_source_factor_valid(desc->source_color_factor) &&
          ringpu_blend_factor_valid(desc->destination_color_factor) &&
          ringpu_blend_operation_valid(desc->color_operation) &&
          ringpu_blend_source_factor_valid(desc->source_alpha_factor) &&
          ringpu_blend_factor_valid(desc->destination_alpha_factor) &&
          ringpu_blend_operation_valid(desc->alpha_operation)
        : desc->source_color_factor == 0u &&
          desc->destination_color_factor == 0u &&
          desc->color_operation == 0u &&
          desc->source_alpha_factor == 0u &&
          desc->destination_alpha_factor == 0u &&
          desc->alpha_operation == 0u;
    if (!blend_valid) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return ringpu_create_graphics_pipeline_internal(
        core, desc->vertex_shader, desc->fragment_shader, desc->color_format,
        desc->primitive_topology, desc->depth_format, desc->depth_compare,
        desc->depth_write_enabled, desc->stencil_test_enabled,
        desc->stencil_compare, desc->stencil_reference,
        desc->stencil_read_mask, desc->stencil_write_mask,
        desc->stencil_fail_operation,
        desc->stencil_depth_fail_operation,
        desc->stencil_pass_operation, desc->blend_enabled,
        desc->source_color_factor, desc->destination_color_factor,
        desc->color_operation, desc->source_alpha_factor,
        desc->destination_alpha_factor, desc->alpha_operation,
        desc->color_write_mask, 1, NULL, attribute_count, 0u,
        attributes, vertex_bindings, vertex_binding_count,
        1, desc->flags, desc->position_output_location, varyings, varying_count,
        desc->cull_mode, desc->front_face,
        desc->separate_stencil_enabled, desc->back_stencil_compare,
        desc->back_stencil_reference, desc->back_stencil_read_mask,
        desc->back_stencil_write_mask, desc->back_stencil_fail_operation,
        desc->back_stencil_depth_fail_operation,
        desc->back_stencil_pass_operation, pipeline, NULL, NULL, 0u);
}

int ringpu_create_graphics_pipeline_native_v2(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline)
{
    const RinGpuGraphicsPipelineNativeDescV1* base;
    float blend_constants[4];
    int blend_valid;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->base.abi_version, desc->base.struct_size,
                          sizeof(*desc))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    base = &desc->base;
    blend_constants[0] = desc->blend_constant_red;
    blend_constants[1] = desc->blend_constant_green;
    blend_constants[2] = desc->blend_constant_blue;
    blend_constants[3] = desc->blend_constant_alpha;
    if (!ringpu_blend_constants_valid(base->color_format, blend_constants) ||
        !ringpu_color_format(base->color_format) ||
        !ringpu_primitive_topology_valid(base->primitive_topology) ||
        !ringpu_depth_pipeline_valid(base->depth_format, base->depth_compare,
                                     base->depth_write_enabled, 1) ||
        !ringpu_stencil_pipeline_valid(
            base->depth_format, base->stencil_test_enabled,
            base->stencil_compare, base->stencil_reference,
            base->stencil_read_mask, base->stencil_write_mask,
            base->stencil_fail_operation, base->stencil_depth_fail_operation,
            base->stencil_pass_operation, base->separate_stencil_enabled,
            base->back_stencil_compare, base->back_stencil_reference,
            base->back_stencil_read_mask, base->back_stencil_write_mask,
            base->back_stencil_fail_operation,
            base->back_stencil_depth_fail_operation,
            base->back_stencil_pass_operation) ||
        base->blend_enabled > 1u ||
        (base->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        !ringpu_cull_mode_valid(base->cull_mode) ||
        !ringpu_front_face_valid(base->front_face) ||
        (base->flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        base->reserved0 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    blend_valid = base->blend_enabled != 0u
        ? ringpu_blend_source_factor_v2_valid(base->source_color_factor) &&
          ringpu_blend_factor_v2_valid(base->destination_color_factor) &&
          ringpu_blend_operation_valid(base->color_operation) &&
          ringpu_blend_source_factor_v2_valid(base->source_alpha_factor) &&
          ringpu_blend_factor_v2_valid(base->destination_alpha_factor) &&
          ringpu_blend_operation_valid(base->alpha_operation)
        : base->source_color_factor == 0u &&
          base->destination_color_factor == 0u &&
          base->color_operation == 0u && base->source_alpha_factor == 0u &&
          base->destination_alpha_factor == 0u && base->alpha_operation == 0u &&
          blend_constants[0] == 0.0f && blend_constants[1] == 0.0f &&
          blend_constants[2] == 0.0f && blend_constants[3] == 0.0f;
    if (!blend_valid) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return ringpu_create_graphics_pipeline_internal(
        core, base->vertex_shader, base->fragment_shader, base->color_format,
        base->primitive_topology, base->depth_format, base->depth_compare,
        base->depth_write_enabled, base->stencil_test_enabled,
        base->stencil_compare, base->stencil_reference,
        base->stencil_read_mask, base->stencil_write_mask,
        base->stencil_fail_operation, base->stencil_depth_fail_operation,
        base->stencil_pass_operation, base->blend_enabled,
        base->source_color_factor, base->destination_color_factor,
        base->color_operation, base->source_alpha_factor,
        base->destination_alpha_factor, base->alpha_operation,
        base->color_write_mask, 1, attributes, attribute_count,
        base->vertex_stride, NULL, NULL, 0u, 1, base->flags,
        base->position_output_location, varyings, varying_count,
        base->cull_mode, base->front_face, base->separate_stencil_enabled,
        base->back_stencil_compare, base->back_stencil_reference,
        base->back_stencil_read_mask, base->back_stencil_write_mask,
        base->back_stencil_fail_operation,
        base->back_stencil_depth_fail_operation,
        base->back_stencil_pass_operation, pipeline, blend_constants, NULL,
        0u);
}

int ringpu_create_graphics_pipeline_native_v3(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV3* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline)
{
    const RinGpuGraphicsPipelineNativeDescV2* base;
    float blend_constants[4];
    int blend_valid;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->base.base.abi_version,
                          desc->base.base.struct_size, sizeof(*desc)) ||
        desc->blend_target_mask !=
            ((UINT32_C(1) << RIN_GPU_MAX_COLOR_TARGETS) - 1u) ||
        desc->reserved != 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < RIN_GPU_MAX_COLOR_TARGETS; ++index) {
        if (!ringpu_independent_blend_target_valid(&desc->blend_targets[index]))
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    base = &desc->base;
    blend_constants[0] = base->blend_constant_red;
    blend_constants[1] = base->blend_constant_green;
    blend_constants[2] = base->blend_constant_blue;
    blend_constants[3] = base->blend_constant_alpha;
    if (!ringpu_blend_constants_valid(base->base.color_format,
                                      blend_constants) ||
        !ringpu_color_format(base->base.color_format) ||
        !ringpu_primitive_topology_valid(base->base.primitive_topology) ||
        !ringpu_depth_pipeline_valid(
            base->base.depth_format, base->base.depth_compare,
            base->base.depth_write_enabled, 1) ||
        !ringpu_stencil_pipeline_valid(
            base->base.depth_format, base->base.stencil_test_enabled,
            base->base.stencil_compare, base->base.stencil_reference,
            base->base.stencil_read_mask, base->base.stencil_write_mask,
            base->base.stencil_fail_operation,
            base->base.stencil_depth_fail_operation,
            base->base.stencil_pass_operation,
            base->base.separate_stencil_enabled,
            base->base.back_stencil_compare,
            base->base.back_stencil_reference,
            base->base.back_stencil_read_mask,
            base->base.back_stencil_write_mask,
            base->base.back_stencil_fail_operation,
            base->base.back_stencil_depth_fail_operation,
            base->base.back_stencil_pass_operation) ||
        base->base.blend_enabled > 1u ||
        (base->base.color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        !ringpu_cull_mode_valid(base->base.cull_mode) ||
        !ringpu_front_face_valid(base->base.front_face) ||
        (base->base.flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) !=
            0u ||
        base->base.reserved0 != 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    blend_valid = base->base.blend_enabled != 0u
        ? ringpu_blend_source_factor_v2_valid(
              base->base.source_color_factor) &&
          ringpu_blend_factor_v2_valid(base->base.destination_color_factor) &&
          ringpu_blend_operation_valid(base->base.color_operation) &&
          ringpu_blend_source_factor_v2_valid(
              base->base.source_alpha_factor) &&
          ringpu_blend_factor_v2_valid(
              base->base.destination_alpha_factor) &&
          ringpu_blend_operation_valid(base->base.alpha_operation)
        : base->base.source_color_factor == 0u &&
          base->base.destination_color_factor == 0u &&
          base->base.color_operation == 0u &&
          base->base.source_alpha_factor == 0u &&
          base->base.destination_alpha_factor == 0u &&
          base->base.alpha_operation == 0u &&
          blend_constants[0] == 0.0f && blend_constants[1] == 0.0f &&
          blend_constants[2] == 0.0f && blend_constants[3] == 0.0f;
    if (!blend_valid) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return ringpu_create_graphics_pipeline_internal(
        core, base->base.vertex_shader, base->base.fragment_shader,
        base->base.color_format, base->base.primitive_topology,
        base->base.depth_format, base->base.depth_compare,
        base->base.depth_write_enabled, base->base.stencil_test_enabled,
        base->base.stencil_compare, base->base.stencil_reference,
        base->base.stencil_read_mask, base->base.stencil_write_mask,
        base->base.stencil_fail_operation,
        base->base.stencil_depth_fail_operation,
        base->base.stencil_pass_operation, base->base.blend_enabled,
        base->base.source_color_factor,
        base->base.destination_color_factor, base->base.color_operation,
        base->base.source_alpha_factor,
        base->base.destination_alpha_factor, base->base.alpha_operation,
        base->base.color_write_mask, 1, attributes, attribute_count,
        base->base.vertex_stride, NULL, NULL, 0u, 1, base->base.flags,
        base->base.position_output_location, varyings, varying_count,
        base->base.cull_mode, base->base.front_face,
        base->base.separate_stencil_enabled,
        base->base.back_stencil_compare, base->base.back_stencil_reference,
        base->base.back_stencil_read_mask,
        base->base.back_stencil_write_mask,
        base->base.back_stencil_fail_operation,
        base->base.back_stencil_depth_fail_operation,
        base->base.back_stencil_pass_operation, pipeline, blend_constants,
        desc->blend_targets, desc->blend_target_mask);
}

int ringpu_create_graphics_pipeline_native_vertex_bindings_v2(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline)
{
    const RinGpuGraphicsPipelineNativeDescV1* base;
    float blend_constants[4];
    int blend_valid;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->base.abi_version, desc->base.struct_size,
                          sizeof(*desc))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    base = &desc->base;
    blend_constants[0] = desc->blend_constant_red;
    blend_constants[1] = desc->blend_constant_green;
    blend_constants[2] = desc->blend_constant_blue;
    blend_constants[3] = desc->blend_constant_alpha;
    if (!ringpu_blend_constants_valid(base->color_format, blend_constants) ||
        !ringpu_color_format(base->color_format) ||
        !ringpu_primitive_topology_valid(base->primitive_topology) ||
        base->vertex_stride != 0u ||
        !ringpu_depth_pipeline_valid(base->depth_format, base->depth_compare,
                                     base->depth_write_enabled, 1) ||
        !ringpu_stencil_pipeline_valid(
            base->depth_format, base->stencil_test_enabled,
            base->stencil_compare, base->stencil_reference,
            base->stencil_read_mask, base->stencil_write_mask,
            base->stencil_fail_operation, base->stencil_depth_fail_operation,
            base->stencil_pass_operation, base->separate_stencil_enabled,
            base->back_stencil_compare, base->back_stencil_reference,
            base->back_stencil_read_mask, base->back_stencil_write_mask,
            base->back_stencil_fail_operation,
            base->back_stencil_depth_fail_operation,
            base->back_stencil_pass_operation) ||
        base->blend_enabled > 1u ||
        (base->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
        !ringpu_cull_mode_valid(base->cull_mode) ||
        !ringpu_front_face_valid(base->front_face) ||
        (base->flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        base->reserved0 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    blend_valid = base->blend_enabled != 0u
        ? ringpu_blend_source_factor_v2_valid(base->source_color_factor) &&
          ringpu_blend_factor_v2_valid(base->destination_color_factor) &&
          ringpu_blend_operation_valid(base->color_operation) &&
          ringpu_blend_source_factor_v2_valid(base->source_alpha_factor) &&
          ringpu_blend_factor_v2_valid(base->destination_alpha_factor) &&
          ringpu_blend_operation_valid(base->alpha_operation)
        : base->source_color_factor == 0u &&
          base->destination_color_factor == 0u &&
          base->color_operation == 0u && base->source_alpha_factor == 0u &&
          base->destination_alpha_factor == 0u && base->alpha_operation == 0u &&
          blend_constants[0] == 0.0f && blend_constants[1] == 0.0f &&
          blend_constants[2] == 0.0f && blend_constants[3] == 0.0f;
    if (!blend_valid) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return ringpu_create_graphics_pipeline_internal(
        core, base->vertex_shader, base->fragment_shader, base->color_format,
        base->primitive_topology, base->depth_format, base->depth_compare,
        base->depth_write_enabled, base->stencil_test_enabled,
        base->stencil_compare, base->stencil_reference,
        base->stencil_read_mask, base->stencil_write_mask,
        base->stencil_fail_operation, base->stencil_depth_fail_operation,
        base->stencil_pass_operation, base->blend_enabled,
        base->source_color_factor, base->destination_color_factor,
        base->color_operation, base->source_alpha_factor,
        base->destination_alpha_factor, base->alpha_operation,
        base->color_write_mask, 1, NULL, attribute_count, 0u, attributes,
        vertex_bindings, vertex_binding_count, 1, base->flags,
        base->position_output_location, varyings, varying_count,
        base->cull_mode, base->front_face, base->separate_stencil_enabled,
        base->back_stencil_compare, base->back_stencil_reference,
        base->back_stencil_read_mask, base->back_stencil_write_mask,
        base->back_stencil_fail_operation,
        base->back_stencil_depth_fail_operation,
        base->back_stencil_pass_operation, pipeline, blend_constants, NULL,
        0u);
}

int ringpu_graphics_image_kind(uint32_t kind) {
    return kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
           kind == RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE ||
           kind == RIN_SHADER_RESOURCE_STORAGE_IMAGE;
}

int ringpu_graphics_sampler_kind(uint32_t kind) {
    return kind == RIN_SHADER_RESOURCE_SAMPLER ||
           kind == RIN_SHADER_RESOURCE_COMPARISON_SAMPLER;
}

uint32_t ringpu_graphics_binding_mip_count(
    const RinGpuGraphicsBindingV1* binding, const RinGpuImageDescV1* desc);

int ringpu_graphics_binding_slot(
    RinGpuCore* core, const RinGpuGraphicsBindingV1* binding,
    RinGpuObjectSlot** slot) {
    uint16_t type;
    if (!core || !binding || !slot) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
        type = RIN_GPU_OBJECT_BUFFER;
    } else if (ringpu_graphics_image_kind(binding->kind)) {
        type = RIN_GPU_OBJECT_IMAGE;
    } else if (ringpu_graphics_sampler_kind(binding->kind)) {
        type = RIN_GPU_OBJECT_SAMPLER;
    } else {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_slot(core, binding->resource, type, NULL, slot);
}

int ringpu_graphics_binding_reference(
    RinGpuCore* core, const RinGpuGraphicsBindingV1* binding, int acquire) {
    RinGpuObjectSlot* slot;
    uint32_t* references;
    int result = ringpu_graphics_binding_slot(core, binding, &slot);
    if (result != RIN_GPU_OK) return result;
    if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
        references = &slot->value.buffer.reference_count;
    } else if (ringpu_graphics_image_kind(binding->kind)) {
        references = &slot->value.image.reference_count;
    } else {
        references = &slot->value.sampler.reference_count;
    }
    if (acquire) {
        if (*references == UINT32_MAX) return RIN_GPU_ERROR_LIMIT;
        (*references)++;
    } else {
        if (*references == 0u) return RIN_GPU_ERROR_STATE;
        (*references)--;
    }
    return RIN_GPU_OK;
}

int ringpu_create_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group) {
    RinGpuGraphicsBindingV1* typed = NULL;
    int result;
    if (binding_count > RIN_SHADER_MAX_RESOURCES ||
        (binding_count == 0u && bindings != NULL) ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (binding_count != 0u) {
        typed = (RinGpuGraphicsBindingV1*)calloc(binding_count,
                                                  sizeof(*typed));
        if (!typed) return RIN_GPU_ERROR_NO_MEMORY;
    }
    for (uint32_t index = 0u; index < binding_count; index++) {
        const RinGpuBufferBindingV1* source = &bindings[index];
        if (!ringpu_versioned(source->abi_version, source->struct_size,
                              sizeof(*source))) {
            free(typed);
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        typed[index].abi_version = source->abi_version;
        typed[index].struct_size = sizeof(typed[index]);
        typed[index].binding = source->binding;
        typed[index].kind = RIN_SHADER_RESOURCE_STORAGE_BUFFER;
        typed[index].access = source->access;
        typed[index].flags = source->flags;
        typed[index].resource = source->buffer;
        typed[index].offset = source->offset;
        typed[index].size_bytes = source->size_bytes;
        typed[index].reserved0 = source->reserved;
    }
    result = ringpu_create_graphics_bind_group_typed(
        core, pipeline, typed, binding_count, bind_group);
    free(typed);
    return result;
}

int ringpu_create_graphics_bind_group_typed(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuGraphicsBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group) {
    RinGpuObjectSlot* pipeline_slot;
    RinGpuObjectSlot* slot;
    RinGpuGraphicsBindingV1* snapshot = NULL;
    RinGpuBackendGraphicsBindingV1* backend_bindings = NULL;
    uint64_t seen = 0u;
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!bind_group || binding_count > RIN_SHADER_MAX_RESOURCES ||
        (binding_count == 0u && bindings != NULL) ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *bind_group = 0u;
    result = ringpu_slot(core, pipeline, RIN_GPU_OBJECT_GRAPHICS_PIPELINE,
                         NULL, &pipeline_slot);
    if (result != RIN_GPU_OK) return result;
    if (binding_count == 0u ||
        binding_count !=
            pipeline_slot->value.graphics_pipeline.resource_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    snapshot = (RinGpuGraphicsBindingV1*)calloc(binding_count,
                                                 sizeof(*snapshot));
    backend_bindings = (RinGpuBackendGraphicsBindingV1*)calloc(
        binding_count, sizeof(*backend_bindings));
    if (!snapshot || !backend_bindings) {
        result = RIN_GPU_ERROR_NO_MEMORY;
        goto fail;
    }
    for (uint32_t index = 0u; index < binding_count; index++) {
        const RinGpuGraphicsBindingV1* binding = &bindings[index];
        RinGpuObjectSlot* resource;
        if (!ringpu_versioned(binding->abi_version, binding->struct_size,
                              sizeof(*binding)) ||
            binding->binding >= binding_count ||
            (seen & (UINT64_C(1) << binding->binding)) != 0u ||
            binding->kind != pipeline_slot->value.graphics_pipeline
                .resource_kinds[binding->binding] ||
            binding->access != pipeline_slot->value.graphics_pipeline
                .resource_access[binding->binding] ||
            (binding->flags & ~RIN_GPU_GRAPHICS_BINDING_KNOWN_FLAGS) != 0u ||
            binding->reserved0 != 0u ||
            binding->reserved1 != 0u) {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        result = ringpu_graphics_binding_slot(core, binding, &resource);
        if (result != RIN_GPU_OK) goto fail;
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            if (binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->mip_level != 0u || binding->array_layer != 0u ||
                (binding->offset & 3u) != 0u ||
                (binding->size_bytes & 3u) != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            if ((resource->value.buffer.usage & RIN_GPU_BUFFER_STORAGE) == 0u ||
                !ringpu_buffer_upload_ready(resource)) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
            if (!ringpu_range(binding->offset, binding->size_bytes,
                              resource->value.buffer.size_bytes)) {
                result = RIN_GPU_ERROR_BOUNDS;
                goto fail;
            }
            backend_bindings[binding->binding].offset = binding->offset;
            backend_bindings[binding->binding].size_bytes =
                binding->size_bytes;
        } else if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_IMAGE) {
            const RinGpuImageDescV1* desc = &resource->value.image.descriptor;
            if (binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->offset != 0u || binding->size_bytes != 0u ||
                binding->flags != 0u || binding->mip_level >= desc->mip_levels ||
                binding->array_layer >= desc->array_layers ||
                desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                desc->format != RIN_GPU_FORMAT_R8_UNORM ||
                desc->sample_count != 1u ||
                (desc->usage & RIN_GPU_IMAGE_STORAGE) == 0u) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
            backend_bindings[binding->binding].mip_level =
                binding->mip_level;
            backend_bindings[binding->binding].array_layer =
                binding->array_layer;
        } else if (ringpu_graphics_image_kind(binding->kind)) {
            const RinGpuImageDescV1* desc = &resource->value.image.descriptor;
            if (binding->access != RIN_GPU_RESOURCE_READ ||
                binding->offset != 0u || binding->size_bytes != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            if ((desc->usage & RIN_GPU_IMAGE_SAMPLED) == 0u ||
                desc->sample_count != 1u ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE
                     ? (!ringpu_sampled_image_format(desc->format) &&
                        !ringpu_depth_stencil_format(desc->format))
                     : desc->format != RIN_GPU_FORMAT_D32_FLOAT)) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
            if (binding->mip_level >= desc->mip_levels ||
                binding->array_layer >= desc->array_layers) {
                result = RIN_GPU_ERROR_BOUNDS;
                goto fail;
            }
            if ((binding->flags != 0u &&
                 (binding->kind != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
                  binding->flags !=
                      RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN)) ||
                ringpu_graphics_binding_mip_count(binding, desc) == 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            backend_bindings[binding->binding].mip_level =
                binding->mip_level;
            backend_bindings[binding->binding].array_layer =
                binding->array_layer;
        } else if (ringpu_graphics_sampler_kind(binding->kind)) {
            if (binding->access != 0u || binding->offset != 0u ||
                binding->size_bytes != 0u || binding->mip_level != 0u ||
                binding->array_layer != 0u || binding->flags != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            if (binding->kind == RIN_SHADER_RESOURCE_SAMPLER
                    ? resource->value.sampler.descriptor.compare_op != 0u
                    : resource->value.sampler.descriptor.compare_op == 0u) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
        } else {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        seen |= UINT64_C(1) << binding->binding;
        snapshot[binding->binding] = *binding;
        snapshot[binding->binding].struct_size = sizeof(*snapshot);
        backend_bindings[binding->binding].binding = binding->binding;
        backend_bindings[binding->binding].kind = binding->kind;
        backend_bindings[binding->binding].access = binding->access;
        backend_bindings[binding->binding].flags = binding->flags;
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            backend_bindings[binding->binding].resource_cookie =
                resource->value.buffer.backend_cookie;
        } else if (ringpu_graphics_image_kind(binding->kind)) {
            backend_bindings[binding->binding].resource_cookie =
                resource->value.image.backend_cookie;
        } else {
            backend_bindings[binding->binding].resource_cookie =
                resource->value.sampler.backend_cookie;
        }
    }
    for (uint32_t current = 0u; current < binding_count; current++) {
        for (uint32_t prior = 0u; prior < current; prior++) {
            if (snapshot[current].kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                snapshot[prior].kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                snapshot[current].resource == snapshot[prior].resource &&
                snapshot[current].offset <
                    snapshot[prior].offset + snapshot[prior].size_bytes &&
                snapshot[prior].offset <
                    snapshot[current].offset + snapshot[current].size_bytes &&
                ((snapshot[current].access | snapshot[prior].access) &
                 RIN_GPU_RESOURCE_WRITE) != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
        }
    }
    result = core->backend.create_graphics_bind_group(
        core->backend_context,
        pipeline_slot->value.graphics_pipeline.backend_cookie,
        backend_bindings, binding_count, &cookie);
    if (result != RIN_GPU_OK) goto fail;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP,
                             bind_group, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_graphics_bind_group(core->backend_context,
                                                  cookie);
        goto fail;
    }
    slot->value.graphics_bind_group.pipeline = pipeline;
    slot->value.graphics_bind_group.backend_cookie = cookie;
    slot->value.graphics_bind_group.bindings = snapshot;
    slot->value.graphics_bind_group.binding_count = binding_count;
    pipeline_slot->value.graphics_pipeline.reference_count++;
    for (uint32_t index = 0u; index < binding_count; index++) {
        (void)ringpu_graphics_binding_reference(core, &snapshot[index], 1);
    }
    free(backend_bindings);
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP,
                           binding_count, RIN_GPU_OK);
    return RIN_GPU_OK;

fail:
    free(snapshot);
    free(backend_bindings);
    return result;
}

uint32_t ringpu_graphics_binding_mip_count(
    const RinGpuGraphicsBindingV1* binding, const RinGpuImageDescV1* desc)
{
    if (!binding || !desc || binding->mip_level >= desc->mip_levels)
        return 0u;
    return (binding->flags & RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN) != 0u
        ? desc->mip_levels - binding->mip_level : 1u;
}

int ringpu_validate_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline_handle,
    const RinGpuObjectSlot* pipeline, RinGpuHandle bind_group_handle,
    RinGpuObjectSlot** bind_group_out) {
    RinGpuObjectSlot* bind_group;
    int result;
    if (!core || !pipeline ||
        pipeline->type != RIN_GPU_OBJECT_GRAPHICS_PIPELINE ||
        pipeline->value.graphics_pipeline.resource_count == 0u ||
        bind_group_handle == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, bind_group_handle,
                         RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                         &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (bind_group->value.graphics_bind_group.pipeline != pipeline_handle ||
        bind_group->value.graphics_bind_group.binding_count !=
            pipeline->value.graphics_pipeline.resource_count) {
        return RIN_GPU_ERROR_STATE;
    }
    for (uint32_t index = 0u;
         index < bind_group->value.graphics_bind_group.binding_count;
         index++) {
        const RinGpuGraphicsBindingV1* binding =
            &bind_group->value.graphics_bind_group.bindings[index];
        RinGpuObjectSlot* resource;
        result = ringpu_graphics_binding_slot(core, binding, &resource);
        if (result != RIN_GPU_OK) return result;
        if (binding->abi_version != RIN_GPU_ABI_VERSION ||
            binding->struct_size != sizeof(*binding) ||
            binding->binding != index ||
            binding->kind !=
                pipeline->value.graphics_pipeline.resource_kinds[index] ||
            binding->access !=
                pipeline->value.graphics_pipeline.resource_access[index] ||
            (binding->flags & ~RIN_GPU_GRAPHICS_BINDING_KNOWN_FLAGS) != 0u ||
            binding->reserved0 != 0u ||
            binding->reserved1 != 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            if (binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->mip_level != 0u || binding->array_layer != 0u ||
                (binding->offset & 3u) != 0u ||
                (binding->size_bytes & 3u) != 0u ||
                (resource->value.buffer.usage & RIN_GPU_BUFFER_STORAGE) == 0u ||
                !ringpu_buffer_upload_ready(resource) ||
                !ringpu_range(binding->offset, binding->size_bytes,
                              resource->value.buffer.size_bytes)) {
                return RIN_GPU_ERROR_STATE;
            }
        } else if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_IMAGE) {
            const RinGpuImageDescV1* desc = &resource->value.image.descriptor;
            if (binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->offset != 0u || binding->size_bytes != 0u ||
                binding->flags != 0u ||
                desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                desc->format != RIN_GPU_FORMAT_R8_UNORM ||
                desc->sample_count != 1u ||
                (desc->usage & RIN_GPU_IMAGE_STORAGE) == 0u ||
                binding->mip_level >= desc->mip_levels ||
                binding->array_layer >= desc->array_layers) {
                return RIN_GPU_ERROR_STATE;
            }
        } else if (ringpu_graphics_image_kind(binding->kind)) {
            const RinGpuImageDescV1* desc = &resource->value.image.descriptor;
            if (binding->access != RIN_GPU_RESOURCE_READ ||
                binding->offset != 0u || binding->size_bytes != 0u ||
                (desc->usage & RIN_GPU_IMAGE_SAMPLED) == 0u ||
                desc->sample_count != 1u ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE
                     ? (!ringpu_sampled_image_format(desc->format) &&
                        !ringpu_depth_stencil_format(desc->format))
                     : desc->format != RIN_GPU_FORMAT_D32_FLOAT) ||
                binding->mip_level >= desc->mip_levels ||
                binding->array_layer >= desc->array_layers ||
                (binding->flags != 0u &&
                 (binding->kind != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
                  binding->flags !=
                      RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN)) ||
                ringpu_graphics_binding_mip_count(binding, desc) == 0u) {
                return RIN_GPU_ERROR_STATE;
            }
        } else if (ringpu_graphics_sampler_kind(binding->kind)) {
            if (binding->access != 0u || binding->offset != 0u ||
                binding->size_bytes != 0u || binding->mip_level != 0u ||
                binding->array_layer != 0u || binding->flags != 0u ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLER
                     ? resource->value.sampler.descriptor.compare_op != 0u
                     : resource->value.sampler.descriptor.compare_op == 0u)) {
                return RIN_GPU_ERROR_STATE;
            }
        } else {
            return RIN_GPU_ERROR_STATE;
        }
    }
    for (uint32_t current = 0u;
         current < bind_group->value.graphics_bind_group.binding_count;
         current++) {
        const RinGpuGraphicsBindingV1* current_binding =
            &bind_group->value.graphics_bind_group.bindings[current];
        for (uint32_t prior = 0u; prior < current; prior++) {
            const RinGpuGraphicsBindingV1* prior_binding =
                &bind_group->value.graphics_bind_group.bindings[prior];
            if (current_binding->kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                prior_binding->kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                current_binding->resource == prior_binding->resource &&
                current_binding->offset <
                    prior_binding->offset + prior_binding->size_bytes &&
                prior_binding->offset <
                    current_binding->offset + current_binding->size_bytes &&
                ((current_binding->access | prior_binding->access) &
                 RIN_GPU_RESOURCE_WRITE) != 0u) {
                return RIN_GPU_ERROR_STATE;
            }
        }
    }
    if (bind_group_out) *bind_group_out = bind_group;
    return RIN_GPU_OK;
}
