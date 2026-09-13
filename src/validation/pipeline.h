// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_PIPELINE_VALIDATION_H
#define RIN_GPU_PIPELINE_VALIDATION_H

#include "../core/core.h"

int ringpu_finite_float(float value);
int ringpu_blend_constants_valid(uint32_t color_format,
                                 const float* constants);
int ringpu_raster_state_valid(const RinGpuRasterStateV1* state);
int ringpu_blend_factor_valid(uint32_t factor);
int ringpu_blend_source_factor_valid(uint32_t factor);
int ringpu_blend_factor_v2_valid(uint32_t factor);
int ringpu_blend_source_factor_v2_valid(uint32_t factor);
int ringpu_blend_operation_valid(uint32_t operation);
int ringpu_cull_mode_valid(uint32_t mode);
int ringpu_front_face_valid(uint32_t front_face);
int ringpu_varying_type_valid(uint32_t type);
int ringpu_varying_interpolation_valid(uint32_t interpolation);
int ringpu_vertex_format_valid(uint32_t format);
uint32_t ringpu_vertex_format_bytes(uint32_t format);
int ringpu_sampler_filter_valid(uint32_t filter);
int ringpu_sampler_mip_filter_valid(uint32_t filter);
int ringpu_sampler_address_valid(uint32_t address);
int ringpu_compare_op_valid(uint32_t compare);
int ringpu_stencil_op_valid(uint32_t operation);
int ringpu_sampler_desc_valid(const RinGpuSamplerDescV1* desc);
int ringpu_depth_pipeline_valid(uint32_t format, uint32_t compare,
                                uint32_t write_enabled, int allow_disabled);
int ringpu_stencil_face_valid(uint32_t compare, uint32_t reference,
                              uint32_t read_mask, uint32_t write_mask,
                              uint32_t stencil_fail, uint32_t depth_fail,
                              uint32_t depth_pass);
int ringpu_stencil_pipeline_valid(
    uint32_t depth_format, uint32_t enabled, uint32_t compare,
    uint32_t reference, uint32_t read_mask, uint32_t write_mask,
    uint32_t stencil_fail, uint32_t depth_fail, uint32_t depth_pass,
    uint32_t separate_enabled, uint32_t back_compare,
    uint32_t back_reference, uint32_t back_read_mask,
    uint32_t back_write_mask, uint32_t back_stencil_fail,
    uint32_t back_depth_fail, uint32_t back_depth_pass);
uint32_t ringpu_index_format_bytes(uint32_t format);
int ringpu_base_vertex_has_valid_index(int32_t base_vertex,
                                       uint32_t vertex_count,
                                       uint32_t index_stride);

#endif /* RIN_GPU_PIPELINE_VALIDATION_H */
