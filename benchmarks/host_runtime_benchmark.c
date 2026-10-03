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
        submit.signal_value = iteration;
        result = ringpu_runtime_queue_submit(state->runtime, state->queue,
                                             &submit);
    }
    if (result == RIN_GPU_OK)
        result = ringpu_runtime_wait_fence(state->runtime, state->fence,
                                           iteration,
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
        if (!timer_now_ns(&start_ns)) return RIN_GPU_ERROR_STATE;
        result = run_iteration_fn(state, sequence, &checksum);
        if (result != RIN_GPU_OK) {
            fprintf(stderr, "%s sample failed at %" PRIu32
                    ": RinGPU status %d\n", metric, index, result);
            return result;
        }
        if (!timer_now_ns(&end_ns) || end_ns < start_ns)
            return RIN_GPU_ERROR_STATE;
        samples[index] = end_ns - start_ns;
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
    result = create_descriptor_resources(&state);
    if (result != RIN_GPU_OK) goto cleanup;
    printf("benchmark,backend,compiler,iterations,warmup,min_ns,"
           "median_ns,p95_ns,mean_ns,ops_per_second,checksum\n");
    result = measure_metric(&state, "command_roundtrip", run_iteration,
                            iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "memory_buffer_bind_roundtrip",
                            run_memory_iteration, iterations, warmup, samples);
    if (result != RIN_GPU_OK) goto cleanup;
    result = measure_metric(&state, "compute_bind_group_churn",
                            run_descriptor_iteration,
                            iterations, warmup, samples);

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
