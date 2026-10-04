/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_PLATFORM_BRIDGE_H
#define RINGPU_PUBLIC_PLATFORM_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_GPU_BACKEND_CONTRACT_VERSION 1u
#define RIN_GPU_OS_CORE_BACKEND_ADAPTER_VERSION 1u
#define RIN_GPU_BACKEND_COMMAND_ABI_VERSION 1u
#define RIN_GPU_BACKEND_COMMAND_RECORD_SIZE_V1 872u
#define RIN_GPU_BACKEND_COMMAND_PAYLOAD_BYTES_V1 864u
#define RIN_GPU_BACKEND_GRAPHICS_PIPELINE_COMMON_BYTES_V1 2028u
#define RIN_GPU_BACKEND_GRAPHICS_PIPELINE_EXTENSION_BYTES_V1 152u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_VERSION 1u

/* Command opcodes are shared by every backend implementation. Keep this
 * enumeration in the public contract rather than duplicating it in private
 * runtimes; OS-Core adapters may implement a subset but preserve these IDs. */
typedef enum RinGpuBackendCommandTypeV1 {
    RIN_GPU_BACKEND_COMMAND_COPY_BUFFER = 1,
    RIN_GPU_BACKEND_COMMAND_COPY_IMAGE = 2,
    RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE = 3,
    RIN_GPU_BACKEND_COMMAND_DISPATCH = 4,
    RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER = 5,
    RIN_GPU_BACKEND_COMMAND_DRAW = 6,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS = 7,
    RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS = 8,
    RIN_GPU_BACKEND_COMMAND_PRESENT = 9,
    RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES = 10,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED = 11,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH = 12,
    RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER = 13,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX = 14,
    RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE = 15,
    RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2 = 16,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2 = 17,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL = 18,
    RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT = 19,
    RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER = 20,
    RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE = 21,
    RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE = 22,
    RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2 = 23,
    RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2 = 24,
    RIN_GPU_BACKEND_COMMAND_SET_PUSH_CONSTANTS = 25,
    RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY = 26,
    RIN_GPU_BACKEND_COMMAND_END_QUERY = 27,
    RIN_GPU_BACKEND_COMMAND_RESET_QUERY = 28,
    RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP = 29,
    RIN_GPU_BACKEND_COMMAND_RESOLVE_IMAGE = 30,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDIRECT = 31,
    RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_INDIRECT = 32,
    RIN_GPU_BACKEND_COMMAND_DISPATCH_INDIRECT = 33,
    RIN_GPU_BACKEND_COMMAND_LAST_V1 =
        RIN_GPU_BACKEND_COMMAND_DISPATCH_INDIRECT
} RinGpuBackendCommandTypeV1;

/* Canonical API-neutral command envelope. The tagged payload uses the V1
 * command schema; adapters copy only the active operation payload when an
 * OS-Core internal record has a different size or layout. */
typedef struct RinGpuBackendCommandRecordV1 {
    uint32_t type;
    uint32_t reserved;
    uint8_t payload[RIN_GPU_BACKEND_COMMAND_PAYLOAD_BYTES_V1];
} RinGpuBackendCommandRecordV1;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinGpuBackendCommandRecordV1) ==
                   RIN_GPU_BACKEND_COMMAND_RECORD_SIZE_V1,
               "RinGPU backend command-record ABI drift");
#endif

typedef struct RinGpuBackendBlendTargetRecordV1 {
    uint32_t blend_enabled;
    uint32_t source_color_factor;
    uint32_t destination_color_factor;
    uint32_t color_operation;
    uint32_t source_alpha_factor;
    uint32_t destination_alpha_factor;
    uint32_t alpha_operation;
    uint32_t color_write_mask;
    uint32_t reserved;
} RinGpuBackendBlendTargetRecordV1;

typedef struct RinGpuBackendGraphicsPipelineExtensionV1 {
    uint32_t independent_blend_enabled;
    uint32_t independent_blend_mask;
    RinGpuBackendBlendTargetRecordV1 blend_targets[4];
} RinGpuBackendGraphicsPipelineExtensionV1;

