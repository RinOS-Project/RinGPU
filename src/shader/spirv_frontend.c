/* SPDX-License-Identifier: MIT */
#include "../../include/ringpu/spirv_frontend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This frontend intentionally implements a small, auditable SPIR-V profile.
 * It is a translator, not a string/byte-pattern recognizer: every accepted
 * instruction is decoded from its declared operands and the generated RSH1
 * is sent through the same validator used by RinGPU. */
enum {
    SPV_OP_NAME = 5,
    SPV_OP_MEMORY_MODEL = 14,
    SPV_OP_ENTRY_POINT = 15,
    SPV_OP_EXECUTION_MODE = 16,
    SPV_OP_CAPABILITY = 17,
    SPV_OP_TYPE_VOID = 19,
    SPV_OP_TYPE_BOOL = 20,
    SPV_OP_TYPE_INT = 21,
    SPV_OP_TYPE_FLOAT = 22,
    SPV_OP_TYPE_VECTOR = 23,
    SPV_OP_TYPE_MATRIX = 24,
    SPV_OP_TYPE_IMAGE = 25,
    SPV_OP_TYPE_SAMPLER = 26,
    SPV_OP_TYPE_SAMPLED_IMAGE = 27,
    SPV_OP_TYPE_ARRAY = 28,
    SPV_OP_TYPE_STRUCT = 30,
    SPV_OP_TYPE_POINTER = 32,
    SPV_OP_TYPE_FUNCTION = 33,
    SPV_OP_CONSTANT_TRUE = 41,
    SPV_OP_CONSTANT_FALSE = 42,
    SPV_OP_CONSTANT = 43,
    SPV_OP_SPEC_CONSTANT_TRUE = 48,
    SPV_OP_SPEC_CONSTANT_FALSE = 49,
    SPV_OP_SPEC_CONSTANT = 50,
    SPV_OP_FUNCTION = 54,
    SPV_OP_FUNCTION_END = 56,
    SPV_OP_VARIABLE = 59,
    SPV_OP_LOAD = 61,
    SPV_OP_STORE = 62,
    SPV_OP_ACCESS_CHAIN = 65,
    SPV_OP_DECORATE = 71,
    SPV_OP_MEMBER_DECORATE = 72,
    SPV_OP_VECTOR_SHUFFLE = 79,
    SPV_OP_COMPOSITE_CONSTRUCT = 80,
    SPV_OP_COMPOSITE_EXTRACT = 81,
    SPV_OP_COPY_OBJECT = 83,
    SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    SPV_OP_F_NEGATE = 127,
    SPV_OP_I_ADD = 128,
    SPV_OP_F_ADD = 129,
    SPV_OP_I_SUB = 130,
    SPV_OP_F_SUB = 131,
    SPV_OP_I_MUL = 132,
    SPV_OP_F_MUL = 133,
    SPV_OP_F_DIV = 136,
    SPV_OP_FUNCTION_PARAMETER = 55,
    SPV_OP_LABEL = 248,
    SPV_OP_BRANCH = 249,
    SPV_OP_BRANCH_CONDITIONAL = 250,
    SPV_OP_RETURN = 253,
    SPV_OP_RETURN_VALUE = 254,
    SPV_OP_UNREACHABLE = 255
};

enum {
    SPV_DECORATION_SPEC_ID = 1,
    SPV_DECORATION_BLOCK = 2,
    SPV_DECORATION_BUFFER_BLOCK = 3,
    SPV_DECORATION_BUILT_IN = 11,
    SPV_DECORATION_LOCATION = 30,
    SPV_DECORATION_BINDING = 33,
    SPV_DECORATION_DESCRIPTOR_SET = 34,
    SPV_DECORATION_OFFSET = 35
};

enum {
    SPV_STORAGE_UNIFORM_CONSTANT = 0,
    SPV_STORAGE_INPUT = 1,
    SPV_STORAGE_UNIFORM = 2,
    SPV_STORAGE_OUTPUT = 3,
    SPV_STORAGE_FUNCTION = 7,
    SPV_STORAGE_STORAGE_BUFFER = 12,
    SPV_STORAGE_PUSH_CONSTANT = 9
};

enum {
    SPV_EXEC_VERTEX = 0,
    SPV_EXEC_FRAGMENT = 4,
    SPV_EXEC_COMPUTE = 5,
    SPV_EXECUTION_MODE_LOCAL_SIZE = 17,
    SPV_ADDRESSING_LOGICAL = 0,
    SPV_MEMORY_GLSL450 = 1,
    SPV_MEMORY_VULKAN = 3,
    SPV_CAPABILITY_SHADER = 1
};

enum {
    SPV_BUILTIN_POSITION = 0
};

enum SpvTypeKind {
    SPV_TYPE_NONE = 0,
    SPV_TYPE_VOID_KIND,
    SPV_TYPE_BOOL_KIND,
    SPV_TYPE_INT_KIND,
    SPV_TYPE_FLOAT_KIND,
    SPV_TYPE_VECTOR_KIND,
    SPV_TYPE_MATRIX_KIND,
    SPV_TYPE_IMAGE_KIND,
    SPV_TYPE_SAMPLER_KIND,
    SPV_TYPE_SAMPLED_IMAGE_KIND,
    SPV_TYPE_ARRAY_KIND,
    SPV_TYPE_STRUCT_KIND,
    SPV_TYPE_POINTER_KIND,
    SPV_TYPE_FUNCTION_KIND
};

typedef struct SpvType {
    uint32_t kind;
    uint32_t element;
    uint32_t length;
    uint32_t width;
    uint32_t columns;
    uint32_t rows;
    uint32_t storage_class;
    uint32_t member_count;
    uint32_t members[16];
    uint32_t member_offsets[16];
    uint32_t member_offsets_set;
} SpvType;

typedef struct SpvDecorations {
    uint32_t location;
    uint32_t has_location;
    uint32_t binding;
    uint32_t has_binding;
    uint32_t descriptor_set;
    uint32_t has_descriptor_set;
    uint32_t builtin;
    uint32_t has_builtin;
    uint32_t spec_id;
    uint32_t has_spec_id;
} SpvDecorations;

typedef struct SpvValue {
    uint32_t type_id;
    uint32_t bits;
    uint32_t defined;
    uint32_t specialization;
    uint32_t component_count;
    uint16_t component_registers[4];
} SpvValue;

typedef struct SpvVariable {
    uint32_t type_id;
    uint32_t storage_class;
    uint32_t defined;
    uint32_t pointer_defined;
    uint32_t pointer_base_id;
    uint32_t pointer_offset;
} SpvVariable;

typedef struct SpvInstruction {
    uint16_t opcode;
    uint16_t word_count;
    const uint32_t* words;
} SpvInstruction;

typedef struct SpvBuilder {
    RinShaderInstructionV1 instructions[1024];
    uint32_t count;
    uint16_t registers[65536];
    uint8_t register_defined[65536];
    uint16_t next_register;
    uint32_t resource_count;
} SpvBuilder;

static void diag(RinSpirvTranslationInfoV1* info, const char* format, ...)
{
    va_list args;

    if (!info)
        return;
    va_start(args, format);
    (void)vsnprintf(info->diagnostic, sizeof(info->diagnostic), format, args);
    va_end(args);
}

static int id_valid(uint32_t id, uint32_t bound)
{
    return id != 0u && id < bound;
}

static int instruction_valid(const uint32_t* instruction, size_t remaining,
                            uint16_t* opcode, uint16_t* word_count)
{
    uint32_t first;

    if (!instruction || remaining == 0u || !opcode || !word_count)
        return 0;
    first = instruction[0];
    *word_count = (uint16_t)(first >> 16u);
    *opcode = (uint16_t)(first & UINT32_C(0xffff));
    return *word_count >= 1u && *word_count <= remaining;
}

static int string_operand(const uint32_t* words, uint32_t word_count,
                          uint32_t operand, char* output, size_t capacity)
{
    size_t bytes;
    size_t index;
    const char* source;

    if (!words || !output || capacity == 0u || operand >= word_count)
        return 0;
    bytes = ((size_t)word_count - operand) * sizeof(uint32_t);
    source = (const char*)(words + operand);
    for (index = 0u; index < bytes; ++index) {
        if (source[index] == '\0') {
            size_t length = index < capacity - 1u ? index : capacity - 1u;
            memcpy(output, source, length);
            output[length] = '\0';
            return 1;
        }
    }
    return 0;
}

