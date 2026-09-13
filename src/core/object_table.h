// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_OBJECT_TABLE_H
#define RIN_GPU_OBJECT_TABLE_H

#include "core.h"

int ringpu_slot_const(const RinGpuCore* core, RinGpuHandle handle,
                      uint16_t expected_type, uint32_t* index_out,
                      const RinGpuObjectSlot** slot_out);
int ringpu_slot(RinGpuCore* core, RinGpuHandle handle, uint16_t expected_type,
                uint32_t* index_out, RinGpuObjectSlot** slot_out);
int ringpu_allocate(RinGpuCore* core, uint16_t type, RinGpuHandle* handle,
                    RinGpuObjectSlot** slot_out);
void ringpu_release_slot(RinGpuObjectSlot* slot);

#endif /* RIN_GPU_OBJECT_TABLE_H */
