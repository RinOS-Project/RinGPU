// SPDX-License-Identifier: MIT
#ifndef RIN_API_RIN_SHADER_H
#define RIN_API_RIN_SHADER_H

#include <stddef.h>
#include <stdint.h>

#include "../../../rinresource/include/rinresource/loader.h"

#define RIN_SHADER_MAGIC UINT32_C(0x31485352) /* "RSH1" */
#define RIN_SHADER_IR_VERSION 1u
#define RIN_SHADER_MAX_INSTRUCTIONS 65536u
#define RIN_SHADER_MAX_REGISTERS 256u
#define RIN_SHADER_MAX_IO 32u
#define RIN_SHADER_MAX_RESOURCES 64u
#define RIN_SHADER_UNUSED UINT16_C(0xffff)
#define RIN_SHADER_PUSH_CONSTANT_BYTES 128u
#define RIN_SHADER_FLAG_PUSH_CONSTANTS UINT32_C(0x00000001)
#define RIN_SHADER_KNOWN_FLAGS RIN_SHADER_FLAG_PUSH_CONSTANTS

/* `SAMPLE_IMAGE_I32`/`SAMPLE_IMAGE_F32` use source0 as one normalized
 * coordinate for a 1D image; source1 is unused. `SAMPLE_COMPARE_*` uses
 * source0 as that coordinate and source1 as the reference depth. They return
 * one scalar with the same type as their operands; a comparison result is
 * exactly zero or one. The 2D variant returns one selected normalized/color
 * component in its scalar
 * destination register. The selector is carried in the instruction flags
 * field; all other sampling opcodes require zero flags. Four 2D instructions
 * with selectors R/G/B/A form an explicit RGBA sample without imposing a
 * hidden four-register destination convention.
 *
 * `SAMPLE_IMAGE_2D_LOD_F32` and `SAMPLE_IMAGE_2D_BIAS_F32` name an initialized
 * finite binary32 register in `immediate`. `SAMPLE_IMAGE_2D_GRAD_F32` names
 * the first of four initialized Float32 registers in `immediate`, ordered as
 * dU/dX, dU/dY, dV/dX, dV/dY. Their `resource` field packs the image and
 * sampler binding indices below, leaving source0/source1 available for the
 * live U/V coordinates and destination for the selected component. These are
 * distinct opcodes rather than embedding-side lookups, so explicit LOD, bias,
 * and gradients remain part of validated RSH1 execution. */
#define RIN_SHADER_SAMPLE_COMPONENT_RED   0u
#define RIN_SHADER_SAMPLE_COMPONENT_GREEN 1u
#define RIN_SHADER_SAMPLE_COMPONENT_BLUE  2u
#define RIN_SHADER_SAMPLE_COMPONENT_ALPHA 3u
#define RIN_SHADER_SAMPLE_COMPONENT_MASK  UINT16_C(0x0003)
#define RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS 6u
#define RIN_SHADER_SAMPLE_2D_LOD_BINDING_MASK UINT16_C(0x003f)
#define RIN_SHADER_SAMPLE_2D_LOD_PACK_BINDINGS(image_binding, sampler_binding) \
    ((uint16_t)((((uint16_t)(image_binding)) & RIN_SHADER_SAMPLE_2D_LOD_BINDING_MASK) | \
                ((((uint16_t)(sampler_binding)) & RIN_SHADER_SAMPLE_2D_LOD_BINDING_MASK) \
                 << RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS)))
#define RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(packed) \
    ((uint16_t)((packed) & RIN_SHADER_SAMPLE_2D_LOD_BINDING_MASK))
#define RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(packed) \
    ((uint16_t)(((packed) >> RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS) & \
                RIN_SHADER_SAMPLE_2D_LOD_BINDING_MASK))
#define RIN_SHADER_SAMPLE_2D_BIAS_PACK_BINDINGS(image_binding, sampler_binding) \
    RIN_SHADER_SAMPLE_2D_LOD_PACK_BINDINGS(image_binding, sampler_binding)
#define RIN_SHADER_SAMPLE_2D_BIAS_IMAGE_BINDING(packed) \
    RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(packed)
#define RIN_SHADER_SAMPLE_2D_BIAS_SAMPLER_BINDING(packed) \
    RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(packed)
#define RIN_SHADER_SAMPLE_2D_GRAD_PACK_BINDINGS(image_binding, sampler_binding) \
    RIN_SHADER_SAMPLE_2D_LOD_PACK_BINDINGS(image_binding, sampler_binding)
#define RIN_SHADER_SAMPLE_2D_GRAD_IMAGE_BINDING(packed) \
    RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(packed)
