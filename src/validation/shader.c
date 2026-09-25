// SPDX-License-Identifier: MIT
#include "shader.h"

#include <stdlib.h>
#include <string.h>

enum ShaderValueType {
    SHADER_VALUE_I32 = 1,
    SHADER_VALUE_F32 = 2
};

static int shader_register(uint16_t value, uint32_t register_count) {
    return value != RIN_SHADER_UNUSED && value < register_count;
}

static int shader_unused(uint16_t value) {
    return value == RIN_SHADER_UNUSED;
}

static int shader_bit_test(const uint64_t* state, uint16_t reg) {
    return (state[reg / 64u] & (UINT64_C(1) << (reg % 64u))) != 0u;
}

static void shader_bit_set(uint64_t* state, uint16_t reg) {
    state[reg / 64u] |= UINT64_C(1) << (reg % 64u);
}

static void shader_bit_clear(uint64_t* state, uint16_t reg) {
    state[reg / 64u] &= ~(UINT64_C(1) << (reg % 64u));
}

static int shader_initialized(const uint64_t* i32_state,
                              const uint64_t* f32_state, uint16_t reg) {
    return shader_bit_test(i32_state, reg) || shader_bit_test(f32_state, reg);
}

static int shader_has_type(const uint64_t* i32_state,
                           const uint64_t* f32_state, uint16_t reg,
                           enum ShaderValueType type) {
    return type == SHADER_VALUE_I32 ? shader_bit_test(i32_state, reg)
                                    : shader_bit_test(f32_state, reg);
}

static void shader_define(uint64_t* i32_state, uint64_t* f32_state,
                          uint16_t reg, enum ShaderValueType type) {
    shader_bit_clear(i32_state, reg);
    shader_bit_clear(f32_state, reg);
    shader_bit_set(type == SHADER_VALUE_I32 ? i32_state : f32_state, reg);
}

static int shader_resource_set_kind(uint8_t* kinds, uint32_t resource,
                                    uint8_t kind) {
    if (kinds[resource] == RIN_SHADER_RESOURCE_NONE) {
        kinds[resource] = kind;
        return 1;
    }
    return kinds[resource] == kind;
}

static int shader_sampler_kind(uint32_t kind)
{
    return kind == RIN_SHADER_RESOURCE_SAMPLER ||
           kind == RIN_SHADER_RESOURCE_COMPARISON_SAMPLER;
}

