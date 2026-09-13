/* SPDX-License-Identifier: MIT */
#include "fault_policy.h"

#include <string.h>

static int fault_type_valid(uint32_t type)
{
    return type >= RIN_GPU_FAULT_QUEUE_HANG &&
           type <= RIN_GPU_FAULT_SURPRISE_REMOVAL;
}

int rin_gpu_fault_policy_plan(const RinGpuFaultPolicyRequestV1* request,
                              RinGpuFaultPolicyDecisionV1* decision_out)
{
    RinGpuFaultPolicyRequestV1 snapshot;
    RinGpuFaultPolicyDecisionV1 decision;

    if (decision_out != NULL)
        memset(decision_out, 0, sizeof(*decision_out));
    if (request == NULL || decision_out == NULL)
        return RIN_GPU_FAULT_POLICY_INVALID_ARGUMENT;
    memcpy(&snapshot, request, sizeof(snapshot));
    if (snapshot.struct_size != sizeof(snapshot) ||
        snapshot.version != RIN_GPU_FAULT_POLICY_VERSION ||
        !fault_type_valid(snapshot.fault_type) ||
        (snapshot.capabilities & ~RIN_GPU_FAULT_CAP_KNOWN) != 0u ||
        snapshot.device_epoch == 0u ||
        snapshot.max_reset_attempts == 0u ||
        snapshot.reset_attempts > snapshot.max_reset_attempts ||
        snapshot.repeated_faults == 0u || snapshot.reserved[0] != 0u ||
        snapshot.reserved[1] != 0u) {
        return RIN_GPU_FAULT_POLICY_PROTOCOL;
    }

    memset(&decision, 0, sizeof(decision));
    decision.struct_size = sizeof(decision);
    decision.version = RIN_GPU_FAULT_POLICY_VERSION;
    decision.device_epoch = snapshot.device_epoch;
    decision.next_reset_attempt = snapshot.reset_attempts;

    if (snapshot.fault_type == RIN_GPU_FAULT_SURPRISE_REMOVAL ||
        snapshot.repeated_faults >= RIN_GPU_FAULT_POLICY_LOST_THRESHOLD) {
        decision.action = RIN_GPU_FAULT_ACTION_LOST;
        decision.terminal = 1u;
    } else if (snapshot.repeated_faults >=
               RIN_GPU_FAULT_POLICY_DEGRADED_THRESHOLD) {
        decision.action = RIN_GPU_FAULT_ACTION_DEGRADED;
        decision.terminal = 0u;
    } else if (snapshot.fault_type == RIN_GPU_FAULT_FIRMWARE &&
               (snapshot.capabilities & RIN_GPU_FAULT_CAP_FIRMWARE_RESTART) !=
                   0u) {
        decision.action = RIN_GPU_FAULT_ACTION_FIRMWARE_RESTART;
        decision.restart_firmware = 1u;
    } else if ((snapshot.fault_type == RIN_GPU_FAULT_QUEUE_HANG ||
                snapshot.fault_type == RIN_GPU_FAULT_TIMEOUT) &&
               (snapshot.capabilities & RIN_GPU_FAULT_CAP_QUEUE_RESET) != 0u &&
               snapshot.reset_attempts < snapshot.max_reset_attempts) {
        decision.action = RIN_GPU_FAULT_ACTION_QUEUE_RESET;
        decision.preserve_other_queues = 1u;
    } else if (snapshot.fault_type == RIN_GPU_FAULT_ENGINE &&
               (snapshot.capabilities & RIN_GPU_FAULT_CAP_ENGINE_RESET) != 0u &&
               snapshot.reset_attempts < snapshot.max_reset_attempts) {
        decision.action = RIN_GPU_FAULT_ACTION_ENGINE_RESET;
        decision.preserve_other_queues = 1u;
    } else {
        decision.action = RIN_GPU_FAULT_ACTION_DEVICE_RESET;
    }

    *decision_out = decision;
    return RIN_GPU_FAULT_POLICY_OK;
}