#define RIN_SHADER_SAMPLE_2D_GRAD_SAMPLER_BINDING(packed) \
    RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(packed)

/* Cube lookups use the same six-bit image/sampler packing as explicit-LOD
 * 2D lookups. The instruction immediate names the third coordinate register. */
#define RIN_SHADER_SAMPLE_CUBE_BINDING_BITS RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS
#define RIN_SHADER_SAMPLE_CUBE_BINDING_MASK RIN_SHADER_SAMPLE_2D_LOD_BINDING_MASK
#define RIN_SHADER_SAMPLE_CUBE_PACK_BINDINGS(image_binding, sampler_binding) \
    RIN_SHADER_SAMPLE_2D_LOD_PACK_BINDINGS(image_binding, sampler_binding)
#define RIN_SHADER_SAMPLE_CUBE_IMAGE_BINDING(packed) \
    RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(packed)
#define RIN_SHADER_SAMPLE_CUBE_SAMPLER_BINDING(packed) \
    RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(packed)

typedef enum RinShaderResult {
    RIN_SHADER_OK = 0,
    RIN_SHADER_ERROR_INVALID_ARGUMENT = -1,
    RIN_SHADER_ERROR_BAD_HEADER = -2,
    RIN_SHADER_ERROR_BOUNDS = -3,
    RIN_SHADER_ERROR_UNSUPPORTED = -4,
    RIN_SHADER_ERROR_INVALID_INSTRUCTION = -5,
    RIN_SHADER_ERROR_INVALID_REGISTER = -6,
    RIN_SHADER_ERROR_UNINITIALIZED_REGISTER = -7,
    RIN_SHADER_ERROR_INVALID_RESOURCE = -8,
    RIN_SHADER_ERROR_CONTROL_FLOW = -9,
    RIN_SHADER_ERROR_NO_MEMORY = -10
} RinShaderResult;

typedef enum RinShaderStage {
    RIN_SHADER_STAGE_VERTEX = 1,
    RIN_SHADER_STAGE_FRAGMENT = 2,
    RIN_SHADER_STAGE_COMPUTE = 3
} RinShaderStage;

typedef enum RinShaderResourceKind {
    RIN_SHADER_RESOURCE_NONE = 0,
    RIN_SHADER_RESOURCE_STORAGE_BUFFER = 1,
    RIN_SHADER_RESOURCE_SAMPLED_IMAGE = 2,
    RIN_SHADER_RESOURCE_SAMPLER = 3,
    RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE = 4,
    RIN_SHADER_RESOURCE_COMPARISON_SAMPLER = 5
} RinShaderResourceKind;

