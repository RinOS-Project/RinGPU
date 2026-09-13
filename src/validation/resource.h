// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_RESOURCE_VALIDATION_H
#define RIN_GPU_RESOURCE_VALIDATION_H

#include "../core/core.h"

int ringpu_range(uint64_t offset, uint64_t size, uint64_t limit);
int ringpu_versioned(uint32_t version, uint32_t size, uint32_t required_size);
int ringpu_buffer_upload_ready(const RinGpuObjectSlot* slot);
int ringpu_image_upload_ready(const RinGpuObjectSlot* slot);
int ringpu_multiply_u64(uint64_t left, uint64_t right, uint64_t* result);
uint32_t ringpu_image_format_bytes(uint32_t format);
int ringpu_color_format(uint32_t format);
int ringpu_sampled_image_format(uint32_t format);
int ringpu_primitive_topology_valid(uint32_t topology);
int ringpu_depth_stencil_format(uint32_t format);
int ringpu_depth_aspect_format(uint32_t format);
int ringpu_stencil_aspect_format(uint32_t format);
int ringpu_scanout_format(uint32_t format);

#endif /* RIN_GPU_RESOURCE_VALIDATION_H */