static int string_end_word(const uint32_t* words, uint32_t word_count,
                           uint32_t operand, uint32_t* end_word)
{
    uint32_t index;

    if (!words || !end_word || operand >= word_count)
        return 0;
    for (index = operand; index < word_count; ++index) {
        uint32_t value = words[index];
        if ((value & UINT32_C(0xff)) == 0u ||
            ((value >> 8u) & UINT32_C(0xff)) == 0u ||
            ((value >> 16u) & UINT32_C(0xff)) == 0u ||
            ((value >> 24u) & UINT32_C(0xff)) == 0u) {
            *end_word = index + 1u;
            return 1;
        }
    }
    return 0;
}

static int type_scalar(const SpvType* types, uint32_t bound, uint32_t id,
                       uint32_t* base_type)
{
    const SpvType* type;

    if (!types || !base_type || !id_valid(id, bound))
        return 0;
    type = &types[id];
    if (type->kind == SPV_TYPE_FLOAT_KIND && type->width == 32u) {
        *base_type = 2u;
        return 1;
    }
    if (type->kind == SPV_TYPE_INT_KIND && type->width == 32u) {
        *base_type = 1u;
        return 1;
    }
    return 0;
}

static int type_scalar_or_vector(const SpvType* types, uint32_t bound,
                                 uint32_t id, uint32_t* base_type,
                                 uint32_t* width)
{
    uint32_t element;

    if (type_scalar(types, bound, id, base_type)) {
        *width = 1u;
        return 1;
    }
    if (!id_valid(id, bound) || types[id].kind != SPV_TYPE_VECTOR_KIND ||
        types[id].length < 2u || types[id].length > 4u ||
        !type_scalar(types, bound, types[id].element, &element))
        return 0;
    *base_type = element;
    *width = types[id].length;
    return 1;
}

static uint32_t type_size(const SpvType* types, uint32_t bound, uint32_t id)
{
    const SpvType* type;
    uint64_t size = 0u;
    uint32_t index;

    if (!id_valid(id, bound))
        return 0u;
    type = &types[id];
    if (type->kind == SPV_TYPE_POINTER_KIND)
        return type_size(types, bound, type->element);
    switch (type->kind) {
        case SPV_TYPE_BOOL_KIND:
        case SPV_TYPE_INT_KIND:
        case SPV_TYPE_FLOAT_KIND:
            return type->width == 32u ? 4u : 0u;
        case SPV_TYPE_VECTOR_KIND:
            size = (uint64_t)type_size(types, bound, type->element) *
                   type->length;
            break;
        case SPV_TYPE_MATRIX_KIND:
            size = (uint64_t)type_size(types, bound, type->element) *
                   type->columns;
            break;
        case SPV_TYPE_ARRAY_KIND:
            size = (uint64_t)type_size(types, bound, type->element) *
                   type->length;
            break;
        case SPV_TYPE_STRUCT_KIND:
            for (index = 0u; index < type->member_count; ++index) {
                uint32_t member_size = type_size(types, bound,
                                                 type->members[index]);
                uint32_t offset = type->member_offsets_set &
                                          (UINT32_C(1) << index)
                                      ? type->member_offsets[index]
                                      : (uint32_t)size;
                uint64_t end = (uint64_t)offset + member_size;
                if (member_size == 0u || end > UINT32_MAX)
                    return 0u;
                if (end > size)
                    size = end;
            }
            break;
        default:
            return 0u;
    }
    return size == 0u || size > UINT32_MAX ? 0u : (uint32_t)size;
}

static int descriptor_type(const SpvType* types, uint32_t bound,
                           uint32_t type_id, uint32_t* kind)
{
    const SpvType* type;

    if (!kind || !id_valid(type_id, bound))
        return 0;
    type = &types[type_id];
    if (type->kind == SPV_TYPE_POINTER_KIND)
        return descriptor_type(types, bound, type->element, kind);
    if (type->kind == SPV_TYPE_SAMPLER_KIND) {
        *kind = RIN_SHADER_RESOURCE_SAMPLER;
        return 1;
    }
    if (type->kind == SPV_TYPE_IMAGE_KIND ||
        type->kind == SPV_TYPE_SAMPLED_IMAGE_KIND) {
        *kind = RIN_SHADER_RESOURCE_SAMPLED_IMAGE;
        return 1;
    }
    return 0;
}

static uint32_t stage_from_execution_model(uint32_t model)
{
    switch (model) {
        case SPV_EXEC_VERTEX: return RIN_SHADER_STAGE_VERTEX;
        case SPV_EXEC_FRAGMENT: return RIN_SHADER_STAGE_FRAGMENT;
        case SPV_EXEC_COMPUTE: return RIN_SHADER_STAGE_COMPUTE;
        default: return 0u;
    }
}

static int append_instruction(SpvBuilder* builder,
                              RinShaderInstructionV1 instruction)
{
    if (!builder || builder->count >= sizeof(builder->instructions) /
                                 sizeof(builder->instructions[0]))
        return 0;
    builder->instructions[builder->count++] = instruction;
    return 1;
}

static int alloc_register(SpvBuilder* builder, uint32_t id, uint16_t* output)
{
    uint16_t result;

    if (!builder || !output || id >= 65536u)
        return 0;
    if (builder->register_defined[id]) {
        *output = builder->registers[id];
        return 1;
    }
    if (builder->next_register >= RIN_SHADER_MAX_REGISTERS)
        return 0;
    result = builder->next_register++;
    builder->registers[id] = result;
    builder->register_defined[id] = 1u;
    *output = result;
    return 1;
}

static int source_register(const SpvBuilder* builder, uint32_t id,
                           uint16_t* output)
{
    if (!builder || !output || id >= 65536u || !builder->register_defined[id])
        return 0;
    *output = builder->registers[id];
    return 1;
}

static int alloc_temporary_register(SpvBuilder* builder, uint16_t* output)
{
    if (!builder || !output ||
        builder->next_register >= RIN_SHADER_MAX_REGISTERS)
        return 0;
    *output = builder->next_register++;
    return 1;
}

static RinShaderInstructionV1 instruction(uint16_t opcode, uint16_t dst,
                                          uint16_t source0, uint16_t source1,
                                          uint32_t immediate)
{
    RinShaderInstructionV1 value;

    memset(&value, 0, sizeof(value));
    value.opcode = opcode;
    value.destination = dst;
    value.source0 = source0;
    value.source1 = source1;
    value.resource = RIN_SHADER_UNUSED;
    value.immediate = immediate;
    return value;
}

static int record_io(RinSpirvTranslationInfoV1* info, const SpvDecorations* dec,
                     const SpvType* types, uint32_t bound, uint32_t id,
                     uint32_t storage, uint32_t type_id,
                     const char* name)
{
    RinSpirvIoV1* io;
    uint32_t base;
    uint32_t width;

    if (!info || !dec || !dec->has_location ||
        dec->location >= RIN_SPIRV_MAX_IO ||
        !type_scalar_or_vector(types, bound, type_id, &base, &width)) {
        return 0;
    }
    if (width > RIN_SPIRV_MAX_IO - dec->location)
        return 0;
    if (storage == SPV_STORAGE_INPUT) {
        if (info->input_count >= RIN_SPIRV_MAX_IO)
            return 0;
        io = &info->inputs[info->input_count];
        ++info->input_count;
    } else {
        if (info->output_count >= RIN_SPIRV_MAX_IO)
            return 0;
        io = &info->outputs[info->output_count];
        ++info->output_count;
    }
    memset(io, 0, sizeof(*io));
    io->id = id;
    io->storage = storage == SPV_STORAGE_INPUT ? RIN_SPIRV_IO_INPUT
                                               : RIN_SPIRV_IO_OUTPUT;
    io->location = dec->location;
    io->width = width;
    io->base_type = base;
    if (dec->has_builtin)
        io->builtin = dec->builtin;
    if (name)
        (void)snprintf(io->name, sizeof(io->name), "%s", name);
    return 1;
}

static uint32_t io_slot_count(const RinSpirvIoV1* io, uint32_t count)
{
    uint32_t slots = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        uint32_t end = io[index].location + io[index].width;
        if (end > slots) slots = end;
    }
    return slots;
}

