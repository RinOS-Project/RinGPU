/* SPDX-License-Identifier: MIT */
#include "ringpu/ringpu.h"

#define CHECK(expression) do { if (!(expression)) return 1; } while (0)

int main(void) {
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_OK) == RIN_GPU_OK);
    CHECK(rin_gpu_memory_result_to_gpu(
              RIN_GPU_MEMORY_INVALID_ARGUMENT) ==
          RIN_GPU_ERROR_INVALID_ARGUMENT);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_STATE) ==
          RIN_GPU_ERROR_STATE);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_BUSY) ==
          RIN_GPU_ERROR_BUSY);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_NO_SPACE) ==
          RIN_GPU_ERROR_NO_MEMORY);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_BACKEND_FAILED) ==
          RIN_GPU_ERROR_BACKEND);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_PROTOCOL) ==
          RIN_GPU_ERROR_PROTOCOL);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_LOST) ==
          RIN_GPU_ERROR_DEVICE_LOST);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_STALE) ==
          RIN_GPU_ERROR_INVALID_HANDLE);
    CHECK(rin_gpu_memory_result_to_gpu(RIN_GPU_MEMORY_LIMIT) ==
          RIN_GPU_ERROR_LIMIT);
    CHECK(rin_gpu_memory_result_to_gpu((RinGpuMemoryRuntimeResult)12345) ==
          RIN_GPU_ERROR_BACKEND);

#if RINGPU_ENABLE_PRESENTATION
    CHECK(rin_gpu_presentation_result_to_gpu(RIN_GPU_PRESENTATION_OK) ==
          RIN_GPU_OK);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_INVALID_ARGUMENT) ==
          RIN_GPU_ERROR_INVALID_ARGUMENT);
    CHECK(rin_gpu_presentation_result_to_gpu(RIN_GPU_PRESENTATION_STATE) ==
          RIN_GPU_ERROR_STATE);
    CHECK(rin_gpu_presentation_result_to_gpu(RIN_GPU_PRESENTATION_BUSY) ==
          RIN_GPU_ERROR_BUSY);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_BACKEND) == RIN_GPU_ERROR_BACKEND);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_DEVICE_LOST) ==
          RIN_GPU_ERROR_DEVICE_LOST);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_STALE) == RIN_GPU_ERROR_INVALID_HANDLE);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_LIMIT) == RIN_GPU_ERROR_LIMIT);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_UNSUPPORTED) == RIN_GPU_ERROR_UNSUPPORTED);
    CHECK(rin_gpu_presentation_result_to_gpu(
              RIN_GPU_PRESENTATION_TIMEOUT) == RIN_GPU_ERROR_TIMEOUT);
    CHECK(rin_gpu_presentation_result_to_gpu(
              (RinGpuPresentationResult)12345) == RIN_GPU_ERROR_BACKEND);
#endif
    return 0;
}
