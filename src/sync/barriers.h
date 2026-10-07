// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_SYNC_BARRIERS_H
#define RIN_GPU_SYNC_BARRIERS_H

#include "../core/core.h"

static inline int ringpu_sync2_barrier_scopes_valid(
    uint32_t source_stage, uint32_t destination_stage,
    uint32_t source_access, uint32_t destination_access,
    uint32_t flags, uint32_t reserved)
{
    /* A zero stage cannot carry access. Access may independently be NONE for
     * an execution-only dependency; an entirely empty barrier is a no-op. */
    return (source_stage | destination_stage) != 0u &&
           (source_stage != 0u || source_access == 0u) &&
           (destination_stage != 0u || destination_access == 0u) &&
           (source_stage & ~RIN_GPU_PIPELINE_STAGE_KNOWN) == 0u &&
           (destination_stage & ~RIN_GPU_PIPELINE_STAGE_KNOWN) == 0u &&
           (source_access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) == 0u &&
           (destination_access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) == 0u &&
           flags == 0u && reserved == 0u;
}

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
