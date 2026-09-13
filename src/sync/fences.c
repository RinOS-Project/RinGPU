// SPDX-License-Identifier: MIT
#include "fences.h"

#include "../core/object_table.h"

int ringpu_create_fence(RinGpuCore* core, uint64_t initial_value,
                        RinGpuHandle* fence)
{
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!fence) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_FENCE, fence, &slot);
    if (result == RIN_GPU_OK) {
        slot->value.fence.value = initial_value;
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_FENCE, *fence, 0u,
                               initial_value, 0u, RIN_GPU_OK);
    }
    return result;
}

int ringpu_fence_value(const RinGpuCore* core, RinGpuHandle fence,
                       uint64_t* value)
{
    const RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!value) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_slot_const(core, fence, RIN_GPU_OBJECT_FENCE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    *value = slot->value.fence.value;
    return RIN_GPU_OK;
}

int ringpu_wait_fence(RinGpuCore* core, RinGpuHandle fence,
                      uint64_t value, uint64_t timeout_ns)
{
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, fence, RIN_GPU_OBJECT_FENCE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if (value == 0u || value > slot->value.fence.value)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!core->backend.wait_for_completion)
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = core->backend.wait_for_completion(core->backend_context,
                                               timeout_ns);
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_QUEUE_WAIT, fence, 0u,
                           value, timeout_ns, result);
    return result;
}
