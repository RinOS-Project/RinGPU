/* SPDX-License-Identifier: MIT */
#include "differential.h"

#include <ringpu/runtime.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int present(void* context,
                   const RinGpuSoftwarePresentedImageV1* image)
{
    (void)context;
    return image != NULL && image->pixels != NULL ? RIN_GPU_OK
                                                   : RIN_GPU_ERROR_INVALID_ARGUMENT;
}

static int acquire(void* context, const RinGpuImageDescV1* descriptor,
                   uint64_t allocation_bytes,
                   RinGpuSoftwareExternalImageV1* storage)
{
    (void)context;
    (void)descriptor;
    (void)allocation_bytes;
    if (storage == NULL) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(storage, 0, sizeof(*storage));
    return RIN_GPU_OK;
}

static int make_runtime(RinGpuRuntime** runtime_out)
{
    RinGpuRuntimeSoftwareSurfaceDescV1 descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.struct_size = sizeof(descriptor);
    descriptor.version = RIN_GPU_RUNTIME_VERSION;
    descriptor.device_generation = 1u;
    descriptor.handle_secret = UINT64_C(0x4459464645523031);
    descriptor.max_buffer_size = 1024u * 1024u;
    descriptor.max_image_size = 1024u * 1024u;
    descriptor.max_total_allocation_size = 4u * 1024u * 1024u;
    descriptor.max_image_dimension = 64u;
    descriptor.max_image_layers = 1u;
    descriptor.max_image_mip_levels = 1u;
    descriptor.max_image_sample_count = 1u;
    descriptor.adapter.abi_version = RIN_GPU_ABI_VERSION;
    descriptor.adapter.struct_size = sizeof(descriptor.adapter);
    descriptor.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY;
    memcpy(descriptor.adapter.name, "determinism", 12u);
    descriptor.display.abi_version = RIN_GPU_ABI_VERSION;
    descriptor.display.struct_size = sizeof(descriptor.display);
    descriptor.display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    descriptor.display.flags = RIN_GPU_DISPLAY_CONNECTED |
                               RIN_GPU_DISPLAY_PRIMARY;
    descriptor.display.width = 1u;
    descriptor.display.height = 1u;
    descriptor.display.refresh_millihertz = 60000u;
    descriptor.display.format = RIN_GPU_FORMAT_RGBA8_UNORM;
    descriptor.display.physical_width_mm = 1u;
    descriptor.display.physical_height_mm = 1u;
    descriptor.display.scale_milli = 1000u;
    memcpy(descriptor.display.name, "determinism", 12u);
    descriptor.present_callback = present;
    descriptor.acquire_image = acquire;
    return ringpu_runtime_software_surface_create(&descriptor, runtime_out);
}

static int run_workload(RinGpuRuntime* runtime, uint8_t output[16u])
{
    RinGpuQueueDescV1 queue_descriptor;
    RinGpuCommandListDescV1 command_descriptor;
    RinGpuBufferDescV1 buffer_descriptor;
    RinGpuBufferClearV1 clear;
    RinGpuSubmitInfoV1 submit;
    RinGpuHandle queue = 0u;
    RinGpuHandle command_list = 0u;
    RinGpuHandle fence = 0u;
    RinGpuHandle buffer = 0u;
    static const uint8_t initial[16u] = {0u};
    int result;

    memset(&queue_descriptor, 0, sizeof(queue_descriptor));
    queue_descriptor.abi_version = RIN_GPU_ABI_VERSION;
    queue_descriptor.struct_size = sizeof(queue_descriptor);
    queue_descriptor.capabilities = RIN_GPU_QUEUE_COPY;
    result = ringpu_runtime_create_queue(runtime, &queue_descriptor, &queue);
    if (result != RIN_GPU_OK) return result;

    memset(&command_descriptor, 0, sizeof(command_descriptor));
    command_descriptor.abi_version = RIN_GPU_ABI_VERSION;
    command_descriptor.struct_size = sizeof(command_descriptor);
    command_descriptor.capabilities = RIN_GPU_QUEUE_COPY;
    result = ringpu_runtime_create_command_list(runtime, &command_descriptor,
                                                &command_list);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_runtime_create_fence(runtime, 0u, &fence);
    if (result != RIN_GPU_OK) return result;

    memset(&buffer_descriptor, 0, sizeof(buffer_descriptor));
    buffer_descriptor.abi_version = RIN_GPU_ABI_VERSION;
    buffer_descriptor.struct_size = sizeof(buffer_descriptor);
    buffer_descriptor.size_bytes = 16u;
    buffer_descriptor.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                              RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_descriptor.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    result = ringpu_runtime_create_buffer(runtime, &buffer_descriptor, &buffer);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_runtime_upload_buffer(runtime, buffer, 0u, initial,
                                          sizeof(initial));
    if (result != RIN_GPU_OK) return result;

    memset(&clear, 0, sizeof(clear));
    clear.abi_version = RIN_GPU_ABI_VERSION;
    clear.struct_size = sizeof(clear);
    clear.size_bytes = 16u;
    clear.pattern = UINT32_C(0xa55a3cc3);
    result = ringpu_runtime_command_clear_buffer(runtime, command_list, buffer,
                                                 &clear);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_runtime_command_list_close(runtime, command_list);
    if (result != RIN_GPU_OK) return result;

    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    submit.signal_fence = fence;
    submit.signal_value = 1u;
    result = ringpu_runtime_queue_submit(runtime, queue, &submit);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_runtime_wait_fence(runtime, fence, 1u,
                                       RIN_GPU_TIMEOUT_INFINITE);
    if (result != RIN_GPU_OK) return result;
    return ringpu_runtime_readback_buffer(runtime, buffer, 0u, output, 16u);
}

int main(void)
{
    RinGpuRuntime* first = NULL;
    RinGpuRuntime* second = NULL;
    RinGpuDifferentialReportV1 report;
    uint8_t first_output[16u];
    uint8_t second_output[16u];
    int result;

    memset(first_output, 0, sizeof(first_output));
    memset(second_output, 0, sizeof(second_output));
    result = make_runtime(&first);
    if (result != RIN_GPU_OK) {
        fprintf(stderr, "first runtime failed: %d\n", result);
        return 1;
    }
    result = make_runtime(&second);
    if (result != RIN_GPU_OK) {
        fprintf(stderr, "second runtime failed: %d\n", result);
        ringpu_runtime_destroy(first);
        return 2;
    }
    result = run_workload(first, first_output);
    if (result != RIN_GPU_OK)
        fprintf(stderr, "first workload failed: %d\n", result);
    if (result == RIN_GPU_OK)
        result = run_workload(second, second_output);
    if (result != RIN_GPU_OK)
        fprintf(stderr, "second workload failed: %d\n", result);
    ringpu_runtime_destroy(first);
    ringpu_runtime_destroy(second);
    if (result != RIN_GPU_OK) return 3;
    if (rin_gpu_differential_report_init(&report) !=
        RIN_GPU_DIFFERENTIAL_MATCH) {
        fprintf(stderr, "report init failed\n");
        return 4;
    }
    if (rin_gpu_differential_compare_bytes(
            RIN_GPU_DIFFERENTIAL_READBACK, first_output, second_output,
            sizeof(first_output), 0u, &report) != RIN_GPU_DIFFERENTIAL_MATCH)
    {
        fprintf(stderr, "readback mismatch: flags=%u values=%llu/%llu\n",
                report.mismatch_flags,
                (unsigned long long)report.compared_values,
                (unsigned long long)report.mismatched_values);
        return 5;
    }
    return 0;
}
