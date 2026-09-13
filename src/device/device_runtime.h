/* SPDX-License-Identifier: MIT */
#ifndef RIN_SUBSYSTEMS_RINGPU_DEVICE_RUNTIME_H
#define RIN_SUBSYSTEMS_RINGPU_DEVICE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "../validation/diagnostics.h"

#define RIN_GPU_DEVICE_RUNTIME_VERSION 1u
#define RIN_GPU_DEVICE_RUNTIME_STATE_QWORDS 576u
#define RIN_GPU_DEVICE_MAX_QUEUES 8u
#define RIN_GPU_DEVICE_MAX_IN_FLIGHT 64u
#define RIN_GPU_DEVICE_MAX_RESET_ATTEMPTS 8u
#define RIN_GPU_DEVICE_MIN_TIMEOUT_NS UINT64_C(1000000)
#define RIN_GPU_DEVICE_MAX_TIMEOUT_NS UINT64_C(30000000000)

#if UINTPTR_MAX == UINT32_MAX
#define RIN_GPU_DEVICE_BACKEND_RESERVED_QWORDS 8u
#else
#define RIN_GPU_DEVICE_BACKEND_RESERVED_QWORDS 5u
#endif

#define RIN_GPU_DEVICE_CAP_DMA_ISOLATED UINT32_C(0x00000001)
#define RIN_GPU_DEVICE_CAP_RESET UINT32_C(0x00000002)
#define RIN_GPU_DEVICE_CAP_POWER UINT32_C(0x00000004)
#define RIN_GPU_DEVICE_CAP_KNOWN                                      \
    (RIN_GPU_DEVICE_CAP_DMA_ISOLATED | RIN_GPU_DEVICE_CAP_RESET |     \
     RIN_GPU_DEVICE_CAP_POWER)

#define RIN_GPU_DEVICE_STATUS_ACTIVE UINT32_C(0x00000001)
#define RIN_GPU_DEVICE_STATUS_SUSPENDED UINT32_C(0x00000002)
#define RIN_GPU_DEVICE_STATUS_LOST UINT32_C(0x00000004)

#define RIN_GPU_DEVICE_REPORT_RESET UINT32_C(0x00000001)
#define RIN_GPU_DEVICE_REPORT_WATCHDOG UINT32_C(0x00000002)
#define RIN_GPU_DEVICE_REPORT_DEVICE_FAULT UINT32_C(0x00000004)

#define RIN_GPU_DEVICE_POWER_SUSPEND 1u
#define RIN_GPU_DEVICE_POWER_RESUME 2u

typedef enum RinGpuDeviceRuntimeResult {
    RIN_GPU_DEVICE_OK = 0,
    RIN_GPU_DEVICE_INVALID_ARGUMENT = -1,
    RIN_GPU_DEVICE_STATE = -2,
    RIN_GPU_DEVICE_BUSY = -3,
    RIN_GPU_DEVICE_BACKEND_FAILED = -4,
    RIN_GPU_DEVICE_PROTOCOL = -5,
    RIN_GPU_DEVICE_LOST = -6,
    RIN_GPU_DEVICE_TIMEOUT = -7,
    RIN_GPU_DEVICE_LIMIT = -8,
    RIN_GPU_DEVICE_STALE = -9
} RinGpuDeviceRuntimeResult;

typedef enum RinGpuDeviceCompletionStatus {
    RIN_GPU_DEVICE_COMPLETION_OK = 0,
    RIN_GPU_DEVICE_COMPLETION_FAULT = 1
} RinGpuDeviceCompletionStatus;

/* Vendor owners normalize hardware-specific fault reports to this common
 * taxonomy before they enter the runtime.  The runtime still owns the
 * escalation decision; a physical driver must not smuggle a register value
 * through the generic completion ABI. */
