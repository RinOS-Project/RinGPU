// SPDX-License-Identifier: MIT
#ifndef RIN_API_RIN_GPU_H
#define RIN_API_RIN_GPU_H

#include <stdint.h>

#include "rin_shader.h"

#define RIN_GPU_ABI_VERSION 1u
#define RIN_GPU_ADAPTER_NAME_MAX 64u
#define RIN_GPU_DISPLAY_NAME_MAX 64u
#define RIN_GPU_MAX_DISPLAYS 16u

typedef uint64_t RinGpuHandle;

typedef enum RinGpuResult {
    RIN_GPU_OK = 0,
    RIN_GPU_ERROR_INVALID_ARGUMENT = -1,
    RIN_GPU_ERROR_UNSUPPORTED = -2,
    RIN_GPU_ERROR_NO_MEMORY = -3,
    RIN_GPU_ERROR_LIMIT = -4,
    RIN_GPU_ERROR_INVALID_HANDLE = -5,
    RIN_GPU_ERROR_WRONG_TYPE = -6,
    RIN_GPU_ERROR_STATE = -7,
    RIN_GPU_ERROR_BOUNDS = -8,
    RIN_GPU_ERROR_BUSY = -9,
    RIN_GPU_ERROR_DEVICE_LOST = -10,
    RIN_GPU_ERROR_BACKEND = -11,
    RIN_GPU_ERROR_SHADER_INVALID = -12,
    RIN_GPU_ERROR_TIMEOUT = -13
} RinGpuResult;

typedef enum RinGpuObjectType {
    RIN_GPU_OBJECT_NONE = 0,
    RIN_GPU_OBJECT_BUFFER = 1,
    RIN_GPU_OBJECT_QUEUE = 2,
    RIN_GPU_OBJECT_COMMAND_LIST = 3,
    RIN_GPU_OBJECT_FENCE = 4,
    RIN_GPU_OBJECT_IMAGE = 5,
    RIN_GPU_OBJECT_SHADER_MODULE = 6,
    RIN_GPU_OBJECT_COMPUTE_PIPELINE = 7,
    RIN_GPU_OBJECT_COMPUTE_BIND_GROUP = 8,
    RIN_GPU_OBJECT_GRAPHICS_PIPELINE = 9,
    RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP = 10,
    RIN_GPU_OBJECT_SAMPLER = 11
} RinGpuObjectType;

#define RIN_GPU_BUFFER_COPY_SOURCE      0x00000001u
#define RIN_GPU_BUFFER_COPY_DESTINATION 0x00000002u
#define RIN_GPU_BUFFER_STORAGE          0x00000004u
#define RIN_GPU_BUFFER_VERTEX           0x00000008u
#define RIN_GPU_BUFFER_INDEX            0x00000010u
#define RIN_GPU_BUFFER_KNOWN_USAGE      0x0000001fu

/* CPU-visible buffers accept immediate CPU uploads through
 * ringpu_upload_buffer(). They must also declare COPY_DESTINATION and remain
 * unavailable to GPU commands until one successful full-allocation upload. */
#define RIN_GPU_BUFFER_CPU_VISIBLE       0x00000001u
#define RIN_GPU_BUFFER_KNOWN_FLAGS       0x00000001u

#define RIN_GPU_RESOURCE_READ  0x00000001u
#define RIN_GPU_RESOURCE_WRITE 0x00000002u
#define RIN_GPU_RESOURCE_KNOWN_ACCESS 0x00000003u

/* A sampled-image graphics binding normally names one subresource. This flag
 * exposes the contiguous mip chain from mip_level through the image's final
 * level for implicit-LOD sampling. All exposed levels must be SHADER_READ. */
#define RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN 0x00000001u
#define RIN_GPU_GRAPHICS_BINDING_KNOWN_FLAGS       0x00000001u

typedef enum RinGpuImageDimension {
    RIN_GPU_IMAGE_DIMENSION_1D = 1,
    RIN_GPU_IMAGE_DIMENSION_2D = 2,
    RIN_GPU_IMAGE_DIMENSION_3D = 3
} RinGpuImageDimension;

typedef enum RinGpuImageFormat {
    RIN_GPU_FORMAT_R8_UNORM = 1,
    RIN_GPU_FORMAT_RGBA8_UNORM = 2,
    RIN_GPU_FORMAT_BGRA8_UNORM = 3,
    RIN_GPU_FORMAT_D32_FLOAT = 4,
    /* CPU upload/readback uses native-endian F32 depth, S8 stencil, then
     * three zero padding bytes per 8-byte texel. Device storage may differ. */
    RIN_GPU_FORMAT_D32_FLOAT_S8_UINT = 5,
    /* CPU upload/readback uses a native-endian 16-bit value with R in bits
     * 11..15, G in 5..10, and B in 0..4. The format has no alpha plane. */
    RIN_GPU_FORMAT_RGB565_UNORM = 6,
    /* CPU upload/readback uses native-endian RGBA with four bits in each
     * component, ordered R in 12..15 through A in 0..3. */
    RIN_GPU_FORMAT_RGBA4_UNORM = 7,
    /* CPU upload/readback uses native-endian RGB with five bits each in
     * 11..15, 6..10, and 1..5; alpha occupies bit 0. */
    RIN_GPU_FORMAT_RGB5_A1_UNORM = 8,
    /* CPU upload/readback is one unsigned stencil byte per texel. This is a
     * stencil-only render target; it has no readable or writable depth
     * aspect. */
    RIN_GPU_FORMAT_S8_UINT = 9,
    /* CPU upload/readback storage is four native-endian IEEE-754 binary32
     * components in RGBA order. Float images may be sampled, used as a color
     * target, and read back when the selected backend accepts those usages;
     * they are never presentable or storage images in the portable profile. */
    RIN_GPU_FORMAT_RGBA32_FLOAT = 10,
    /* CPU upload/readback storage is four native-endian IEEE-754 binary16
     * components in RGBA order. Like RGBA32_FLOAT, half-float images are
     * sampled/offscreen color targets only in the portable profile; they are
     * neither presentable nor storage images. */
    RIN_GPU_FORMAT_RGBA16_FLOAT = 11
} RinGpuImageFormat;

