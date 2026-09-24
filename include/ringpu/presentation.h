/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_PRESENTATION_H
#define RINGPU_PUBLIC_PRESENTATION_H

#include <stdint.h>

#define RIN_GPU_PRESENTATION_VERSION 1u
#define RIN_GPU_PRESENTATION_RUNTIME_STATE_QWORDS 4096u
#define RIN_GPU_PRESENTATION_MAX_OUTPUTS 16u
#define RIN_GPU_PRESENTATION_MAX_IMAGES 64u
#define RIN_GPU_PRESENTATION_MAX_DAMAGE_RECTS 16u

#define RIN_GPU_PRESENTATION_OUTPUT_FIFO UINT32_C(0x00000001)
#define RIN_GPU_PRESENTATION_OUTPUT_MAILBOX UINT32_C(0x00000002)
#define RIN_GPU_PRESENTATION_OUTPUT_IMMEDIATE UINT32_C(0x00000004)
#define RIN_GPU_PRESENTATION_OUTPUT_KNOWN_FLAGS \
    (RIN_GPU_PRESENTATION_OUTPUT_FIFO | RIN_GPU_PRESENTATION_OUTPUT_MAILBOX | \
     RIN_GPU_PRESENTATION_OUTPUT_IMMEDIATE)

#define RIN_GPU_PRESENTATION_IMAGE_RENDER_TARGET UINT32_C(0x00000001)
#define RIN_GPU_PRESENTATION_IMAGE_PRESENT UINT32_C(0x00000002)
#define RIN_GPU_PRESENTATION_IMAGE_KNOWN_USAGE \
    (RIN_GPU_PRESENTATION_IMAGE_RENDER_TARGET | \
     RIN_GPU_PRESENTATION_IMAGE_PRESENT)

#define RIN_GPU_PRESENTATION_SUBMIT_FULL_DAMAGE UINT32_C(0x00000001)
#define RIN_GPU_PRESENTATION_SUBMIT_DIRECT_SCANOUT UINT32_C(0x00000002)
#define RIN_GPU_PRESENTATION_SUBMIT_KNOWN_FLAGS \
    (RIN_GPU_PRESENTATION_SUBMIT_FULL_DAMAGE | \
     RIN_GPU_PRESENTATION_SUBMIT_DIRECT_SCANOUT)

#define RIN_GPU_PRESENTATION_SCANOUT_CANDIDATE_FULLSCREEN \
    UINT32_C(0x00000001)
#define RIN_GPU_PRESENTATION_SCANOUT_CANDIDATE_KNOWN_FLAGS \
    RIN_GPU_PRESENTATION_SCANOUT_CANDIDATE_FULLSCREEN

typedef enum RinGpuPresentationResult {
    RIN_GPU_PRESENTATION_OK = 0,
    RIN_GPU_PRESENTATION_INVALID_ARGUMENT = -1,
    RIN_GPU_PRESENTATION_STATE = -2,
    RIN_GPU_PRESENTATION_BUSY = -3,
    RIN_GPU_PRESENTATION_BACKEND = -4,
    RIN_GPU_PRESENTATION_DEVICE_LOST = -5,
    RIN_GPU_PRESENTATION_STALE = -6,
    RIN_GPU_PRESENTATION_LIMIT = -7,
    RIN_GPU_PRESENTATION_UNSUPPORTED = -8,
    RIN_GPU_PRESENTATION_TIMEOUT = -9
} RinGpuPresentationResult;

typedef enum RinGpuPresentationMode {
    RIN_GPU_PRESENTATION_MODE_FIFO = 1,
    RIN_GPU_PRESENTATION_MODE_MAILBOX = 2,
    RIN_GPU_PRESENTATION_MODE_IMMEDIATE = 3
} RinGpuPresentationMode;

typedef enum RinGpuPresentationImageState {
    RIN_GPU_PRESENTATION_IMAGE_AVAILABLE = 1,
    RIN_GPU_PRESENTATION_IMAGE_ACQUIRED = 2,
    RIN_GPU_PRESENTATION_IMAGE_SUBMITTED = 3,
    RIN_GPU_PRESENTATION_IMAGE_SCANNING = 4,
    RIN_GPU_PRESENTATION_IMAGE_INVALID = 5
} RinGpuPresentationImageState;

typedef struct RinGpuPresentationOutputV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t display_id;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihertz;
    uint32_t format;
    uint64_t output_generation;
    uint64_t device_generation;
    uint64_t reserved[2];
} RinGpuPresentationOutputV1;

typedef struct RinGpuPresentationImageV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t image_token;
    uint32_t display_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint64_t output_generation;
    uint64_t device_generation;
    uint64_t reserved[2];
} RinGpuPresentationImageV1;

typedef struct RinGpuPresentationDamageRectV1 {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} RinGpuPresentationDamageRectV1;