typedef enum RinShaderOpcode {
    RIN_SHADER_OP_NOP = 0,
    RIN_SHADER_OP_CONST_I32 = 1,
    RIN_SHADER_OP_MOV = 2,
    RIN_SHADER_OP_ADD_I32 = 3,
    RIN_SHADER_OP_MUL_I32 = 4,
    RIN_SHADER_OP_MIN_I32 = 5,
    RIN_SHADER_OP_MAX_I32 = 6,
    RIN_SHADER_OP_LOAD_INPUT = 7,
    RIN_SHADER_OP_STORE_OUTPUT = 8,
    RIN_SHADER_OP_LOAD_RESOURCE_I32 = 9,
    RIN_SHADER_OP_STORE_RESOURCE_I32 = 10,
    RIN_SHADER_OP_JUMP = 11,
    RIN_SHADER_OP_JUMP_IF = 12,
    RIN_SHADER_OP_RETURN = 13,
    RIN_SHADER_OP_SAMPLE_IMAGE_I32 = 14,
    RIN_SHADER_OP_SAMPLE_COMPARE_I32 = 15,
    RIN_SHADER_OP_CONST_F32 = 16,
    RIN_SHADER_OP_SUB_I32 = 17,
    RIN_SHADER_OP_DIV_I32 = 18,
    RIN_SHADER_OP_MOD_I32 = 19,
    RIN_SHADER_OP_ADD_F32 = 20,
    RIN_SHADER_OP_SUB_F32 = 21,
    RIN_SHADER_OP_MUL_F32 = 22,
    RIN_SHADER_OP_DIV_F32 = 23,
    RIN_SHADER_OP_MIN_F32 = 24,
    RIN_SHADER_OP_MAX_F32 = 25,
    RIN_SHADER_OP_AND_I32 = 26,
    RIN_SHADER_OP_OR_I32 = 27,
    RIN_SHADER_OP_XOR_I32 = 28,
    RIN_SHADER_OP_SHL_I32 = 29,
    RIN_SHADER_OP_SHR_I32 = 30,
    RIN_SHADER_OP_CMP_EQ_I32 = 31,
    RIN_SHADER_OP_CMP_NE_I32 = 32,
    RIN_SHADER_OP_CMP_LT_I32 = 33,
    RIN_SHADER_OP_CMP_LE_I32 = 34,
    RIN_SHADER_OP_CMP_GT_I32 = 35,
    RIN_SHADER_OP_CMP_GE_I32 = 36,
    RIN_SHADER_OP_CMP_EQ_F32 = 37,
    RIN_SHADER_OP_CMP_NE_F32 = 38,
    RIN_SHADER_OP_CMP_LT_F32 = 39,
    RIN_SHADER_OP_CMP_LE_F32 = 40,
    RIN_SHADER_OP_CMP_GT_F32 = 41,
    RIN_SHADER_OP_CMP_GE_F32 = 42,
    RIN_SHADER_OP_I32_TO_F32 = 43,
    RIN_SHADER_OP_F32_TO_I32 = 44,
    RIN_SHADER_OP_LOAD_INPUT_F32 = 45,
    RIN_SHADER_OP_STORE_OUTPUT_F32 = 46,
    RIN_SHADER_OP_LOAD_RESOURCE_F32 = 47,
    RIN_SHADER_OP_STORE_RESOURCE_F32 = 48,
    RIN_SHADER_OP_SAMPLE_IMAGE_F32 = 49,
    RIN_SHADER_OP_SAMPLE_COMPARE_F32 = 50,
    RIN_SHADER_OP_LOAD_BUILTIN_I32 = 51,
    RIN_SHADER_OP_LOAD_BUILTIN_F32 = 52,
    RIN_SHADER_OP_DISCARD = 53,
    /* Source0/source1 are the U/V coordinates. `resource` names the sampled
     * RGBA image and `immediate` names the sampler resource. `flags` selects
     * the returned R/G/B/A scalar component. */
    RIN_SHADER_OP_SAMPLE_IMAGE_2D_I32 = 54,
    RIN_SHADER_OP_SAMPLE_IMAGE_2D_F32 = 55,
    /* Fragment-stage scalar derivatives. Source0 is an F32 value whose
     * screen-space gradients are supplied by the raster executor; all other
     * operands and flags are zero/unused. FWIDTH is abs(dFdx)+abs(dFdy).
     * They deliberately remain scalar so GLSL vector overloads lower to one
     * instruction per component without changing the RSH1 wire layout. */
    RIN_SHADER_OP_DFDX_F32 = 56,
    RIN_SHADER_OP_DFDY_F32 = 57,
    RIN_SHADER_OP_FWIDTH_F32 = 58,
    /* Scalar floating-point floor. It is kept explicit in RSH1 so GLSL
     * rounding/remainder expressions execute in the validated backend rather
     * than being evaluated by an embedding. */
    RIN_SHADER_OP_FLOOR_F32 = 59,
    /* Scalar finite binary32 square root for GLSL scalar/vector geometry. */
    RIN_SHADER_OP_SQRT_F32 = 60,
    /* Scalar finite binary32 trigonometric operations. The GLSL frontend
     * scalarizes vector overloads; ATAN2 takes y in source0 and x in source1. */
    RIN_SHADER_OP_SIN_F32 = 61,
    RIN_SHADER_OP_COS_F32 = 62,
    RIN_SHADER_OP_ATAN_F32 = 63,
    RIN_SHADER_OP_ATAN2_F32 = 64,
    RIN_SHADER_OP_ASIN_F32 = 65,
    RIN_SHADER_OP_ACOS_F32 = 66,
    /* Scalar finite binary32 exponential/logarithmic operations. POW takes
     * the positive base in source0 and exponent in source1. */
    RIN_SHADER_OP_EXP2_F32 = 67,
    RIN_SHADER_OP_LOG2_F32 = 68,
    RIN_SHADER_OP_POW_F32 = 69,
    /* Explicit fragment 2D LOD sampling. See the packed binding contract
     * above; immediate names the initialized Float32 LOD register. */
    RIN_SHADER_OP_SAMPLE_IMAGE_2D_LOD_F32 = 70,
    /* Fragment implicit 2D sampling with a live Float32 LOD bias register.
     * The software backend keeps derivative/aniso selection and applies this
     * bias before the sampler's min/max LOD clamp. */
    RIN_SHADER_OP_SAMPLE_IMAGE_2D_BIAS_F32 = 71,
    /* Fragment 2D sampling with four explicit Float32 coordinate gradients
     * held at consecutive registers beginning at immediate. */
    RIN_SHADER_OP_SAMPLE_IMAGE_2D_GRAD_F32 = 72,
    RIN_SHADER_OP_SAMPLE_IMAGE_CUBE_F32 = 73,
    RIN_SHADER_OP_LOAD_PUSH_CONSTANT_I32 = 74,
    RIN_SHADER_OP_LOAD_PUSH_CONSTANT_F32 = 75,
    RIN_SHADER_OP_LAST = RIN_SHADER_OP_LOAD_PUSH_CONSTANT_F32
} RinShaderOpcode;