typedef enum RinGpuImageState {
    RIN_GPU_IMAGE_STATE_UNDEFINED = 0,
    RIN_GPU_IMAGE_STATE_COPY_SOURCE = 1,
    RIN_GPU_IMAGE_STATE_COPY_DESTINATION = 2,
    RIN_GPU_IMAGE_STATE_COLOR_TARGET = 3,
    RIN_GPU_IMAGE_STATE_PRESENT = 4,
    RIN_GPU_IMAGE_STATE_DEPTH_TARGET = 5,
    RIN_GPU_IMAGE_STATE_SHADER_READ = 6
} RinGpuImageState;

#define RIN_GPU_IMAGE_COPY_SOURCE       0x00000001u
#define RIN_GPU_IMAGE_COPY_DESTINATION  0x00000002u
#define RIN_GPU_IMAGE_SAMPLED           0x00000004u
#define RIN_GPU_IMAGE_STORAGE           0x00000008u
#define RIN_GPU_IMAGE_COLOR_TARGET      0x00000010u
#define RIN_GPU_IMAGE_DEPTH_STENCIL     0x00000020u
#define RIN_GPU_IMAGE_PRESENT           0x00000040u
#define RIN_GPU_IMAGE_KNOWN_USAGE       0x0000007fu

/* CPU-visible images accept immediate CPU uploads through
 * ringpu_upload_image() (or a mip/subresource variant) and require
 * COPY_DESTINATION. CPU-visible images remain unavailable to GPU commands
 * until every mip/subresource has received a successful full upload; a failed
 * upload resets that recovery state. CPU-readable images permit a
 * corresponding readback from COPY_SOURCE after ringpu_wait_fence(). Both
 * flags require one sample. */
#define RIN_GPU_IMAGE_CPU_VISIBLE        0x00000001u
#define RIN_GPU_IMAGE_CPU_READABLE        0x00000002u
#define RIN_GPU_IMAGE_KNOWN_FLAGS        0x00000003u

#define RIN_GPU_QUEUE_COPY    0x00000001u
#define RIN_GPU_QUEUE_COMPUTE 0x00000002u
#define RIN_GPU_QUEUE_GRAPHICS 0x00000004u
#define RIN_GPU_QUEUE_KNOWN_CAPABILITIES 0x00000007u
#define RIN_GPU_MAX_DISPATCH_GROUPS 65535u
#define RIN_GPU_MAX_DRAW_VERTICES 16777216u
#define RIN_GPU_MAX_DRAW_INDICES 16777216u
#define RIN_GPU_MAX_DRAW_INSTANCES 1048576u
#define RIN_GPU_MAX_VERTEX_ATTRIBUTES RIN_SHADER_MAX_IO
#define RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS RIN_GPU_MAX_VERTEX_ATTRIBUTES
#define RIN_GPU_MAX_VERTEX_STRIDE 2048u
#define RIN_GPU_MAX_VARYINGS (RIN_SHADER_MAX_IO - 4u)
#define RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT 0x00000001u
#define RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS \
    RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT
#define RIN_GPU_PRIMARY_DISPLAY 0u
#define RIN_GPU_TIMEOUT_INFINITE UINT64_MAX

#define RIN_GPU_DISPLAY_CONNECTED 0x00000001u
#define RIN_GPU_DISPLAY_PRIMARY   0x00000002u
#define RIN_GPU_DISPLAY_KNOWN_FLAGS 0x00000003u
#define RIN_GPU_DISPLAY_MIN_REFRESH_MILLIHERTZ 1000u
#define RIN_GPU_DISPLAY_MAX_REFRESH_MILLIHERTZ 1000000u
#define RIN_GPU_DISPLAY_MIN_SCALE_MILLI 250u
#define RIN_GPU_DISPLAY_MAX_SCALE_MILLI 8000u

typedef enum RinGpuPrimitiveTopology {
    RIN_GPU_PRIMITIVE_TRIANGLE_LIST = 1,
    RIN_GPU_PRIMITIVE_POINT_LIST = 2,
    RIN_GPU_PRIMITIVE_LINE_LIST = 3,
    RIN_GPU_PRIMITIVE_LINE_STRIP = 4,
    RIN_GPU_PRIMITIVE_LINE_LOOP = 5,
    RIN_GPU_PRIMITIVE_TRIANGLE_STRIP = 6,
    RIN_GPU_PRIMITIVE_TRIANGLE_FAN = 7
} RinGpuPrimitiveTopology;

typedef enum RinGpuIndexFormat {
    RIN_GPU_INDEX_UINT16 = 1,
    RIN_GPU_INDEX_UINT32 = 2,
    /* Additive V1 format: preserves the established UINT16/UINT32 values. */
    RIN_GPU_INDEX_UINT8 = 3
} RinGpuIndexFormat;

typedef enum RinGpuVertexFormat {
    /* Vertex inputs are converted to scalar Float32 values before execution
     * by the portable RinGPU backends. The normalized forms follow the
     * WebGL/OpenGL ES rules: unsigned maps to [0, 1], signed minimum maps to
     * -1, and signed maximum maps to 1. */
    RIN_GPU_VERTEX_UINT32 = 1,
    RIN_GPU_VERTEX_SINT32 = 2,
    RIN_GPU_VERTEX_FLOAT32 = 3,
    RIN_GPU_VERTEX_UINT8 = 4,
    RIN_GPU_VERTEX_SINT8 = 5,
    RIN_GPU_VERTEX_UNORM8 = 6,
    RIN_GPU_VERTEX_SNORM8 = 7,
    RIN_GPU_VERTEX_UINT16 = 8,
    RIN_GPU_VERTEX_SINT16 = 9,
    RIN_GPU_VERTEX_UNORM16 = 10,
    RIN_GPU_VERTEX_SNORM16 = 11
} RinGpuVertexFormat;

/* A constant attribute supplies its IEEE-754 binary32 bit pattern through
 * RinGpuVertexAttributeV1.offset. It is legal only with FLOAT32 and lets a
 * vertex draw omit the vertex buffer when every input is constant. */
#define RIN_GPU_VERTEX_ATTRIBUTE_CONSTANT_FLOAT32 0x00000001u

typedef enum RinGpuSamplerFilter {
    /* Valid only for RinGpuSamplerDescV1::mip_filter. */
    RIN_GPU_SAMPLER_MIP_FILTER_NONE = 0,
    RIN_GPU_SAMPLER_FILTER_NEAREST = 1,
    RIN_GPU_SAMPLER_FILTER_LINEAR = 2
} RinGpuSamplerFilter;

typedef enum RinGpuSamplerAddressMode {
    RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE = 1,
    RIN_GPU_SAMPLER_ADDRESS_REPEAT = 2,
    RIN_GPU_SAMPLER_ADDRESS_MIRRORED_REPEAT = 3
} RinGpuSamplerAddressMode;

