/* SPDX-License-Identifier: MIT */
#include "../src/core/core.h"
#include "../src/core/object_table.h"
#include "../src/software/software_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SMALL_ALLOCATION_COUNT 16u
#define TEST_SMALL_ALLOCATION_BYTES UINT64_C(4096)
#define TEST_LARGE_ALLOCATION_COUNT 8u
#define TEST_LARGE_ALLOCATION_BYTES UINT64_C(8192)
#define TEST_POOL_BYTES UINT64_C(65536)
#define TEST_TOTAL_BYTES (UINT64_C(4) * 1024u * 1024u)

int main(void)
{
    RinGpuSoftwareBackendDescV4 backend_desc;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuSoftwareMemoryPoolStatsV1 pool_stats;
    RinGpuCoreConfigV1 config;
    RinGpuCore core;
    RinGpuMemoryDescV1 memory_desc;
    RinGpuBufferDescV1 buffer_desc;
    RinGpuResourceMemoryBindingV1 binding;
    RinGpuHandle small_memory[TEST_SMALL_ALLOCATION_COUNT] = {0};
    RinGpuHandle small_buffer[TEST_SMALL_ALLOCATION_COUNT] = {0};
    RinGpuHandle large_memory[TEST_LARGE_ALLOCATION_COUNT] = {0};
    RinGpuHandle dedicated_memory = 0u;
    RinGpuHandle pressure_buffer = 0u;
    uint8_t source[TEST_SMALL_ALLOCATION_COUNT]
                 [(size_t)TEST_SMALL_ALLOCATION_BYTES];
    uint8_t readback[(size_t)TEST_SMALL_ALLOCATION_BYTES];
    uint32_t index;
    int result = 1;
    int core_initialized = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        goto cleanup; \
    } \
} while (0)

    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.base.base.base.struct_size = sizeof(backend_desc);
    backend_desc.base.base.base.version = RIN_GPU_SOFTWARE_BACKEND_VERSION_4;
    backend_desc.base.base.base.max_total_bytes = TEST_TOTAL_BYTES;
    backend_desc.base.base.base.flags =
        RIN_GPU_SOFTWARE_BACKEND_FLAG_HEADLESS |
        RIN_GPU_SOFTWARE_BACKEND_FLAG_MEMORY_SUBALLOCATOR;
    CHECK(ringpu_software_backend_create(&backend_desc.base.base.base,
                                         &backend) == RIN_GPU_OK);

    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x535542414c4c4f43);
    config.max_buffer_size = TEST_TOTAL_BYTES;
    config.max_image_size = TEST_TOTAL_BYTES;
    config.max_total_allocation_size = TEST_TOTAL_BYTES;
    config.max_image_dimension = 64u;
    config.max_image_layers = 1u;
    config.max_image_mip_levels = 1u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY;
    memcpy(config.adapter.name, "suballocator-test",
           sizeof("suballocator-test"));
    config.backend = *ringpu_software_backend_ops();
    config.backend_context = backend;
    config.backend_family = RIN_GPU_BACKEND_FAMILY_SOFTWARE;
    CHECK(ringpu_core_init(&core, &config) == RIN_GPU_OK);
    core_initialized = 1;

    memset(&memory_desc, 0, sizeof(memory_desc));
    memory_desc.abi_version = RIN_GPU_ABI_VERSION;
    memory_desc.struct_size = sizeof(memory_desc);
    memory_desc.size_bytes = TEST_SMALL_ALLOCATION_BYTES;
    memory_desc.alignment = TEST_SMALL_ALLOCATION_BYTES;
    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = TEST_SMALL_ALLOCATION_BYTES;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_SOURCE |
                        RIN_GPU_BUFFER_COPY_DESTINATION;
    buffer_desc.flags = RIN_GPU_BUFFER_CPU_VISIBLE;
    memset(&binding, 0, sizeof(binding));
    binding.abi_version = RIN_GPU_ABI_VERSION;
    binding.struct_size = sizeof(binding);
    binding.size_bytes = TEST_SMALL_ALLOCATION_BYTES;

    for (index = 0u; index < TEST_SMALL_ALLOCATION_COUNT; ++index) {
        RinGpuObjectSlot* memory_slot;
        CHECK(ringpu_create_memory(&core, &memory_desc,
                                   &small_memory[index]) == RIN_GPU_OK);
        CHECK(ringpu_create_buffer(&core, &buffer_desc,
                                   &small_buffer[index]) == RIN_GPU_OK);
        binding.memory = small_memory[index];
        CHECK(ringpu_bind_buffer_memory(&core, small_buffer[index],
                                        &binding) == RIN_GPU_OK);
        CHECK(ringpu_slot(&core, small_memory[index], RIN_GPU_OBJECT_MEMORY,
                          NULL, &memory_slot) == RIN_GPU_OK);
        CHECK(memory_slot->value.memory.allocation != NULL);
        CHECK((uintptr_t)memory_slot->value.memory.allocation %
                  TEST_SMALL_ALLOCATION_BYTES == 0u);
        memset(source[index], (int)(index + 1u), sizeof(source[index]));
        CHECK(ringpu_upload_buffer(&core, small_buffer[index], 0u,
                                   source[index], sizeof(source[index])) ==
              RIN_GPU_OK);
    }

    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.enabled == 1u && pool_stats.active_suballocations == 16u &&
          pool_stats.backing_allocation_count == 1u &&
          pool_stats.reserved_bytes == TEST_POOL_BYTES &&
          pool_stats.suballocated_bytes == TEST_POOL_BYTES);

    for (index = 0u; index < TEST_SMALL_ALLOCATION_COUNT; ++index) {
        CHECK(ringpu_readback_buffer(&core, small_buffer[index], 0u,
                                     readback, sizeof(readback)) == RIN_GPU_OK);
        CHECK(memcmp(readback, source[index], sizeof(readback)) == 0);
    }

    for (index = 1u; index < TEST_SMALL_ALLOCATION_COUNT; index += 2u) {
        CHECK(ringpu_destroy(&core, small_buffer[index]) == RIN_GPU_OK);
        small_buffer[index] = 0u;
        CHECK(ringpu_destroy(&core, small_memory[index]) == RIN_GPU_OK);
        small_memory[index] = 0u;
    }

    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.active_suballocations == 8u &&
          pool_stats.backing_allocation_count == 1u &&
          pool_stats.reserved_bytes == TEST_POOL_BYTES &&
          pool_stats.suballocated_bytes == TEST_POOL_BYTES / 2u);

    memory_desc.size_bytes = TEST_LARGE_ALLOCATION_BYTES;
    memory_desc.alignment = 2u * TEST_SMALL_ALLOCATION_BYTES;
    for (index = 0u; index < TEST_LARGE_ALLOCATION_COUNT; ++index) {
        RinGpuObjectSlot* memory_slot;
        CHECK(ringpu_create_memory(&core, &memory_desc,
                                   &large_memory[index]) == RIN_GPU_OK);
        CHECK(ringpu_slot(&core, large_memory[index], RIN_GPU_OBJECT_MEMORY,
                          NULL, &memory_slot) == RIN_GPU_OK);
        CHECK((uintptr_t)memory_slot->value.memory.allocation %
                  memory_desc.alignment == 0u);
    }
    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.active_suballocations == 16u &&
          pool_stats.backing_allocation_count == 2u &&
          pool_stats.reserved_bytes == 2u * TEST_POOL_BYTES &&
          pool_stats.suballocated_bytes ==
              TEST_SMALL_ALLOCATION_COUNT / 2u * TEST_SMALL_ALLOCATION_BYTES +
              TEST_LARGE_ALLOCATION_COUNT * TEST_LARGE_ALLOCATION_BYTES);

    for (index = 0u; index < TEST_SMALL_ALLOCATION_COUNT; index += 2u) {
        CHECK(ringpu_destroy(&core, small_buffer[index]) == RIN_GPU_OK);
        small_buffer[index] = 0u;
        CHECK(ringpu_destroy(&core, small_memory[index]) == RIN_GPU_OK);
        small_memory[index] = 0u;
    }
    for (index = 0u; index < TEST_LARGE_ALLOCATION_COUNT; ++index) {
        CHECK(ringpu_destroy(&core, large_memory[index]) == RIN_GPU_OK);
        large_memory[index] = 0u;
    }

    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.active_suballocations == 0u &&
          pool_stats.backing_allocation_count == 2u &&
          pool_stats.reserved_bytes == 2u * TEST_POOL_BYTES &&
          pool_stats.suballocated_bytes == 0u);

    memory_desc.size_bytes = TEST_SMALL_ALLOCATION_BYTES;
    memory_desc.alignment = TEST_SMALL_ALLOCATION_BYTES;
    for (index = 0u; index < TEST_SMALL_ALLOCATION_COUNT; ++index)
        CHECK(ringpu_create_memory(&core, &memory_desc,
                                   &small_memory[index]) == RIN_GPU_OK);
    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.active_suballocations == 16u &&
          pool_stats.backing_allocation_count == 2u &&
          pool_stats.reserved_bytes == 2u * TEST_POOL_BYTES &&
          pool_stats.suballocated_bytes == TEST_POOL_BYTES);

    memory_desc.flags = RIN_GPU_MEMORY_BINDING_DEDICATED;
    {
        RinGpuObjectSlot* memory_slot;
        CHECK(ringpu_create_memory(&core, &memory_desc, &dedicated_memory) ==
              RIN_GPU_OK);
        CHECK(ringpu_slot(&core, dedicated_memory, RIN_GPU_OBJECT_MEMORY,
                          NULL, &memory_slot) == RIN_GPU_OK);
        CHECK((uintptr_t)memory_slot->value.memory.allocation %
                  memory_desc.alignment == 0u);
    }
    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.active_suballocations == 16u &&
          pool_stats.backing_allocation_count == 2u &&
          pool_stats.reserved_bytes == 2u * TEST_POOL_BYTES);
    CHECK(ringpu_destroy(&core, dedicated_memory) == RIN_GPU_OK);
    dedicated_memory = 0u;
    for (index = 0u; index < TEST_SMALL_ALLOCATION_COUNT; ++index) {
        CHECK(ringpu_destroy(&core, small_memory[index]) == RIN_GPU_OK);
        small_memory[index] = 0u;
    }

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.abi_version = RIN_GPU_ABI_VERSION;
    buffer_desc.struct_size = sizeof(buffer_desc);
    buffer_desc.size_bytes = TEST_TOTAL_BYTES - TEST_POOL_BYTES;
    buffer_desc.usage = RIN_GPU_BUFFER_COPY_DESTINATION;
    CHECK(ringpu_create_buffer(&core, &buffer_desc, &pressure_buffer) ==
          RIN_GPU_OK);
    memset(&pool_stats, 0, sizeof(pool_stats));
    pool_stats.struct_size = sizeof(pool_stats);
    pool_stats.version = RIN_GPU_SOFTWARE_MEMORY_POOL_STATS_VERSION;
    CHECK(ringpu_software_backend_query_memory_pool_stats(
              backend, &pool_stats) == RIN_GPU_OK);
    CHECK(pool_stats.active_suballocations == 0u &&
          pool_stats.reserved_bytes == 0u &&
          pool_stats.suballocated_bytes == 0u);
    CHECK(ringpu_destroy(&core, pressure_buffer) == RIN_GPU_OK);
    pressure_buffer = 0u;

    result = 0;

cleanup:
    if (core_initialized) ringpu_core_shutdown(&core);
    if (backend) ringpu_software_backend_destroy(backend);
#undef CHECK
    return result;
}
