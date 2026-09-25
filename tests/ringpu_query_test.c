/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "../src/core/core.h"
#include <ringpu/software.h>

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

static int make_core(RinGpuCore* core, RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackendDescV1 backend_desc;
    RinGpuDisplayInfoV1 display;
    RinGpuCoreConfigV1 config;
    RinGpuSoftwareBackend* backend = NULL;

    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.struct_size = sizeof(backend_desc);
    backend_desc.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend_desc.max_total_bytes = UINT64_C(4) * 1024u * 1024u;
    CHECK(ringpu_software_backend_create(&backend_desc, &backend) ==
          RIN_GPU_OK && backend != NULL);
    memset(&display, 0, sizeof(display));
    display.abi_version = RIN_GPU_ABI_VERSION;
    display.struct_size = sizeof(display);
    display.display_id = RIN_GPU_PRIMARY_DISPLAY;
    display.flags = RIN_GPU_DISPLAY_CONNECTED | RIN_GPU_DISPLAY_PRIMARY;
    display.width = 16u;
    display.height = 16u;
    display.refresh_millihertz = 60000u;
    display.format = RIN_GPU_FORMAT_BGRA8_UNORM;
    display.scale_milli = 1000u;
    memcpy(display.name, "query", 6u);
    memset(&config, 0, sizeof(config));
    config.abi_version = RIN_GPU_ABI_VERSION;
    config.struct_size = sizeof(config);
    config.handle_secret = UINT64_C(0x5155455259544553);
    config.max_buffer_size = UINT64_C(1) << 20u;
    config.max_image_size = UINT64_C(1) << 20u;
    config.max_total_allocation_size = UINT64_C(2) << 20u;
    config.max_image_dimension = 1024u;
    config.max_image_layers = 8u;
    config.max_image_mip_levels = 8u;
    config.max_image_sample_count = 1u;
    config.adapter.abi_version = RIN_GPU_ABI_VERSION;
    config.adapter.struct_size = sizeof(config.adapter);
    config.adapter.queue_capabilities = RIN_GPU_QUEUE_COPY;
    memcpy(config.adapter.name, "query", 6u);
    config.backend = *ringpu_software_backend_ops();
    config.backend_context = backend;
    config.displays = &display;
    config.display_count = 1u;
    config.backend_family = RIN_GPU_BACKEND_FAMILY_SOFTWARE;
    if (ringpu_core_init(core, &config) != RIN_GPU_OK) {
        ringpu_software_backend_destroy(backend);
        return 0;
    }
    *backend_out = backend;
    return 1;
}

static int record_submit(RinGpuCore* core, RinGpuHandle queue,
                         RinGpuHandle list)
{
    RinGpuSubmitInfoV1 submit;

    memset(&submit, 0, sizeof(submit));
    submit.abi_version = RIN_GPU_ABI_VERSION;
    submit.struct_size = sizeof(submit);
    submit.command_list = list;
    return ringpu_queue_submit(core, queue, &submit);
}