#define RIN_GPU_MAX_SAMPLER_ANISOTROPY 16u

typedef enum RinGpuCompareOp {
    RIN_GPU_COMPARE_LESS = 1,
    RIN_GPU_COMPARE_LESS_EQUAL = 2,
    RIN_GPU_COMPARE_ALWAYS = 3,
    RIN_GPU_COMPARE_NEVER = 4,
    RIN_GPU_COMPARE_EQUAL = 5,
    RIN_GPU_COMPARE_GREATER = 6,
    RIN_GPU_COMPARE_NOT_EQUAL = 7,
    RIN_GPU_COMPARE_GREATER_EQUAL = 8
} RinGpuCompareOp;

typedef enum RinGpuStencilOp {
    RIN_GPU_STENCIL_KEEP = 1,
    RIN_GPU_STENCIL_ZERO = 2,
    RIN_GPU_STENCIL_REPLACE = 3,
    RIN_GPU_STENCIL_INCREMENT_CLAMP = 4,
    RIN_GPU_STENCIL_DECREMENT_CLAMP = 5,
    RIN_GPU_STENCIL_INVERT = 6,
    RIN_GPU_STENCIL_INCREMENT_WRAP = 7,
    RIN_GPU_STENCIL_DECREMENT_WRAP = 8
} RinGpuStencilOp;

typedef enum RinGpuBlendFactor {
    RIN_GPU_BLEND_ZERO = 1,
    RIN_GPU_BLEND_ONE = 2,
    RIN_GPU_BLEND_SOURCE_ALPHA = 3,
    RIN_GPU_BLEND_ONE_MINUS_SOURCE_ALPHA = 4,
    RIN_GPU_BLEND_DESTINATION_ALPHA = 5,
    RIN_GPU_BLEND_ONE_MINUS_DESTINATION_ALPHA = 6,
    RIN_GPU_BLEND_SOURCE_COLOR = 7,
    RIN_GPU_BLEND_ONE_MINUS_SOURCE_COLOR = 8,
    RIN_GPU_BLEND_DESTINATION_COLOR = 9,
    RIN_GPU_BLEND_ONE_MINUS_DESTINATION_COLOR = 10,
    RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE = 11,
    RIN_GPU_BLEND_CONSTANT_COLOR = 12,
    RIN_GPU_BLEND_ONE_MINUS_CONSTANT_COLOR = 13,
    RIN_GPU_BLEND_CONSTANT_ALPHA = 14,
    RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA = 15
} RinGpuBlendFactor;

typedef enum RinGpuBlendOp {
    RIN_GPU_BLEND_ADD = 1,
    RIN_GPU_BLEND_SUBTRACT = 2,
    RIN_GPU_BLEND_REVERSE_SUBTRACT = 3,
    RIN_GPU_BLEND_MINIMUM = 4,
    RIN_GPU_BLEND_MAXIMUM = 5
} RinGpuBlendOp;

typedef enum RinGpuCullMode {
    RIN_GPU_CULL_NONE = 1,
    RIN_GPU_CULL_FRONT = 2,
    RIN_GPU_CULL_BACK = 3
} RinGpuCullMode;

typedef enum RinGpuFrontFace {
    RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE = 1,
    RIN_GPU_FRONT_FACE_CLOCKWISE = 2
} RinGpuFrontFace;

typedef enum RinGpuVaryingType {
    RIN_GPU_VARYING_FLOAT32 = 1,
    RIN_GPU_VARYING_SINT32 = 2
} RinGpuVaryingType;

typedef enum RinGpuVaryingInterpolation {
    RIN_GPU_INTERPOLATION_PERSPECTIVE = 1,
    RIN_GPU_INTERPOLATION_NO_PERSPECTIVE = 2,
    RIN_GPU_INTERPOLATION_FLAT = 3
} RinGpuVaryingInterpolation;

#define RIN_GPU_COLOR_WRITE_RED   0x00000001u
#define RIN_GPU_COLOR_WRITE_GREEN 0x00000002u
#define RIN_GPU_COLOR_WRITE_BLUE  0x00000004u
#define RIN_GPU_COLOR_WRITE_ALPHA 0x00000008u
#define RIN_GPU_COLOR_WRITE_ALL   0x0000000fu

typedef enum RinGpuRenderLoadOp {
    RIN_GPU_RENDER_LOAD = 1,
    RIN_GPU_RENDER_CLEAR = 2
} RinGpuRenderLoadOp;

typedef enum RinGpuRenderStoreOp {
    RIN_GPU_RENDER_STORE = 1
} RinGpuRenderStoreOp;

typedef struct RinGpuAdapterInfoV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_memory_bytes;
    uint64_t shared_memory_bytes;
    uint32_t queue_capabilities;
    uint32_t flags;
    char name[RIN_GPU_ADAPTER_NAME_MAX];
} RinGpuAdapterInfoV1;

typedef struct RinGpuBufferDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t size_bytes;
    uint32_t usage;
    uint32_t flags;
} RinGpuBufferDescV1;

typedef struct RinGpuImageDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t dimension;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_layers;
    uint32_t mip_levels;
    uint32_t sample_count;
    uint32_t usage;
    uint32_t flags;
} RinGpuImageDescV1;

typedef struct RinGpuImageInfoV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuImageDescV1 descriptor;
    uint64_t allocation_bytes;
} RinGpuImageInfoV1;

typedef struct RinGpuSamplerDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    float mip_lod_bias;
    float min_lod;
    float max_lod;
    uint32_t max_anisotropy;
    uint32_t flags;
    union {
        uint32_t compare_op;
        uint32_t reserved;
    };
} RinGpuSamplerDescV1;

typedef struct RinGpuImageCopyRegionV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t source_mip_level;
    uint32_t source_array_layer;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t source_z;
    uint32_t destination_mip_level;
    uint32_t destination_array_layer;
    uint32_t destination_x;
    uint32_t destination_y;
    uint32_t destination_z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t flags;
} RinGpuImageCopyRegionV1;

/* Uploads a rectangular image extent from CPU memory. A zero row or slice
 * pitch selects the tightly packed pitch for the specified extent. */
typedef struct RinGpuImageUploadV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint64_t source_row_pitch_bytes;
    uint64_t source_slice_pitch_bytes;
    uint32_t flags;
    uint32_t reserved;
} RinGpuImageUploadV1;

