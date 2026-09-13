/* SPDX-License-Identifier: MIT */
#ifndef RIN_SUBSYSTEMS_RINGPU_FAULT_POLICY_H
#define RIN_SUBSYSTEMS_RINGPU_FAULT_POLICY_H

#include <stdint.h>

#define RIN_GPU_FAULT_POLICY_VERSION 1u
#define RIN_GPU_FAULT_POLICY_DEGRADED_THRESHOLD 3u
#define RIN_GPU_FAULT_POLICY_LOST_THRESHOLD 6u

typedef enum RinGpuFaultPolicyFaultTypeV1 {
    RIN_GPU_FAULT_QUEUE_HANG = 1u,
    RIN_GPU_FAULT_ENGINE = 2u,
    RIN_GPU_FAULT_FIRMWARE = 3u,
    RIN_GPU_FAULT_MMU_PAGE = 4u,
    RIN_GPU_FAULT_DISPLAY = 5u,
    RIN_GPU_FAULT_TIMEOUT = 6u,
    RIN_GPU_FAULT_SURPRISE_REMOVAL = 7u
} RinGpuFaultPolicyFaultTypeV1;

typedef enum RinGpuFaultPolicyActionV1 {
    RIN_GPU_FAULT_ACTION_NONE = 0u,
    RIN_GPU_FAULT_ACTION_QUEUE_RESET = 1u,
    RIN_GPU_FAULT_ACTION_ENGINE_RESET = 2u,
    RIN_GPU_FAULT_ACTION_FIRMWARE_RESTART = 3u,
    RIN_GPU_FAULT_ACTION_DEVICE_RESET = 4u,
    RIN_GPU_FAULT_ACTION_DEGRADED = 5u,
    RIN_GPU_FAULT_ACTION_LOST = 6u
} RinGpuFaultPolicyActionV1;

#define RIN_GPU_FAULT_CAP_QUEUE_RESET UINT32_C(0x00000001)
#define RIN_GPU_FAULT_CAP_ENGINE_RESET UINT32_C(0x00000002)
#define RIN_GPU_FAULT_CAP_FIRMWARE_RESTART UINT32_C(0x00000004)
#define RIN_GPU_FAULT_CAP_KNOWN \
    (RIN_GPU_FAULT_CAP_QUEUE_RESET | RIN_GPU_FAULT_CAP_ENGINE_RESET | \
     RIN_GPU_FAULT_CAP_FIRMWARE_RESTART)

typedef struct RinGpuFaultPolicyRequestV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t fault_type;
    uint32_t capabilities;
    uint32_t repeated_faults;
    uint32_t reset_attempts;
    uint32_t max_reset_attempts;
    uint32_t queue_id;
    uint64_t device_epoch;
    uint64_t reserved[2];
} RinGpuFaultPolicyRequestV1;

typedef struct RinGpuFaultPolicyDecisionV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t action;
    uint32_t terminal;
    uint32_t preserve_other_queues;
    uint32_t restart_firmware;
    uint32_t next_reset_attempt;
    uint32_t reserved0;
    uint64_t device_epoch;
} RinGpuFaultPolicyDecisionV1;

typedef enum RinGpuFaultPolicyResult {
    RIN_GPU_FAULT_POLICY_OK = 0,
    RIN_GPU_FAULT_POLICY_INVALID_ARGUMENT = -1,
    RIN_GPU_FAULT_POLICY_PROTOCOL = -2
} RinGpuFaultPolicyResult;

/* Selects the least disruptive recovery action which the backend explicitly
 * supports. The decision contains no implicit fallback: a caller must
 * execute the selected action and report its result through the device
 * runtime. */
int rin_gpu_fault_policy_plan(const RinGpuFaultPolicyRequestV1* request,
                              RinGpuFaultPolicyDecisionV1* decision_out);

#if defined(__cplusplus)
static_assert(sizeof(RinGpuFaultPolicyRequestV1) == 56u,
              "RinGPU fault-policy request drift");
static_assert(sizeof(RinGpuFaultPolicyDecisionV1) == 40u,
              "RinGPU fault-policy decision drift");
#else
_Static_assert(sizeof(RinGpuFaultPolicyRequestV1) == 56u,
               "RinGPU fault-policy request drift");
_Static_assert(sizeof(RinGpuFaultPolicyDecisionV1) == 40u,
               "RinGPU fault-policy decision drift");
#endif

#endif /* RIN_SUBSYSTEMS_RINGPU_FAULT_POLICY_H */
