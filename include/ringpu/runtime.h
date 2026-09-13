// SPDX-License-Identifier: MIT
#ifndef RINGPU_PUBLIC_RUNTIME_H
#define RINGPU_PUBLIC_RUNTIME_H

#include <stdint.h>

#include "ringpu.h"
#include "software.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_RUNTIME_VERSION 1u

typedef struct RinGpuRuntime RinGpuRuntime;

/* Public host/runtime seam for a caller-owned software surface.  The
 * implementation owns the logical device, command recorder, and software
 * backend; the caller owns only callback storage and the final surface. */
typedef struct RinGpuRuntimeSoftwareSurfaceDescV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t device_generation;
    uint64_t handle_secret;
    uint64_t max_buffer_size;
    uint64_t max_image_size;
    uint64_t max_total_allocation_size;
    uint32_t max_image_dimension;
    uint32_t max_image_layers;
    uint32_t max_image_mip_levels;
    uint32_t max_image_sample_count;
    RinGpuAdapterInfoV1 adapter;
    RinGpuDisplayInfoV1 display;
    RinGpuSoftwarePresentCallbackV1 present_callback;
    void* present_context;
    RinGpuSoftwareAcquireImageCallbackV1 acquire_image;
    void* image_context;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t reserved[2];
} RinGpuRuntimeSoftwareSurfaceDescV1;

int ringpu_runtime_software_surface_create(
    const RinGpuRuntimeSoftwareSurfaceDescV1* desc,
    RinGpuRuntime** runtime_out);
void ringpu_runtime_destroy(RinGpuRuntime* runtime);
int ringpu_runtime_device_lost(const RinGpuRuntime* runtime);

int ringpu_runtime_create_queue(RinGpuRuntime* runtime,
                                const RinGpuQueueDescV1* desc,
                                RinGpuHandle* queue_out);
int ringpu_runtime_create_command_list(
    RinGpuRuntime* runtime, const RinGpuCommandListDescV1* desc,
    RinGpuHandle* command_list_out);
int ringpu_runtime_create_image(RinGpuRuntime* runtime,
                                const RinGpuImageDescV1* desc,
                                RinGpuHandle* image_out);
int ringpu_runtime_command_list_reset(RinGpuRuntime* runtime,
                                      RinGpuHandle command_list);
int ringpu_runtime_command_list_close(RinGpuRuntime* runtime,
                                      RinGpuHandle command_list);
int ringpu_runtime_command_transition_image(
    RinGpuRuntime* runtime, RinGpuHandle command_list, RinGpuHandle image,
    const RinGpuImageTransitionV1* transition);
int ringpu_runtime_queue_submit(RinGpuRuntime* runtime, RinGpuHandle queue,
                                const RinGpuSubmitInfoV1* submit);

#ifdef __cplusplus
}
#endif

#endif /* RINGPU_PUBLIC_RUNTIME_H */
