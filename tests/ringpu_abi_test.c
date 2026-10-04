/* SPDX-License-Identifier: MIT */
#include <ringpu/ringpu.h>
#include <ringpu/rin_shader.h>
#include <ringpu/backend_command_v1.h>
#include <ringpu/platform.h>

#include <stddef.h>

_Static_assert(RIN_GPU_ABI_VERSION == 1u, "unexpected RinGPU ABI version");
_Static_assert(sizeof(RinShaderHeaderV1) == 64u, "RSH1 header drift");
_Static_assert(sizeof(RinShaderInstructionV1) == 16u, "RSH1 instruction drift");
_Static_assert(sizeof(RinShaderInfoV1) == 32u, "RSH1 info drift");
_Static_assert(sizeof(RinGpuMemoryAllocationDescV1) == 64u,
               "RinGPU public memory descriptor drift");
_Static_assert(sizeof(RinGpuMemoryBackendV1) == 192u,
               "RinGPU public memory backend drift");
_Static_assert(sizeof(RinGpuMemoryStatusV1) == 128u,
               "RinGPU public memory status drift");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(RinGpuPlatformThreadSchedulerV1) == 24u,
               "RinGPU platform scheduler ABI drift");
_Static_assert(sizeof(RinGpuPlatformServicesV1) == 48u,
               "RinGPU platform services ABI drift");
_Static_assert(sizeof(RinGpuPlatformServicesV2) == 64u,
               "RinGPU platform services V2 ABI drift");
_Static_assert(sizeof(RinGpuPlatformServicesV3) == 80u,
               "RinGPU platform services V3 ABI drift");
_Static_assert(sizeof(RinGpuPlatformBackendBridgeV1) == 56u,
               "RinGPU backend bridge ABI drift");
_Static_assert(sizeof(RinGpuBackendCommandV1) == 872u,
               "RinGPU canonical backend command ABI drift");
#else
_Static_assert(sizeof(RinGpuPlatformThreadSchedulerV1) == 16u,
               "RinGPU platform scheduler ABI drift");
_Static_assert(sizeof(RinGpuPlatformServicesV1) == 32u,
               "RinGPU platform services ABI drift");
_Static_assert(sizeof(RinGpuPlatformServicesV2) == 40u,
               "RinGPU platform services V2 ABI drift");
_Static_assert(sizeof(RinGpuPlatformServicesV3) == 56u,
               "RinGPU platform services V3 ABI drift");
_Static_assert(sizeof(RinGpuPlatformBackendBridgeV1) == 52u,
               "RinGPU backend bridge ABI drift");
_Static_assert(sizeof(RinGpuBackendCommandV1) == 872u,
               "RinGPU canonical backend command ABI drift");
#endif

int main(void) {
    return offsetof(RinGpuBufferDescV1, size_bytes) == 8u ? 0 : 1;
}