/* Reads a rectangular source image region into CPU storage. A zero row or
 * slice pitch selects tightly packed output. The source image must be
 * CPU-readable, be in COPY_SOURCE, have completed CPU upload recovery, and
 * have all prior writes completed through ringpu_wait_fence(). */
typedef struct RinGpuImageReadbackV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint64_t destination_row_pitch_bytes;
    uint64_t destination_slice_pitch_bytes;
    uint32_t flags;
    uint32_t reserved;
} RinGpuImageReadbackV1;

typedef struct RinGpuImageTransitionV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    uint32_t before_state;
    uint32_t after_state;
    uint32_t flags;
    uint32_t reserved;
} RinGpuImageTransitionV1;

typedef struct RinGpuComputePipelineDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle shader_module;
    uint32_t flags;
    uint32_t reserved;
} RinGpuComputePipelineDescV1;

typedef struct RinGpuGraphicsPipelineDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle vertex_shader;
    RinGpuHandle fragment_shader;
    uint32_t color_format;
    uint32_t primitive_topology;
    uint32_t flags;
    uint32_t reserved;
} RinGpuGraphicsPipelineDescV1;

typedef struct RinGpuGraphicsPipelineVertexDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle vertex_shader;
    RinGpuHandle fragment_shader;
    uint32_t color_format;
    uint32_t primitive_topology;
    uint32_t vertex_stride;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuGraphicsPipelineVertexDescV1;

typedef struct RinGpuVertexAttributeV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t location;
    uint32_t format;
    /* Byte offset for streamed data, or binary32 bits when CONSTANT_FLOAT32
     * is set. */
    uint32_t offset;
    /* RIN_GPU_VERTEX_ATTRIBUTE_*. */
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuVertexAttributeV1;

/* Additive multi-buffer vertex layout. V1 remains a single-stream contract;
 * V2 carries a binding index while preserving the exact V1 prefix. */
typedef struct RinGpuVertexAttributeV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t location;
    uint32_t format;
    uint32_t offset;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t binding;
    uint32_t reserved2;
} RinGpuVertexAttributeV2;

typedef struct RinGpuVertexBufferLayoutV1 {
    uint32_t binding;
    uint32_t stride;
    /* Zero advances this stream once per vertex. A non-zero value is an
     * instance divisor: element floor(instance_index / flags) is used for
     * every vertex in that instance. This preserves the v1 layout ABI while
     * making instanced vertex streams explicit; a divisor is never inferred
     * from draw count or buffer size. */
    uint32_t flags;
    uint32_t reserved;
} RinGpuVertexBufferLayoutV1;

typedef struct RinGpuVertexBufferBindingV1 {
    uint32_t binding;
    uint32_t reserved;
    RinGpuHandle buffer;
    uint64_t offset;
} RinGpuVertexBufferBindingV1;

typedef struct RinGpuGraphicsPipelineDepthDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle vertex_shader;
    RinGpuHandle fragment_shader;
    uint32_t color_format;
    uint32_t primitive_topology;
    uint32_t depth_format;
    uint32_t depth_compare;
    uint32_t depth_write_enabled;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuGraphicsPipelineDepthDescV1;

typedef struct RinGpuGraphicsPipelineBlendDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle vertex_shader;
    RinGpuHandle fragment_shader;
    uint32_t color_format;
    uint32_t primitive_topology;
    uint32_t depth_format;
    uint32_t depth_compare;
    uint32_t depth_write_enabled;
    uint32_t source_color_factor;
    uint32_t destination_color_factor;
    uint32_t color_operation;
    uint32_t source_alpha_factor;
    uint32_t destination_alpha_factor;
    uint32_t alpha_operation;
    /* R/G/B/A write bits. Zero is valid: it suppresses color stores while
     * depth and stencil operations of the same graphics pass still run. */
    uint32_t color_write_mask;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuGraphicsPipelineBlendDescV1;

/* Complete native graphics contract. It composes explicit vertex input,
 * depth/stencil, blending and winding state rather than forcing callers to
 * select mutually exclusive legacy pipeline descriptors. Position occupies
 * four consecutive Float32 vertex output locations beginning at
 * position_output_location; all user varyings are supplied separately. When
 * RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT is set, the following
 * Float32 vertex output is the programmable point size (clamped by the
 * backend's advertised point-size range). */
typedef struct RinGpuGraphicsPipelineNativeDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle vertex_shader;
    RinGpuHandle fragment_shader;
    uint32_t color_format;
    uint32_t primitive_topology;
    uint32_t vertex_stride;
    uint32_t position_output_location;
    uint32_t depth_format;
    uint32_t depth_compare;
    uint32_t depth_write_enabled;
    uint32_t blend_enabled;
    uint32_t source_color_factor;
    uint32_t destination_color_factor;
    uint32_t color_operation;
    uint32_t source_alpha_factor;
    uint32_t destination_alpha_factor;
    uint32_t alpha_operation;
    /* R/G/B/A write bits. Zero is valid for depth/stencil-only draws. */
    uint32_t color_write_mask;
    uint32_t cull_mode;
    uint32_t front_face;
    uint32_t flags;
    uint32_t reserved0;
    /* Front-face stencil state. Disabled state is fully canonical (all
     * stencil fields zero); enabled state requires D32_FLOAT_S8_UINT. */
    uint32_t stencil_test_enabled;
    uint32_t stencil_compare;
    uint32_t stencil_reference;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_fail_operation;
    uint32_t stencil_depth_fail_operation;
    uint32_t stencil_pass_operation;
    /* A zero separate_stencil_enabled requires every back-face field to be
     * zero and reuses the front-face state for both faces. */
    uint32_t separate_stencil_enabled;
    uint32_t back_stencil_compare;
    uint32_t back_stencil_reference;
    uint32_t back_stencil_read_mask;
    uint32_t back_stencil_write_mask;
    uint32_t back_stencil_fail_operation;
    uint32_t back_stencil_depth_fail_operation;
    uint32_t back_stencil_pass_operation;
} RinGpuGraphicsPipelineNativeDescV1;

/* Additive native descriptor. The V1 prefix remains exact; V2 is the only
 * native path that can carry constant blend factors and their RGBA value. */
typedef struct RinGpuGraphicsPipelineNativeDescV2 {
    RinGpuGraphicsPipelineNativeDescV1 base;
    float blend_constant_red;
    float blend_constant_green;
    float blend_constant_blue;
    float blend_constant_alpha;
} RinGpuGraphicsPipelineNativeDescV2;

