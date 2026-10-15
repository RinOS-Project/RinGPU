// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_SYNC_BARRIERS_H
#define RIN_GPU_SYNC_BARRIERS_H

#include "../core/core.h"

static inline int ringpu_sync2_barrier_scopes_valid(
    uint32_t source_stage, uint32_t destination_stage,
    uint32_t source_access, uint32_t destination_access,
    uint32_t flags, uint32_t reserved)
{
    const uint32_t source_access_stages =
        source_stage & ~(RIN_GPU_PIPELINE_STAGE_TOP_OF_PIPE |
                         RIN_GPU_PIPELINE_STAGE_BOTTOM_OF_PIPE);
    const uint32_t destination_access_stages =
        destination_stage & ~(RIN_GPU_PIPELINE_STAGE_TOP_OF_PIPE |
                             RIN_GPU_PIPELINE_STAGE_BOTTOM_OF_PIPE);

    /* Synchronization2 permits NONE scopes and execution-only dependencies.
     * A zero stage cannot carry a non-empty access scope. TOP_OF_PIPE and
     * BOTTOM_OF_PIPE are execution-only endpoints too; a non-empty memory
     * access scope needs at least one stage that can perform accesses. */
    return (source_stage != 0u || source_access == 0u) &&
           (destination_stage != 0u || destination_access == 0u) &&
           (source_access == 0u || source_access_stages != 0u) &&
           (destination_access == 0u || destination_access_stages != 0u) &&
           (source_stage & ~RIN_GPU_PIPELINE_STAGE_KNOWN) == 0u &&
           (destination_stage & ~RIN_GPU_PIPELINE_STAGE_KNOWN) == 0u &&
           (source_access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) == 0u &&
           (destination_access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) == 0u &&
           flags == 0u && reserved == 0u;
}

static inline int ringpu_sync2_barrier_stages_supported(
    uint32_t stages, uint32_t queue_capabilities)
{
    const uint32_t graphics_stages =
        RIN_GPU_PIPELINE_STAGE_VERTEX_INPUT |
        RIN_GPU_PIPELINE_STAGE_VERTEX_SHADER |
        RIN_GPU_PIPELINE_STAGE_FRAGMENT_SHADER |
        RIN_GPU_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS |
        RIN_GPU_PIPELINE_STAGE_LATE_FRAGMENT_TESTS |
        RIN_GPU_PIPELINE_STAGE_COLOR_OUTPUT;

    if ((stages & ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
        ((stages & RIN_GPU_PIPELINE_STAGE_COPY) != 0u &&
         (queue_capabilities & RIN_GPU_QUEUE_COPY) == 0u) ||
        ((stages & RIN_GPU_PIPELINE_STAGE_COMPUTE_SHADER) != 0u &&
         (queue_capabilities & RIN_GPU_QUEUE_COMPUTE) == 0u) ||
        ((stages & graphics_stages) != 0u &&
         (queue_capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u)) {
        return 0;
    }
    return 1;
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