static int find_io_location(const RinSpirvTranslationInfoV1* info,
                            uint32_t storage, uint32_t id, uint32_t* location)
{
    uint32_t index;

    if (!info || !location)
        return 0;
    if (storage == SPV_STORAGE_INPUT) {
        for (index = 0u; index < info->input_count; ++index) {
            if (info->inputs[index].id == id) {
                *location = info->inputs[index].location;
                return 1;
            }
        }
    } else {
        for (index = 0u; index < info->output_count; ++index) {
            if (info->outputs[index].id == id) {
                *location = info->outputs[index].location;
                return 1;
            }
        }
    }
    return 0;
}

static int collect_descriptor(RinSpirvTranslationInfoV1* info,
                              const SpvDecorations* decorations,
                              const SpvType* types, uint32_t bound, uint32_t id,
                              const SpvVariable* variable, const char* name)
{
    uint32_t kind = RIN_SHADER_RESOURCE_NONE;
    RinSpirvDescriptorV1* descriptor;

    if (!info || !decorations || !variable || !variable->defined)
        return 0;
    if (variable->storage_class == SPV_STORAGE_UNIFORM_CONSTANT) {
        if (!descriptor_type(types, bound, variable->type_id, &kind))
            return 0;
    } else if (variable->storage_class == SPV_STORAGE_UNIFORM ||
               variable->storage_class == SPV_STORAGE_STORAGE_BUFFER) {
        kind = RIN_SHADER_RESOURCE_STORAGE_BUFFER;
    } else {
        return 1;
    }
    if (!decorations->has_binding || !decorations->has_descriptor_set ||
        info->descriptor_count >= RIN_SPIRV_MAX_RESOURCES)
        return 0;
    descriptor = &info->descriptors[info->descriptor_count];
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->id = id;
    descriptor->set = decorations->descriptor_set;
    descriptor->binding = decorations->binding;
    descriptor->resource_index = info->descriptor_count;
    descriptor->resource_kind = kind;
    descriptor->storage_class = variable->storage_class;
    descriptor->type_id = variable->type_id;
    if (name)
        (void)snprintf(descriptor->name, sizeof(descriptor->name), "%s", name);
    ++info->descriptor_count;
    return 1;
}

static int resource_pointer_info(const RinSpirvTranslationInfoV1* info,
                                 const SpvVariable* variables,
                                 uint32_t bound, uint32_t pointer_id,
                                 uint32_t* resource_index_out,
                                 uint32_t* byte_offset_out)
{
    uint32_t base_id;
    uint32_t index;

    if (!info || !variables || !resource_index_out || !byte_offset_out ||
        !id_valid(pointer_id, bound))
        return 0;
    if (variables[pointer_id].pointer_defined) {
        base_id = variables[pointer_id].pointer_base_id;
        *byte_offset_out = variables[pointer_id].pointer_offset;
    } else if (variables[pointer_id].defined &&
               variables[pointer_id].storage_class ==
                   SPV_STORAGE_STORAGE_BUFFER) {
        base_id = pointer_id;
        *byte_offset_out = 0u;
    } else {
        return 0;
    }
    for (index = 0u; index < info->descriptor_count; ++index) {
        if (info->descriptors[index].id == base_id &&
            info->descriptors[index].resource_kind ==
                RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
            info->descriptors[index].resource_index <
                RIN_SHADER_MAX_RESOURCES) {
            *resource_index_out =
                info->descriptors[index].resource_index;
            return 1;
        }
    }
    return 0;
}

static int collect_storage_access_chain(
    const uint32_t* instruction_words, uint16_t word_count,
    const SpvType* types, const SpvValue* values, SpvVariable* variables,
    uint32_t bound)
{
    uint32_t result_type_id;
    uint32_t result_id;
    uint32_t base_id;
    uint32_t root_id;
    uint32_t current_type_id;
    uint32_t byte_offset;
    uint32_t index;
    const SpvType* pointer_type;

    if (!instruction_words || !types || !values || !variables ||
        word_count < 5u)
        return 0;
    result_type_id = instruction_words[1];
    result_id = instruction_words[2];
    base_id = instruction_words[3];
    if (!id_valid(result_type_id, bound) || !id_valid(result_id, bound) ||
        !id_valid(base_id, bound) || variables[result_id].defined ||
        variables[result_id].pointer_defined)
        return 0;
    pointer_type = &types[result_type_id];
    if (pointer_type->kind != SPV_TYPE_POINTER_KIND ||
        pointer_type->storage_class != SPV_STORAGE_STORAGE_BUFFER)
        return 0;
    if (variables[base_id].pointer_defined) {
        root_id = variables[base_id].pointer_base_id;
        byte_offset = variables[base_id].pointer_offset;
        if (!id_valid(variables[base_id].type_id, bound) ||
            types[variables[base_id].type_id].kind != SPV_TYPE_POINTER_KIND)
            return 0;
        current_type_id = types[variables[base_id].type_id].element;
    } else if (variables[base_id].defined &&
               variables[base_id].storage_class ==
                   SPV_STORAGE_STORAGE_BUFFER) {
        root_id = base_id;
        byte_offset = 0u;
        if (!id_valid(variables[base_id].type_id, bound) ||
            types[variables[base_id].type_id].kind != SPV_TYPE_POINTER_KIND ||
            types[variables[base_id].type_id].storage_class !=
                SPV_STORAGE_STORAGE_BUFFER)
            return 0;
        current_type_id = types[variables[base_id].type_id].element;
    } else {
        return 0;
    }
    if (!id_valid(root_id, bound)) return 0;

    for (index = 4u; index < word_count; ++index) {
        uint32_t member_index;
        const SpvType* aggregate;
        uint32_t member_offset;
        if (!id_valid(instruction_words[index], bound) ||
            !values[instruction_words[index]].defined ||
            values[instruction_words[index]].specialization ||
            !id_valid(values[instruction_words[index]].type_id, bound) ||
            types[values[instruction_words[index]].type_id].kind !=
                SPV_TYPE_INT_KIND ||
            types[values[instruction_words[index]].type_id].width != 32u)
            return 0;
        member_index = values[instruction_words[index]].bits;
        if (!id_valid(current_type_id, bound)) return 0;
        aggregate = &types[current_type_id];
        if (aggregate->kind != SPV_TYPE_STRUCT_KIND ||
            member_index >= aggregate->member_count || member_index >= 16u ||
            (aggregate->member_offsets_set &
             (UINT32_C(1) << member_index)) == 0u)
            return 0;
        member_offset = aggregate->member_offsets[member_index];
        if (byte_offset > UINT32_MAX - member_offset) return 0;
        byte_offset += member_offset;
        current_type_id = aggregate->members[member_index];
    }
    if (current_type_id != pointer_type->element) return 0;

    variables[result_id].type_id = result_type_id;
    variables[result_id].storage_class = SPV_STORAGE_STORAGE_BUFFER;
    variables[result_id].pointer_defined = 1u;
    variables[result_id].pointer_base_id = root_id;
    variables[result_id].pointer_offset = byte_offset;
    return 1;
}

static int collect_push_constant(RinSpirvTranslationInfoV1* info,
                                 const SpvType* types, uint32_t bound,
                                 uint32_t id, const SpvVariable* variable,
                                 const char* name)
{
    RinSpirvPushConstantV1* push;

    if (!info || !variable || !variable->defined ||
        variable->storage_class != SPV_STORAGE_PUSH_CONSTANT)
        return 1;
    if (info->push_constant_count >= RIN_SPIRV_MAX_RESOURCES)
        return 0;
    push = &info->push_constants[info->push_constant_count++];
    memset(push, 0, sizeof(*push));
    push->id = id;
    push->type_id = variable->type_id;
    push->size_bytes = type_size(types, bound, variable->type_id);
    if (push->size_bytes == 0u)
        return 0;
    if (name)
        (void)snprintf(push->name, sizeof(push->name), "%s", name);
    return 1;
}

static int collect_specialization(RinSpirvTranslationInfoV1* info,
                                  const SpvDecorations* decorations,
                                  const SpvType* types, uint32_t bound,
                                  uint32_t id, const SpvValue* value)
{
    RinSpirvSpecializationV1* specialization;
    uint32_t base;

    if (!info || !decorations || !value || !value->defined ||
        !value->specialization || !decorations->has_spec_id ||
        !type_scalar(types, bound, value->type_id, &base) ||
        info->specialization_count >= RIN_SPIRV_MAX_SPECIALIZATIONS)
        return value && !value->specialization;
    specialization = &info->specializations[info->specialization_count++];
    memset(specialization, 0, sizeof(*specialization));
    specialization->id = id;
    specialization->spec_id = decorations->spec_id;
    specialization->base_type = base;
    specialization->default_bits = value->bits;
    specialization->override_bits = value->bits;
    return 1;
}