typedef struct RinGpuVaryingV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t vertex_output_location;
    uint32_t fragment_input_location;
    uint32_t type;
    uint32_t interpolation;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuVaryingV1;

typedef struct RinGpuViewportV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
    uint32_t flags;
    uint32_t reserved;
} RinGpuViewportV1;

typedef struct RinGpuScissorV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t enabled;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuScissorV1;

/* Viewport/scissor use a lower-left pixel origin. Viewport x/y may be
 * negative and its rectangle is clipped to the active attachment. A disabled
 * scissor carries canonical zero coordinates and dimensions; an enabled empty
 * scissor is a valid no-raster state. */
typedef struct RinGpuRasterStateV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuViewportV1 viewport;
    RinGpuScissorV1 scissor;
    uint32_t flags;
    uint32_t reserved;
} RinGpuRasterStateV1;

/* Optional V2 suffix for dynamic polygon offset. A V1 descriptor has the
 * canonical disabled state; APIs taking a V1 pointer accept only the exact
 * supported layouts and copy extensions before caller storage is released. */
typedef struct RinGpuRasterStateV2 {
    RinGpuRasterStateV1 base;
    uint32_t polygon_offset_fill_enabled;
    uint32_t reserved0;
    float polygon_offset_factor;
    float polygon_offset_units;
} RinGpuRasterStateV2;

/* Optional V3 suffix for dynamic aliased line width. V1/V2 descriptors retain
 * the canonical one-pixel line width. */
typedef struct RinGpuRasterStateV3 {
    RinGpuRasterStateV2 base;
    float line_width;
    uint32_t reserved1;
} RinGpuRasterStateV3;

/* Optional V4 suffix for WebGL sample coverage. V1/V2/V3 descriptors keep
 * the canonical disabled state, so existing clients retain full coverage.
 * The current software target has one storage sample, but the state is still
 * carried explicitly: zero coverage (and its inverted form) must suppress
 * color/depth/stencil fragment operations rather than becoming a no-op. */
typedef struct RinGpuRasterStateV4 {
    RinGpuRasterStateV3 base;
    uint32_t sample_coverage_enabled;
    float sample_coverage_value;
    uint32_t sample_coverage_invert;
    uint32_t reserved2;
} RinGpuRasterStateV4;

/* Optional V5 suffix for ordered quantization of normalized color targets.
 * Earlier descriptors retain a disabled state; GLES-facing callers submit V5
 * explicitly because its default is enabled. */
typedef struct RinGpuRasterStateV5 {
    RinGpuRasterStateV4 base;
    uint32_t dither_enabled;
    uint32_t reserved3;
} RinGpuRasterStateV5;

typedef struct RinGpuBufferBindingV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t binding;
    uint32_t access;
    RinGpuHandle buffer;
    uint64_t offset;
    uint64_t size_bytes;
    uint32_t flags;
    uint32_t reserved;
} RinGpuBufferBindingV1;

typedef struct RinGpuGraphicsBindingV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t binding;
    uint32_t kind;
    uint32_t access;
    uint32_t flags;
    RinGpuHandle resource;
    uint64_t offset;
    uint64_t size_bytes;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuGraphicsBindingV1;

typedef struct RinGpuDispatchV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle bind_group;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuDispatchV1;

typedef struct RinGpuComputeBarrierV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t source_access;
    uint32_t destination_access;
    uint32_t flags;
    uint32_t reserved;
} RinGpuComputeBarrierV1;

typedef struct RinGpuGraphicsBarrierV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t source_access;
    uint32_t destination_access;
    uint32_t flags;
    uint32_t reserved;
} RinGpuGraphicsBarrierV1;

typedef struct RinGpuDrawV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle color_target;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
    uint32_t flags;
    uint32_t reserved;
} RinGpuDrawV1;

typedef struct RinGpuDrawVerticesV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle color_target;
    RinGpuHandle vertex_buffer;
    uint64_t vertex_offset;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
    uint32_t flags;
    uint32_t reserved;
} RinGpuDrawVerticesV1;

/* V2 draw commands source vertex attributes from the pipeline's named
 * bindings. Every streamed pipeline binding must occur exactly once; a
 * constant-only pipeline requires binding_count == 0. */
typedef struct RinGpuDrawVerticesV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle color_target;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
    uint32_t binding_count;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    RinGpuVertexBufferBindingV1
        vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
} RinGpuDrawVerticesV2;

typedef struct RinGpuDrawIndexedV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle color_target;
    RinGpuHandle vertex_buffer;
    RinGpuHandle index_buffer;
    uint64_t vertex_offset;
    uint64_t index_offset;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t first_instance;
    uint32_t flags;
    uint32_t reserved;
} RinGpuDrawIndexedV1;

typedef struct RinGpuDrawIndexedV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle color_target;
    RinGpuHandle index_buffer;
    uint64_t index_offset;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t first_instance;
    uint32_t binding_count;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    RinGpuVertexBufferBindingV1
        vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
} RinGpuDrawIndexedV2;

/* Additive indexed-draw form. Existing V1 callers retain the exact original
 * layout and implicitly use base_vertex == 0. */
typedef struct RinGpuDrawIndexedBaseVertexV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle pipeline;
    RinGpuHandle color_target;
    RinGpuHandle vertex_buffer;
    RinGpuHandle index_buffer;
    uint64_t vertex_offset;
    uint64_t index_offset;
    uint32_t index_format;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t first_instance;
    uint32_t flags;
    int32_t base_vertex;
} RinGpuDrawIndexedBaseVertexV1;

/* Lower-left clear region shared by color and depth render passes. Disabled
 * regions are canonical all-zero/full-attachment clears; enabled empty
 * regions are valid no-ops. */
typedef struct RinGpuClearRegionV1 {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t enabled;
    uint32_t reserved;
} RinGpuClearRegionV1;

/* WebGL 1's WEBGL_draw_buffers baseline requires four independently selected
 * color attachments.  Keep the attachment description small and explicit:
 * a zero handle is legal only for a bit that is clear in the accompanying
 * active-color mask. */
#define RIN_GPU_MAX_COLOR_TARGETS 4u

typedef struct RinGpuColorAttachmentV1 {
    RinGpuHandle target;
    uint32_t mip_level;
    uint32_t array_layer;
} RinGpuColorAttachmentV1;

typedef struct RinGpuRenderPassDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle color_target;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t load_op;
    uint32_t store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    uint32_t flags;
    uint32_t reserved;
    /* R/G/B/A write bits; LOAD requires this to be zero. */
    uint32_t color_write_mask;
    uint32_t reserved1;
    RinGpuClearRegionV1 clear_region;
} RinGpuRenderPassDescV1;