typedef enum RinGpuDeviceFaultTypeV1 {
    RIN_GPU_DEVICE_FAULT_QUEUE_HANG = 1,
    RIN_GPU_DEVICE_FAULT_ENGINE = 2,
    RIN_GPU_DEVICE_FAULT_FIRMWARE = 3,
    RIN_GPU_DEVICE_FAULT_MMU_PAGE = 4,
    RIN_GPU_DEVICE_FAULT_DISPLAY = 5,
    RIN_GPU_DEVICE_FAULT_TIMEOUT = 6,
    RIN_GPU_DEVICE_FAULT_SURPRISE_REMOVAL = 7
} RinGpuDeviceFaultTypeV1;

typedef struct RinGpuDeviceSubmissionV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t queue_id;
    uint32_t flags;
    uint64_t sequence;
    uint64_t command_cookie;
    uint64_t completion_value;
    uint64_t iommu_domain_cookie;
    uint64_t iommu_map_generation;
    uint64_t device_epoch;
    uint64_t deadline_ns;
    uint64_t reserved;
} RinGpuDeviceSubmissionV1;

typedef struct RinGpuDeviceCompletionV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t queue_id;
    uint32_t status;
    uint64_t sequence;
    uint64_t completion_value;
    uint64_t device_epoch;
    uint64_t reserved;
} RinGpuDeviceCompletionV1;

typedef uint64_t (*RinGpuDeviceNowNsFn)(void* context);
typedef int (*RinGpuDeviceSubmitFn)(
    void* context, const RinGpuDeviceSubmissionV1* submission);
typedef int (*RinGpuDevicePollFn)(
    void* context, RinGpuDeviceCompletionV1* completions,
    uint32_t capacity, uint32_t* completion_count);
/* Returning success asserts that old-epoch execution and DMA have stopped.
 * The returned mapping generation must be strictly newer. */
typedef int (*RinGpuDeviceResetFn)(
    void* context, uint64_t expected_epoch, uint64_t new_epoch,
    uint64_t deadline_ns, uint64_t* iommu_map_generation_out);
/* Suspend must preserve the mapping generation. Resume must publish a newer
 * generation and stop all old-epoch DMA before returning success. */
typedef int (*RinGpuDeviceSetPowerFn)(
    void* context, uint32_t power_state, uint64_t expected_epoch,
    uint64_t new_epoch, uint64_t deadline_ns,
    uint64_t* iommu_map_generation_out);

typedef struct RinGpuDeviceBackendV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t capabilities;
    uint32_t queue_count;
    uint32_t max_in_flight;
    uint32_t max_reset_attempts;
    uint64_t minimum_timeout_ns;
    uint64_t maximum_timeout_ns;
    uint64_t iommu_domain_cookie;
    uint64_t iommu_map_generation;
    void* context;
    RinGpuDeviceNowNsFn now_ns;
    RinGpuDeviceSubmitFn submit;
    RinGpuDevicePollFn poll;
    RinGpuDeviceResetFn reset;
    RinGpuDeviceSetPowerFn set_power;
    uint64_t reserved[RIN_GPU_DEVICE_BACKEND_RESERVED_QWORDS];
} RinGpuDeviceBackendV1;

typedef struct RinGpuDeviceStatusV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t queue_count;
    uint64_t device_epoch;
    uint64_t iommu_domain_cookie;
    uint64_t iommu_map_generation;
    uint64_t last_monotonic_ns;
    uint32_t in_flight_count;
    uint32_t reset_count;
    uint32_t max_in_flight;
    uint32_t max_reset_attempts;
    uint64_t completed_values[RIN_GPU_DEVICE_MAX_QUEUES];
} RinGpuDeviceStatusV1;

typedef struct RinGpuDevicePollReportV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t completed_count;
    uint32_t failed_count;
    uint32_t in_flight_count;
    uint32_t reset_count;
    uint32_t reserved0;
    uint64_t device_epoch;
    uint64_t observed_monotonic_ns;
    uint64_t reserved[2];
} RinGpuDevicePollReportV1;

/* Opaque, fixed-size, allocation-free owner state. */
typedef struct RinGpuDeviceRuntime {
    uint64_t opaque[RIN_GPU_DEVICE_RUNTIME_STATE_QWORDS];
} RinGpuDeviceRuntime;

int rin_gpu_device_runtime_init(RinGpuDeviceRuntime* runtime,
                                const RinGpuDeviceBackendV1* backend);
