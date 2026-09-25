// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_COMMAND_COMMANDS_H
#define RIN_GPU_COMMAND_COMMANDS_H

#include "../core/core.h"

int ringpu_command_copy_buffer(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle destination,
                               uint64_t destination_offset,
                               RinGpuHandle source, uint64_t source_offset,
                               uint64_t size_bytes);
int ringpu_command_clear_buffer(RinGpuCore* core, RinGpuHandle command_list,
                                RinGpuHandle destination,
                                const RinGpuBufferClearV1* clear);
int ringpu_command_copy_image(RinGpuCore* core, RinGpuHandle command_list,
                              RinGpuHandle destination, RinGpuHandle source,
                              const RinGpuImageCopyRegionV1* region);
int ringpu_command_clear_image(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle destination,
                               const RinGpuImageClearV1* clear);
int ringpu_command_dispatch(RinGpuCore* core, RinGpuHandle command_list,
                            const RinGpuDispatchV1* dispatch);

#endif /* RIN_GPU_COMMAND_COMMANDS_H */