/* A multi-render-target graphics pass. Every set bit in active_color_mask
 * names the corresponding physical color attachment; all active attachments
 * must be 2D single-sample COLOR_TARGET images with identical dimensions.
 * CLEAR writes the same clear color through the shared color write mask to
 * each active attachment, while LOAD requires a zero mask. Depth and stencil
 * are optional and may name the same D32S8 image or separate compatible
 * images. This is deliberately a new ABI instead of silently treating the
 * first target as the whole pass. */
typedef struct RinGpuRenderPassMrtDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t active_color_mask;
    uint32_t reserved0;
    RinGpuColorAttachmentV1 color_attachments[RIN_GPU_MAX_COLOR_TARGETS];
    RinGpuHandle depth_target;
    RinGpuHandle stencil_target;
    uint32_t depth_mip_level;
    uint32_t depth_array_layer;
    uint32_t stencil_mip_level;
    uint32_t stencil_array_layer;
    uint32_t color_load_op;
    uint32_t color_store_op;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    float clear_depth;
    uint32_t clear_stencil;
    uint32_t stencil_write_mask;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t color_write_mask;
    uint32_t reserved2;
    RinGpuClearRegionV1 clear_region;
} RinGpuRenderPassMrtDescV1;

typedef struct RinGpuRenderPassDepthDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle color_target;
    RinGpuHandle depth_target;
    uint32_t color_mip_level;
    uint32_t color_array_layer;
    uint32_t depth_mip_level;
    uint32_t depth_array_layer;
    uint32_t color_load_op;
    uint32_t color_store_op;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    float clear_depth;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    /* D32_FLOAT requires canonical zero fields. D32_FLOAT_S8_UINT supports
     * LOAD/CLEAR stencil operations, one 8-bit clear value, and an 8-bit
     * front-face write mask for a CLEAR operation. */
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    uint32_t clear_stencil;
    uint32_t stencil_write_mask;
    uint32_t reserved2;
    /* R/G/B/A write bits; a color LOAD requires this to be zero. */
    uint32_t color_write_mask;
    uint32_t reserved3;
    RinGpuClearRegionV1 clear_region;
} RinGpuRenderPassDepthDescV1;

/* A depth/stencil render pass with distinct physical attachments. The depth
 * target must expose a D32 plane (D32_FLOAT or D32_FLOAT_S8_UINT) and the
 * stencil target must expose an S8 plane (S8_UINT or D32_FLOAT_S8_UINT).
 * Pipelines using either aspect use D32_FLOAT_S8_UINT as their logical
 * depth_format. The targets must remain distinct even when both use D32S8. */
typedef struct RinGpuRenderPassDepthStencilDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle color_target;
    RinGpuHandle depth_target;
    RinGpuHandle stencil_target;
    uint32_t color_mip_level;
    uint32_t color_array_layer;
    uint32_t depth_mip_level;
    uint32_t depth_array_layer;
    uint32_t stencil_mip_level;
    uint32_t stencil_array_layer;
    uint32_t color_load_op;
    uint32_t color_store_op;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    float clear_red;
    float clear_green;
    float clear_blue;
    float clear_alpha;
    float clear_depth;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    uint32_t clear_stencil;
    uint32_t stencil_write_mask;
    uint32_t reserved2;
    uint32_t color_write_mask;
    uint32_t reserved3;
    RinGpuClearRegionV1 clear_region;
} RinGpuRenderPassDepthStencilDescV1;

typedef struct RinGpuPresentV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle image;
    uint32_t display_id;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} RinGpuPresentV1;

typedef struct RinGpuDisplayInfoV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t display_id;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihertz;
    uint32_t format;
    uint32_t physical_width_mm;
    uint32_t physical_height_mm;
    uint32_t scale_milli;
    uint32_t reserved0;
    char name[RIN_GPU_DISPLAY_NAME_MAX];
} RinGpuDisplayInfoV1;

typedef struct RinGpuQueueDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t capabilities;
    uint32_t flags;
} RinGpuQueueDescV1;

typedef struct RinGpuCommandListDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t capabilities;
    uint32_t flags;
} RinGpuCommandListDescV1;

typedef struct RinGpuSubmitInfoV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RinGpuHandle command_list;
    RinGpuHandle signal_fence;
    uint64_t signal_value;
} RinGpuSubmitInfoV1;

typedef struct RinGpuCore RinGpuCore;

int ringpu_get_adapter_info(const RinGpuCore* core, RinGpuAdapterInfoV1* info);
int ringpu_get_display_count(const RinGpuCore* core, uint32_t* count);
int ringpu_get_display_info(const RinGpuCore* core, uint32_t index,
                            RinGpuDisplayInfoV1* info);
int ringpu_create_buffer(RinGpuCore* core, const RinGpuBufferDescV1* desc,
                         RinGpuHandle* buffer);
/* Copies a non-empty CPU range into a CPU-visible buffer.  A successful call
 * has completed the backend's required CPU-to-device cache synchronization. */
int ringpu_upload_buffer(RinGpuCore* core, RinGpuHandle buffer,
                         uint64_t destination_offset, const void* source,
                         uint64_t size_bytes);
int ringpu_create_image(RinGpuCore* core, const RinGpuImageDescV1* desc,
                        RinGpuHandle* image);
/* Copies a non-empty CPU region into a CPU-visible image. `source_size` must
 * cover every source row and slice selected by `upload`; success includes the
 * backend's required CPU-to-device cache synchronization. A complete mip/layer
 * upload records that subresource as COPY_DESTINATION. Partial uploads retain
 * their prior state so unread initialized bytes are never implied. */
int ringpu_upload_image(RinGpuCore* core, RinGpuHandle image,
                        const RinGpuImageUploadV1* upload,
                        const void* source, uint64_t source_size);
int ringpu_create_sampler(RinGpuCore* core, const RinGpuSamplerDescV1* desc,
                          RinGpuHandle* sampler);
int ringpu_get_sampler_info(const RinGpuCore* core, RinGpuHandle sampler,
                            RinGpuSamplerDescV1* info);
int ringpu_get_image_info(const RinGpuCore* core, RinGpuHandle image,
                          RinGpuImageInfoV1* info);
int ringpu_get_image_state(const RinGpuCore* core, RinGpuHandle image,
                           uint32_t mip_level, uint32_t array_layer,
                           uint32_t* state);
