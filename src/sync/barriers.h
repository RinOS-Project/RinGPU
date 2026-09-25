// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_SYNC_BARRIERS_H
#define RIN_GPU_SYNC_BARRIERS_H

#include "../core/core.h"

int ringpu_command_transition_image(
    RinGpuCore* core, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageTransitionV1* transition);
int ringpu_command_transfer_image_ownership(
    RinGpuCore* core, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageOwnershipTransferV1* transfer);
int ringpu_command_compute_barrier(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuComputeBarrierV1* barrier);
int ringpu_command_graphics_barrier(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuGraphicsBarrierV1* barrier);

#endif /* RIN_GPU_SYNC_BARRIERS_H */