int ringpu_shader_resource_layout(const RinGpuObjectSlot* shader,
                                  uint32_t* access, uint32_t* kinds)
{
    const RinShaderHeaderV1* header;
    const RinShaderInstructionV1* instructions;

    if (!shader || !access || !kinds ||
        shader->type != RIN_GPU_OBJECT_SHADER_MODULE ||
        !shader->value.shader_module.rin_shader_ir ||
        shader->value.shader_module.shader_size < sizeof(*header)) {
        return RIN_GPU_ERROR_STATE;
    }
    header = (const RinShaderHeaderV1*)
        shader->value.shader_module.rin_shader_ir;
    instructions = (const RinShaderInstructionV1*)(
        shader->value.shader_module.rin_shader_ir + sizeof(*header));
    memset(access, 0, RIN_SHADER_MAX_RESOURCES * sizeof(*access));
    memset(kinds, 0, RIN_SHADER_MAX_RESOURCES * sizeof(*kinds));
    for (uint32_t index = 0u; index < header->instruction_count; index++) {
        const RinShaderInstructionV1* instruction = &instructions[index];
        if (instruction->opcode == RIN_SHADER_OP_LOAD_RESOURCE_I32 ||
            instruction->opcode == RIN_SHADER_OP_LOAD_RESOURCE_F32) {
            if (kinds[instruction->resource] != RIN_SHADER_RESOURCE_NONE &&
                kinds[instruction->resource] !=
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
                return RIN_GPU_ERROR_SHADER_INVALID;
            }
            kinds[instruction->resource] = RIN_SHADER_RESOURCE_STORAGE_BUFFER;
            access[instruction->resource] |= RIN_GPU_RESOURCE_READ;
        } else if (instruction->opcode ==
                       RIN_SHADER_OP_STORE_RESOURCE_I32 ||
                   instruction->opcode ==
                       RIN_SHADER_OP_STORE_RESOURCE_F32) {
            if (kinds[instruction->resource] != RIN_SHADER_RESOURCE_NONE &&
                kinds[instruction->resource] !=
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
                return RIN_GPU_ERROR_SHADER_INVALID;
            }
            kinds[instruction->resource] = RIN_SHADER_RESOURCE_STORAGE_BUFFER;
            access[instruction->resource] |= RIN_GPU_RESOURCE_WRITE;
        } else if (instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_I32 ||
                   instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_F32 ||
                   instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_I32 ||
                   instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_F32) {
            uint32_t image = instruction->resource;
            uint32_t sampler = instruction->immediate;
            if ((kinds[image] != RIN_SHADER_RESOURCE_NONE &&
                 kinds[image] != RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                (kinds[sampler] != RIN_SHADER_RESOURCE_NONE &&
                 kinds[sampler] != RIN_SHADER_RESOURCE_SAMPLER)) {
                return RIN_GPU_ERROR_SHADER_INVALID;
            }
            kinds[image] = RIN_SHADER_RESOURCE_SAMPLED_IMAGE;
            kinds[sampler] = RIN_SHADER_RESOURCE_SAMPLER;
            access[image] |= RIN_GPU_RESOURCE_READ;
        } else if (instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_LOD_F32 ||
                   instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_BIAS_F32 ||
                   instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_GRAD_F32) {
            uint32_t image = RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(
                instruction->resource);
            uint32_t sampler = RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(
                instruction->resource);
            if ((kinds[image] != RIN_SHADER_RESOURCE_NONE &&
                 kinds[image] != RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                (kinds[sampler] != RIN_SHADER_RESOURCE_NONE &&
                 kinds[sampler] != RIN_SHADER_RESOURCE_SAMPLER)) {
                return RIN_GPU_ERROR_SHADER_INVALID;
            }
            kinds[image] = RIN_SHADER_RESOURCE_SAMPLED_IMAGE;
            kinds[sampler] = RIN_SHADER_RESOURCE_SAMPLER;
            access[image] |= RIN_GPU_RESOURCE_READ;
        } else if (instruction->opcode == RIN_SHADER_OP_SAMPLE_COMPARE_I32 ||
                   instruction->opcode == RIN_SHADER_OP_SAMPLE_COMPARE_F32) {
            uint32_t image = instruction->resource;
            uint32_t sampler = instruction->immediate;
            if ((kinds[image] != RIN_SHADER_RESOURCE_NONE &&
                 kinds[image] != RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE) ||
                (kinds[sampler] != RIN_SHADER_RESOURCE_NONE &&
                 kinds[sampler] != RIN_SHADER_RESOURCE_COMPARISON_SAMPLER)) {
                return RIN_GPU_ERROR_SHADER_INVALID;
            }
            kinds[image] = RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE;
            kinds[sampler] = RIN_SHADER_RESOURCE_COMPARISON_SAMPLER;
            access[image] |= RIN_GPU_RESOURCE_READ;
        }
    }
    for (uint32_t resource = 0u; resource < header->resource_count;
         resource++) {
        if (kinds[resource] == RIN_SHADER_RESOURCE_NONE ||
            (!shader_sampler_kind(kinds[resource]) &&
             access[resource] == 0u) ||
            (shader_sampler_kind(kinds[resource]) &&
             access[resource] != 0u)) {
            return RIN_GPU_ERROR_SHADER_INVALID;
        }
    }
    return RIN_GPU_OK;
}

static void shader_propagate(uint64_t* i32_states, uint64_t* f32_states,
                             uint32_t words, uint32_t successor,
                             const uint64_t* source_i32,
                             const uint64_t* source_f32,
                             uint32_t* incoming) {
    uint64_t* destination_i32 = i32_states + (size_t)successor * words;
    uint64_t* destination_f32 = f32_states + (size_t)successor * words;
    if (incoming[successor] == 0u) {
        memcpy(destination_i32, source_i32,
               (size_t)words * sizeof(*source_i32));
        memcpy(destination_f32, source_f32,
               (size_t)words * sizeof(*source_f32));
    } else {
        for (uint32_t word = 0u; word < words; word++) {
            destination_i32[word] &= source_i32[word];
            destination_f32[word] &= source_f32[word];
        }
    }
    incoming[successor]++;
}

static int shader_plain_operands(const RinShaderInstructionV1* instruction) {
    return shader_unused(instruction->destination) &&
           shader_unused(instruction->source0) &&
           shader_unused(instruction->source1) &&
           shader_unused(instruction->resource) &&
           instruction->immediate == 0u;
}

static int shader_destination_only(const RinShaderInstructionV1* instruction,
                                   uint32_t register_count) {
    return shader_register(instruction->destination, register_count) &&
           shader_unused(instruction->source0) &&
           shader_unused(instruction->source1) &&
           shader_unused(instruction->resource);
}

static int shader_unary_operands(const RinShaderInstructionV1* instruction,
                                 uint32_t register_count) {
    return shader_register(instruction->destination, register_count) &&
           shader_register(instruction->source0, register_count) &&
           shader_unused(instruction->source1) &&
           shader_unused(instruction->resource) &&
           instruction->immediate == 0u;
}

static int shader_binary_operands(const RinShaderInstructionV1* instruction,
                                  uint32_t register_count) {
    return shader_register(instruction->destination, register_count) &&
           shader_register(instruction->source0, register_count) &&
           shader_register(instruction->source1, register_count) &&
           shader_unused(instruction->resource) &&
           instruction->immediate == 0u;
}

static int shader_builtin_type(uint32_t stage, uint32_t builtin,
                               enum ShaderValueType* type) {
    if (!type || builtin > RIN_SHADER_BUILTIN_LAST) return 0;
    switch (builtin) {
        case RIN_SHADER_BUILTIN_VERTEX_INDEX:
        case RIN_SHADER_BUILTIN_INSTANCE_INDEX:
            if (stage != RIN_SHADER_STAGE_VERTEX) return 0;
            *type = SHADER_VALUE_I32;
            return 1;
        case RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_X:
        case RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_Y:
        case RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_Z:
        case RIN_SHADER_BUILTIN_LOCAL_INVOCATION_X:
        case RIN_SHADER_BUILTIN_LOCAL_INVOCATION_Y:
        case RIN_SHADER_BUILTIN_LOCAL_INVOCATION_Z:
        case RIN_SHADER_BUILTIN_WORKGROUP_X:
        case RIN_SHADER_BUILTIN_WORKGROUP_Y:
        case RIN_SHADER_BUILTIN_WORKGROUP_Z:
            if (stage != RIN_SHADER_STAGE_COMPUTE) return 0;
            *type = SHADER_VALUE_I32;
            return 1;
        case RIN_SHADER_BUILTIN_FRAG_COORD_X:
        case RIN_SHADER_BUILTIN_FRAG_COORD_Y:
        case RIN_SHADER_BUILTIN_FRAG_COORD_Z:
        case RIN_SHADER_BUILTIN_FRAG_COORD_W:
        case RIN_SHADER_BUILTIN_POINT_COORD_X:
        case RIN_SHADER_BUILTIN_POINT_COORD_Y:
            if (stage != RIN_SHADER_STAGE_FRAGMENT) return 0;
            *type = SHADER_VALUE_F32;
            return 1;
        case RIN_SHADER_BUILTIN_FRONT_FACING:
            if (stage != RIN_SHADER_STAGE_FRAGMENT) return 0;
            *type = SHADER_VALUE_I32;
            return 1;
    }
    return 0;
}

int ringpu_shader_validate(const void* shader, size_t shader_size,
                           RinShaderInfoV1* info) {
    RinShaderHeaderV1 header;
    const RinShaderInstructionV1* instructions;
    uint64_t* i32_states = NULL;
    uint64_t* f32_states = NULL;
    uint32_t* incoming = NULL;
    uint8_t resource_kinds[RIN_SHADER_MAX_RESOURCES] = {0};
    uint32_t words;
    uint64_t expected_size;
    int result = RIN_SHADER_OK;

    if (info) memset(info, 0, sizeof(*info));
    if (!shader || !info || shader_size < sizeof(header)) {
        return RIN_SHADER_ERROR_INVALID_ARGUMENT;
    }
    memcpy(&header, shader, sizeof(header));
    if (header.magic != RIN_SHADER_MAGIC ||
        header.version != RIN_SHADER_IR_VERSION ||
        header.header_size != sizeof(header) ||
        (header.flags & ~RIN_SHADER_KNOWN_FLAGS) != 0u ||
        header.reserved0 != 0u || header.reserved1 != 0u ||
        header.entry_instruction != 0u) {
        return RIN_SHADER_ERROR_BAD_HEADER;
    }
    if (header.stage < RIN_SHADER_STAGE_VERTEX ||
        header.stage > RIN_SHADER_STAGE_COMPUTE ||
        header.instruction_count == 0u ||
        header.instruction_count > RIN_SHADER_MAX_INSTRUCTIONS ||
        header.register_count == 0u ||
        header.register_count > RIN_SHADER_MAX_REGISTERS ||
        header.input_count > RIN_SHADER_MAX_IO ||
        header.output_count > RIN_SHADER_MAX_IO ||
        header.resource_count > RIN_SHADER_MAX_RESOURCES) {
        return RIN_SHADER_ERROR_BAD_HEADER;
    }
    expected_size = (uint64_t)sizeof(header) +
        (uint64_t)header.instruction_count * sizeof(RinShaderInstructionV1);
    if (expected_size > UINT32_MAX || header.total_size != expected_size ||
        header.total_size != shader_size) {
        return RIN_SHADER_ERROR_BOUNDS;
    }
    if (header.stage == RIN_SHADER_STAGE_COMPUTE) {
        uint64_t workgroup_size;
        if (header.workgroup_x == 0u || header.workgroup_y == 0u ||
            header.workgroup_z == 0u || header.workgroup_x > 1024u ||
            header.workgroup_y > 1024u || header.workgroup_z > 64u) {
            return RIN_SHADER_ERROR_BAD_HEADER;
        }
        workgroup_size = (uint64_t)header.workgroup_x * header.workgroup_y *
                         header.workgroup_z;
        if (workgroup_size > 1024u) return RIN_SHADER_ERROR_BAD_HEADER;
    } else if (header.workgroup_x != 0u || header.workgroup_y != 0u ||
               header.workgroup_z != 0u) {
        return RIN_SHADER_ERROR_BAD_HEADER;
    }

    instructions = (const RinShaderInstructionV1*)((const uint8_t*)shader +
                                                    sizeof(header));
    words = (header.register_count + 63u) / 64u;
    i32_states = (uint64_t*)calloc((size_t)header.instruction_count * words,
                                   sizeof(*i32_states));
    f32_states = (uint64_t*)calloc((size_t)header.instruction_count * words,
                                   sizeof(*f32_states));
    incoming = (uint32_t*)calloc(header.instruction_count, sizeof(*incoming));
    if (!i32_states || !f32_states || !incoming) {
        result = RIN_SHADER_ERROR_NO_MEMORY;
        goto done;
    }
    incoming[0] = 1u;
    for (uint32_t index = 0u; index < header.instruction_count; index++) {
        const RinShaderInstructionV1* instruction = &instructions[index];
        uint64_t* i32_state = i32_states + (size_t)index * words;
        uint64_t* f32_state = f32_states + (size_t)index * words;
        uint32_t next = index + 1u;
        uint32_t target = instruction->immediate;
        int has_fallthrough = 1;

        if (incoming[index] == 0u) {
            result = RIN_SHADER_ERROR_CONTROL_FLOW;
            goto done;
        }
        if (instruction->opcode > RIN_SHADER_OP_LAST ||
            (instruction->flags != 0u &&
             !((instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_I32 ||
                instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_F32 ||
                instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_LOD_F32 ||
                instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_BIAS_F32 ||
                instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_GRAD_F32 ||
                instruction->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_CUBE_F32) &&
               instruction->flags <= RIN_SHADER_SAMPLE_COMPONENT_MASK))) {
            result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
            goto done;
        }
        switch (instruction->opcode) {
            case RIN_SHADER_OP_NOP:
                if (!shader_plain_operands(instruction))
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                break;
            case RIN_SHADER_OP_CONST_I32:
            case RIN_SHADER_OP_CONST_F32: {
                enum ShaderValueType type =
                    instruction->opcode == RIN_SHADER_OP_CONST_I32
                        ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (!shader_destination_only(instruction,
                                             header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_MOV:
                if (!shader_unary_operands(instruction,
                                           header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                if (!shader_initialized(i32_state, f32_state,
                                        instruction->source0)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state, instruction->destination,
                    shader_bit_test(i32_state, instruction->source0)
                        ? SHADER_VALUE_I32 : SHADER_VALUE_F32);
                break;
            case RIN_SHADER_OP_ADD_I32:
            case RIN_SHADER_OP_SUB_I32:
            case RIN_SHADER_OP_MUL_I32:
            case RIN_SHADER_OP_DIV_I32:
            case RIN_SHADER_OP_MOD_I32:
            case RIN_SHADER_OP_MIN_I32:
            case RIN_SHADER_OP_MAX_I32:
            case RIN_SHADER_OP_AND_I32:
            case RIN_SHADER_OP_OR_I32:
            case RIN_SHADER_OP_XOR_I32:
            case RIN_SHADER_OP_SHL_I32:
            case RIN_SHADER_OP_SHR_I32:
            case RIN_SHADER_OP_CMP_EQ_I32:
            case RIN_SHADER_OP_CMP_NE_I32:
            case RIN_SHADER_OP_CMP_LT_I32:
            case RIN_SHADER_OP_CMP_LE_I32:
            case RIN_SHADER_OP_CMP_GT_I32:
            case RIN_SHADER_OP_CMP_GE_I32:
                if (!shader_binary_operands(instruction,
                                            header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_I32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1,
                                     SHADER_VALUE_I32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, SHADER_VALUE_I32);
                break;
            case RIN_SHADER_OP_ADD_F32:
            case RIN_SHADER_OP_SUB_F32:
            case RIN_SHADER_OP_MUL_F32:
            case RIN_SHADER_OP_DIV_F32:
            case RIN_SHADER_OP_MIN_F32:
            case RIN_SHADER_OP_MAX_F32:
            case RIN_SHADER_OP_CMP_EQ_F32:
            case RIN_SHADER_OP_CMP_NE_F32:
            case RIN_SHADER_OP_CMP_LT_F32:
            case RIN_SHADER_OP_CMP_LE_F32:
            case RIN_SHADER_OP_CMP_GT_F32:
            case RIN_SHADER_OP_CMP_GE_F32: {
                int comparison = instruction->opcode >=
                                     RIN_SHADER_OP_CMP_EQ_F32 &&
                                 instruction->opcode <=
                                     RIN_SHADER_OP_CMP_GE_F32;
                if (!shader_binary_operands(instruction,
                                            header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1,
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state, instruction->destination,
                              comparison ? SHADER_VALUE_I32
                                         : SHADER_VALUE_F32);
                break;
            }
            case RIN_SHADER_OP_DFDX_F32:
            case RIN_SHADER_OP_DFDY_F32:
            case RIN_SHADER_OP_FWIDTH_F32:
                /* RSH1 derivatives are only meaningful in a fragment
                 * invocation.  They consume the executor-provided
                 * screen-space gradient of one F32 register and never carry
                 * hidden resources or immediates. */
                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_unary_operands(instruction,
                                           header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state, instruction->destination,
                              SHADER_VALUE_F32);
                break;
            case RIN_SHADER_OP_FLOOR_F32:
            case RIN_SHADER_OP_SQRT_F32:
            case RIN_SHADER_OP_SIN_F32:
            case RIN_SHADER_OP_COS_F32:
            case RIN_SHADER_OP_ATAN_F32:
            case RIN_SHADER_OP_ASIN_F32:
            case RIN_SHADER_OP_ACOS_F32:
            case RIN_SHADER_OP_EXP2_F32:
            case RIN_SHADER_OP_LOG2_F32:
                /* These are pure scalar F32 operations. In particular, do
                 * not allow an immediate or a hidden second source to turn a
                 * valid RSH1 module into an embedding-specific operation. */
                if (!shader_unary_operands(instruction,
                                           header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state, instruction->destination,
                              SHADER_VALUE_F32);
                break;
            case RIN_SHADER_OP_ATAN2_F32:
            case RIN_SHADER_OP_POW_F32:
                if (!shader_binary_operands(instruction,
                                            header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1,
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state, instruction->destination,
                              SHADER_VALUE_F32);
                break;
            case RIN_SHADER_OP_I32_TO_F32:
            case RIN_SHADER_OP_F32_TO_I32: {
                enum ShaderValueType source_type =
                    instruction->opcode == RIN_SHADER_OP_I32_TO_F32
                        ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                enum ShaderValueType destination_type =
                    source_type == SHADER_VALUE_I32 ? SHADER_VALUE_F32
                                                    : SHADER_VALUE_I32;
                if (!shader_unary_operands(instruction,
                                           header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_REGISTER;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0, source_type)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, destination_type);
                break;
            }
            case RIN_SHADER_OP_LOAD_INPUT:
            case RIN_SHADER_OP_LOAD_INPUT_F32: {
                enum ShaderValueType type =
                    instruction->opcode == RIN_SHADER_OP_LOAD_INPUT
                        ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (!shader_destination_only(instruction,
                                             header.register_count) ||
                    instruction->immediate >= header.input_count) {
                    result = RIN_SHADER_ERROR_BOUNDS;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_LOAD_PUSH_CONSTANT_I32:
            case RIN_SHADER_OP_LOAD_PUSH_CONSTANT_F32: {
                enum ShaderValueType type =
                    instruction->opcode == RIN_SHADER_OP_LOAD_PUSH_CONSTANT_I32
                        ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if ((header.flags & RIN_SHADER_FLAG_PUSH_CONSTANTS) == 0u ||
                    !shader_destination_only(instruction,
                                             header.register_count) ||
                    (instruction->immediate & 3u) != 0u ||
                    instruction->immediate >
                        RIN_SHADER_PUSH_CONSTANT_BYTES - sizeof(uint32_t)) {
                    result = RIN_SHADER_ERROR_INVALID_ARGUMENT;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_STORE_OUTPUT:
            case RIN_SHADER_OP_STORE_OUTPUT_F32: {
                enum ShaderValueType type =
                    instruction->opcode == RIN_SHADER_OP_STORE_OUTPUT
                        ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (!shader_unused(instruction->destination) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_unused(instruction->source1) ||
                    !shader_unused(instruction->resource) ||
                    instruction->immediate >= header.output_count) {
                    result = RIN_SHADER_ERROR_BOUNDS;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0, type)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                }
                break;
            }
            case RIN_SHADER_OP_LOAD_RESOURCE_I32:
            case RIN_SHADER_OP_LOAD_RESOURCE_F32: {
                enum ShaderValueType type = instruction->opcode ==
                        RIN_SHADER_OP_LOAD_RESOURCE_I32
                    ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (!shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_unused(instruction->source1) ||
                    instruction->resource >= header.resource_count ||
                    instruction->immediate != 0u) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_I32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, instruction->resource,
                        RIN_SHADER_RESOURCE_STORAGE_BUFFER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_STORE_RESOURCE_I32:
            case RIN_SHADER_OP_STORE_RESOURCE_F32: {
                enum ShaderValueType value_type = instruction->opcode ==
                        RIN_SHADER_OP_STORE_RESOURCE_I32
                    ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (!shader_unused(instruction->destination) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_register(instruction->source1,
                                     header.register_count) ||
                    instruction->resource >= header.resource_count ||
                    instruction->immediate != 0u) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_I32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1, value_type)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, instruction->resource,
                        RIN_SHADER_RESOURCE_STORAGE_BUFFER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                }
                break;
            }
            case RIN_SHADER_OP_SAMPLE_IMAGE_I32:
            case RIN_SHADER_OP_SAMPLE_IMAGE_F32: {
                enum ShaderValueType type = instruction->opcode ==
                        RIN_SHADER_OP_SAMPLE_IMAGE_I32
                    ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_unused(instruction->source1) ||
                    instruction->resource >= header.resource_count ||
                    instruction->immediate >= header.resource_count ||
                    instruction->resource == instruction->immediate) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0, type)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, instruction->resource,
                        RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                    !shader_resource_set_kind(
                        resource_kinds, instruction->immediate,
                        RIN_SHADER_RESOURCE_SAMPLER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_SAMPLE_IMAGE_2D_I32:
            case RIN_SHADER_OP_SAMPLE_IMAGE_2D_F32: {
                enum ShaderValueType type = instruction->opcode ==
                        RIN_SHADER_OP_SAMPLE_IMAGE_2D_I32
                    ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_register(instruction->source1,
                                     header.register_count) ||
                    instruction->resource >= header.resource_count ||
                    instruction->immediate >= header.resource_count ||
                    instruction->resource == instruction->immediate) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0, type) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1, type)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, instruction->resource,
                        RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                    !shader_resource_set_kind(
                        resource_kinds, instruction->immediate,
                        RIN_SHADER_RESOURCE_SAMPLER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_SAMPLE_IMAGE_2D_LOD_F32:
            case RIN_SHADER_OP_SAMPLE_IMAGE_2D_BIAS_F32: {
                uint16_t image = RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(
                    instruction->resource);
                uint16_t sampler = RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(
                    instruction->resource);

                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_register(instruction->source1,
                                     header.register_count) ||
                    (instruction->resource >>
                     (RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS * 2u)) != 0u ||
                    image >= header.resource_count ||
                    sampler >= header.resource_count || image == sampler ||
                    instruction->immediate > UINT16_MAX ||
                    !shader_register((uint16_t)instruction->immediate,
                                     header.register_count)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     (uint16_t)instruction->immediate,
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, image,
                        RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                    !shader_resource_set_kind(resource_kinds, sampler,
                                              RIN_SHADER_RESOURCE_SAMPLER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, SHADER_VALUE_F32);
                break;
            }
            case RIN_SHADER_OP_SAMPLE_IMAGE_2D_GRAD_F32: {
                uint16_t image = RIN_SHADER_SAMPLE_2D_GRAD_IMAGE_BINDING(
                    instruction->resource);
                uint16_t sampler = RIN_SHADER_SAMPLE_2D_GRAD_SAMPLER_BINDING(
                    instruction->resource);
                uint32_t gradient_base = instruction->immediate;

                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_register(instruction->source1,
                                     header.register_count) ||
                    (instruction->resource >>
                     (RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS * 2u)) != 0u ||
                    image >= header.resource_count ||
                    sampler >= header.resource_count || image == sampler ||
                    gradient_base > UINT16_MAX ||
                    gradient_base >= header.register_count ||
                    header.register_count - gradient_base < 4u) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     (uint16_t)gradient_base,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     (uint16_t)(gradient_base + 1u),
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     (uint16_t)(gradient_base + 2u),
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     (uint16_t)(gradient_base + 3u),
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, image,
                        RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                    !shader_resource_set_kind(resource_kinds, sampler,
                                              RIN_SHADER_RESOURCE_SAMPLER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, SHADER_VALUE_F32);
                break;
            }
            case RIN_SHADER_OP_SAMPLE_IMAGE_CUBE_F32: {
                uint16_t image = RIN_SHADER_SAMPLE_CUBE_IMAGE_BINDING(
                    instruction->resource);
                uint16_t sampler = RIN_SHADER_SAMPLE_CUBE_SAMPLER_BINDING(
                    instruction->resource);

                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_register(instruction->source1,
                                     header.register_count) ||
                    instruction->immediate >= header.register_count ||
                    (instruction->resource >>
                     (RIN_SHADER_SAMPLE_CUBE_BINDING_BITS * 2u)) != 0u ||
                    image >= header.resource_count ||
                    sampler >= header.resource_count || image == sampler) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (instruction->flags > RIN_SHADER_SAMPLE_COMPONENT_MASK ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1,
                                     SHADER_VALUE_F32) ||
                    !shader_has_type(i32_state, f32_state,
                                     (uint16_t)instruction->immediate,
                                     SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, image,
                        RIN_SHADER_RESOURCE_SAMPLED_IMAGE) ||
                    !shader_resource_set_kind(resource_kinds, sampler,
                                              RIN_SHADER_RESOURCE_SAMPLER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, SHADER_VALUE_F32);
                break;
            }
            case RIN_SHADER_OP_SAMPLE_COMPARE_I32:
            case RIN_SHADER_OP_SAMPLE_COMPARE_F32: {
                enum ShaderValueType type = instruction->opcode ==
                        RIN_SHADER_OP_SAMPLE_COMPARE_I32
                    ? SHADER_VALUE_I32 : SHADER_VALUE_F32;
                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_register(instruction->destination,
                                     header.register_count) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_register(instruction->source1,
                                     header.register_count) ||
                    instruction->resource >= header.resource_count ||
                    instruction->immediate >= header.resource_count ||
                    instruction->resource == instruction->immediate) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0, type) ||
                    !shader_has_type(i32_state, f32_state,
                                     instruction->source1, type)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (!shader_resource_set_kind(
                        resource_kinds, instruction->resource,
                        RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE) ||
                    !shader_resource_set_kind(
                        resource_kinds, instruction->immediate,
                        RIN_SHADER_RESOURCE_COMPARISON_SAMPLER)) {
                    result = RIN_SHADER_ERROR_INVALID_RESOURCE;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_LOAD_BUILTIN_I32:
            case RIN_SHADER_OP_LOAD_BUILTIN_F32: {
                enum ShaderValueType type;
                if (!shader_destination_only(instruction,
                                             header.register_count) ||
                    !shader_builtin_type(header.stage,
                                         instruction->immediate, &type) ||
                    (instruction->opcode == RIN_SHADER_OP_LOAD_BUILTIN_I32 &&
                         type != SHADER_VALUE_I32) ||
                    (instruction->opcode == RIN_SHADER_OP_LOAD_BUILTIN_F32 &&
                         type != SHADER_VALUE_F32)) {
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                    break;
                }
                shader_define(i32_state, f32_state,
                              instruction->destination, type);
                break;
            }
            case RIN_SHADER_OP_JUMP:
                if (!shader_unused(instruction->destination) ||
                    !shader_unused(instruction->source0) ||
                    !shader_unused(instruction->source1) ||
                    !shader_unused(instruction->resource)) {
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                    break;
                }
                if (target <= index || target >= header.instruction_count) {
                    result = RIN_SHADER_ERROR_CONTROL_FLOW;
                    break;
                }
                shader_propagate(i32_states, f32_states, words, target,
                                 i32_state, f32_state, incoming);
                has_fallthrough = 0;
                break;
            case RIN_SHADER_OP_JUMP_IF:
                if (!shader_unused(instruction->destination) ||
                    !shader_register(instruction->source0,
                                     header.register_count) ||
                    !shader_unused(instruction->source1) ||
                    !shader_unused(instruction->resource)) {
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                    break;
                }
                if (!shader_has_type(i32_state, f32_state,
                                     instruction->source0,
                                     SHADER_VALUE_I32)) {
                    result = RIN_SHADER_ERROR_UNINITIALIZED_REGISTER;
                    break;
                }
                if (target <= index || target >= header.instruction_count) {
                    result = RIN_SHADER_ERROR_CONTROL_FLOW;
                    break;
                }
                shader_propagate(i32_states, f32_states, words, target,
                                 i32_state, f32_state, incoming);
                break;
            case RIN_SHADER_OP_DISCARD:
                if (header.stage != RIN_SHADER_STAGE_FRAGMENT ||
                    !shader_plain_operands(instruction)) {
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                }
                break;
            case RIN_SHADER_OP_RETURN:
                if (!shader_plain_operands(instruction))
                    result = RIN_SHADER_ERROR_INVALID_INSTRUCTION;
                has_fallthrough = 0;
                break;
        }
        if (result != RIN_SHADER_OK) goto done;
        if (has_fallthrough) {
            if (next >= header.instruction_count) {
                result = RIN_SHADER_ERROR_CONTROL_FLOW;
                goto done;
            }
            shader_propagate(i32_states, f32_states, words, next,
                             i32_state, f32_state, incoming);
        }
    }
    if (instructions[header.instruction_count - 1u].opcode !=
        RIN_SHADER_OP_RETURN) {
        result = RIN_SHADER_ERROR_CONTROL_FLOW;
        goto done;
    }
    for (uint32_t resource = 0u; resource < header.resource_count;
         resource++) {
        if (resource_kinds[resource] == RIN_SHADER_RESOURCE_NONE) {
            result = RIN_SHADER_ERROR_INVALID_RESOURCE;
            goto done;
        }
    }
    info->abi_version = RIN_SHADER_IR_VERSION;
    info->struct_size = sizeof(*info);
    info->stage = header.stage;
    info->instruction_count = header.instruction_count;
    info->register_count = header.register_count;
    info->resource_count = header.resource_count;
    info->input_count = header.input_count;
    info->output_count = header.output_count;

done:
    free(i32_states);
    free(f32_states);
    free(incoming);
    return result;
}

int ringpu_shader_validate_resource(
    const RinResourceCatalogV1* catalog, uint32_t resource_id,
    RinResourceCatalogReadPathFunction read_path, void* context,
    uint8_t* storage, uint64_t storage_capacity, uint64_t* storage_size,
    RinShaderInfoV1* info) {
    RinResourceCatalogStatus status;
    int result;

    if (storage_size == NULL || info == NULL) {
        if (storage_size != NULL) *storage_size = 0u;
        if (info != NULL) memset(info, 0, sizeof(*info));
        return RIN_SHADER_ERROR_INVALID_ARGUMENT;
    }
    *storage_size = 0u;
    memset(info, 0, sizeof(*info));
    if (storage_capacity > SIZE_MAX ||
        (storage_capacity != 0u && storage == NULL))
        return RIN_SHADER_ERROR_INVALID_ARGUMENT;

    status = rin_resource_catalog_load(
        catalog, RIN_RESOURCE_CATALOG_TYPE_SHADER, resource_id, read_path,
        context, storage, storage_capacity, storage_size);
    if (status != RIN_RESOURCE_CATALOG_OK || *storage_size > SIZE_MAX ||
        *storage_size == 0u) {
        *storage_size = 0u;
        return status == RIN_RESOURCE_CATALOG_BUFFER_TOO_SMALL
            ? RIN_SHADER_ERROR_BOUNDS : RIN_SHADER_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_shader_validate(storage, (size_t)*storage_size, info);
    if (result != RIN_SHADER_OK) {
        *storage_size = 0u;
        memset(info, 0, sizeof(*info));
    }
    return result;
}
