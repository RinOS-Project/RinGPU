/* SPDX-License-Identifier: MIT */
#include <ringpu/rin_shader.h>

#include <stdint.h>
#include <string.h>

int main(void) {
    uint8_t malformed[sizeof(RinShaderHeaderV1)] = {0};
    struct {
        RinShaderHeaderV1 header;
        RinShaderInstructionV1 instructions[4];
    } atomic_shader;
    RinShaderInfoV1 info;
    memset(&atomic_shader, 0, sizeof(atomic_shader));
    memset(&info, 0, sizeof(info));
    if (ringpu_shader_validate(malformed, sizeof(malformed), &info) == RIN_SHADER_OK)
        return 1;
    atomic_shader.header.magic = RIN_SHADER_MAGIC;
    atomic_shader.header.version = RIN_SHADER_IR_VERSION;
    atomic_shader.header.header_size = sizeof(atomic_shader.header);
    atomic_shader.header.total_size = sizeof(atomic_shader);
    atomic_shader.header.stage = RIN_SHADER_STAGE_COMPUTE;
    atomic_shader.header.instruction_count = 4u;
    atomic_shader.header.register_count = 3u;
    atomic_shader.header.resource_count = 1u;
    atomic_shader.header.workgroup_x = 1u;
    atomic_shader.header.workgroup_y = 1u;
    atomic_shader.header.workgroup_z = 1u;
    atomic_shader.instructions[0].opcode = RIN_SHADER_OP_CONST_I32;
    atomic_shader.instructions[0].destination = 0u;
    atomic_shader.instructions[0].source0 = RIN_SHADER_UNUSED;
    atomic_shader.instructions[0].source1 = RIN_SHADER_UNUSED;
    atomic_shader.instructions[0].resource = RIN_SHADER_UNUSED;
    atomic_shader.instructions[1].opcode = RIN_SHADER_OP_CONST_I32;
    atomic_shader.instructions[1].destination = 1u;
    atomic_shader.instructions[1].immediate = 1u;
    atomic_shader.instructions[1].source0 = RIN_SHADER_UNUSED;
    atomic_shader.instructions[1].source1 = RIN_SHADER_UNUSED;
    atomic_shader.instructions[1].resource = RIN_SHADER_UNUSED;
    atomic_shader.instructions[2].opcode = RIN_SHADER_OP_ATOMIC_ADD_I32;
    atomic_shader.instructions[2].destination = 2u;
    atomic_shader.instructions[2].source0 = 0u;
    atomic_shader.instructions[2].source1 = 1u;
    atomic_shader.instructions[2].resource = 0u;
    atomic_shader.instructions[3].opcode = RIN_SHADER_OP_RETURN;
    atomic_shader.instructions[3].destination = RIN_SHADER_UNUSED;
    atomic_shader.instructions[3].source0 = RIN_SHADER_UNUSED;
    atomic_shader.instructions[3].source1 = RIN_SHADER_UNUSED;
    atomic_shader.instructions[3].resource = RIN_SHADER_UNUSED;
    {
        int atomic_result = ringpu_shader_validate(
            &atomic_shader, sizeof(atomic_shader), &info);
        if (atomic_result != RIN_SHADER_OK ||
            info.stage != RIN_SHADER_STAGE_COMPUTE || info.resource_count != 1u)
            return atomic_result == RIN_SHADER_OK ? 2 : 20 - atomic_result;
    }
    return 0;
}
