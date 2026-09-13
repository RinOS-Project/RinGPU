// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_PRESENTATION_PRESENT_H
#define RIN_GPU_PRESENTATION_PRESENT_H

#include "../core/core.h"

const RinGpuDisplayInfoV1* ringpu_find_display(
    const RinGpuCore* core, uint32_t display_id);
int ringpu_command_present(RinGpuCore* core, RinGpuHandle command_list,
                           const RinGpuPresentV1* present);

#endif /* RIN_GPU_PRESENTATION_PRESENT_H */
