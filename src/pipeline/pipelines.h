// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_PIPELINE_PIPELINES_H
#define RIN_GPU_PIPELINE_PIPELINES_H

#include "../core/core.h"

int ringpu_create_compute_pipeline(
    RinGpuCore* core, const RinGpuComputePipelineDescV1* desc,
    RinGpuHandle* pipeline);
int ringpu_create_compute_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group);

int ringpu_graphics_image_kind(uint32_t kind);
int ringpu_graphics_sampler_kind(uint32_t kind);
uint32_t ringpu_graphics_binding_mip_count(
    const RinGpuGraphicsBindingV1* binding, const RinGpuImageDescV1* desc);
int ringpu_graphics_binding_slot(
    RinGpuCore* core, const RinGpuGraphicsBindingV1* binding,
    RinGpuObjectSlot** slot);
int ringpu_graphics_binding_reference(
    RinGpuCore* core, const RinGpuGraphicsBindingV1* binding, int acquire);
int ringpu_validate_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline_handle,
    const RinGpuObjectSlot* pipeline, RinGpuHandle bind_group_handle,
    RinGpuObjectSlot** bind_group_out);

#endif /* RIN_GPU_PIPELINE_PIPELINES_H */
