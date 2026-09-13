// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_RESOURCE_VALIDATION_H
#define RIN_GPU_RESOURCE_VALIDATION_H

#include "../core/core.h"

int ringpu_range(uint64_t offset, uint64_t size, uint64_t limit);
int ringpu_versioned(uint32_t version, uint32_t size, uint32_t required_size);
int ringpu_buffer_upload_ready(const RinGpuObjectSlot* slot);
int ringpu_image_upload_ready(const RinGpuObjectSlot* slot);
int ringpu_image_allocation_size(const RinGpuCore* core,
                                 const RinGpuImageDescV1* desc,
                                 uint64_t* size_out);
uint32_t ringpu_image_mip_extent(uint32_t extent, uint32_t mip_level);
int ringpu_image_copy_side_valid(
    const RinGpuImageDescV1* desc, uint32_t mip_level, uint32_t array_layer,
    uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height,
    uint32_t depth);
int ringpu_image_upload_source_valid(
    const RinGpuImageDescV1* desc, RinGpuImageUploadV1* upload,
    uint64_t source_size);
int ringpu_image_readback_destination_valid(
    const RinGpuImageDescV1* desc, RinGpuImageReadbackV1* readback,
    uint64_t destination_size);
int ringpu_image_state_allowed(const RinGpuImageDescV1* desc, uint32_t state);
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