static int translate_function_instruction(
    SpvBuilder* builder, RinSpirvTranslationInfoV1* info,
    const SpvType* types, const SpvDecorations* decorations,
    SpvValue* values, const SpvVariable* variables, uint32_t bound,
    const SpvInstruction* current, int* saw_return)
{
    const uint32_t* words = current->words;
    uint16_t opcode = current->opcode;
    uint32_t location;
    uint32_t base;
    uint32_t width;
    uint16_t dst;
    uint16_t source0;
    uint16_t source1;
    RinShaderInstructionV1 emitted;

    if (opcode == SPV_OP_CONSTANT || opcode == SPV_OP_SPEC_CONSTANT)
        return current->word_count == 4u;
    if (opcode == SPV_OP_LABEL || opcode == SPV_OP_FUNCTION ||
        opcode == SPV_OP_FUNCTION_END || opcode == SPV_OP_ACCESS_CHAIN ||
        opcode == SPV_OP_VARIABLE)
        return opcode != SPV_OP_VARIABLE;
    if (opcode == SPV_OP_RETURN) {
        if (current->word_count != 1u || !append_instruction(
                builder, instruction(RIN_SHADER_OP_RETURN, RIN_SHADER_UNUSED,
                                     RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u)))
            return 0;
        *saw_return = 1;
        return 1;
    }
    if (opcode == SPV_OP_RETURN_VALUE || opcode == SPV_OP_UNREACHABLE ||
        opcode == SPV_OP_BRANCH || opcode == SPV_OP_BRANCH_CONDITIONAL)
        return 0;
    if (opcode == SPV_OP_COMPOSITE_CONSTRUCT) {
        uint32_t result_type;
        uint32_t element_type;
        uint32_t expected_width;
        uint32_t component_count = 0u;
        uint32_t base_type;
        if (current->word_count < 5u || !id_valid(words[1], bound) ||
            !id_valid(words[2], bound) ||
            types[words[1]].kind != SPV_TYPE_VECTOR_KIND ||
            !type_scalar(types, bound, types[words[1]].element, &base_type))
            return 0;
        result_type = words[1];
        element_type = types[result_type].element;
        expected_width = types[result_type].length;
        if (expected_width < 2u || expected_width > 4u ||
            values[words[2]].component_count != 0u)
            return 0;
        for (uint32_t operand = 3u; operand < current->word_count;
             ++operand) {
            const uint32_t source_id = words[operand];
            uint32_t source_width = 1u;
            if (!id_valid(source_id, bound) ||
                values[source_id].type_id == 0u)
                return 0;
            if (values[source_id].component_count != 0u) {
                if (!id_valid(values[source_id].type_id, bound) ||
                    types[values[source_id].type_id].kind !=
                        SPV_TYPE_VECTOR_KIND ||
                    types[values[source_id].type_id].element != element_type)
                    return 0;
                source_width = values[source_id].component_count;
            } else if (values[source_id].type_id != element_type) {
                return 0;
            }
            if (source_width > expected_width - component_count)
                return 0;
            for (uint32_t component = 0u; component < source_width;
                 ++component) {
                uint16_t source_register_value;
                if (values[source_id].component_count != 0u) {
                    source_register_value =
                        values[source_id].component_registers[component];
                } else if (!source_register(builder, source_id,
                                             &source_register_value)) {
                    return 0;
                }
                values[words[2]].component_registers[component_count++] =
                    source_register_value;
            }
        }
        if (component_count != expected_width)
            return 0;
        values[words[2]].type_id = result_type;
        values[words[2]].component_count = component_count;
        return 1;
    }
    if (opcode == SPV_OP_COMPOSITE_EXTRACT) {
        uint32_t composite_id;
        uint32_t component;
        if (current->word_count != 5u || !id_valid(words[1], bound) ||
            !id_valid(words[2], bound) || !id_valid(words[3], bound) ||
            values[words[3]].component_count == 0u ||
            words[4] >= values[words[3]].component_count ||
            values[words[2]].component_count != 0u ||
            !id_valid(values[words[3]].type_id, bound) ||
            types[values[words[3]].type_id].kind != SPV_TYPE_VECTOR_KIND ||
            types[values[words[3]].type_id].element != words[1])
            return 0;
        composite_id = words[3];
        component = words[4];
        if (!alloc_register(builder, words[2], &dst) ||
            !append_instruction(
                builder, instruction(RIN_SHADER_OP_MOV, dst,
                                     values[composite_id]
                                         .component_registers[component],
                                     RIN_SHADER_UNUSED, 0u)))
            return 0;
        values[words[2]].type_id = words[1];
        return 1;
    }
    if (opcode == SPV_OP_COPY_OBJECT) {
        if (current->word_count != 4u || !id_valid(words[1], bound) ||
            !id_valid(words[2], bound) ||
            !id_valid(words[3], bound) ||
            values[words[3]].type_id != words[1] ||
            !source_register(builder, words[3], &source0) ||
            !alloc_register(builder, words[2], &dst))
            return 0;
        if (!append_instruction(builder,
                                instruction(RIN_SHADER_OP_MOV, dst, source0,
                                            RIN_SHADER_UNUSED, 0u)))
            return 0;
        values[words[2]].type_id = words[1];
        return 1;
    }
    if (opcode == SPV_OP_LOAD) {
        const SpvVariable* variable;
        uint32_t pointer_id;

        if (current->word_count != 4u || !id_valid(words[1], bound) ||
            !id_valid(words[2], bound) || !id_valid(words[3], bound))
            return 0;
        pointer_id = words[3];
        variable = &variables[pointer_id];
        if ((variable->pointer_defined ||
             (variable->defined &&
              variable->storage_class == SPV_STORAGE_STORAGE_BUFFER))) {
            uint32_t resource_index;
            uint32_t byte_offset;
            uint32_t value_type;
            uint16_t address_register;
            if (!resource_pointer_info(info, variables, bound, pointer_id,
                                       &resource_index, &byte_offset) ||
                !type_scalar(types, bound, words[1], &value_type) ||
                !id_valid(variable->type_id, bound) ||
                types[variable->type_id].kind != SPV_TYPE_POINTER_KIND ||
                types[variable->type_id].element != words[1] ||
                byte_offset > INT32_MAX ||
                !alloc_register(builder, words[2], &dst))
                return 0;
            if (builder->next_register >= RIN_SHADER_MAX_REGISTERS)
                return 0;
            address_register = builder->next_register++;
            if (!append_instruction(
                    builder, instruction(RIN_SHADER_OP_CONST_I32,
                                         address_register, RIN_SHADER_UNUSED,
                                         RIN_SHADER_UNUSED, byte_offset)))
                return 0;
            emitted = instruction(value_type == 2u
                                      ? RIN_SHADER_OP_LOAD_RESOURCE_F32
                                      : RIN_SHADER_OP_LOAD_RESOURCE_I32,
                                  dst, address_register, RIN_SHADER_UNUSED, 0u);
            emitted.resource = (uint16_t)resource_index;
            if (builder->resource_count <= resource_index)
                builder->resource_count = resource_index + 1u;
            if (!append_instruction(builder, emitted)) return 0;
            values[words[2]].type_id = words[1];
            return 1;
        }
        if (!variable->defined || variable->storage_class != SPV_STORAGE_INPUT ||
            !find_io_location(info, SPV_STORAGE_INPUT, pointer_id, &location) ||
            !id_valid(variable->type_id, bound) ||
            types[variable->type_id].kind != SPV_TYPE_POINTER_KIND ||
            types[variable->type_id].element != words[1] ||
            !type_scalar_or_vector(types, bound, words[1], &base, &width))
            return 0;
        if (width > RIN_SHADER_MAX_IO - location ||
            values[words[2]].type_id != 0u ||
            values[words[2]].component_count != 0u)
            return 0;
        values[words[2]].type_id = words[1];
        if (width == 1u) {
            if (!alloc_register(builder, words[2], &dst)) return 0;
            emitted = instruction(base == 2u ? RIN_SHADER_OP_LOAD_INPUT_F32
                                            : RIN_SHADER_OP_LOAD_INPUT,
                                  dst, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                                  location);
            return append_instruction(builder, emitted);
        }
        values[words[2]].component_count = width;
        for (uint32_t component = 0u; component < width; ++component) {
            if (!alloc_temporary_register(builder, &dst)) return 0;
            values[words[2]].component_registers[component] = dst;
            emitted = instruction(base == 2u ? RIN_SHADER_OP_LOAD_INPUT_F32
                                            : RIN_SHADER_OP_LOAD_INPUT,
                                  dst, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                                  location + component);
            if (!append_instruction(builder, emitted)) return 0;
        }
        return 1;
    }
    if (opcode == SPV_OP_STORE) {
        const SpvVariable* variable;

        if (current->word_count != 3u || !id_valid(words[1], bound) ||
            !id_valid(words[2], bound))
            return 0;
        variable = &variables[words[1]];
        if (variable->pointer_defined ||
            (variable->defined &&
             variable->storage_class == SPV_STORAGE_STORAGE_BUFFER)) {
            uint32_t resource_index;
            uint32_t byte_offset;
            uint32_t value_type;
            uint16_t address_register;
            uint32_t pointer_type_id = variable->type_id;
            if (!resource_pointer_info(info, variables, bound, words[1],
                                       &resource_index, &byte_offset) ||
                !id_valid(pointer_type_id, bound) ||
                types[pointer_type_id].kind != SPV_TYPE_POINTER_KIND ||
                !type_scalar(types, bound,
                             types[pointer_type_id].element, &value_type) ||
                !source_register(builder, words[2], &source0) ||
                byte_offset > INT32_MAX ||
                builder->next_register >= RIN_SHADER_MAX_REGISTERS)
                return 0;
            address_register = builder->next_register++;
            if (!append_instruction(
                    builder, instruction(RIN_SHADER_OP_CONST_I32,
                                         address_register, RIN_SHADER_UNUSED,
                                         RIN_SHADER_UNUSED, byte_offset)))
                return 0;
            emitted = instruction(value_type == 2u
                                      ? RIN_SHADER_OP_STORE_RESOURCE_F32
                                      : RIN_SHADER_OP_STORE_RESOURCE_I32,
                                  RIN_SHADER_UNUSED, address_register, source0,
                                  0u);
            emitted.resource = (uint16_t)resource_index;
            if (builder->resource_count <= resource_index)
                builder->resource_count = resource_index + 1u;
            return append_instruction(builder, emitted);
        }
        if (!variable->defined ||
            variable->storage_class != SPV_STORAGE_OUTPUT) {
            diag(info, "SPIR-V output store targets a non-output variable");
            return 0;
        }
        if (!id_valid(variable->type_id, bound) ||
            types[variable->type_id].kind != SPV_TYPE_POINTER_KIND) {
            diag(info, "SPIR-V output store pointer type is invalid");
            return 0;
        }
        if (values[words[2]].type_id != types[variable->type_id].element) {
            diag(info,
                 "SPIR-V output store type mismatch: value=%u output=%u",
                 values[words[2]].type_id,
                 types[variable->type_id].element);
            return 0;
        }
        if (!type_scalar_or_vector(types, bound,
                                   types[variable->type_id].element, &base,
                                   &width)) {
            diag(info, "SPIR-V output type is outside scalar/vector profile");
            return 0;
        }
        if (decorations[words[1]].has_builtin) {
            if (variable->storage_class != SPV_STORAGE_OUTPUT ||
                decorations[words[1]].builtin != SPV_BUILTIN_POSITION ||
                base != 2u || width != 4u)
                return 0;
            location = 0u;
        } else if (!find_io_location(info, SPV_STORAGE_OUTPUT, words[1],
                                     &location)) {
            diag(info, "SPIR-V output store has no reflected location");
            return 0;
        }
        if (width > RIN_SHADER_MAX_IO - location) {
            diag(info, "SPIR-V output store exceeds the RSH1 location range");
            return 0;
        }
        if (width == 1u) {
            if (!source_register(builder, words[2], &source0)) return 0;
            emitted = instruction(base == 2u ? RIN_SHADER_OP_STORE_OUTPUT_F32
                                            : RIN_SHADER_OP_STORE_OUTPUT,
                                  RIN_SHADER_UNUSED, source0,
                                  RIN_SHADER_UNUSED, location);
            return append_instruction(builder, emitted);
        }
        if (values[words[2]].component_count != width) {
            diag(info, "SPIR-V vector output store lacks scalar components");
            return 0;
        }
        for (uint32_t component = 0u; component < width; ++component) {
            emitted = instruction(base == 2u ? RIN_SHADER_OP_STORE_OUTPUT_F32
                                            : RIN_SHADER_OP_STORE_OUTPUT,
                                  RIN_SHADER_UNUSED,
                                  values[words[2]].component_registers[component],
                                  RIN_SHADER_UNUSED, location + component);
            if (!append_instruction(builder, emitted)) return 0;
        }
        return 1;
    }
    if (opcode == SPV_OP_F_NEGATE) {
        uint16_t zero;

        if (current->word_count != 4u || !id_valid(words[1], bound) ||
            !id_valid(words[2], bound) || !id_valid(words[3], bound) ||
            !type_scalar(types, bound, words[1], &base) || base != 2u ||
            !source_register(builder, words[3], &source0) ||
            !alloc_register(builder, words[2], &dst) ||
            builder->next_register >= RIN_SHADER_MAX_REGISTERS)
            return 0;
        zero = builder->next_register++;
        if (!append_instruction(builder, instruction(RIN_SHADER_OP_CONST_F32,
                                                     zero, RIN_SHADER_UNUSED,
                                                     RIN_SHADER_UNUSED, 0u)))
            return 0;
        if (!append_instruction(builder, instruction(RIN_SHADER_OP_SUB_F32,
                                                     dst, zero, source0, 0u)))
            return 0;
        values[words[2]].type_id = words[1];
        return 1;
    }
    if (current->word_count != 5u || !id_valid(words[1], bound) ||
        !id_valid(words[2], bound) || !id_valid(words[3], bound) ||
        !id_valid(words[4], bound) || !type_scalar(types, bound, words[1], &base) ||
        !source_register(builder, words[3], &source0) ||
        !source_register(builder, words[4], &source1) ||
        !alloc_register(builder, words[2], &dst))
        return 0;
    switch (opcode) {
        case SPV_OP_I_ADD: emitted = instruction(RIN_SHADER_OP_ADD_I32, dst,
                                                  source0, source1, 0u); break;
        case SPV_OP_I_SUB: emitted = instruction(RIN_SHADER_OP_SUB_I32, dst,
                                                  source0, source1, 0u); break;
        case SPV_OP_I_MUL: emitted = instruction(RIN_SHADER_OP_MUL_I32, dst,
                                                  source0, source1, 0u); break;
        case SPV_OP_F_ADD: emitted = instruction(RIN_SHADER_OP_ADD_F32, dst,
                                                  source0, source1, 0u); break;
        case SPV_OP_F_SUB: emitted = instruction(RIN_SHADER_OP_SUB_F32, dst,
                                                  source0, source1, 0u); break;
        case SPV_OP_F_MUL: emitted = instruction(RIN_SHADER_OP_MUL_F32, dst,
                                                  source0, source1, 0u); break;
        case SPV_OP_F_DIV: emitted = instruction(RIN_SHADER_OP_DIV_F32, dst,
                                                  source0, source1, 0u); break;
        default: return 0;
    }
    if ((base == 1u && (opcode == SPV_OP_F_ADD || opcode == SPV_OP_F_SUB ||
                        opcode == SPV_OP_F_MUL || opcode == SPV_OP_F_DIV)) ||
        (base == 2u && (opcode == SPV_OP_I_ADD || opcode == SPV_OP_I_SUB ||
                        opcode == SPV_OP_I_MUL)))
        return 0;
    if (!append_instruction(builder, emitted)) return 0;
    values[words[2]].type_id = words[1];
    return 1;
}

