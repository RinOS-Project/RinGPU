/* SPDX-License-Identifier: MIT */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <ringpu/runtime.h>
#include <ringpu/rin_shader.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#define BENCHMARK_BUFFER_BYTES 4096u
#define BENCHMARK_READBACK_BYTES BENCHMARK_BUFFER_BYTES
#define FRAME_WIDTH 32u
#define FRAME_HEIGHT 32u
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 4u)
#define DEFAULT_ITERATIONS 1000u
#define DEFAULT_WARMUP 100u
#define MAX_ITERATIONS 1000000u

typedef struct BenchmarkRuntime {
    RinGpuRuntime* runtime;
    RinGpuHandle queue;
    RinGpuHandle fence;
    RinGpuHandle buffer;
    RinGpuHandle descriptor_buffer;
    RinGpuHandle descriptor_shader;
    RinGpuHandle descriptor_pipeline;
    uint64_t submission_value;
    RinGpuRuntime* frame_runtime;
    RinGpuHandle frame_queue;
    RinGpuHandle frame_fence;
    RinGpuHandle frame_command_list;
    RinGpuHandle frame_image;
    uint32_t presented_frames;
    uint64_t presented_hash;
    uint8_t frame_storage[FRAME_BYTES];
    uint64_t timed_phase_ns;
} BenchmarkRuntime;

static int parse_count(const char* text, uint32_t* count_out)
{
    char* end = NULL;
    unsigned long value;
    if (text == NULL || count_out == NULL || text[0] == '\0' ||
        text[0] == '-')
        return 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > MAX_ITERATIONS)
        return 0;
    *count_out = (uint32_t)value;
    return 1;
}

static int parse_arguments(int argc, char** argv, uint32_t* iterations_out,
                           uint32_t* warmup_out)
{
    int index;
    *iterations_out = DEFAULT_ITERATIONS;
    *warmup_out = DEFAULT_WARMUP;
    for (index = 1; index < argc; ++index) {
        uint32_t value;
        if (index + 1 >= argc || !parse_count(argv[index + 1], &value))
            return 0;
        if (strcmp(argv[index], "--iterations") == 0) {
            if (value == 0u) return 0;
            *iterations_out = value;
        } else if (strcmp(argv[index], "--warmup") == 0) {
            *warmup_out = value;
        } else {
            return 0;
        }
        ++index;
    }
    return 1;
}

static int timer_now_ns(uint64_t* nanoseconds_out)
{
    if (nanoseconds_out == NULL) return 0;
#if defined(_WIN32)
    {
        LARGE_INTEGER counter;
        LARGE_INTEGER frequency;
        long double value;
        if (!QueryPerformanceFrequency(&frequency) ||
            !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0 ||
            counter.QuadPart < 0)
            return 0;
        value = ((long double)counter.QuadPart * 1000000000.0L) /
                (long double)frequency.QuadPart;
        if (value > (long double)UINT64_MAX) return 0;
        *nanoseconds_out = (uint64_t)value;
        return 1;
    }
#else
    {
        struct timespec value;
        if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0)
            return 0;
        *nanoseconds_out = (uint64_t)value.tv_sec * UINT64_C(1000000000) +
                           (uint64_t)value.tv_nsec;
        return 1;
    }
#endif
}

static const char* compiler_name(void)
{
#if defined(_MSC_VER)
    return "MSVC";
#elif defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "C11";
#endif
}

