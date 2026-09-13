# RSH1 public format

RSH1 is the little-endian `RIN_SHADER_MAGIC` binary intermediate format. A
fixed 64-byte `RinShaderHeaderV1` is followed by exactly
`instruction_count * sizeof(RinShaderInstructionV1)` bytes. The validator
checks stage, register and resource limits, instruction operands, control-flow
targets, initialized-register dataflow, finite constants, and the required
terminal return before any module is published.

The opcode, register, resource-binding, and stage enumerations are defined in
`include/ringpu/rin_shader.h`. The same header is consumed by RinGL and the
software backend; there is no second private shader contract.