int ringpu_spirv_translate(const uint32_t* words, size_t word_count,
                           uint32_t expected_stage,
                           const RinSpirvSpecializationValueV1* overrides,
                           uint32_t override_count,
                           void* rin_shader_out, size_t rin_shader_capacity,
                           RinSpirvTranslationInfoV1* info)
{
    uint32_t bound;
    SpvType* types = NULL;
    SpvDecorations* decorations = NULL;
    SpvValue* values = NULL;
    SpvVariable* variables = NULL;
    uint8_t* interface_ids = NULL;
    char (*names)[RIN_SPIRV_NAME_MAX] = NULL;
    size_t cursor;
    uint32_t entry_id = 0u;
    uint32_t execution_model = UINT32_MAX;
    uint32_t selected_function = 0u;
    uint32_t function_count = 0u;
    uint32_t current_function = 0u;
    uint32_t memory_model = UINT32_MAX;
    uint32_t addressing_model = UINT32_MAX;
    uint32_t capability_shader = 0u;
    uint32_t execution_mode_entry = 0u;
    uint32_t workgroup_x = 0u;
    uint32_t workgroup_y = 0u;
    uint32_t workgroup_z = 0u;
    uint32_t local_size_seen = 0u;
    uint32_t builtin_position_output = 0u;
    uint32_t stage;
    int in_selected_function = 0;
    int saw_function_end = 0;
    int saw_return = 0;
    SpvBuilder builder;
    uint8_t candidate[sizeof(RinShaderHeaderV1) + 1024u *
                      sizeof(RinShaderInstructionV1)];
    RinShaderHeaderV1 header;
    RinShaderInfoV1 shader_info;
    uint32_t index;
    int result = RIN_SPIRV_ERROR_MALFORMED;

    if (info)
        memset(info, 0, sizeof(*info));
    if (info) {
        info->struct_size = sizeof(*info);
        info->api_version = 1u;
    }
    if (!words || word_count < 5u || !rin_shader_out || !info)
        return RIN_SPIRV_ERROR_INVALID_ARGUMENT;
    if (words[0] != RIN_SPIRV_MAGIC || words[1] < 0x00010000u ||
        words[1] > 0x00010600u || words[3] == 0u || words[4] != 0u)
        return RIN_SPIRV_ERROR_MALFORMED;
    bound = words[3];
    if (bound > 65536u)
        return RIN_SPIRV_ERROR_BOUNDS;

    types = (SpvType*)calloc(bound, sizeof(*types));
    decorations = (SpvDecorations*)calloc(bound, sizeof(*decorations));
    values = (SpvValue*)calloc(bound, sizeof(*values));
    variables = (SpvVariable*)calloc(bound, sizeof(*variables));
    interface_ids = (uint8_t*)calloc(bound, sizeof(*interface_ids));
    names = (char (*)[RIN_SPIRV_NAME_MAX])calloc(bound, sizeof(*names));
    if (!types || !decorations || !values || !variables || !interface_ids ||
        !names) {
        result = RIN_SPIRV_ERROR_NO_MEMORY;
        diag(info, "SPIR-V bounded tables could not be allocated");
        goto done;
    }

    /* Pass one validates word framing and collects all declarations. */
    cursor = 5u * sizeof(uint32_t);
    while (cursor < word_count * sizeof(uint32_t)) {
        const uint32_t* instruction_words = words + cursor / sizeof(uint32_t);
        size_t remaining = word_count - cursor / sizeof(uint32_t);
        uint16_t opcode;
        uint16_t word_count_instruction;
        uint32_t id;

        if (!instruction_valid(instruction_words, remaining, &opcode,
                               &word_count_instruction)) {
            diag(info, "malformed SPIR-V instruction framing");
            goto done;
        }
        if (word_count_instruction == 0u ||
            cursor > SIZE_MAX - (size_t)word_count_instruction * 4u) {
            diag(info, "SPIR-V instruction size overflow");
            goto done;
        }
        switch (opcode) {
            case SPV_OP_SPEC_CONSTANT:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound)) {
                    diag(info, "SPIR-V scalar constant is malformed");
                    goto done;
                }
                values[instruction_words[2]].type_id = instruction_words[1];
                values[instruction_words[2]].bits = instruction_words[3];
                values[instruction_words[2]].defined = 1u;
                values[instruction_words[2]].specialization = 1u;
                break;
            case SPV_OP_CAPABILITY:
                if (word_count_instruction != 2u ||
                    instruction_words[1] != SPV_CAPABILITY_SHADER) {
                    diag(info, "SPIR-V capability is outside the bounded profile");
                    result = RIN_SPIRV_ERROR_UNSUPPORTED;
                    goto done;
                }
                capability_shader = 1u;
                break;
            case SPV_OP_MEMORY_MODEL:
                if (word_count_instruction != 3u ||
                    instruction_words[1] != SPV_ADDRESSING_LOGICAL ||
                    (instruction_words[2] != SPV_MEMORY_GLSL450 &&
                     instruction_words[2] != SPV_MEMORY_VULKAN)) {
                    diag(info, "SPIR-V memory model is outside the bounded profile");
                    result = RIN_SPIRV_ERROR_UNSUPPORTED;
                    goto done;
                }
                addressing_model = instruction_words[1];
                memory_model = instruction_words[2];
                break;
            case SPV_OP_ENTRY_POINT:
                {
                    uint32_t interface_start;
                if (word_count_instruction < 4u ||
                    !id_valid(instruction_words[2], bound) ||
                    execution_model != UINT32_MAX) {
                    diag(info, "SPIR-V entry point is malformed or duplicated");
                    goto done;
                }
                execution_model = instruction_words[1];
                entry_id = instruction_words[2];
                if (!string_operand(instruction_words, word_count_instruction, 3u,
                                    info->entry_name, sizeof(info->entry_name))) {
                    diag(info, "SPIR-V entry point name is unterminated");
                    goto done;
                }
                if (!string_end_word(instruction_words, word_count_instruction,
                                     3u, &interface_start)) {
                    diag(info, "SPIR-V entry point name has no terminator");
                    goto done;
                }
                for (index = interface_start; index < word_count_instruction;
                     ++index) {
                    if (!id_valid(instruction_words[index], bound)) {
                        diag(info, "SPIR-V entry interface ID is out of bounds");
                        goto done;
                    }
                    interface_ids[instruction_words[index]] = 1u;
                }
                break;
                }
            case SPV_OP_NAME:
                if (word_count_instruction < 3u ||
                    !id_valid(instruction_words[1], bound) ||
                    !string_operand(instruction_words, word_count_instruction, 2u,
                                    names[instruction_words[1]],
                                    sizeof(names[instruction_words[1]]))) {
                    diag(info, "SPIR-V OpName is malformed");
                    goto done;
                }
                break;
            case SPV_OP_DECORATE:
                if (word_count_instruction < 3u ||
                    !id_valid(instruction_words[1], bound)) {
                    diag(info, "SPIR-V decoration target is out of bounds");
                    goto done;
                }
                id = instruction_words[1];
                switch (instruction_words[2]) {
                    case SPV_DECORATION_LOCATION:
                        if (word_count_instruction != 4u) goto bad_decor;
                        decorations[id].location = instruction_words[3];
                        decorations[id].has_location = 1u;
                        break;
                    case SPV_DECORATION_BINDING:
                        if (word_count_instruction != 4u) goto bad_decor;
                        decorations[id].binding = instruction_words[3];
                        decorations[id].has_binding = 1u;
                        break;
                    case SPV_DECORATION_DESCRIPTOR_SET:
                        if (word_count_instruction != 4u) goto bad_decor;
                        decorations[id].descriptor_set = instruction_words[3];
                        decorations[id].has_descriptor_set = 1u;
                        break;
                    case SPV_DECORATION_BUILT_IN:
                        if (word_count_instruction != 4u) goto bad_decor;
                        decorations[id].builtin = instruction_words[3];
                        decorations[id].has_builtin = 1u;
                        break;
                    case SPV_DECORATION_SPEC_ID:
                        if (word_count_instruction != 4u) goto bad_decor;
                        decorations[id].spec_id = instruction_words[3];
                        decorations[id].has_spec_id = 1u;
                        break;
                    case SPV_DECORATION_BLOCK:
                    case SPV_DECORATION_BUFFER_BLOCK:
                        if (word_count_instruction != 3u) goto bad_decor;
                        break;
                    default:
                        diag(info, "SPIR-V decoration %u is unsupported",
                             instruction_words[2]);
                        result = RIN_SPIRV_ERROR_UNSUPPORTED;
                        goto done;
                }
                break;
bad_decor:
                diag(info, "SPIR-V decoration operand count is invalid");
                goto done;
            case SPV_OP_MEMBER_DECORATE:
                if (word_count_instruction != 5u ||
                    !id_valid(instruction_words[1], bound) ||
                    instruction_words[2] >= 16u ||
                    instruction_words[3] != SPV_DECORATION_OFFSET) {
                    diag(info, "SPIR-V member offset decoration is invalid");
                    goto done;
                }
                types[instruction_words[1]].member_offsets[instruction_words[2]] =
                    instruction_words[4];
                types[instruction_words[1]].member_offsets_set |=
                    UINT32_C(1) << instruction_words[2];
                break;
            case SPV_OP_TYPE_VOID:
                if (word_count_instruction != 2u ||
                    !id_valid(instruction_words[1], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_VOID_KIND;
                break;
            case SPV_OP_TYPE_BOOL:
                if (word_count_instruction != 2u ||
                    !id_valid(instruction_words[1], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_BOOL_KIND;
                types[instruction_words[1]].width = 32u;
                break;
            case SPV_OP_TYPE_INT:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    instruction_words[2] != 32u) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_INT_KIND;
                types[instruction_words[1]].width = 32u;
                types[instruction_words[1]].rows = instruction_words[3];
                break;
            case SPV_OP_TYPE_FLOAT:
                if (word_count_instruction != 3u ||
                    !id_valid(instruction_words[1], bound) ||
                    instruction_words[2] != 32u) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_FLOAT_KIND;
                types[instruction_words[1]].width = 32u;
                break;
            case SPV_OP_TYPE_VECTOR:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound) ||
                    instruction_words[3] < 2u || instruction_words[3] > 4u)
                    goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_VECTOR_KIND;
                types[instruction_words[1]].element = instruction_words[2];
                types[instruction_words[1]].length = instruction_words[3];
                break;
            case SPV_OP_TYPE_MATRIX:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound) ||
                    instruction_words[3] == 0u || instruction_words[3] > 4u)
                    goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_MATRIX_KIND;
                types[instruction_words[1]].element = instruction_words[2];
                types[instruction_words[1]].columns = instruction_words[3];
                break;
            case SPV_OP_TYPE_IMAGE:
                if (word_count_instruction < 2u ||
                    !id_valid(instruction_words[1], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_IMAGE_KIND;
                break;
            case SPV_OP_TYPE_SAMPLER:
                if (word_count_instruction != 2u ||
                    !id_valid(instruction_words[1], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_SAMPLER_KIND;
                break;
            case SPV_OP_TYPE_SAMPLED_IMAGE:
                if (word_count_instruction != 3u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_SAMPLED_IMAGE_KIND;
                types[instruction_words[1]].element = instruction_words[2];
                break;
            case SPV_OP_TYPE_ARRAY:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound) ||
                    !id_valid(instruction_words[3], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_ARRAY_KIND;
                types[instruction_words[1]].element = instruction_words[2];
                types[instruction_words[1]].length = instruction_words[3];
                break;
            case SPV_OP_TYPE_STRUCT:
                if (word_count_instruction < 2u ||
                    word_count_instruction - 2u > 16u ||
                    !id_valid(instruction_words[1], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_STRUCT_KIND;
                types[instruction_words[1]].member_count =
                    word_count_instruction - 2u;
                for (index = 0u; index < word_count_instruction - 2u; ++index) {
                    if (!id_valid(instruction_words[2u + index], bound))
                        goto bad_type;
                    types[instruction_words[1]].members[index] =
                        instruction_words[2u + index];
                }
                break;
            case SPV_OP_TYPE_POINTER:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[3], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_POINTER_KIND;
                types[instruction_words[1]].storage_class = instruction_words[2];
                types[instruction_words[1]].element = instruction_words[3];
                break;
            case SPV_OP_TYPE_FUNCTION:
                if (word_count_instruction < 3u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound)) goto bad_type;
                types[instruction_words[1]].kind = SPV_TYPE_FUNCTION_KIND;
                types[instruction_words[1]].element = instruction_words[2];
                break;
bad_type:
                diag(info, "SPIR-V type declaration is malformed");
                goto done;
            case SPV_OP_VARIABLE:
                if (word_count_instruction < 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound)) {
                    diag(info, "SPIR-V variable declaration is malformed");
                    goto done;
                }
                variables[instruction_words[2]].type_id = instruction_words[1];
                variables[instruction_words[2]].storage_class =
                    instruction_words[3];
                variables[instruction_words[2]].defined = 1u;
                break;
            case SPV_OP_CONSTANT:
                if (word_count_instruction != 4u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound)) {
                    diag(info, "SPIR-V scalar constant is malformed");
                    goto done;
                }
                values[instruction_words[2]].type_id = instruction_words[1];
                values[instruction_words[2]].bits = instruction_words[3];
                values[instruction_words[2]].defined = 1u;
                values[instruction_words[2]].specialization = 0u;
                break;
            case SPV_OP_FUNCTION:
                if (word_count_instruction != 5u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound)) {
                    diag(info, "SPIR-V function declaration is malformed");
                    goto done;
                }
                current_function = instruction_words[2];
                ++function_count;
                if (current_function == entry_id)
                    selected_function = current_function;
                break;
            case SPV_OP_EXECUTION_MODE:
                if (word_count_instruction < 3u) {
                    diag(info, "SPIR-V execution mode is malformed");
                    goto done;
                }
                if (instruction_words[2] ==
                    SPV_EXECUTION_MODE_LOCAL_SIZE) {
                    if (word_count_instruction != 6u || local_size_seen ||
                        !id_valid(instruction_words[1], bound)) {
                        diag(info, "SPIR-V LocalSize mode is malformed or duplicated");
                        goto done;
                    }
                    execution_mode_entry = instruction_words[1];
                    workgroup_x = instruction_words[3];
                    workgroup_y = instruction_words[4];
                    workgroup_z = instruction_words[5];
                    if (workgroup_x == 0u || workgroup_y == 0u ||
                        workgroup_z == 0u || workgroup_x > 1024u ||
                        workgroup_y > 1024u || workgroup_z > 64u ||
                        (uint64_t)workgroup_x * workgroup_y * workgroup_z >
                            1024u) {
                        diag(info, "SPIR-V LocalSize is outside the RSH1 profile");
                        result = RIN_SPIRV_ERROR_UNSUPPORTED;
                        goto done;
                    }
                    local_size_seen = 1u;
                }
                break;
            case SPV_OP_ACCESS_CHAIN:
                if (word_count_instruction < 5u ||
                    !id_valid(instruction_words[1], bound) ||
                    !id_valid(instruction_words[2], bound) ||
                    !id_valid(instruction_words[3], bound)) {
                    diag(info, "SPIR-V storage access chain is malformed");
                    goto done;
                }
                if (!collect_storage_access_chain(
                        instruction_words, word_count_instruction, types,
                        values, variables, bound)) {
                    diag(info, "SPIR-V access chain is outside the bounded storage-buffer profile");
                    result = RIN_SPIRV_ERROR_UNSUPPORTED;
                    goto done;
                }
                break;
            case SPV_OP_CONSTANT_TRUE:
            case SPV_OP_CONSTANT_FALSE:
            case SPV_OP_SPEC_CONSTANT_TRUE:
            case SPV_OP_SPEC_CONSTANT_FALSE:
            case SPV_OP_FUNCTION_PARAMETER:
            case SPV_OP_VECTOR_SHUFFLE:
            case SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD:
            case SPV_OP_UNREACHABLE:
                diag(info, "SPIR-V instruction %u is outside the bounded profile",
                     opcode);
                result = RIN_SPIRV_ERROR_UNSUPPORTED;
                goto done;
            default:
                /* Arithmetic and control-flow opcodes are validated in the
                 * second pass, where result types and SSA registers exist. */
                break;
        }
        cursor += (size_t)word_count_instruction * sizeof(uint32_t);
    }
    if (!capability_shader || memory_model == UINT32_MAX ||
        addressing_model == UINT32_MAX || execution_model == UINT32_MAX ||
        selected_function == 0u || function_count != 1u) {
        diag(info, "SPIR-V entry missing cap=%u mem=%u addr=%u model=%u entry=%u selected=%u functions=%u",
             capability_shader, memory_model, addressing_model,
             execution_model, entry_id, selected_function, function_count);
        goto done;
    }
    stage = stage_from_execution_model(execution_model);
    if (stage == 0u || (expected_stage != 0u && expected_stage != stage)) {
        diag(info, "SPIR-V execution model does not match requested stage");
        result = RIN_SPIRV_ERROR_UNSUPPORTED;
        goto done;
    }
    if ((stage == RIN_SHADER_STAGE_COMPUTE &&
         (!local_size_seen || execution_mode_entry != entry_id)) ||
        (stage != RIN_SHADER_STAGE_COMPUTE && local_size_seen)) {
        diag(info, "SPIR-V stage requires a matching supported LocalSize mode");
        result = RIN_SPIRV_ERROR_UNSUPPORTED;
        goto done;
    }
    info->stage = stage;
    info->entry_id = entry_id;

    for (index = 1u; index < bound; ++index) {
        if (!variables[index].defined)
            continue;
        if (interface_ids[index] &&
            (variables[index].storage_class == SPV_STORAGE_INPUT ||
             variables[index].storage_class == SPV_STORAGE_OUTPUT)) {
            uint32_t pointed = variables[index].type_id;
            if (!id_valid(pointed, bound) ||
                types[pointed].kind != SPV_TYPE_POINTER_KIND)
                goto bad_interface;
            pointed = types[pointed].element;
            if (!decorations[index].has_location) {
                if (stage == RIN_SHADER_STAGE_VERTEX &&
                    variables[index].storage_class == SPV_STORAGE_OUTPUT &&
                    decorations[index].has_builtin &&
                    decorations[index].builtin == SPV_BUILTIN_POSITION) {
                    uint32_t base_type;
                    uint32_t width;
                    if (builtin_position_output ||
                        !type_scalar_or_vector(types, bound, pointed,
                                              &base_type, &width) ||
                        base_type != 2u || width != 4u)
                        goto bad_interface;
                    builtin_position_output = 1u;
                }
                continue;
            }
            if (!record_io(info, &decorations[index], types, bound, index,
                           variables[index].storage_class, pointed,
                           names[index]))
                goto bad_interface;
        }
        if (!collect_descriptor(info, &decorations[index], types, bound, index,
                                &variables[index], names[index]) ||
            !collect_push_constant(info, types, bound, index, &variables[index],
                                   names[index]))
            goto bad_interface;
    }
    for (index = 1u; index < bound; ++index) {
        if (!collect_specialization(info, &decorations[index], types, bound,
                                    index, &values[index]))
            goto bad_specialization;
    }
    goto metadata_done;
bad_interface:
    diag(info, "SPIR-V interface or descriptor cannot be represented by RSH1");
    result = RIN_SPIRV_ERROR_UNSUPPORTED;
    goto done;
bad_specialization:
    diag(info, "SPIR-V specialization constant is not a bounded scalar");
    result = RIN_SPIRV_ERROR_UNSUPPORTED;
    goto done;
metadata_done:

    memset(&builder, 0, sizeof(builder));
    memset(builder.registers, 0xff, sizeof(builder.registers));
    builder.next_register = 0u;
    /* Constants are emitted before the entry function so all SSA operands are
     * initialized when the function body is lowered. */
    cursor = 5u * sizeof(uint32_t);
    while (cursor < word_count * sizeof(uint32_t)) {
        const uint32_t* instruction_words = words + cursor / sizeof(uint32_t);
        uint16_t opcode = (uint16_t)(instruction_words[0] & 0xffffu);
        uint16_t instruction_words_count = (uint16_t)(instruction_words[0] >> 16u);
        uint32_t value_id;
        uint16_t destination;
        uint32_t bits;
        uint32_t base_type;

        if ((opcode == SPV_OP_CONSTANT || opcode == SPV_OP_SPEC_CONSTANT) &&
            instruction_words_count == 4u) {
            value_id = instruction_words[2];
            if (!type_scalar(types, bound, instruction_words[1], &base_type) ||
                !alloc_register(&builder, value_id, &destination)) {
                diag(info, "SPIR-V constant type is outside scalar RSH1");
                goto translate_unsupported;
            }
            bits = values[value_id].bits;
            if (values[value_id].specialization) {
                uint32_t override_index;
                for (override_index = 0u; override_index < override_count;
                     ++override_index) {
                    if (decorations[value_id].has_spec_id &&
                        overrides && overrides[override_index].spec_id ==
                                         decorations[value_id].spec_id) {
                        bits = overrides[override_index].bits;
                        for (uint32_t specialization_index = 0u;
                             specialization_index < info->specialization_count;
                             ++specialization_index) {
                            if (info->specializations[specialization_index]
                                    .spec_id == decorations[value_id].spec_id)
                                info->specializations[specialization_index]
                                    .override_bits = bits;
                        }
                    }
                }
            }
            if (!append_instruction(&builder,
                                    instruction(base_type == 2u
                                                    ? RIN_SHADER_OP_CONST_F32
                                                    : RIN_SHADER_OP_CONST_I32,
                                                destination, RIN_SHADER_UNUSED,
                                                RIN_SHADER_UNUSED, bits)))
                goto translate_bounds;
        }
        cursor += (size_t)instruction_words_count * sizeof(uint32_t);
    }

    cursor = 5u * sizeof(uint32_t);
    while (cursor < word_count * sizeof(uint32_t)) {
        const uint32_t* instruction_words = words + cursor / sizeof(uint32_t);
        uint16_t opcode = (uint16_t)(instruction_words[0] & 0xffffu);
        uint16_t instruction_words_count = (uint16_t)(instruction_words[0] >> 16u);
        SpvInstruction current = {opcode, instruction_words_count,
                                  instruction_words};

        if (opcode == SPV_OP_FUNCTION)
            in_selected_function = instruction_words[2] == selected_function;
        else if (opcode == SPV_OP_FUNCTION_END) {
            if (in_selected_function)
                saw_function_end = 1;
            in_selected_function = 0;
        } else if (in_selected_function) {
            if (!translate_function_instruction(
                    &builder, info, types, decorations, values, variables,
                    bound, &current, &saw_return)) {
                if (info->diagnostic[0] == '\0')
                    diag(info,
                         "SPIR-V instruction %u cannot be lowered to RSH1",
                         opcode);
                result = RIN_SPIRV_ERROR_UNSUPPORTED;
                goto done;
            }
        }
        cursor += (size_t)instruction_words_count * sizeof(uint32_t);
    }
    if (!saw_function_end || !saw_return || builder.count == 0u ||
        builder.count > RIN_SHADER_MAX_INSTRUCTIONS) {
        diag(info, "SPIR-V entry function has no complete bounded return path");
        goto translate_unsupported;
    }
    memset(&header, 0, sizeof(header));
    header.magic = RIN_SHADER_MAGIC;
    header.version = RIN_SHADER_IR_VERSION;
    header.header_size = sizeof(header);
    header.total_size = sizeof(header) + builder.count * sizeof(builder.instructions[0]);
    header.stage = stage;
    header.instruction_count = builder.count;
    header.register_count = builder.next_register == 0u ? 1u : builder.next_register;
    header.input_count = io_slot_count(info->inputs, info->input_count);
    header.output_count = io_slot_count(info->outputs, info->output_count);
    if (builtin_position_output && header.output_count < 4u)
        header.output_count = 4u;
    /* The RSH1 resource count is the active binding count. Declarations that
     * are not referenced by the lowered entry function remain in the SPIR-V
     * reflection record but are intentionally not forced into the executable
     * bind group. */
    header.resource_count = builder.resource_count;
    if (stage == RIN_SHADER_STAGE_COMPUTE) {
        header.workgroup_x = workgroup_x;
        header.workgroup_y = workgroup_y;
        header.workgroup_z = workgroup_z;
    }
    if (header.total_size > sizeof(candidate) ||
        rin_shader_capacity < header.total_size)
        goto translate_bounds;
    memcpy(candidate, &header, sizeof(header));
    memcpy(candidate + sizeof(header), builder.instructions,
           builder.count * sizeof(builder.instructions[0]));
    if (ringpu_shader_validate(candidate, header.total_size, &shader_info) !=
        RIN_SHADER_OK) {
        diag(info, "generated RSH1 did not pass the RinGPU validator");
        result = RIN_SPIRV_ERROR_RSH1;
        goto done;
    }
    memcpy(rin_shader_out, candidate, header.total_size);
    info->shader = shader_info;
    result = RIN_SPIRV_OK;
    goto done;
translate_unsupported:
    if (result == RIN_SPIRV_ERROR_MALFORMED)
        result = RIN_SPIRV_ERROR_UNSUPPORTED;
    goto done;
translate_bounds:
    diag(info, "SPIR-V translation exceeds the bounded RSH1 output");
    result = RIN_SPIRV_ERROR_BOUNDS;
done:
    if (result == RIN_SPIRV_ERROR_MALFORMED && info->diagnostic[0] == '\0')
        diag(info, "SPIR-V translation stopped at an unclassified malformed input");
    free(names);
    free(interface_ids);
    free(variables);
    free(values);
    free(decorations);
    free(types);
    return result;
}
