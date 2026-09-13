// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_SHADER_MODULES_H
#define RIN_GPU_SHADER_MODULES_H

#include "../core/core.h"

int ringpu_create_shader_module(RinGpuCore* core, const void* rin_shader_ir,
                                uint64_t shader_size,
                                RinGpuHandle* shader_module);
int ringpu_get_shader_info(const RinGpuCore* core,
                           RinGpuHandle shader_module,
                           RinShaderInfoV1* info);
void ringpu_shader_cache_release(RinGpuCore* core, uint32_t index);

#endif /* RIN_GPU_SHADER_MODULES_H */
