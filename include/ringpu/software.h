// SPDX-License-Identifier: MIT
#ifndef RINGPU_PUBLIC_SOFTWARE_H
#define RINGPU_PUBLIC_SOFTWARE_H

#include <stdint.h>

#include "ringpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_SOFTWARE_BACKEND_VERSION 1u
#define RIN_GPU_SOFTWARE_BACKEND_VERSION_2 2u
#define RIN_GPU_SOFTWARE_BACKEND_VERSION_3 3u
#define RIN_GPU_SOFTWARE_BACKEND_VERSION_4 4u
#define RIN_GPU_SOFTWARE_EXTERNAL_IMAGE_VERSION 1u

/* V4 makes the reference backend observable without making execution
 * dependent on host addresses or wall-clock state.  The flags are deliberately
 * opt-in so older callers retain their exact V1--V3 admission contract. */
#define RIN_GPU_SOFTWARE_BACKEND_FLAG_DETERMINISTIC UINT32_C(0x00000001)
#define RIN_GPU_SOFTWARE_BACKEND_FLAG_COLLECT_STATS UINT32_C(0x00000002)
#define RIN_GPU_SOFTWARE_BACKEND_FLAGS_KNOWN \
    (RIN_GPU_SOFTWARE_BACKEND_FLAG_DETERMINISTIC | \
     RIN_GPU_SOFTWARE_BACKEND_FLAG_COLLECT_STATS)

#define RIN_GPU_SOFTWARE_FLOAT_POLICY_IEEE754_BINARY32 UINT32_C(1)
#define RIN_GPU_SOFTWARE_FLOAT_ROUND_TO_NEAREST UINT32_C(1)

typedef struct RinGpuSoftwareBackend RinGpuSoftwareBackend;
typedef struct RinGpuBackendOpsV1 RinGpuBackendOpsV1;

typedef struct RinGpuSoftwareBackendDescV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t max_total_bytes;
    uint32_t flags;
    uint32_t reserved0;
} RinGpuSoftwareBackendDescV1;

/* Optional V2 presentation hook for callers that own the final scanout
 * storage. The software backend remains the sole renderer: it exposes the
 * completed, tightly packed image only while processing PRESENT, after all
 * commands in that submission have succeeded. The callback must either copy
 * the whole image or return an error without modifying its destination.
 * `pixels` is read-only and becomes invalid when the callback returns. */
typedef struct RinGpuSoftwarePresentedImageV1 {
    uint32_t struct_size;
    uint32_t version;
    const uint8_t* pixels;
    uint64_t size_bytes;
    uint64_t row_pitch_bytes;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t display_id;
} RinGpuSoftwarePresentedImageV1;

typedef int (*RinGpuSoftwarePresentCallbackV1)(
    void* context, const RinGpuSoftwarePresentedImageV1* image);

typedef struct RinGpuSoftwareBackendDescV2 {
    RinGpuSoftwareBackendDescV1 base;
    RinGpuSoftwarePresentCallbackV1 present_callback;
    void* present_context;
} RinGpuSoftwareBackendDescV2;

/* V3 lets an embedding bind caller-owned storage to selected 2D, single-mip,
 * single-layer images. A successful acquire callback may leave this record
 * fully zeroed to request the ordinary private allocation instead. Otherwise
 * `pixels` describes complete tightly packed sample planes; for multisample
 * images the planes are laid out one after another, each with the supplied
 * row pitch. D32S8 may instead provide
 * both `depth_pixels` and `stencil_pixels` as separate planes; mixed planar
 * and interleaved D32S8 storage is rejected. All external storage remains
 * owned by the callback's caller and must remain live until destroy_image(). */
typedef struct RinGpuSoftwareExternalImageV1 {
    uint32_t struct_size;
    uint32_t version;
    uint8_t* pixels;
    uint64_t size_bytes;
    uint64_t row_pitch_bytes;
    float* depth_pixels;
    uint64_t depth_size_bytes;
    uint64_t depth_row_pitch_bytes;
    uint8_t* stencil_pixels;
    uint64_t stencil_size_bytes;
    uint64_t stencil_row_pitch_bytes;
} RinGpuSoftwareExternalImageV1;

typedef int (*RinGpuSoftwareAcquireImageCallbackV1)(
    void* context, const RinGpuImageDescV1* descriptor,
    uint64_t allocation_bytes, RinGpuSoftwareExternalImageV1* storage_out);

typedef struct RinGpuSoftwareBackendDescV3 {
    RinGpuSoftwareBackendDescV2 base;
    RinGpuSoftwareAcquireImageCallbackV1 acquire_image;
    void* image_context;
} RinGpuSoftwareBackendDescV3;

/* V4 retains the V3 callbacks and adds the seed used by deterministic output
 * hashing.  The seed is not a source of rendering randomness: it only lets a
 * differential-test harness partition independent hash namespaces. */
typedef struct RinGpuSoftwareBackendDescV4 {
    RinGpuSoftwareBackendDescV3 base;
    uint64_t deterministic_seed;
    uint64_t reserved[2];
} RinGpuSoftwareBackendDescV4;

typedef struct RinGpuSoftwareBackendStatsV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t submitted_commands;
    uint64_t copy_commands;
    uint64_t draw_commands;
    uint64_t dispatch_commands;
    uint64_t transition_commands;
    uint64_t barrier_commands;
    uint64_t present_commands;
    uint64_t output_hash;
    uint64_t output_hash_count;
    uint64_t deterministic_seed;
    uint64_t reserved[2];
} RinGpuSoftwareBackendStatsV1;

int ringpu_software_backend_create(
    const RinGpuSoftwareBackendDescV1* desc,
    RinGpuSoftwareBackend** backend_out);
void ringpu_software_backend_destroy(RinGpuSoftwareBackend* backend);

/* Returns the counters and last successful PRESENT hash for a V4 backend.
 * Hashes include the logical image descriptor and tightly packed presented
 * rows, never the process-local cookie or padding bytes. */
int ringpu_software_backend_query_stats(
    const RinGpuSoftwareBackend* backend,
    RinGpuSoftwareBackendStatsV1* stats_out);

/* Returns the generic RinGPU backend operation table. The returned table has
 * static lifetime; pass the RinGpuSoftwareBackend instance as backend_context.
 */
const RinGpuBackendOpsV1* ringpu_software_backend_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* RINGPU_PUBLIC_SOFTWARE_H */
