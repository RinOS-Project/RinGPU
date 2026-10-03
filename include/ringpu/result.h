/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_RESULT_H
#define RINGPU_PUBLIC_RESULT_H

/* Canonical result vocabulary shared by the RinGPU core, memory, and
 * presentation APIs. Domain-specific enums retain their detailed ABI values
 * and provide explicit conversion functions to this common result set. */
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
    RIN_GPU_ERROR_TIMEOUT = -13,
    RIN_GPU_ERROR_PROTOCOL = -14
} RinGpuResult;

#endif /* RINGPU_PUBLIC_RESULT_H */