typedef struct RinGpuPresentationAcquireV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t display_id;
    uint32_t mode;
    uint64_t image_token;
    uint64_t output_generation;
    uint64_t device_generation;
    uint64_t frame_id;
    uint64_t reserved[2];
} RinGpuPresentationAcquireV1;

typedef struct RinGpuPresentationSubmitV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t display_id;
    uint32_t mode;
    uint64_t image_token;
    uint64_t output_generation;
    uint64_t device_generation;
    uint64_t frame_id;
    uint32_t flags;
    uint32_t damage_count;
    RinGpuPresentationDamageRectV1
        damage[RIN_GPU_PRESENTATION_MAX_DAMAGE_RECTS];
    uint64_t reserved[2];
} RinGpuPresentationSubmitV1;

/* A scanout candidate is an image lease, never a raw surface pointer.  The
 * candidate is valid only for the output/device generations that admitted the
 * image. */
typedef struct RinGpuPresentationScanoutCandidateV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t display_id;
    uint32_t flags;
    uint64_t image_token;
    uint64_t output_generation;
    uint64_t device_generation;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t reserved0;
    uint64_t reserved[1];
} RinGpuPresentationScanoutCandidateV1;

typedef struct RinGpuPresentationScanoutTransactionV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t display_id;
    uint32_t reserved0;
    uint64_t transaction_id;
    uint64_t image_token;
    uint64_t previous_image_token;
    uint64_t output_generation;
    uint64_t device_generation;
    uint64_t frame_id;
    uint64_t reserved[2];
} RinGpuPresentationScanoutTransactionV1;

typedef struct RinGpuPresentationCompletionV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t display_id;
    uint32_t status;
    uint64_t image_token;
    uint64_t output_generation;
    uint64_t device_generation;
    uint64_t frame_id;
    uint64_t fence_value;
    uint64_t reserved[2];
} RinGpuPresentationCompletionV1;

#define RIN_GPU_PRESENTATION_COMPLETION_SUCCESS 0u
#define RIN_GPU_PRESENTATION_COMPLETION_FAILURE 1u

typedef int (*RinGpuPresentationSubmitFn)(
    void* context, const RinGpuPresentationSubmitV1* submit,
    uint64_t fence_value);
/* Retires an already submitted frame before it can become scanout. The
 * callback must return only after the backend no longer owns the image. */
typedef int (*RinGpuPresentationCancelFn)(
    void* context, const RinGpuPresentationCompletionV1* pending);

typedef struct RinGpuPresentationBackendV1 {
    uint32_t struct_size;
    uint32_t version;
    void* context;
    RinGpuPresentationSubmitFn submit;
    RinGpuPresentationCancelFn cancel;
    uint64_t reserved[4];
} RinGpuPresentationBackendV1;

typedef struct RinGpuPresentationStatusV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t state;
    uint32_t output_count;
    uint32_t image_count;
    uint32_t pending_count;
    uint32_t scanning_count;
    uint32_t full_redraw_required;
    uint64_t device_generation;
    uint64_t next_fence_value;
    uint64_t submitted_count;
    uint64_t completed_count;
    uint64_t dropped_count;
    uint64_t stale_completion_count;
    uint64_t reserved[3];
} RinGpuPresentationStatusV1;

#define RIN_GPU_PRESENTATION_STATE_ACTIVE 1u
#define RIN_GPU_PRESENTATION_STATE_SUSPENDED 2u
#define RIN_GPU_PRESENTATION_STATE_LOST 3u