int main(void)
{
    RinGpuCore core;
    RinGpuSoftwareBackend* backend = NULL;
    RinGpuQueueDescV1 queue_desc;
    RinGpuCommandListDescV1 command_desc;
    RinGpuQueryDescV1 query_desc;
    RinGpuQueryResultV1 query_result;
    RinGpuHandle queue = 0u;
    RinGpuHandle list = 0u;
    RinGpuHandle timestamp = 0u;
    RinGpuHandle occlusion = 0u;
    RinGpuHandle pipeline = 0u;
    uint64_t period = 0u;

    memset(&core, 0, sizeof(core));
    CHECK(make_core(&core, &backend));
    CHECK(ringpu_get_timestamp_period(&core, &period) == RIN_GPU_OK &&
          period == 1u);
    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.abi_version = RIN_GPU_ABI_VERSION;
    queue_desc.struct_size = sizeof(queue_desc);
    queue_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_queue(&core, &queue_desc, &queue) == RIN_GPU_OK);
    memset(&command_desc, 0, sizeof(command_desc));
    command_desc.abi_version = RIN_GPU_ABI_VERSION;
    command_desc.struct_size = sizeof(command_desc);
    command_desc.capabilities = RIN_GPU_QUEUE_COPY;
    CHECK(ringpu_create_command_list(&core, &command_desc, &list) ==
          RIN_GPU_OK);
    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.abi_version = RIN_GPU_ABI_VERSION;
    query_desc.struct_size = sizeof(query_desc);
    query_desc.query_type = RIN_GPU_QUERY_TIMESTAMP;
    CHECK(ringpu_create_query(&core, &query_desc, &timestamp) == RIN_GPU_OK);
    query_desc.query_type = RIN_GPU_QUERY_OCCLUSION;
    CHECK(ringpu_create_query(&core, &query_desc, &occlusion) == RIN_GPU_OK);
    query_desc.query_type = RIN_GPU_QUERY_PIPELINE_STATISTICS;
    CHECK(ringpu_create_query(&core, &query_desc, &pipeline) == RIN_GPU_OK);

    CHECK(ringpu_command_begin_query(&core, list, timestamp) == RIN_GPU_OK);
    memset(&query_result, 0, sizeof(query_result));
    CHECK(ringpu_get_query_result(&core, timestamp, 0u, &query_result) ==
          RIN_GPU_ERROR_BUSY);
    CHECK(ringpu_command_end_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_begin_query(&core, list, occlusion) == RIN_GPU_OK);
    CHECK(ringpu_command_end_query(&core, list, occlusion) == RIN_GPU_OK);
    CHECK(ringpu_command_begin_query(&core, list, pipeline) == RIN_GPU_OK);
    CHECK(ringpu_command_end_query(&core, list, pipeline) == RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, list) == RIN_GPU_OK);
    CHECK(record_submit(&core, queue, list) == RIN_GPU_OK);

    memset(&query_result, 0, sizeof(query_result));
    CHECK(ringpu_get_query_result(&core, timestamp, 0u, &query_result) ==
          RIN_GPU_OK && query_result.available != 0u &&
          query_result.query_type == RIN_GPU_QUERY_TIMESTAMP &&
          query_result.values[0] != 0u);
    memset(&query_result, 0, sizeof(query_result));
    CHECK(ringpu_get_query_result(&core, occlusion, 0u, &query_result) ==
          RIN_GPU_OK && query_result.available != 0u &&
          query_result.values[0] == 0u);
    memset(&query_result, 0, sizeof(query_result));
    CHECK(ringpu_get_query_result(&core, pipeline, 0u, &query_result) ==
          RIN_GPU_OK && query_result.available != 0u &&
          query_result.values[RIN_GPU_PIPELINE_STAT_COUNT] == 0u);

    CHECK(ringpu_command_list_reset(&core, list) == RIN_GPU_OK);
    CHECK(ringpu_command_reset_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_begin_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_end_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, list) == RIN_GPU_OK);
    CHECK(record_submit(&core, queue, list) == RIN_GPU_OK);
    CHECK(ringpu_get_query_result(&core, timestamp, 0u, &query_result) ==
          RIN_GPU_OK && query_result.available != 0u);

    /* A backend without the query callbacks must reject query submission. */
    CHECK(ringpu_command_list_reset(&core, list) == RIN_GPU_OK);
    CHECK(ringpu_command_reset_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_begin_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_end_query(&core, list, timestamp) == RIN_GPU_OK);
    CHECK(ringpu_command_list_close(&core, list) == RIN_GPU_OK);
    core.backend.get_query_result = NULL;
    CHECK(record_submit(&core, queue, list) == RIN_GPU_ERROR_UNSUPPORTED);
    core.backend.get_query_result = (int (*)(void*, uint64_t, uint32_t,
        uint64_t*, uint32_t*))NULL;
    (void)backend;
    ringpu_core_shutdown(&core);
    ringpu_software_backend_destroy(backend);
    return 0;
}
