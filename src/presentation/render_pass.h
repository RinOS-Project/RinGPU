// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_PRESENTATION_RENDER_PASS_H
#define RIN_GPU_PRESENTATION_RENDER_PASS_H

#include "../core/core.h"

int ringpu_render_color_clear_valid(uint32_t load_op, float red, float green,
                                    float blue, float alpha,
                                    uint32_t color_write_mask);
int ringpu_render_color_clear_valid_for_format(
    uint32_t format, uint32_t load_op, float red, float green, float blue,
    float alpha, uint32_t color_write_mask);
int ringpu_clear_region_valid(const RinGpuClearRegionV1* region,
                             uint32_t width, uint32_t height);
int ringpu_render_clear_valid(const RinGpuRenderPassDescV1* pass);
int ringpu_render_depth_clear_valid(uint32_t load_op, float depth);
int ringpu_render_stencil_clear_valid(uint32_t format, uint32_t load_op,
                                      uint32_t store_op,
                                      uint32_t clear_stencil,
                                      uint32_t write_mask);
uint32_t ringpu_mip_dimension(uint32_t dimension, uint32_t mip_level);

#endif /* RIN_GPU_PRESENTATION_RENDER_PASS_H */
