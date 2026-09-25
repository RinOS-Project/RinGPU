// SPDX-License-Identifier: MIT
#include "../core/core.h"
#include "../core/object_table.h"
#include "../command/record.h"
#include "../validation/resource.h"

#include <string.h>

static int ringpu_query_type_valid(uint32_t type)
{
    return type == RIN_GPU_QUERY_TIMESTAMP ||
           type == RIN_GPU_QUERY_OCCLUSION ||
           type == RIN_GPU_QUERY_PIPELINE_STATISTICS;
}

int ringpu_create_query(RinGpuCore* core, const RinGpuQueryDescV1* desc,
                        RinGpuHandle* query)
{
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !query ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        !ringpu_query_type_valid(desc->query_type) || desc->flags != 0u ||
        desc->reserved0 != 0u || desc->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *query = 0u;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_QUERY, query, &slot);
    if (result != RIN_GPU_OK) return result;
    slot->value.query.query_type = desc->query_type;
    return RIN_GPU_OK;
}

static int ringpu_record_query_command(RinGpuCore* core,
                                       RinGpuHandle command_list,
                                       RinGpuHandle query, uint32_t type)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* query_slot;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST,
                         NULL, &list);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, query, RIN_GPU_OBJECT_QUERY, NULL, &query_slot);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        !ringpu_query_type_valid(query_slot->value.query.query_type) ||
        (type != RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY &&
         type != RIN_GPU_BACKEND_COMMAND_END_QUERY &&
         type != RIN_GPU_BACKEND_COMMAND_RESET_QUERY)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = type;
    command->destination = query;
    command->value.query.query_type = query_slot->value.query.query_type;
    command->value.query.reserved = 0u;
    list->value.command_list.count++;
    return RIN_GPU_OK;
}

int ringpu_command_begin_query(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle query)
{
    return ringpu_record_query_command(core, command_list, query,
                                       RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY);
}

int ringpu_command_end_query(RinGpuCore* core, RinGpuHandle command_list,
                             RinGpuHandle query)
{
    return ringpu_record_query_command(core, command_list, query,
                                       RIN_GPU_BACKEND_COMMAND_END_QUERY);
}

int ringpu_command_reset_query(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle query)
{
    return ringpu_record_query_command(core, command_list, query,
                                       RIN_GPU_BACKEND_COMMAND_RESET_QUERY);
}

int ringpu_get_timestamp_period(const RinGpuCore* core,
                                uint64_t* period_nanoseconds)
{
    if (!core || !period_nanoseconds) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!core->initialized) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (core->device_lost) return RIN_GPU_ERROR_DEVICE_LOST;
    if (!core->backend.get_timestamp_period)
        return RIN_GPU_ERROR_UNSUPPORTED;
    *period_nanoseconds = 0u;
    return core->backend.get_timestamp_period(core->backend_context,
                                              period_nanoseconds);
}

int ringpu_get_query_result(RinGpuCore* core, RinGpuHandle query,
                            uint32_t flags, RinGpuQueryResultV1* result)
{
    RinGpuObjectSlot* slot;
    uint64_t values[RIN_GPU_QUERY_RESULT_VALUE_COUNT];
    uint32_t available = 0u;
    uint32_t type;
    int status;
    int result_code = ringpu_core_ready(core);

    if (result_code != RIN_GPU_OK) return result_code;
    if (!result || (flags & ~RIN_GPU_QUERY_RESULT_KNOWN_FLAGS) != 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result_code = ringpu_slot(core, query, RIN_GPU_OBJECT_QUERY, NULL, &slot);
    if (result_code != RIN_GPU_OK) return result_code;
    type = slot->value.query.query_type;
    memset(values, 0, sizeof(values));
    if (slot->value.query.available != 0u) {
        memcpy(values, slot->value.query.values, sizeof(values));
        available = 1u;
    } else {
        if (slot->value.query.active != 0u)
            return RIN_GPU_ERROR_BUSY;
        if (!core->backend.get_query_result)
            return RIN_GPU_ERROR_UNSUPPORTED;
        status = core->backend.get_query_result(
            core->backend_context, query, type, values, &available);
        if (status != RIN_GPU_OK) return status;
        if (available == 0u && (flags & RIN_GPU_QUERY_RESULT_WAIT) != 0u) {
            if (!core->backend.wait_for_completion)
                return RIN_GPU_ERROR_BUSY;
            status = core->backend.wait_for_completion(
                core->backend_context, RIN_GPU_TIMEOUT_INFINITE);
            if (status != RIN_GPU_OK) return status;
            status = core->backend.get_query_result(
                core->backend_context, query, type, values, &available);
            if (status != RIN_GPU_OK) return status;
        }
        if (available != 0u) {
            memcpy(slot->value.query.values, values, sizeof(values));
            slot->value.query.available = 1u;
        }
    }
    memset(result, 0, sizeof(*result));
    result->abi_version = RIN_GPU_ABI_VERSION;
    result->struct_size = sizeof(*result);
    result->query_type = type;
    result->available = available;
    if (available != 0u)
        memcpy(result->values, values, sizeof(result->values));
    return available != 0u ? RIN_GPU_OK : RIN_GPU_ERROR_BUSY;
}
