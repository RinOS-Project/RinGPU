/* SPDX-License-Identifier: MIT */
#include <ringpu/ringpu.h>
#include <ringpu/rin_shader.h>

#include <stddef.h>

_Static_assert(RIN_GPU_ABI_VERSION == 1u, "unexpected RinGPU ABI version");
_Static_assert(sizeof(RinShaderHeaderV1) == 64u, "RSH1 header drift");
_Static_assert(sizeof(RinShaderInstructionV1) == 16u, "RSH1 instruction drift");
_Static_assert(sizeof(RinShaderInfoV1) == 32u, "RSH1 info drift");

int main(void) {
    return offsetof(RinGpuBufferDescV1, size_bytes) == 8u ? 0 : 1;
}
