// SPDX-License-Identifier: MIT
#include "pipeline.h"
#include "resource.h"

int ringpu_finite_float(float value)
{
    return value == value && value <= 3.402823466e+38f &&
           value >= -3.402823466e+38f;
}

int ringpu_blend_constants_valid(uint32_t color_format,
                                 const float* constants)
{
    if (constants == NULL || !ringpu_finite_float(constants[0]) ||
        !ringpu_finite_float(constants[1]) ||
        !ringpu_finite_float(constants[2]) ||
        !ringpu_finite_float(constants[3])) {
        return 0;
    }
    /* Float color targets preserve finite blend constants outside the
     * normalized range. Fixed-point targets remain deliberately bounded. */
    return color_format == RIN_GPU_FORMAT_RGBA16_FLOAT ||
           color_format == RIN_GPU_FORMAT_RGBA32_FLOAT ||
           (constants[0] >= 0.0f && constants[0] <= 1.0f &&
            constants[1] >= 0.0f && constants[1] <= 1.0f &&
            constants[2] >= 0.0f && constants[2] <= 1.0f &&
            constants[3] >= 0.0f && constants[3] <= 1.0f);
}

int ringpu_raster_state_valid(const RinGpuRasterStateV1* state)
{
    const RinGpuViewportV1* viewport;
    const RinGpuScissorV1* scissor;

    if (!state || !ringpu_versioned(state->abi_version, state->struct_size,
                                    sizeof(*state)) ||
        (state->struct_size != sizeof(RinGpuRasterStateV1) &&
         state->struct_size != sizeof(RinGpuRasterStateV2) &&
         state->struct_size != sizeof(RinGpuRasterStateV3) &&
         state->struct_size != sizeof(RinGpuRasterStateV4) &&
         state->struct_size != sizeof(RinGpuRasterStateV5)) ||
        state->flags != 0u || state->reserved != 0u) {
        return 0;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV2) ||
        state->struct_size == sizeof(RinGpuRasterStateV3) ||
        state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV2* extended =
            (const RinGpuRasterStateV2*)(const void*)state;
        if (extended->polygon_offset_fill_enabled > 1u ||
            extended->reserved0 != 0u ||
            !ringpu_finite_float(extended->polygon_offset_factor) ||
            !ringpu_finite_float(extended->polygon_offset_units)) {
            return 0;
        }
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV3) ||
        state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV3* extended =
            (const RinGpuRasterStateV3*)(const void*)state;
        if (!ringpu_finite_float(extended->line_width) ||
            extended->line_width < 1.0f || extended->line_width > 64.0f ||
            extended->reserved1 != 0u) {
            return 0;
        }
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV4* extended =
            (const RinGpuRasterStateV4*)(const void*)state;
        if (extended->sample_coverage_enabled > 1u ||
            extended->sample_coverage_invert > 1u ||
            !ringpu_finite_float(extended->sample_coverage_value) ||
            extended->sample_coverage_value < 0.0f ||
            extended->sample_coverage_value > 1.0f ||
            extended->reserved2 != 0u) {
            return 0;
        }
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV5* extended =
            (const RinGpuRasterStateV5*)(const void*)state;
        if (extended->dither_enabled > 1u || extended->reserved3 != 0u)
            return 0;
    }
    viewport = &state->viewport;
    scissor = &state->scissor;
    if (!ringpu_versioned(viewport->abi_version, viewport->struct_size,
                          sizeof(*viewport)) ||
        !ringpu_finite_float(viewport->x) ||
        !ringpu_finite_float(viewport->y) ||
        !ringpu_finite_float(viewport->width) ||
        !ringpu_finite_float(viewport->height) ||
        !ringpu_finite_float(viewport->min_depth) ||
        !ringpu_finite_float(viewport->max_depth) ||
        viewport->width <= 0.0f || viewport->height <= 0.0f ||
        viewport->min_depth < 0.0f || viewport->min_depth > 1.0f ||
        viewport->max_depth < 0.0f || viewport->max_depth > 1.0f ||
        viewport->flags != 0u || viewport->reserved != 0u ||
        !ringpu_versioned(scissor->abi_version, scissor->struct_size,
                          sizeof(*scissor)) || scissor->enabled > 1u ||
        scissor->flags != 0u || scissor->reserved0 != 0u ||
        scissor->reserved1 != 0u) {
        return 0;
    }
    if (scissor->enabled == 0u)
        return scissor->x == 0 && scissor->y == 0 &&
               scissor->width == 0u && scissor->height == 0u;
    return scissor->x >= 0 && scissor->y >= 0;
}

int ringpu_blend_factor_valid(uint32_t factor)
{
    return factor >= RIN_GPU_BLEND_ZERO &&
           factor <= RIN_GPU_BLEND_ONE_MINUS_DESTINATION_COLOR;
}

int ringpu_blend_source_factor_valid(uint32_t factor)
{
    return ringpu_blend_factor_valid(factor) ||
           factor == RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE;
}

