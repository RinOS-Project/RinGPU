// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_COMMAND_RECORD_H
#define RIN_GPU_COMMAND_RECORD_H

#include "../core/core.h"

int ringpu_record_command(RinGpuObjectSlot* list,
                          RinGpuRecordedCommand** command_out);

#endif /* RIN_GPU_COMMAND_RECORD_H */