static int benchmark_present_frame(
    void* context, const RinGpuSoftwarePresentedImageV1* image)
{
    BenchmarkRuntime* state = (BenchmarkRuntime*)context;
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t row;
    if (state == NULL || image == NULL || image->pixels == NULL ||
        image->struct_size != sizeof(*image) ||
        image->version != RIN_GPU_SOFTWARE_BACKEND_VERSION ||
        image->format != RIN_GPU_FORMAT_BGRA8_UNORM ||
        image->width != FRAME_WIDTH || image->height != FRAME_HEIGHT ||
        image->row_pitch_bytes < (uint64_t)FRAME_WIDTH * 4u ||
        image->size_bytes < image->row_pitch_bytes * (FRAME_HEIGHT - 1u) +
                                (uint64_t)FRAME_WIDTH * 4u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    for (row = 0u; row < FRAME_HEIGHT; ++row) {
        const uint8_t* pixels = image->pixels + row * image->row_pitch_bytes;
        uint32_t column;
        for (column = 0u; column < FRAME_WIDTH; ++column) {
            const uint8_t* pixel = pixels + column * 4u;
            if (pixel[0] != 0u || pixel[1] != 0u || pixel[2] != 255u ||
                pixel[3] != 255u)
                return RIN_GPU_ERROR_STATE;
            hash = (hash ^ pixel[0]) * UINT64_C(1099511628211);
            hash = (hash ^ pixel[1]) * UINT64_C(1099511628211);
            hash = (hash ^ pixel[2]) * UINT64_C(1099511628211);
            hash = (hash ^ pixel[3]) * UINT64_C(1099511628211);
        }
    }
    state->presented_hash = hash;
    ++state->presented_frames;
    return RIN_GPU_OK;
}

static int benchmark_acquire_image(
    void* context, const RinGpuImageDescV1* descriptor,
    uint64_t allocation_bytes, RinGpuSoftwareExternalImageV1* storage_out)
{
    BenchmarkRuntime* state = (BenchmarkRuntime*)context;
    if (state == NULL || descriptor == NULL || storage_out == NULL ||
        descriptor->format != RIN_GPU_FORMAT_BGRA8_UNORM ||
        descriptor->width != FRAME_WIDTH ||
        descriptor->height != FRAME_HEIGHT ||
        allocation_bytes > sizeof(state->frame_storage))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(storage_out, 0, sizeof(*storage_out));
    storage_out->struct_size = sizeof(*storage_out);
    storage_out->version = RIN_GPU_SOFTWARE_EXTERNAL_IMAGE_VERSION;
    storage_out->pixels = state->frame_storage;
    storage_out->size_bytes = sizeof(state->frame_storage);
    storage_out->row_pitch_bytes = (uint64_t)FRAME_WIDTH * 4u;
    return RIN_GPU_OK;
}

static int create_frame_runtime(BenchmarkRuntime* state)
{
    RinGpuRuntimeDescV1 runtime_desc;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuImageDescV1 image_desc;
    int result;

    memset(&runtime_desc, 0, sizeof(runtime_desc));
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.version = RIN_GPU_RUNTIME_VERSION;
    runtime_desc.device_generation = 1u;
    runtime_desc.handle_secret = UINT64_C(0x4652414d4542454e);
    runtime_desc.max_buffer_size = BENCHMARK_BUFFER_BYTES;
    runtime_desc.max_image_size = FRAME_BYTES;
    runtime_desc.max_total_allocation_size = UINT64_C(16) * 1024u * 1024u;
    runtime_desc.max_image_dimension = 64u;
    runtime_desc.max_image_layers = 1u;
    runtime_desc.max_image_mip_levels = 1u;
    runtime_desc.max_image_sample_count = 1u;
    runtime_desc.adapter.abi_version = RIN_GPU_ABI_VERSION;
    runtime_desc.adapter.struct_size = sizeof(runtime_desc.adapter);
    runtime_desc.adapter.queue_capabilities = RIN_GPU_QUEUE_GRAPHICS |
                                              RIN_GPU_QUEUE_PRESENT;
    memcpy(runtime_desc.adapter.name, "software-frame-benchmark",
           sizeof("software-frame-benchmark"));
    runtime_desc.display.abi_version = RIN_GPU_ABI_VERSION;
    runtime_desc.display.struct_size = sizeof(runtime_desc.display);
    runtime_desc.display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    runtime_desc.display.flags = RIN_GPU_DISPLAY_CONNECTED |
                                 RIN_GPU_DISPLAY_PRIMARY;
    runtime_desc.display.width = FRAME_WIDTH;
    runtime_desc.display.height = FRAME_HEIGHT;
    runtime_desc.display.refresh_millihertz = 60000u;
    runtime_desc.display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    runtime_desc.display.physical_width_mm = 1u;
    runtime_desc.display.physical_height_mm = 1u;
    runtime_desc.display.scale_milli = 1000u;
    memcpy(runtime_desc.display.name, "software-frame",
           sizeof("software-frame"));
    runtime_desc.present_callback = benchmark_present_frame;
    runtime_desc.present_context = state;
    runtime_desc.acquire_image = benchmark_acquire_image;
    runtime_desc.image_context = state;
    result = ringpu_runtime_create(&runtime_desc, &state->frame_runtime);
    if (result != RIN_GPU_OK) return result;

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_GRAPHICS |
                              RIN_GPU_QUEUE_PRESENT;
    result = ringpu_runtime_create_queue(state->frame_runtime, &queue_desc,
                                         &state->frame_queue);
    if (result != RIN_GPU_OK) return result;

    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = queue_desc.capabilities;
    result = ringpu_runtime_create_command_list(
        state->frame_runtime, &command_desc, &state->frame_command_list);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_runtime_create_fence(state->frame_runtime, 0u,
                                         &state->frame_fence);
    if (result != RIN_GPU_OK) return result;

    memset(&image_desc, 0, sizeof(image_desc));
    image_desc.abi_version = RIN_GPU_ABI_VERSION;
    image_desc.struct_size = sizeof(image_desc);
    image_desc.dimension = RIN_GPU_IMAGE_DIMENSION_2D;
    image_desc.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    image_desc.width = FRAME_WIDTH;
    image_desc.height = FRAME_HEIGHT;
    image_desc.depth = 1u;
    image_desc.array_layers = 1u;
    image_desc.mip_levels = 1u;
    image_desc.sample_count = 1u;
    image_desc.usage = RIN_GPU_IMAGE_COLOR_TARGET | RIN_GPU_IMAGE_PRESENT;
    return ringpu_runtime_create_image(state->frame_runtime, &image_desc,
                                       &state->frame_image);
}

static void set_shader_instruction(RinShaderInstructionV1* instruction,
                                   uint16_t opcode, uint16_t destination,
                                   uint16_t source0, uint16_t source1,
                                   uint16_t resource, uint32_t immediate)
{
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->destination = destination;
    instruction->source0 = source0;
    instruction->source1 = source1;
    instruction->resource = resource;
    instruction->immediate = immediate;
}

static int create_descriptor_resources(BenchmarkRuntime* state)
{
    struct DescriptorShader {
        RinShaderHeaderV1 header;
        RinShaderInstructionV1 instructions[4];
    } shader;
    RinGpuComputePipelineDescV1 pipeline_desc;
    RinGpuBufferDescV1 buffer_desc;
    const uint32_t initial_value = 0u;
    int result;

    memset(&shader, 0, sizeof(shader));
    shader.header.magic = RIN_SHADER_MAGIC;
    shader.header.version = RIN_SHADER_IR_VERSION;
    shader.header.header_size = sizeof(shader.header);
    shader.header.total_size = sizeof(shader);
    shader.header.stage = RIN_SHADER_STAGE_COMPUTE;
    shader.header.instruction_count = 4u;
    shader.header.register_count = 2u;
    shader.header.resource_count = 1u;
    shader.header.workgroup_x = 1u;
    shader.header.workgroup_y = 1u;
    shader.header.workgroup_z = 1u;
    set_shader_instruction(&shader.instructions[0], RIN_SHADER_OP_CONST_I32,
                           0u, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                           RIN_SHADER_UNUSED, 0u);
    set_shader_instruction(&shader.instructions[1], RIN_SHADER_OP_CONST_I32,
                           1u, RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                           RIN_SHADER_UNUSED, 1u);
    set_shader_instruction(&shader.instructions[2],
                           RIN_SHADER_OP_STORE_RESOURCE_I32,
                           RIN_SHADER_UNUSED, 0u, 1u, 0u, 0u);
    set_shader_instruction(&shader.instructions[3], RIN_SHADER_OP_RETURN,
                           RIN_SHADER_UNUSED, RIN_SHADER_UNUSED,
                           RIN_SHADER_UNUSED, RIN_SHADER_UNUSED, 0u);
    result = ringpu_runtime_create_shader_module(
        state->runtime, &shader, sizeof(shader), &state->descriptor_shader);
    if (result != RIN_GPU_OK) return result;

    memset(&pipeline_desc, 0, sizeof(pipeline_desc));
    pipeline_desc.abi_version = RIN_GPU_ABI_VERSION;
    pipeline_desc.struct_size = sizeof(pipeline_desc);
    pipeline_desc.shader_module = state->descriptor_shader;
    result = ringpu_runtime_create_compute_pipeline(
        state->runtime, &pipeline_desc, &state->descriptor_pipeline);
    if (result != RIN_GPU_OK) return result;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = sizeof(initial_value);
    buffer_desc.usage = RIN_GPU_BUFFER_STORAGE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    result = ringpu_runtime_create_buffer(state->runtime, &buffer_desc,
                                          &state->descriptor_buffer);
    if (result != RIN_GPU_OK) return result;
    return ringpu_runtime_upload_buffer(
        state->runtime, state->descriptor_buffer, 0u, &initial_value,
        sizeof(initial_value));
}

static int create_benchmark_runtime(BenchmarkRuntime* state)
{
    RinGpuRuntimeDescV1 runtime_desc;
    RinGpuQueueDescV1 queue_desc;
    RinGpuBufferDescV1 buffer_desc;
    static const uint8_t zeroes[BENCHMARK_BUFFER_BYTES] = {0u};
    int result;

    memset(state, 0, sizeof(*state));
    memset(&runtime_desc, 0, sizeof(runtime_desc));
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.version = RIN_GPU_RUNTIME_VERSION;
    runtime_desc.device_generation = 1u;
    runtime_desc.handle_secret = UINT64_C(0x42454e43484d4152);
    runtime_desc.max_buffer_size = BENCHMARK_BUFFER_BYTES;
    runtime_desc.max_image_size = BENCHMARK_BUFFER_BYTES;
    runtime_desc.max_total_allocation_size = UINT64_C(16) * 1024u * 1024u;
    runtime_desc.max_image_dimension = 64u;
    runtime_desc.max_image_layers = 1u;
    runtime_desc.max_image_mip_levels = 1u;
    runtime_desc.max_image_sample_count = 1u;
    runtime_desc.adapter.abi_version = RIN_GPU_ABI_VERSION;
    runtime_desc.adapter.struct_size = sizeof(runtime_desc.adapter);
    runtime_desc.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY |
                                              RIN_GPU_QUEUE_COMPUTE;
    memcpy(runtime_desc.adapter.name, "host-software-benchmark",
           sizeof("host-software-benchmark"));
    runtime_desc.flags = RIN_GPU_RUNTIME_FLAG_HEADLESS;
    result = ringpu_runtime_create(&runtime_desc, &state->runtime);
    if (result != RIN_GPU_OK) return result;

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    result = ringpu_runtime_create_queue(state->runtime, &queue_desc,
                                         &state->queue);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_runtime_create_fence(state->runtime, 0u, &state->fence);
    if (result != RIN_GPU_OK) return result;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = BENCHMARK_BUFFER_BYTES;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    result = ringpu_runtime_create_buffer(state->runtime, &buffer_desc,
                                          &state->buffer);
    if (result != RIN_GPU_OK) return result;
    return ringpu_runtime_upload_buffer(state->runtime, state->buffer, 0u,
                                        zeroes, sizeof(zeroes));
}

static int destroy_benchmark_runtime(BenchmarkRuntime* state)
{
    int first_error = RIN_GPU_OK;
    int result;
    if (state->frame_runtime != NULL && state->frame_command_list != 0u) {
        result = ringpu_runtime_destroy_object(state->frame_runtime,
                                              state->frame_command_list);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->frame_runtime != NULL && state->frame_image != 0u) {
        result = ringpu_runtime_destroy_object(state->frame_runtime,
                                              state->frame_image);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->frame_runtime != NULL && state->frame_fence != 0u) {
        result = ringpu_runtime_destroy_object(state->frame_runtime,
                                              state->frame_fence);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->frame_runtime != NULL && state->frame_queue != 0u) {
        result = ringpu_runtime_destroy_object(state->frame_runtime,
                                              state->frame_queue);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->frame_runtime != NULL)
        ringpu_runtime_destroy(state->frame_runtime);
    if (state->runtime != NULL && state->descriptor_buffer != 0u) {
        result = ringpu_runtime_destroy_object(state->runtime,
                                              state->descriptor_buffer);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->runtime != NULL && state->descriptor_pipeline != 0u) {
        result = ringpu_runtime_destroy_object(state->runtime,
                                              state->descriptor_pipeline);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->runtime != NULL && state->descriptor_shader != 0u) {
        result = ringpu_runtime_destroy_object(state->runtime,
                                              state->descriptor_shader);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->runtime != NULL && state->buffer != 0u) {
        result = ringpu_runtime_destroy_object(state->runtime, state->buffer);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->runtime != NULL && state->fence != 0u) {
        result = ringpu_runtime_destroy_object(state->runtime, state->fence);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->runtime != NULL && state->queue != 0u) {
        result = ringpu_runtime_destroy_object(state->runtime, state->queue);
        if (first_error == RIN_GPU_OK && result != RIN_GPU_OK)
            first_error = result;
    }
    if (state->runtime != NULL) ringpu_runtime_destroy(state->runtime);
    memset(state, 0, sizeof(*state));
    return first_error;
}

static int run_iteration(BenchmarkRuntime* state, uint64_t iteration,
                         uint64_t* checksum_out)
{
    RinGpuCommandListDescV1 list_desc;
    RinGpuBufferClearV1 clear;
    RinGpuSubmitInfoV1 submit;
    RinGpuHandle command_list = 0u;
    uint8_t readback[BENCHMARK_READBACK_BYTES];
    uint8_t expected[sizeof(clear.pattern)];
    uint64_t signal_value;
    uint64_t index;
    int result;
    int cleanup_result;

    memset(&list_desc, 0, sizeof(list_desc));
    list_desc.abi_version = RIN_GPU_ABI_VERSION;
    list_desc.struct_size = sizeof(list_desc);
    list_desc.capabilities = RIN_GPU_QUEUE_COPY;
    result = ringpu_runtime_create_command_list(state->runtime, &list_desc,
                                                &command_list);
    if (result != RIN_GPU_OK) return result;

    memset(&clear, 0, sizeof(clear));
    clear.abi_version = RIN_GPU_ABI_VERSION;
    clear.struct_size = sizeof(clear);
    clear.size_bytes = BENCHMARK_BUFFER_BYTES;
    clear.pattern = UINT32_C(0xa55a3cc3) +
                    (uint32_t)iteration * UINT32_C(0x9e3779b9);
    result = ringpu_runtime_command_clear_buffer(state->runtime, command_list,
                                                 state->buffer, &clear);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_command_list_close(state->runtime,
                                                   command_list);
    if (result == RIN_GPU_OK) {
        memset(&submit, 0, sizeof(submit));
        submit.abi_version = RIN_GPU_ABI_VERSION;
        submit.struct_size = sizeof(submit);
        submit.command_list = command_list;
        submit.signal_fence = state->fence;
        signal_value = state->submission_value + 1u;
        submit.signal_value = signal_value;
        result = ringpu_runtime_queue_submit(state->runtime, state->queue,
                                             &submit);
        if (result == RIN_GPU_OK) state->submission_value = signal_value;
    }
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_wait_fence(state->runtime, state->fence,
                                           signal_value,
                                           RIN_GPU_TIMEOUT_INFINITE);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_readback_buffer(
            state->runtime, state->buffer, 0u, readback, sizeof(readback));
    memcpy(expected, &clear.pattern, sizeof(expected));
    if (result == RIN_GPU_OK) {
        for (index = 0u; index < sizeof(readback); ++index) {
            if (readback[index] != expected[index % sizeof(expected)]) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
        }
    }
    if (result == RIN_GPU_OK) {
        *checksum_out = (*checksum_out * UINT64_C(1099511628211)) ^
                        (uint64_t)clear.pattern;
    }
    cleanup_result = ringpu_runtime_destroy_object(state->runtime,
                                                   command_list);
    if (result == RIN_GPU_OK && cleanup_result != RIN_GPU_OK)
        result = cleanup_result;
    return result;
}

static int run_command_record_iteration(BenchmarkRuntime* state,
                                        uint64_t iteration,
                                        uint64_t* checksum_out)
{
    RinGpuCommandListDescV1 list_desc;
    RinGpuBufferClearV1 clear;
    RinGpuHandle command_list = 0u;
    int result;
    int cleanup_result;

    memset(&list_desc, 0, sizeof(list_desc));
    list_desc.abi_version = RIN_GPU_ABI_VERSION;
    list_desc.struct_size = sizeof(list_desc);
    list_desc.capabilities = RIN_GPU_QUEUE_COPY;
    result = ringpu_runtime_create_command_list(state->runtime, &list_desc,
                                                &command_list);
    if (result != RIN_GPU_OK) return result;
    memset(&clear, 0, sizeof(clear));
    clear.abi_version = RIN_GPU_ABI_VERSION;
    clear.struct_size = sizeof(clear);
    clear.size_bytes = BENCHMARK_BUFFER_BYTES;
    clear.pattern = UINT32_C(0x7f4a7c15) +
                    (uint32_t)iteration * UINT32_C(0x9e3779b9);
    result = ringpu_runtime_command_clear_buffer(state->runtime, command_list,
                                                 state->buffer, &clear);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_command_list_close(state->runtime,
                                                   command_list);
    if (result == RIN_GPU_OK)
        *checksum_out = (*checksum_out * UINT64_C(1099511628211)) ^
                        clear.pattern;
    cleanup_result = ringpu_runtime_destroy_object(state->runtime,
                                                   command_list);
    if (result == RIN_GPU_OK && cleanup_result != RIN_GPU_OK)
        result = cleanup_result;
    return result;
}

static int run_submit_wait_iteration(BenchmarkRuntime* state,
                                     uint64_t iteration,
                                     uint64_t* checksum_out)
{
    RinGpuCommandListDescV1 list_desc;
    RinGpuBufferClearV1 clear;
    RinGpuSubmitInfoV1 submit;
    RinGpuHandle command_list = 0u;
    uint8_t readback[BENCHMARK_READBACK_BYTES];
    uint8_t expected[sizeof(clear.pattern)];
    uint64_t signal_value;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t index;
    int result;
    int cleanup_result;

    state->timed_phase_ns = 0u;
    memset(&list_desc, 0, sizeof(list_desc));
    list_desc.abi_version = RIN_GPU_ABI_VERSION;
    list_desc.struct_size = sizeof(list_desc);
    list_desc.capabilities = RIN_GPU_QUEUE_COPY;
    result = ringpu_runtime_create_command_list(state->runtime, &list_desc,
                                                &command_list);
    if (result != RIN_GPU_OK) return result;
    memset(&clear, 0, sizeof(clear));
    clear.abi_version = RIN_GPU_ABI_VERSION;
    clear.struct_size = sizeof(clear);
    clear.size_bytes = BENCHMARK_BUFFER_BYTES;
    clear.pattern = UINT32_C(0x243f6a88) ^
                    (uint32_t)iteration * UINT32_C(0x9e3779b9);
    result = ringpu_runtime_command_clear_buffer(state->runtime, command_list,
                                                 state->buffer, &clear);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_command_list_close(state->runtime,
                                                   command_list);
    if (result != RIN_GPU_OK) goto cleanup;

    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = command_list;
    submit.signal_fence = state->fence;
    signal_value = state->submission_value + 1u;
    submit.signal_value = signal_value;
    if (!timer_now_ns(&start_ns)) {
        result = RIN_GPU_ERROR_STATE;
        goto cleanup;
    }
    result = ringpu_runtime_queue_submit(state->runtime, state->queue, &submit);
    if (result == RIN_GPU_OK) state->submission_value = signal_value;
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_wait_fence(state->runtime, state->fence,
                                           signal_value,
                                           RIN_GPU_TIMEOUT_INFINITE);
    if (!timer_now_ns(&end_ns) || end_ns < start_ns) {
        result = RIN_GPU_ERROR_STATE;
        goto cleanup;
    }
    if (result != RIN_GPU_OK) goto cleanup;
    state->timed_phase_ns = end_ns - start_ns;

    result = ringpu_runtime_readback_buffer(state->runtime, state->buffer, 0u,
                                            readback, sizeof(readback));
    memcpy(expected, &clear.pattern, sizeof(expected));
    if (result == RIN_GPU_OK) {
        for (index = 0u; index < sizeof(readback); ++index) {
            if (readback[index] != expected[index % sizeof(expected)]) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
        }
    }
    if (result == RIN_GPU_OK)
        *checksum_out = (*checksum_out * UINT64_C(1099511628211)) ^
                        clear.pattern;

cleanup:
    cleanup_result = ringpu_runtime_destroy_object(state->runtime,
                                                   command_list);
    if (result == RIN_GPU_OK && cleanup_result != RIN_GPU_OK)
        result = cleanup_result;
    return result;
}

static int run_memory_iteration(BenchmarkRuntime* state, uint64_t iteration,
                               uint64_t* checksum_out)
{
    RinGpuMemoryDescV1 memory_desc;
    RinGpuBufferDescV1 buffer_desc;
    RinGpuResourceMemoryBindingV1 binding;
    RinGpuHandle memory = 0u;
    RinGpuHandle buffer = 0u;
    uint8_t source[BENCHMARK_BUFFER_BYTES];
    uint8_t readback[BENCHMARK_BUFFER_BYTES];
    uint8_t pattern_bytes[sizeof(uint32_t)];
    const uint32_t pattern = UINT32_C(0x1f2e3d4c) ^
                             (uint32_t)iteration * UINT32_C(0x9e3779b9);
    uint64_t index;
    int result;
    int cleanup_result;

    memcpy(pattern_bytes, &pattern, sizeof(pattern_bytes));
    for (index = 0u; index < sizeof(source); ++index)
        source[index] = pattern_bytes[index % sizeof(pattern_bytes)];

    memset(&memory_desc, 0, sizeof(memory_desc));
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = BENCHMARK_BUFFER_BYTES;
    memory_desc.alignment = 256u;
    memory_desc.flags = RIN_GPU_MEMORY_BINDING_DEDICATED;
    result = ringpu_runtime_create_memory(state->runtime, &memory_desc,
                                          &memory);
    if (result != RIN_GPU_OK) goto cleanup;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = BENCHMARK_BUFFER_BYTES;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    result = ringpu_runtime_create_buffer(state->runtime, &buffer_desc,
                                          &buffer);
    if (result != RIN_GPU_OK) goto cleanup;

    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.memory = memory;
    binding.size_bytes = BENCHMARK_BUFFER_BYTES;
    result = ringpu_runtime_bind_buffer_memory(state->runtime, buffer,
                                               &binding);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_upload_buffer(state->runtime, buffer, 0u,
                                              source, sizeof(source));
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_readback_buffer(state->runtime, buffer, 0u,
                                                readback, sizeof(readback));
    if (result == RIN_GPU_OK && memcmp(source, readback, sizeof(source)) != 0)
        result = RIN_GPU_ERROR_STATE;
    if (result == RIN_GPU_OK)
        *checksum_out = (*checksum_out * UINT64_C(1099511628211)) ^ pattern;

cleanup:
    if (buffer != 0u) {
        cleanup_result = ringpu_runtime_destroy_object(state->runtime, buffer);
        if (result == RIN_GPU_OK && cleanup_result != RIN_GPU_OK)
            result = cleanup_result;
    }
    if (memory != 0u) {
        cleanup_result = ringpu_runtime_destroy_object(state->runtime, memory);
        if (result == RIN_GPU_OK && cleanup_result != RIN_GPU_OK)
            result = cleanup_result;
    }
    return result;
}

static int run_descriptor_iteration(BenchmarkRuntime* state,
                                    uint64_t iteration,
                                    uint64_t* checksum_out)
{
    RinGpuBufferBindingV1 binding;
    RinGpuHandle bind_group = 0u;
    int result;
    int cleanup_result;

    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.binding = 0u;
    binding.access = RIN_GPU_RESOURCE_WRITE;
    binding.buffer = state->descriptor_buffer;
    binding.size_bytes = sizeof(uint32_t);
    result = ringpu_runtime_create_compute_bind_group(
        state->runtime, state->descriptor_pipeline, &binding, 1u, &bind_group);
    if (result != RIN_GPU_OK) return result;
    *checksum_out = (*checksum_out * UINT64_C(1099511628211)) ^
                    (iteration + binding.size_bytes);
    cleanup_result = ringpu_runtime_destroy_object(state->runtime, bind_group);
    return cleanup_result;
}

static int run_frame_iteration(BenchmarkRuntime* state, uint64_t iteration,
                               uint64_t* checksum_out)
{
    RinGpuImageTransitionV1 transition;
    RinGpuRenderPassDescV1 render_pass;
    RinGpuPresentV1 present;
    RinGpuSubmitInfoV1 submit;
    const uint32_t previous_frame_count = state->presented_frames;
    int result;

    if (iteration > 1u) {
        result = ringpu_runtime_command_list_reset(
            state->frame_runtime, state->frame_command_list);
        if (result != RIN_GPU_OK) return result;
    }
    memset(&transition, 0, sizeof(transition));
    transition.abi_version = RIN_GPU_ABI_VERSION;
    transition.struct_size = sizeof(transition);
    transition.mip_level_count = 1u;
    transition.array_layer_count = 1u;
    transition.before_state = iteration == 1u
                                  ? RIN_GPU_IMAGE_STATE_UNDEFINED
                                  : RIN_GPU_IMAGE_STATE_PRESENT;
    transition.after_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    result = ringpu_runtime_command_transition_image(
        state->frame_runtime, state->frame_command_list, state->frame_image,
        &transition);
    if (result != RIN_GPU_OK) return result;

    memset(&render_pass, 0, sizeof(render_pass));
    render_pass.abi_version = RIN_GPU_ABI_VERSION;
    render_pass.struct_size = sizeof(render_pass);
    render_pass.color_target = state->frame_image;
    render_pass.load_op = RIN_GPU_RENDER_CLEAR;
    render_pass.store_op = RIN_GPU_RENDER_STORE;
    render_pass.clear_red = 1.0f;
    render_pass.clear_alpha = 1.0f;
    render_pass.color_write_mask = RIN_GPU_COLOR_WRITE_ALL;
    result = ringpu_runtime_command_begin_render_pass(
        state->frame_runtime, state->frame_command_list, &render_pass);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_command_end_render_pass(
            state->frame_runtime, state->frame_command_list);
    if (result != RIN_GPU_OK) return result;

    transition.before_state = RIN_GPU_IMAGE_STATE_COLOR_TARGET;
    transition.after_state = RIN_GPU_IMAGE_STATE_PRESENT;
    result = ringpu_runtime_command_transition_image(
        state->frame_runtime, state->frame_command_list, state->frame_image,
        &transition);
    if (result != RIN_GPU_OK) return result;

    memset(&present, 0, sizeof(present));
    present.abi_version = RIN_GPU_ABI_VERSION;
    present.struct_size = sizeof(present);
    present.image = state->frame_image;
    present.display_id = RIN_GPU_PRIMARY_DISPLAY;
    result = ringpu_runtime_command_present(state->frame_runtime,
                                            state->frame_command_list,
                                            &present);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_command_list_close(
            state->frame_runtime, state->frame_command_list);
    if (result != RIN_GPU_OK) return result;

    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = state->frame_command_list;
    submit.signal_fence = state->frame_fence;
    submit.signal_value = iteration;
    result = ringpu_runtime_queue_submit(state->frame_runtime,
                                         state->frame_queue, &submit);
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_wait_fence(
            state->frame_runtime, state->frame_fence, iteration,
            RIN_GPU_TIMEOUT_INFINITE);
    if (result != RIN_GPU_OK) return result;
    if (state->presented_frames != previous_frame_count + 1u ||
        state->presented_hash == 0u)
        return RIN_GPU_ERROR_STATE;
    *checksum_out = (*checksum_out * UINT64_C(1099511628211)) ^
                    state->presented_hash ^ iteration;
    return RIN_GPU_OK;
}

static int compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

typedef int (*BenchmarkIterationFn)(BenchmarkRuntime*, uint64_t, uint64_t*);

static int measure_metric(BenchmarkRuntime* state, const char* metric,
                          BenchmarkIterationFn run_iteration_fn,
                          uint32_t iterations, uint32_t warmup,
                          uint64_t* samples)
{
    uint64_t checksum = UINT64_C(1469598103934665603);
    uint64_t start_ns;
    uint64_t end_ns;
    long double elapsed_sum = 0.0L;
    uint64_t median_ns;
    uint64_t p95_ns;
    uint32_t index;
    int result;

    for (index = 0u; index < warmup; ++index) {
        result = run_iteration_fn(state, (uint64_t)index + 1u, &checksum);
        if (result != RIN_GPU_OK) {
            fprintf(stderr, "%s warmup failed at %" PRIu32
                    ": RinGPU status %d\n", metric, index, result);
            return result;
        }
    }
    for (index = 0u; index < iterations; ++index) {
        const uint64_t sequence = (uint64_t)warmup + index + 1u;
        state->timed_phase_ns = 0u;
        if (!timer_now_ns(&start_ns)) return RIN_GPU_ERROR_STATE;
        result = run_iteration_fn(state, sequence, &checksum);
        if (result != RIN_GPU_OK) {
            fprintf(stderr, "%s sample failed at %" PRIu32
                    ": RinGPU status %d\n", metric, index, result);
            return result;
        }
        if (!timer_now_ns(&end_ns) || end_ns < start_ns)
            return RIN_GPU_ERROR_STATE;
        samples[index] = state->timed_phase_ns != 0u
                             ? state->timed_phase_ns
                             : end_ns - start_ns;
    }

    qsort(samples, iterations, sizeof(*samples), compare_u64);
    for (index = 0u; index < iterations; ++index)
        elapsed_sum += (long double)samples[index];
    if ((iterations & 1u) != 0u) {
        median_ns = samples[iterations / 2u];
    } else {
        const uint64_t lower = samples[iterations / 2u - 1u];
        const uint64_t upper = samples[iterations / 2u];
        median_ns = lower + (upper - lower) / 2u;
    }
    p95_ns = samples[((uint64_t)iterations * 95u + 99u) / 100u - 1u];
    {
        const double mean_ns = (double)(elapsed_sum / (long double)iterations);
        if (mean_ns <= 0.0) return RIN_GPU_ERROR_STATE;
        printf("%s,software-host,%s,%" PRIu32 ",%" PRIu32
               ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%" PRIu64
               "\n",
               metric, compiler_name(), iterations, warmup, samples[0],
               median_ns, p95_ns, mean_ns, 1000000000.0 / mean_ns, checksum);
    }
    return RIN_GPU_OK;
}

static int run_benchmark(uint32_t iterations, uint32_t warmup)
{
    BenchmarkRuntime state;
    uint64_t* samples;
    int result;
    int cleanup_result;

    samples = (uint64_t*)malloc((size_t)iterations * sizeof(*samples));
    if (samples == NULL) return RIN_GPU_ERROR_NO_MEMORY;
    result = create_benchmark_runtime(&state);
    if (result != RIN_GPU_OK) {
        cleanup_result = destroy_benchmark_runtime(&state);
        free(samples);
        return result != RIN_GPU_OK ? result : cleanup_result;
    }
    result = create_frame_runtime(&state);
    if (result != RIN_GPU_OK) goto cleanup;
    result = create_descriptor_resources(&state);
    if (result != RIN_GPU_OK) goto cleanup;
    printf("benchmark,backend,compiler,iterations,warmup,min_ns,"
           "median_ns,p95_ns,mean_ns,ops_per_second,checksum\n");
    result = measure_metric(&state, "command_roundtrip", run_iteration,
                            iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "command_record_only",
                            run_command_record_iteration,
                            iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "queue_submit_wait",
                            run_submit_wait_iteration,
                            iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "memory_buffer_bind_roundtrip",
                            run_memory_iteration, iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "compute_bind_group_churn",
                            run_descriptor_iteration,
                            iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "software_frame_roundtrip",
                            run_frame_iteration, iterations, warmup, samples);

cleanup:
    cleanup_result = destroy_benchmark_runtime(&state);
    free(samples);
    if (result == RIN_GPU_OK && cleanup_result != RIN_GPU_OK)
        result = cleanup_result;
    return result;
}

int main(int argc, char** argv)
{
    uint32_t iterations;
    uint32_t warmup;
    int result;
    if (!parse_arguments(argc, argv, &iterations, &warmup)) {
        fprintf(stderr,
                "usage: %s [--iterations 1..%u] [--warmup 0..%u]\n",
                argv[0], MAX_ITERATIONS, MAX_ITERATIONS);
        return 2;
    }
    result = run_benchmark(iterations, warmup);
    if (result != RIN_GPU_OK) {
        fprintf(stderr, "host benchmark failed: RinGPU status %d\n", result);
        return 1;
    }
    return 0;
}