int ringpu_blend_factor_v2_valid(uint32_t factor)
{
    return (factor >= RIN_GPU_BLEND_ZERO &&
            factor <= RIN_GPU_BLEND_ONE_MINUS_DESTINATION_COLOR) ||
           (factor >= RIN_GPU_BLEND_CONSTANT_COLOR &&
            factor <= RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA);
}

int ringpu_blend_source_factor_v2_valid(uint32_t factor)
{
    return ringpu_blend_factor_v2_valid(factor) ||
           factor == RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE;
}

int ringpu_blend_operation_valid(uint32_t operation)
{
    return operation >= RIN_GPU_BLEND_ADD &&
           operation <= RIN_GPU_BLEND_MAXIMUM;
}

int ringpu_cull_mode_valid(uint32_t mode)
{
    return mode >= RIN_GPU_CULL_NONE && mode <= RIN_GPU_CULL_BACK;
}

int ringpu_front_face_valid(uint32_t front_face)
{
    return front_face == RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE ||
           front_face == RIN_GPU_FRONT_FACE_CLOCKWISE;
}

int ringpu_varying_type_valid(uint32_t type)
{
    return type == RIN_GPU_VARYING_FLOAT32 ||
           type == RIN_GPU_VARYING_SINT32;
}

int ringpu_varying_interpolation_valid(uint32_t interpolation)
{
    return interpolation == RIN_GPU_INTERPOLATION_PERSPECTIVE ||
           interpolation == RIN_GPU_INTERPOLATION_NO_PERSPECTIVE ||
           interpolation == RIN_GPU_INTERPOLATION_FLAT;
}

int ringpu_vertex_format_valid(uint32_t format)
{
    switch (format) {
    case RIN_GPU_VERTEX_UINT32:
    case RIN_GPU_VERTEX_SINT32:
    case RIN_GPU_VERTEX_FLOAT32:
    case RIN_GPU_VERTEX_UINT8:
    case RIN_GPU_VERTEX_SINT8:
    case RIN_GPU_VERTEX_UNORM8:
    case RIN_GPU_VERTEX_SNORM8:
    case RIN_GPU_VERTEX_UINT16:
    case RIN_GPU_VERTEX_SINT16:
    case RIN_GPU_VERTEX_UNORM16:
    case RIN_GPU_VERTEX_SNORM16:
        return 1;
    default:
        return 0;
    }
}

uint32_t ringpu_vertex_format_bytes(uint32_t format)
{
    switch (format) {
    case RIN_GPU_VERTEX_UINT8:
    case RIN_GPU_VERTEX_SINT8:
    case RIN_GPU_VERTEX_UNORM8:
    case RIN_GPU_VERTEX_SNORM8:
        return 1u;
    case RIN_GPU_VERTEX_UINT16:
    case RIN_GPU_VERTEX_SINT16:
    case RIN_GPU_VERTEX_UNORM16:
    case RIN_GPU_VERTEX_SNORM16:
        return 2u;
    case RIN_GPU_VERTEX_UINT32:
    case RIN_GPU_VERTEX_SINT32:
    case RIN_GPU_VERTEX_FLOAT32:
        return 4u;
    default:
        return 0u;
    }
}

int ringpu_sampler_filter_valid(uint32_t filter)
{
    return filter == RIN_GPU_SAMPLER_FILTER_NEAREST ||
           filter == RIN_GPU_SAMPLER_FILTER_LINEAR;
}

int ringpu_sampler_mip_filter_valid(uint32_t filter)
{
    return filter == RIN_GPU_SAMPLER_MIP_FILTER_NONE ||
           ringpu_sampler_filter_valid(filter);
}

int ringpu_sampler_address_valid(uint32_t address)
{
    return address == RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE ||
           address == RIN_GPU_SAMPLER_ADDRESS_REPEAT ||
           address == RIN_GPU_SAMPLER_ADDRESS_MIRRORED_REPEAT;
}

int ringpu_compare_op_valid(uint32_t compare)
{
    return compare == RIN_GPU_COMPARE_NEVER ||
           compare == RIN_GPU_COMPARE_LESS ||
           compare == RIN_GPU_COMPARE_EQUAL ||
           compare == RIN_GPU_COMPARE_LESS_EQUAL ||
           compare == RIN_GPU_COMPARE_GREATER ||
           compare == RIN_GPU_COMPARE_NOT_EQUAL ||
           compare == RIN_GPU_COMPARE_GREATER_EQUAL ||
           compare == RIN_GPU_COMPARE_ALWAYS;
}

int ringpu_stencil_op_valid(uint32_t operation)
{
    return operation == RIN_GPU_STENCIL_KEEP ||
           operation == RIN_GPU_STENCIL_ZERO ||
           operation == RIN_GPU_STENCIL_REPLACE ||
           operation == RIN_GPU_STENCIL_INCREMENT_CLAMP ||
           operation == RIN_GPU_STENCIL_DECREMENT_CLAMP ||
           operation == RIN_GPU_STENCIL_INVERT ||
           operation == RIN_GPU_STENCIL_INCREMENT_WRAP ||
           operation == RIN_GPU_STENCIL_DECREMENT_WRAP;
}