#ifdef __cplusplus
extern "C" {
#endif

/* The owner is allocation-free and contains no shared raw pointer. The
 * backend context is private to the caller that initialized this instance. */
typedef struct RinGpuPresentationRuntime {
    uint64_t opaque[RIN_GPU_PRESENTATION_RUNTIME_STATE_QWORDS];
} RinGpuPresentationRuntime;

int rin_gpu_presentation_runtime_init(
    RinGpuPresentationRuntime* runtime, uint64_t device_generation,
    const RinGpuPresentationBackendV1* backend);
int rin_gpu_presentation_register_output(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationOutputV1* output);
/* Replace the descriptor after an output generation transition has already
 * invalidated its old images.  The generation must equal the current output
 * generation and no frame may be pending. */
int rin_gpu_presentation_update_output(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationOutputV1* output);
int rin_gpu_presentation_register_image(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationImageV1* image);
int rin_gpu_presentation_unregister_image(
    RinGpuPresentationRuntime* runtime, uint64_t image_token);
int rin_gpu_presentation_begin_frame(
    RinGpuPresentationRuntime* runtime,
    RinGpuPresentationAcquireV1* acquire);
int rin_gpu_presentation_abandon_frame(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationAcquireV1* acquire);
int rin_gpu_presentation_submit_frame(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationSubmitV1* submit, uint64_t* fence_value_out);
int rin_gpu_presentation_begin_direct_scanout(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationScanoutCandidateV1* candidate,
    RinGpuPresentationScanoutTransactionV1* transaction_out);
int rin_gpu_presentation_commit_direct_scanout(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationScanoutTransactionV1* transaction,
    uint64_t* fence_value_out);
int rin_gpu_presentation_abort_direct_scanout(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationScanoutTransactionV1* transaction);
int rin_gpu_presentation_complete(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationCompletionV1* completion);
int rin_gpu_presentation_drop_pending(
    RinGpuPresentationRuntime* runtime, uint32_t display_id,
    uint64_t output_generation, uint64_t device_generation);
int rin_gpu_presentation_remove_output(
    RinGpuPresentationRuntime* runtime, uint32_t display_id,
    uint64_t next_output_generation);
int rin_gpu_presentation_suspend(RinGpuPresentationRuntime* runtime);
int rin_gpu_presentation_resume(RinGpuPresentationRuntime* runtime);
int rin_gpu_presentation_device_reset(
    RinGpuPresentationRuntime* runtime, uint64_t next_device_generation);
int rin_gpu_presentation_device_lost(RinGpuPresentationRuntime* runtime);
int rin_gpu_presentation_ack_full_redraw(RinGpuPresentationRuntime* runtime);
int rin_gpu_presentation_get_status(
    RinGpuPresentationRuntime* runtime,
    RinGpuPresentationStatusV1* status_out);

#ifdef __cplusplus
}
#endif

#if UINTPTR_MAX == UINT64_MAX
#define RIN_GPU_PRESENTATION_IMAGE_SIZE 72u
#define RIN_GPU_PRESENTATION_BACKEND_SIZE 64u
#elif UINTPTR_MAX == UINT32_MAX
#define RIN_GPU_PRESENTATION_IMAGE_SIZE 68u
#define RIN_GPU_PRESENTATION_BACKEND_SIZE 52u
#else
#error "RinGPU presentation runtime requires a 32-bit or 64-bit pointer ABI"
#endif

#if defined(__cplusplus)
static_assert(sizeof(RinGpuPresentationOutputV1) == 64u,
              "RinGPU presentation output drift");
static_assert(sizeof(RinGpuPresentationImageV1) ==
                  RIN_GPU_PRESENTATION_IMAGE_SIZE,
              "RinGPU presentation image drift");
static_assert(sizeof(RinGpuPresentationDamageRectV1) == 16u,
              "RinGPU presentation damage drift");
static_assert(sizeof(RinGpuPresentationAcquireV1) == 64u,
              "RinGPU presentation acquire drift");
static_assert(sizeof(RinGpuPresentationSubmitV1) == 328u,
              "RinGPU presentation submit drift");
static_assert(sizeof(RinGpuPresentationScanoutCandidateV1) == 64u,
              "RinGPU presentation scanout candidate drift");
static_assert(sizeof(RinGpuPresentationScanoutTransactionV1) == 80u,
              "RinGPU presentation scanout transaction drift");
static_assert(sizeof(RinGpuPresentationCompletionV1) == 72u,
              "RinGPU presentation completion drift");
static_assert(sizeof(RinGpuPresentationBackendV1) ==
                  RIN_GPU_PRESENTATION_BACKEND_SIZE,
              "RinGPU presentation backend drift");
#else
_Static_assert(sizeof(RinGpuPresentationOutputV1) == 64u,
               "RinGPU presentation output drift");
_Static_assert(sizeof(RinGpuPresentationImageV1) ==
                   RIN_GPU_PRESENTATION_IMAGE_SIZE,
               "RinGPU presentation image drift");
_Static_assert(sizeof(RinGpuPresentationDamageRectV1) == 16u,
               "RinGPU presentation damage drift");
_Static_assert(sizeof(RinGpuPresentationAcquireV1) == 64u,
               "RinGPU presentation acquire drift");
_Static_assert(sizeof(RinGpuPresentationSubmitV1) == 328u,
               "RinGPU presentation submit drift");
_Static_assert(sizeof(RinGpuPresentationScanoutCandidateV1) == 64u,
               "RinGPU presentation scanout candidate drift");
_Static_assert(sizeof(RinGpuPresentationScanoutTransactionV1) == 80u,
               "RinGPU presentation scanout transaction drift");
_Static_assert(sizeof(RinGpuPresentationCompletionV1) == 72u,
               "RinGPU presentation completion drift");
_Static_assert(sizeof(RinGpuPresentationBackendV1) ==
                   RIN_GPU_PRESENTATION_BACKEND_SIZE,
               "RinGPU presentation backend drift");
#endif

#undef RIN_GPU_PRESENTATION_IMAGE_SIZE
#undef RIN_GPU_PRESENTATION_BACKEND_SIZE

#endif /* RINGPU_PUBLIC_PRESENTATION_H */