int rin_gpu_device_runtime_submit(
    RinGpuDeviceRuntime* runtime,
    const RinGpuDeviceSubmissionV1* submission);
/* Snapshots the next submission identity while the device is active.  The
 * result is intentionally still submitted through rin_gpu_device_runtime_submit:
 * a caller that races another producer receives STALE without issuing work. */
int rin_gpu_device_runtime_prepare_submission(
    RinGpuDeviceRuntime* runtime, uint32_t queue_id, uint64_t command_cookie,
    RinGpuDeviceSubmissionV1* submission_out);
int rin_gpu_device_runtime_poll(RinGpuDeviceRuntime* runtime,
                               RinGpuDevicePollReportV1* report);
/* Consume one completion already collected by an IRQ-safe vendor dispatcher.
 * The dispatcher must not call this function from interrupt context: it only
 * latches a versioned completion, and a normal-context owner calls this
 * function to perform locking, generation validation, and recovery. */
int rin_gpu_device_runtime_accept_irq(
    RinGpuDeviceRuntime* runtime,
    const RinGpuDeviceCompletionV1* completion,
    RinGpuDevicePollReportV1* report);
int rin_gpu_device_runtime_watchdog(RinGpuDeviceRuntime* runtime,
                                   RinGpuDevicePollReportV1* report);
/* Explicitly reset an active device through the same bounded backend and
 * generation transition used by watchdog recovery.  All in-flight work is
 * terminalized before success is returned; the report records the reset and
 * the newer device/mapping generations. */
int rin_gpu_device_runtime_reset(RinGpuDeviceRuntime* runtime,
                                 RinGpuDevicePollReportV1* report);
int rin_gpu_device_runtime_suspend(RinGpuDeviceRuntime* runtime);
int rin_gpu_device_runtime_resume(RinGpuDeviceRuntime* runtime);
/* Optional bounded diagnostic sink.  The sink must already be initialized
 * for the current device epoch; reset and resume advance it together with the
 * runtime mapping generation. */
int rin_gpu_device_runtime_attach_diagnostics(
    RinGpuDeviceRuntime* runtime, RinGpuDiagnosticsRuntime* diagnostics);
int rin_gpu_device_runtime_get_status(RinGpuDeviceRuntime* runtime,
                                     RinGpuDeviceStatusV1* status);
/* Destroy succeeds only after every DMA-capable submission is terminal. */
int rin_gpu_device_runtime_destroy(RinGpuDeviceRuntime* runtime);
/* The physical owner must call this only after its backend quiesce callback
 * proves that no submitted command can execute or issue DMA. It makes a lost
 * runtime's outstanding submissions terminal so the owner can detach without
 * reusing any old command cookie. */
int rin_gpu_device_runtime_abandon_in_flight(RinGpuDeviceRuntime* runtime);

#if defined(__cplusplus)
static_assert(sizeof(RinGpuDeviceSubmissionV1) == 80u,
              "RinGPU device submission drift");
static_assert(sizeof(RinGpuDeviceCompletionV1) == 48u,
              "RinGPU device completion drift");
static_assert(sizeof(RinGpuDeviceBackendV1) == 144u,
              "RinGPU device backend drift");
static_assert(sizeof(RinGpuDeviceStatusV1) == 128u,
              "RinGPU device status drift");
static_assert(sizeof(RinGpuDevicePollReportV1) == 64u,
              "RinGPU device report drift");
#else
_Static_assert(sizeof(RinGpuDeviceSubmissionV1) == 80u,
               "RinGPU device submission drift");
_Static_assert(sizeof(RinGpuDeviceCompletionV1) == 48u,
               "RinGPU device completion drift");
_Static_assert(sizeof(RinGpuDeviceBackendV1) == 144u,
               "RinGPU device backend drift");
_Static_assert(sizeof(RinGpuDeviceStatusV1) == 128u,
               "RinGPU device status drift");
_Static_assert(sizeof(RinGpuDevicePollReportV1) == 64u,
               "RinGPU device report drift");
#endif

#endif /* RIN_SUBSYSTEMS_RINGPU_DEVICE_RUNTIME_H */