int ringpu_sampler_desc_valid(const RinGpuSamplerDescV1* desc)
{
    if (!desc ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_sampler_filter_valid(desc->min_filter) ||
        !ringpu_sampler_filter_valid(desc->mag_filter) ||
        !ringpu_sampler_mip_filter_valid(desc->mip_filter) ||
        !ringpu_sampler_address_valid(desc->address_u) ||
        !ringpu_sampler_address_valid(desc->address_v) ||
        !ringpu_sampler_address_valid(desc->address_w) ||
        desc->mip_lod_bias != desc->mip_lod_bias ||
        desc->mip_lod_bias < -16.0f || desc->mip_lod_bias > 16.0f ||
        desc->min_lod != desc->min_lod || desc->min_lod < 0.0f ||
        desc->min_lod > 32.0f || desc->max_lod != desc->max_lod ||
        desc->max_lod < desc->min_lod || desc->max_lod > 32.0f ||
        desc->max_anisotropy == 0u ||
        desc->max_anisotropy > RIN_GPU_MAX_SAMPLER_ANISOTROPY ||
        desc->flags != 0u ||
        (desc->compare_op != 0u &&
         !ringpu_compare_op_valid(desc->compare_op))) {
        return 0;
    }
    return 1;
}

int ringpu_depth_pipeline_valid(uint32_t format, uint32_t compare,
                                uint32_t write_enabled, int allow_disabled)
{
    if (allow_disabled && format == 0u)
        return compare == 0u && write_enabled == 0u;
    if (format == RIN_GPU_FORMAT_S8_UINT)
        return compare == 0u && write_enabled == 0u;
    return (format == RIN_GPU_FORMAT_D32_FLOAT ||
            format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) &&
           ringpu_compare_op_valid(compare) && write_enabled <= 1u;
}

int ringpu_stencil_face_valid(uint32_t compare, uint32_t reference,
                              uint32_t read_mask, uint32_t write_mask,
                              uint32_t stencil_fail, uint32_t depth_fail,
                              uint32_t depth_pass)
{
    return ringpu_compare_op_valid(compare) && reference <= 0xffu &&
           read_mask <= 0xffu && write_mask <= 0xffu &&
           ringpu_stencil_op_valid(stencil_fail) &&
           ringpu_stencil_op_valid(depth_fail) &&
           ringpu_stencil_op_valid(depth_pass);
}

int ringpu_stencil_pipeline_valid(
    uint32_t depth_format, uint32_t enabled, uint32_t compare,
    uint32_t reference, uint32_t read_mask, uint32_t write_mask,
    uint32_t stencil_fail, uint32_t depth_fail, uint32_t depth_pass,
    uint32_t separate_enabled, uint32_t back_compare,
    uint32_t back_reference, uint32_t back_read_mask,
    uint32_t back_write_mask, uint32_t back_stencil_fail,
    uint32_t back_depth_fail, uint32_t back_depth_pass)
{
    if (enabled == 0u) {
        return compare == 0u && reference == 0u && read_mask == 0u &&
               write_mask == 0u && stencil_fail == 0u && depth_fail == 0u &&
               depth_pass == 0u && separate_enabled == 0u &&
               back_compare == 0u && back_reference == 0u &&
               back_read_mask == 0u && back_write_mask == 0u &&
               back_stencil_fail == 0u && back_depth_fail == 0u &&
               back_depth_pass == 0u;
    }
    if (enabled != 1u ||
        (depth_format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
         depth_format != RIN_GPU_FORMAT_S8_UINT) ||
        !ringpu_stencil_face_valid(compare, reference, read_mask, write_mask,
                                   stencil_fail, depth_fail, depth_pass)) {
        return 0;
    }
    if (separate_enabled == 0u) {
        return back_compare == 0u && back_reference == 0u &&
               back_read_mask == 0u && back_write_mask == 0u &&
               back_stencil_fail == 0u && back_depth_fail == 0u &&
               back_depth_pass == 0u;
    }
    return separate_enabled == 1u && ringpu_stencil_face_valid(
        back_compare, back_reference, back_read_mask, back_write_mask,
        back_stencil_fail, back_depth_fail, back_depth_pass);
}

uint32_t ringpu_index_format_bytes(uint32_t format)
{
    if (format == RIN_GPU_INDEX_UINT8) return 1u;
    if (format == RIN_GPU_INDEX_UINT16) return 2u;
    if (format == RIN_GPU_INDEX_UINT32) return 4u;
    return 0u;
}

int ringpu_base_vertex_has_valid_index(int32_t base_vertex,
                                       uint32_t vertex_count,
                                       uint32_t index_stride)
{
    uint64_t first_index;
    uint64_t largest_index;
    if (vertex_count == 0u ||
        (index_stride != 1u && index_stride != 2u && index_stride != 4u)) {
        return 0;
    }
    if (base_vertex >= 0)
        return (uint32_t)base_vertex < vertex_count;
    first_index = (uint64_t)(-(int64_t)base_vertex);
    largest_index = index_stride == 1u ? UINT8_MAX :
        index_stride == 2u ? UINT16_MAX : UINT32_MAX;
    return first_index <= largest_index;
}
