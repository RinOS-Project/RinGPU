// SPDX-License-Identifier: MIT
#include "record.h"

#include <stdlib.h>
#include <string.h>

int ringpu_record_command(RinGpuObjectSlot* list,
                          RinGpuRecordedCommand** command_out)
{
    RinGpuRecordedCommand* commands;
    uint32_t next_capacity;

    if (!list || !command_out) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *command_out = NULL;
    if (list->value.command_list.count == RIN_GPU_CORE_MAX_COMMANDS)
        return RIN_GPU_ERROR_LIMIT;
    if (list->value.command_list.count == list->value.command_list.capacity) {
        next_capacity = list->value.command_list.capacity == 0u ? 8u :
                        list->value.command_list.capacity * 2u;
        if (next_capacity > RIN_GPU_CORE_MAX_COMMANDS)
            next_capacity = RIN_GPU_CORE_MAX_COMMANDS;
        commands = (RinGpuRecordedCommand*)realloc(
            list->value.command_list.commands,
            (size_t)next_capacity * sizeof(*commands));
        if (!commands) return RIN_GPU_ERROR_NO_MEMORY;
        list->value.command_list.commands = commands;
        list->value.command_list.capacity = next_capacity;
    }
    *command_out = &list->value.command_list.commands[
        list->value.command_list.count];
    memset(*command_out, 0, sizeof(**command_out));
    return RIN_GPU_OK;
}