int ringpu_create_shader_module(RinGpuCore* core, const void* rin_shader_ir,
                                uint64_t shader_size,
                                RinGpuHandle* shader_module);
int ringpu_get_shader_info(const RinGpuCore* core,
                           RinGpuHandle shader_module,
                           RinShaderInfoV1* info);
int ringpu_create_compute_pipeline(
    RinGpuCore* core, const RinGpuComputePipelineDescV1* desc,
    RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline(
    RinGpuCore* core, const RinGpuGraphicsPipelineDescV1* desc,
    RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_vertex(
    RinGpuCore* core, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    RinGpuHandle* pipeline);
/* Additive multi-stream form. The V1 descriptor supplies shader and fixed
 * state; every non-constant V2 attribute names one layout binding. */
int ringpu_create_graphics_pipeline_vertex_bindings(
    RinGpuCore* core, const RinGpuGraphicsPipelineVertexDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_depth(
    RinGpuCore* core, const RinGpuGraphicsPipelineDepthDescV1* desc,
    RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_blend(
    RinGpuCore* core, const RinGpuGraphicsPipelineBlendDescV1* desc,
    RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_native(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_native_v2(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV1* attributes, uint32_t attribute_count,
    const RinGpuVaryingV1* varyings, uint32_t varying_count,
    RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_native_vertex_bindings(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV1* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline);
int ringpu_create_graphics_pipeline_native_vertex_bindings_v2(
    RinGpuCore* core, const RinGpuGraphicsPipelineNativeDescV2* desc,
    const RinGpuVertexAttributeV2* attributes, uint32_t attribute_count,
    const RinGpuVertexBufferLayoutV1* vertex_bindings,
    uint32_t vertex_binding_count, const RinGpuVaryingV1* varyings,
    uint32_t varying_count, RinGpuHandle* pipeline);
int ringpu_create_compute_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group);
int ringpu_create_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group);
int ringpu_create_graphics_bind_group_typed(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuGraphicsBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group);
int ringpu_create_queue(RinGpuCore* core, const RinGpuQueueDescV1* desc,
                        RinGpuHandle* queue);
int ringpu_create_command_list(RinGpuCore* core,
                               const RinGpuCommandListDescV1* desc,
                               RinGpuHandle* command_list);
int ringpu_create_fence(RinGpuCore* core, uint64_t initial_value,
                        RinGpuHandle* fence);
int ringpu_command_list_reset(RinGpuCore* core, RinGpuHandle command_list);
int ringpu_command_copy_buffer(RinGpuCore* core, RinGpuHandle command_list,
                               RinGpuHandle destination,
                               uint64_t destination_offset,
                               RinGpuHandle source, uint64_t source_offset,
                               uint64_t size_bytes);
int ringpu_command_copy_image(RinGpuCore* core, RinGpuHandle command_list,
                              RinGpuHandle destination,
                              RinGpuHandle source,
                              const RinGpuImageCopyRegionV1* region);
int ringpu_command_transition_image(RinGpuCore* core,
                                    RinGpuHandle command_list,
                                    RinGpuHandle image,
                                    const RinGpuImageTransitionV1* transition);
int ringpu_command_dispatch(RinGpuCore* core, RinGpuHandle command_list,
                            const RinGpuDispatchV1* dispatch);
int ringpu_command_compute_barrier(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuComputeBarrierV1* barrier);
int ringpu_command_begin_render_pass(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDescV1* render_pass);
int ringpu_command_begin_render_pass_mrt(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassMrtDescV1* render_pass);
int ringpu_command_begin_render_pass_depth(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDepthDescV1* render_pass);
int ringpu_command_begin_render_pass_depth_stencil(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDepthStencilDescV1* render_pass);
int ringpu_command_bind_graphics_resources(RinGpuCore* core,
                                           RinGpuHandle command_list,
                                           RinGpuHandle bind_group);
int ringpu_command_graphics_barrier(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuGraphicsBarrierV1* barrier);
int ringpu_command_set_raster_state(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRasterStateV1* state);
int ringpu_command_draw(RinGpuCore* core, RinGpuHandle command_list,
                        const RinGpuDrawV1* draw);
int ringpu_command_draw_vertices(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawVerticesV1* draw);
int ringpu_command_draw_vertices_v2(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawVerticesV2* draw);
int ringpu_command_draw_indexed(RinGpuCore* core,
                                RinGpuHandle command_list,
                                const RinGpuDrawIndexedV1* draw);
int ringpu_command_draw_indexed_v2(RinGpuCore* core,
                                   RinGpuHandle command_list,
                                   const RinGpuDrawIndexedV2* draw);
int ringpu_command_draw_indexed_base_vertex(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawIndexedBaseVertexV1* draw);
int ringpu_command_end_render_pass(RinGpuCore* core,
                                   RinGpuHandle command_list);
int ringpu_command_present(RinGpuCore* core, RinGpuHandle command_list,
                           const RinGpuPresentV1* present);
int ringpu_command_list_close(RinGpuCore* core, RinGpuHandle command_list);
int ringpu_queue_submit(RinGpuCore* core, RinGpuHandle queue,
                        const RinGpuSubmitInfoV1* submit);
int ringpu_fence_value(const RinGpuCore* core, RinGpuHandle fence,
                       uint64_t* value);
/* Waits until all work submitted before the requested fence value has
 * completed. Submission success alone does not establish this guarantee. */
int ringpu_wait_fence(RinGpuCore* core, RinGpuHandle fence,
                      uint64_t value, uint64_t timeout_ns);
int ringpu_readback_image(RinGpuCore* core, RinGpuHandle image,
                          const RinGpuImageReadbackV1* readback,
                          void* destination, uint64_t destination_size);
int ringpu_destroy(RinGpuCore* core, RinGpuHandle object);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinGpuAdapterInfoV1) == 104u,
               "RinGPU adapter ABI drift");
_Static_assert(sizeof(RinGpuDisplayInfoV1) == 112u,
               "RinGPU display info ABI drift");
_Static_assert(sizeof(RinGpuBufferDescV1) == 24u,
               "RinGPU buffer ABI drift");
_Static_assert(sizeof(RinGpuImageDescV1) == 48u,
               "RinGPU image ABI drift");
_Static_assert(sizeof(RinGpuImageInfoV1) == 64u,
               "RinGPU image info ABI drift");
_Static_assert(sizeof(RinGpuSamplerDescV1) == 56u,
               "RinGPU sampler ABI drift");
_Static_assert(sizeof(RinGpuImageCopyRegionV1) == 64u,
               "RinGPU image copy ABI drift");
_Static_assert(sizeof(RinGpuImageUploadV1) == 64u,
               "RinGPU image upload ABI drift");
_Static_assert(sizeof(RinGpuImageReadbackV1) == 64u,
               "RinGPU image readback ABI drift");
_Static_assert(sizeof(RinGpuImageTransitionV1) == 40u,
               "RinGPU image transition ABI drift");
_Static_assert(sizeof(RinGpuComputePipelineDescV1) == 24u,
               "RinGPU compute pipeline ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineDescV1) == 40u,
               "RinGPU graphics pipeline ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineVertexDescV1) == 48u,
               "RinGPU vertex graphics pipeline ABI drift");
_Static_assert(sizeof(RinGpuVertexAttributeV1) == 32u,
               "RinGPU vertex attribute ABI drift");
_Static_assert(sizeof(RinGpuVertexAttributeV2) == 40u,
               "RinGPU multi-buffer vertex attribute ABI drift");
_Static_assert(sizeof(RinGpuVertexBufferLayoutV1) == 16u,
               "RinGPU vertex-buffer layout ABI drift");
_Static_assert(sizeof(RinGpuVertexBufferBindingV1) == 24u,
               "RinGPU vertex-buffer binding ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineDepthDescV1) == 56u,
               "RinGPU depth graphics pipeline ABI drift");
/* MinGW's i686 ABI aligns uint64_t to eight bytes.  These pointer-free
 * records therefore retain their 64-bit layout there; other 32-bit ABIs use
 * their established four-byte uint64_t alignment until a packed wire ABI is
 * explicitly introduced. */
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuGraphicsPipelineBlendDescV1) == 88u,
               "RinGPU blend graphics pipeline ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineNativeDescV1) == 168u,
               "RinGPU native graphics pipeline ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineNativeDescV2) == 184u,
               "RinGPU native graphics pipeline V2 ABI drift");
#else
_Static_assert(sizeof(RinGpuGraphicsPipelineBlendDescV1) == 84u,
               "RinGPU blend graphics pipeline ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineNativeDescV1) == 164u,
               "RinGPU native graphics pipeline ABI drift");
_Static_assert(sizeof(RinGpuGraphicsPipelineNativeDescV2) == 180u,
               "RinGPU native graphics pipeline V2 ABI drift");
#endif
_Static_assert(sizeof(RinGpuVaryingV1) == 36u,
               "RinGPU varying ABI drift");
_Static_assert(sizeof(RinGpuViewportV1) == 40u,
               "RinGPU viewport ABI drift");
_Static_assert(sizeof(RinGpuScissorV1) == 40u,
               "RinGPU scissor ABI drift");
_Static_assert(sizeof(RinGpuRasterStateV1) == 96u,
               "RinGPU raster state ABI drift");
_Static_assert(sizeof(RinGpuRasterStateV2) == 112u,
               "RinGPU polygon-offset raster state ABI drift");
_Static_assert(sizeof(RinGpuRasterStateV3) == 120u,
               "RinGPU line-width raster state ABI drift");
_Static_assert(sizeof(RinGpuRasterStateV4) == 136u,
               "RinGPU sample-coverage raster state ABI drift");
_Static_assert(sizeof(RinGpuRasterStateV5) == 144u,
               "RinGPU dither raster state ABI drift");
_Static_assert(sizeof(RinGpuBufferBindingV1) == 48u,
               "RinGPU buffer binding ABI drift");
_Static_assert(sizeof(RinGpuGraphicsBindingV1) == 64u,
               "RinGPU graphics binding ABI drift");
_Static_assert(sizeof(RinGpuDispatchV1) == 48u,
               "RinGPU dispatch ABI drift");
_Static_assert(sizeof(RinGpuComputeBarrierV1) == 24u,
               "RinGPU compute barrier ABI drift");
_Static_assert(sizeof(RinGpuGraphicsBarrierV1) == 24u,
               "RinGPU graphics barrier ABI drift");
_Static_assert(sizeof(RinGpuDrawV1) == 56u,
               "RinGPU draw ABI drift");
_Static_assert(sizeof(RinGpuDrawVerticesV1) == 72u,
               "RinGPU vertex draw ABI drift");
_Static_assert(sizeof(RinGpuDrawVerticesV2) == 832u,
               "RinGPU multi-buffer vertex draw ABI drift");
_Static_assert(sizeof(RinGpuDrawIndexedV1) == 96u,
               "RinGPU indexed draw ABI drift");
_Static_assert(sizeof(RinGpuDrawIndexedV2) == 856u,
               "RinGPU multi-buffer indexed draw ABI drift");
_Static_assert(sizeof(RinGpuDrawIndexedBaseVertexV1) == 96u,
               "RinGPU base-vertex indexed draw ABI drift");
_Static_assert(sizeof(RinGpuClearRegionV1) == 24u,
               "RinGPU clear-region ABI drift");
_Static_assert(sizeof(RinGpuRenderPassDescV1) == 88u,
               "RinGPU render-pass ABI drift");
_Static_assert(sizeof(RinGpuColorAttachmentV1) == 16u,
               "RinGPU color attachment ABI drift");
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuRenderPassMrtDescV1) == 208u,
               "RinGPU MRT render-pass ABI drift");
#else
_Static_assert(sizeof(RinGpuRenderPassMrtDescV1) == 204u,
               "RinGPU MRT render-pass ABI drift");
#endif
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuRenderPassDepthDescV1) == 144u,
               "RinGPU depth render-pass ABI drift");
#else
_Static_assert(sizeof(RinGpuRenderPassDepthDescV1) == 140u,
               "RinGPU depth render-pass ABI drift");
#endif
#if UINTPTR_MAX == UINT64_MAX || defined(__MINGW32__)
_Static_assert(sizeof(RinGpuRenderPassDepthStencilDescV1) == 160u,
               "RinGPU separate depth/stencil render-pass ABI drift");
#else
_Static_assert(sizeof(RinGpuRenderPassDepthStencilDescV1) == 156u,
               "RinGPU separate depth/stencil render-pass ABI drift");
#endif
_Static_assert(sizeof(RinGpuPresentV1) == 32u,
               "RinGPU present ABI drift");
_Static_assert(sizeof(RinGpuSubmitInfoV1) == 32u,
               "RinGPU submit ABI drift");
#endif

#endif /* RIN_API_RIN_GPU_H */
