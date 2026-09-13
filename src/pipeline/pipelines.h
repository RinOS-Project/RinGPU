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

#endif /* RIN_GPU_PIPELINE_PIPELINES_H */
