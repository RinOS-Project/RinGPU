// SPDX-License-Identifier: MIT
#include "graphics_layout.h"

#include "pipeline.h"
#include "resource.h"

#include <string.h>

int ringpu_build_vertex_layout(
    uint32_t shader_input_count, int explicit_layout,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    uint32_t vertex_stride, RinGpuBackendGraphicsPipelineDescV1* desc)
{
    uint32_t seen_locations = 0u;
    uint32_t has_streamed_attributes = 0u;

    if (!desc || shader_input_count > RIN_GPU_MAX_VERTEX_ATTRIBUTES)
        return RIN_GPU_ERROR_SHADER_INVALID;
    desc->vertex_input_count = shader_input_count;
    if (!explicit_layout) {
        desc->vertex_stride = shader_input_count * (uint32_t)sizeof(uint32_t);
        desc->vertex_binding_count = shader_input_count != 0u ? 1u : 0u;
        desc->vertex_bindings[0].binding = 0u;
        desc->vertex_bindings[0].stride = desc->vertex_stride;
        for (uint32_t location = 0u; location < shader_input_count;
             location++) {
            desc->vertex_attributes[location].location = location;
            desc->vertex_attributes[location].format = RIN_GPU_VERTEX_UINT32;
            desc->vertex_attributes[location].offset =
                location * (uint32_t)sizeof(uint32_t);
        }
        return RIN_GPU_OK;
    }
    if (attribute_count != shader_input_count ||
        attribute_count > RIN_GPU_MAX_VERTEX_ATTRIBUTES ||
        (attribute_count == 0u && attributes != NULL) ||
        (attribute_count != 0u && attributes == NULL) ||
        (attribute_count == 0u && vertex_stride != 0u) ||
        vertex_stride > RIN_GPU_MAX_VERTEX_STRIDE) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0u; index < attribute_count; index++) {
        const RinGpuVertexAttributeV1* attribute = &attributes[index];
        uint32_t location_bit;
        uint32_t component_bytes;
        uint32_t is_constant;

        component_bytes = ringpu_vertex_format_bytes(attribute->format);
        is_constant = attribute->flags ==
            RIN_GPU_VERTEX_ATTRIBUTE_CONSTANT_FLOAT32;
        if (!ringpu_versioned(attribute->abi_version,
                              attribute->struct_size,
                              sizeof(*attribute)) ||
            attribute->location >= shader_input_count ||
            !ringpu_vertex_format_valid(attribute->format) ||
            component_bytes == 0u || attribute->reserved0 != 0u ||
            attribute->reserved1 != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        if (is_constant) {
            if (attribute->format != RIN_GPU_VERTEX_FLOAT32)
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
        } else {
            if (attribute->flags != 0u || vertex_stride == 0u ||
                attribute->offset > vertex_stride - component_bytes) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            has_streamed_attributes = 1u;
        }
        location_bit = UINT32_C(1) << attribute->location;
        if ((seen_locations & location_bit) != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        for (uint32_t previous = 0u; previous < index; previous++) {
            if (attributes[previous].flags == 0u && !is_constant &&
                attributes[previous].offset == attribute->offset) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        }
        seen_locations |= location_bit;
        desc->vertex_attributes[attribute->location].location =
            attribute->location;
        desc->vertex_attributes[attribute->location].format =
            attribute->format;
        desc->vertex_attributes[attribute->location].offset =
            attribute->offset;
        desc->vertex_attributes[attribute->location].flags = attribute->flags;
        desc->vertex_attributes[attribute->location].binding = 0u;
    }
    if ((has_streamed_attributes == 0u && vertex_stride != 0u) ||
        (has_streamed_attributes != 0u && vertex_stride == 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    desc->vertex_stride = vertex_stride;
    desc->vertex_binding_count = has_streamed_attributes != 0u ? 1u : 0u;
    if (has_streamed_attributes != 0u) {
        desc->vertex_bindings[0].binding = 0u;
        desc->vertex_bindings[0].stride = vertex_stride;
    }
    return RIN_GPU_OK;
}

int ringpu_build_vertex_layout_bindings(
    uint32_t shader_input_count, const RinGpuVertexAttributeV2* attributes,
    uint32_t attribute_count, const RinGpuVertexBufferLayoutV1* bindings,
    uint32_t binding_count, RinGpuBackendGraphicsPipelineDescV1* desc)
{
    uint32_t seen_locations = 0u;
    uint64_t used_bindings = 0u;
    uint32_t has_streamed_attributes = 0u;

    if (!desc || shader_input_count == 0u ||
        shader_input_count > RIN_GPU_MAX_VERTEX_ATTRIBUTES ||
        attribute_count != shader_input_count ||
        attribute_count > RIN_GPU_MAX_VERTEX_ATTRIBUTES ||
        (attribute_count == 0u && attributes != NULL) ||
        (attribute_count != 0u && attributes == NULL) ||
        binding_count > RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS ||
        (binding_count == 0u && bindings != NULL) ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t binding = 0u; binding < binding_count; ++binding) {
        if (bindings[binding].binding != binding ||
            bindings[binding].stride == 0u ||
            bindings[binding].stride > RIN_GPU_MAX_VERTEX_STRIDE ||
            bindings[binding].reserved != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }
    for (uint32_t index = 0u; index < attribute_count; ++index) {
        const RinGpuVertexAttributeV2* attribute = &attributes[index];
        uint32_t location_bit;
        uint32_t component_bytes;
        uint32_t is_constant;

        component_bytes = ringpu_vertex_format_bytes(attribute->format);
        is_constant = attribute->flags ==
            RIN_GPU_VERTEX_ATTRIBUTE_CONSTANT_FLOAT32;
        if (!ringpu_versioned(attribute->abi_version,
                              attribute->struct_size,
                              sizeof(*attribute)) ||
            attribute->location >= shader_input_count ||
            !ringpu_vertex_format_valid(attribute->format) ||
            component_bytes == 0u || attribute->reserved0 != 0u ||
            attribute->reserved1 != 0u || attribute->reserved2 != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        if (is_constant) {
            if (attribute->format != RIN_GPU_VERTEX_FLOAT32 ||
                attribute->binding != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        } else {
            if (attribute->flags != 0u || attribute->binding >= binding_count ||
                attribute->offset >
                    bindings[attribute->binding].stride - component_bytes) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            has_streamed_attributes = 1u;
            used_bindings |= UINT64_C(1) << attribute->binding;
        }
        location_bit = UINT32_C(1) << attribute->location;
        if ((seen_locations & location_bit) != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        for (uint32_t previous = 0u; previous < index; ++previous) {
            if (attributes[previous].flags == 0u && !is_constant &&
                attributes[previous].binding == attribute->binding &&
                attributes[previous].offset == attribute->offset) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        }
        seen_locations |= location_bit;
        desc->vertex_attributes[attribute->location].location =
            attribute->location;
        desc->vertex_attributes[attribute->location].format =
            attribute->format;
        desc->vertex_attributes[attribute->location].offset =
            attribute->offset;
        desc->vertex_attributes[attribute->location].flags = attribute->flags;
        desc->vertex_attributes[attribute->location].binding =
            attribute->binding;
    }
    if ((has_streamed_attributes == 0u && binding_count != 0u) ||
        (has_streamed_attributes != 0u && binding_count == 0u) ||
        (binding_count != 0u &&
         used_bindings != ((UINT64_C(1) << binding_count) - 1u))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    desc->vertex_input_count = shader_input_count;
    desc->vertex_stride = 0u;
    desc->vertex_binding_count = binding_count;
    if (binding_count != 0u) {
        memcpy(desc->vertex_bindings, bindings,
               (size_t)binding_count * sizeof(*bindings));
    }
    return RIN_GPU_OK;
}

static uint32_t shader_interface_location_type(
    const RinGpuObjectSlot* shader, uint32_t location, int output)
{
    const RinShaderHeaderV1* header;
    const RinShaderInstructionV1* instructions;
    uint32_t type = 0u;

    if (!shader || shader->type != RIN_GPU_OBJECT_SHADER_MODULE ||
        !shader->value.shader_module.rin_shader_ir) {
        return 0u;
    }
    header = (const RinShaderHeaderV1*)shader->value.shader_module.rin_shader_ir;
    instructions = (const RinShaderInstructionV1*)(
        shader->value.shader_module.rin_shader_ir + header->header_size);
    for (uint32_t index = 0u; index < header->instruction_count; index++) {
        const RinShaderInstructionV1* instruction = &instructions[index];
        uint32_t instruction_type = 0u;
        if (instruction->immediate != location) continue;
        if (output) {
            if (instruction->opcode == RIN_SHADER_OP_STORE_OUTPUT_F32)
                instruction_type = RIN_GPU_VARYING_FLOAT32;
            else if (instruction->opcode == RIN_SHADER_OP_STORE_OUTPUT)
                instruction_type = RIN_GPU_VARYING_SINT32;
        } else {
            if (instruction->opcode == RIN_SHADER_OP_LOAD_INPUT_F32)
                instruction_type = RIN_GPU_VARYING_FLOAT32;
            else if (instruction->opcode == RIN_SHADER_OP_LOAD_INPUT)
                instruction_type = RIN_GPU_VARYING_SINT32;
        }
        if (instruction_type == 0u) continue;
        if (type != 0u && type != instruction_type) return 0u;
        type = instruction_type;
    }
    return type;
}

int ringpu_build_varying_layout(
    const RinGpuObjectSlot* vertex_shader,
    const RinGpuObjectSlot* fragment_shader,
    uint32_t position_output_location, uint32_t native_flags,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuBackendGraphicsPipelineDescV1* desc)
{
    uint32_t vertex_seen = 0u;
    uint32_t fragment_seen = 0u;
    const RinShaderInfoV1* vertex_info;
    const RinShaderInfoV1* fragment_info;

    if (!vertex_shader || !fragment_shader || !desc ||
        varying_count > RIN_GPU_MAX_VARYINGS ||
        (varying_count == 0u && varyings != NULL) ||
        (varying_count != 0u && varyings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    vertex_info = &vertex_shader->value.shader_module.info;
    fragment_info = &fragment_shader->value.shader_module.info;
    if ((native_flags & ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        position_output_location > vertex_info->output_count ||
        4u > vertex_info->output_count - position_output_location ||
        varying_count != fragment_info->input_count) {
        return RIN_GPU_ERROR_SHADER_INVALID;
    }
    for (uint32_t component = 0u; component < 4u; component++) {
        if (shader_interface_location_type(
                vertex_shader, position_output_location + component, 1) !=
            RIN_GPU_VARYING_FLOAT32) {
            return RIN_GPU_ERROR_SHADER_INVALID;
        }
    }
    if ((native_flags & RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT) !=
        0u) {
        if (5u > vertex_info->output_count - position_output_location ||
            shader_interface_location_type(
                vertex_shader, position_output_location + 4u, 1) !=
                RIN_GPU_VARYING_FLOAT32) {
            return RIN_GPU_ERROR_SHADER_INVALID;
        }
    }
    for (uint32_t index = 0u; index < varying_count; index++) {
        const RinGpuVaryingV1* varying = &varyings[index];
        uint32_t vertex_bit;
        uint32_t fragment_bit;
        if (!ringpu_versioned(varying->abi_version, varying->struct_size,
                              sizeof(*varying)) ||
            varying->vertex_output_location >= vertex_info->output_count ||
            varying->fragment_input_location >= fragment_info->input_count ||
            varying->vertex_output_location >= RIN_SHADER_MAX_IO ||
            varying->fragment_input_location >= RIN_SHADER_MAX_IO ||
            (varying->vertex_output_location >= position_output_location &&
             varying->vertex_output_location < position_output_location + 4u) ||
            ((native_flags &
              RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT) != 0u &&
             varying->vertex_output_location == position_output_location + 4u) ||
            !ringpu_varying_type_valid(varying->type) ||
            !ringpu_varying_interpolation_valid(varying->interpolation) ||
            (varying->type == RIN_GPU_VARYING_SINT32 &&
             varying->interpolation != RIN_GPU_INTERPOLATION_FLAT) ||
            varying->flags != 0u || varying->reserved0 != 0u ||
            varying->reserved1 != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        vertex_bit = UINT32_C(1) << varying->vertex_output_location;
        fragment_bit = UINT32_C(1) << varying->fragment_input_location;
        if ((vertex_seen & vertex_bit) != 0u ||
            (fragment_seen & fragment_bit) != 0u ||
            shader_interface_location_type(
                vertex_shader, varying->vertex_output_location, 1) !=
                varying->type ||
            shader_interface_location_type(
                fragment_shader, varying->fragment_input_location, 0) !=
                varying->type) {
            return RIN_GPU_ERROR_SHADER_INVALID;
        }
        vertex_seen |= vertex_bit;
        fragment_seen |= fragment_bit;
        desc->varyings[index].vertex_output_location =
            varying->vertex_output_location;
        desc->varyings[index].fragment_input_location =
            varying->fragment_input_location;
        desc->varyings[index].type = varying->type;
        desc->varyings[index].interpolation = varying->interpolation;
    }
    if (fragment_seen !=
        (fragment_info->input_count == 32u
             ? UINT32_MAX
             : (UINT32_C(1) << fragment_info->input_count) - 1u)) {
        return RIN_GPU_ERROR_SHADER_INVALID;
    }
    desc->position_output_location = position_output_location;
    desc->varying_count = varying_count;
    return RIN_GPU_OK;
}