/* Optional callback groups supplied by the platform bridge. The core exposes
 * an optional callback only when the corresponding bit is present. */
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_UPLOAD_BUFFER UINT64_C(0x0001)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_READBACK_BUFFER UINT64_C(0x0002)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_UPLOAD_IMAGE UINT64_C(0x0004)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_READBACK_IMAGE UINT64_C(0x0008)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_QUERIES UINT64_C(0x0010)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_WAIT UINT64_C(0x0020)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_HAS_MEMORY UINT64_C(0x0040)
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_CALLBACKS_KNOWN UINT64_C(0x007f)

/* This operation frame is a pointer-sized, synchronous call envelope. Record
 * payloads retain their owning API's ABI and are never reinterpreted as the
 * other API's private structs; the bridge implementation converts each
 * operation explicitly. A pointer in the frame is valid only for invoke(). */
typedef struct RinGpuPlatformBackendBridgeCallV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t operation;
    uint32_t reserved;
    uint64_t values[8];
    const void* records[4];
    uint32_t record_sizes[4];
    uint32_t reserved_sizes[4];
    const void* data[2];
    uint64_t data_sizes[2];
    void* outputs[2];
    uint64_t output_sizes[2];
} RinGpuPlatformBackendBridgeCallV1;

typedef struct RinGpuPlatformBackendBridgeResponseV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t values[8];
} RinGpuPlatformBackendBridgeResponseV1;

typedef int (*RinGpuPlatformBackendBridgeInvokeCallbackV1)(
    void* context, const RinGpuPlatformBackendBridgeCallV1* call,
    RinGpuPlatformBackendBridgeResponseV1* response);

/* The common contract and command record have one authoritative version.
 * adapter_version describes only the OS-Core-specific conversion from this
 * contract to OS-Core's internal resource/command representation. */
typedef struct RinGpuPlatformBackendBridgeV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t backend_family;
    uint32_t contract_version;
    uint32_t adapter_version;
    uint32_t command_abi_version;
    uint32_t command_record_size;
    uint32_t reserved[2];
    uint64_t callback_mask;
    RinGpuPlatformBackendBridgeInvokeCallbackV1 invoke;
} RinGpuPlatformBackendBridgeV1;

typedef int (*RinGpuPlatformResolveBackendBridgeCallbackV1)(
    void* context, uint32_t backend_family,
    const RinGpuPlatformBackendBridgeV1** bridge_out,
    void** bridge_context_out);

/* Stable operation identifiers for RinGpuPlatformBackendBridgeCallV1. */
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_BUFFER 1u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_BUFFER 2u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_UPLOAD_BUFFER 3u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_READBACK_BUFFER 4u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_IMAGE 5u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_IMAGE 6u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_UPLOAD_IMAGE 7u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_READBACK_IMAGE 8u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_SAMPLER 9u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_SAMPLER 10u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_SHADER_MODULE 11u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_SHADER_MODULE 12u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_COMPUTE_PIPELINE 13u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_COMPUTE_PIPELINE 14u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_GRAPHICS_PIPELINE 15u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_GRAPHICS_PIPELINE 16u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_COMPUTE_BIND_GROUP 17u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_COMPUTE_BIND_GROUP 18u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_GRAPHICS_BIND_GROUP 19u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_GRAPHICS_BIND_GROUP 20u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_SUBMIT_COMMANDS 21u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_GET_QUERY_RESULT 22u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_GET_TIMESTAMP_PERIOD 23u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_QUERY 24u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_WAIT_FOR_COMPLETION 25u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_CREATE_MEMORY 26u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_DESTROY_MEMORY 27u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_BIND_BUFFER_MEMORY 28u
#define RIN_GPU_PLATFORM_BACKEND_BRIDGE_OP_BIND_IMAGE_MEMORY 29u

#ifdef __cplusplus
}
#endif

#endif /* RINGPU_PUBLIC_PLATFORM_BRIDGE_H */
