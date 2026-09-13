// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_SHADER_VALIDATION_H
#define RIN_GPU_SHADER_VALIDATION_H

#include "../core/core.h"

int ringpu_shader_resource_layout(const RinGpuObjectSlot* shader,
                                  uint32_t* access, uint32_t* kinds);

#endif /* RIN_GPU_SHADER_VALIDATION_H */
