// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_SYNC_FENCES_H
#define RIN_GPU_SYNC_FENCES_H

#include "../core/core.h"

int ringpu_create_fence(RinGpuCore* core, uint64_t initial_value,
                        RinGpuHandle* fence);
int ringpu_fence_value(const RinGpuCore* core, RinGpuHandle fence,
                       uint64_t* value);
int ringpu_wait_fence(RinGpuCore* core, RinGpuHandle fence,
                      uint64_t value, uint64_t timeout_ns);

#endif /* RIN_GPU_SYNC_FENCES_H */