typedef enum RinShaderBuiltin {
    RIN_SHADER_BUILTIN_VERTEX_INDEX = 0,
    RIN_SHADER_BUILTIN_INSTANCE_INDEX = 1,
    RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_X = 2,
    RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_Y = 3,
    RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_Z = 4,
    RIN_SHADER_BUILTIN_LOCAL_INVOCATION_X = 5,
    RIN_SHADER_BUILTIN_LOCAL_INVOCATION_Y = 6,
    RIN_SHADER_BUILTIN_LOCAL_INVOCATION_Z = 7,
    RIN_SHADER_BUILTIN_WORKGROUP_X = 8,
    RIN_SHADER_BUILTIN_WORKGROUP_Y = 9,
    RIN_SHADER_BUILTIN_WORKGROUP_Z = 10,
    RIN_SHADER_BUILTIN_FRAG_COORD_X = 11,
    RIN_SHADER_BUILTIN_FRAG_COORD_Y = 12,
    RIN_SHADER_BUILTIN_FRAG_COORD_Z = 13,
    RIN_SHADER_BUILTIN_FRAG_COORD_W = 14,
    RIN_SHADER_BUILTIN_FRONT_FACING = 15,
    /* Point-sprite coordinates are fragment builtins. They deliberately
     * follow the existing values so older RSH1 bytecode remains unchanged. */
    RIN_SHADER_BUILTIN_POINT_COORD_X = 16,
    RIN_SHADER_BUILTIN_POINT_COORD_Y = 17,
    RIN_SHADER_BUILTIN_LAST = RIN_SHADER_BUILTIN_POINT_COORD_Y
} RinShaderBuiltin;

/* The RSH1 wire structs are byte-packed. GCC/Clang accept the attribute on
 * the tag; MSVC requires the matching pragma. Keep the ABI declaration
 * portable because the consumer bridge also builds this header with MSVC. */
#if defined(_MSC_VER)
#    define RIN_SHADER_PACKED
#    pragma pack(push, 1)
#else
#    define RIN_SHADER_PACKED __attribute__((packed))
#endif

typedef struct RIN_SHADER_PACKED RinShaderHeaderV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t stage;
    uint32_t flags;
    uint32_t instruction_count;
    uint32_t register_count;
    uint32_t input_count;
    uint32_t output_count;
    uint32_t resource_count;
    uint32_t workgroup_x;
    uint32_t workgroup_y;
    uint32_t workgroup_z;
    uint32_t entry_instruction;
    uint32_t reserved0;
    uint32_t reserved1;
} RinShaderHeaderV1;

typedef struct RIN_SHADER_PACKED RinShaderInstructionV1 {
    uint16_t opcode;
    uint16_t flags;
    uint16_t destination;
    uint16_t source0;
    uint16_t source1;
    uint16_t resource;
    uint32_t immediate;
} RinShaderInstructionV1;

#if defined(_MSC_VER)
#    pragma pack(pop)
#endif
#undef RIN_SHADER_PACKED

typedef struct RinShaderInfoV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t stage;
    uint32_t instruction_count;
    uint32_t register_count;
    uint32_t resource_count;
    uint32_t input_count;
    uint32_t output_count;
} RinShaderInfoV1;

int ringpu_shader_validate(const void* shader, size_t shader_size,
                           RinShaderInfoV1* info);

/* Resolve one public shader resource into caller-owned storage and validate
 * the RSH1 bytes. No filesystem or allocation is performed by the catalog
 * loader; storage_size and info are cleared before any failure is returned. */
int ringpu_shader_validate_resource(
    const RinResourceCatalogV1* catalog, uint32_t resource_id,
    RinResourceCatalogReadPathFunction read_path, void* context,
    uint8_t* storage, uint64_t storage_capacity, uint64_t* storage_size,
    RinShaderInfoV1* info);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinShaderHeaderV1) == 64u,
               "RinShader header ABI drift");
_Static_assert(sizeof(RinShaderInstructionV1) == 16u,
               "RinShader instruction ABI drift");
_Static_assert(sizeof(RinShaderInfoV1) == 32u,
               "RinShader info ABI drift");
#endif

#endif /* RIN_API_RIN_SHADER_H */
