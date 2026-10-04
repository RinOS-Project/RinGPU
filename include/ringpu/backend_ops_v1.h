/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_BACKEND_OPS_V1_H
#define RINGPU_PUBLIC_BACKEND_OPS_V1_H

#include <stdint.h>

#include "backend_command_v1.h"

/* Callback fields have one source of truth. Public RinGPU and OS-Core
 * instantiate the same V1 table with their respective pipeline/command
 * record types; the latter remains private and crosses the versioned adapter
 * through explicit translation. */
#define RIN_GPU_DECLARE_BACKEND_OPS_V1(                                      \
    name, graphics_pipeline_desc_type, command_type, query_value_count)      \
typedef struct name {                                                        \
    uint32_t abi_version;                                                    \
    uint32_t struct_size;                                                     \
    int (*create_buffer)(void* context, const RinGpuBufferDescV1* desc,       \
                         uint64_t* cookie);                                  \
    void (*destroy_buffer)(void* context, uint64_t cookie);                  \
    int (*upload_buffer)(void* context, uint64_t cookie,                      \
                         uint64_t destination_offset, const void* source,    \
                         uint64_t size_bytes);                               \
    int (*readback_buffer)(void* context, uint64_t cookie,                    \
                           uint64_t source_offset, void* destination,        \
                           uint64_t size_bytes);                             \
    int (*create_image)(void* context, const RinGpuImageDescV1* desc,        \
                        uint64_t allocation_bytes, uint64_t* cookie);        \
    void (*destroy_image)(void* context, uint64_t cookie);                   \
    int (*upload_image)(void* context, uint64_t cookie,                       \
                        const RinGpuImageUploadV1* upload,                   \
                        const void* source, uint64_t source_size);            \
    int (*create_sampler)(void* context, const RinGpuSamplerDescV1* desc,    \
                          uint64_t* cookie);                                 \
    void (*destroy_sampler)(void* context, uint64_t cookie);                 \
    int (*create_shader_module)(void* context, const void* rin_shader_ir,     \
                                uint64_t shader_size,                        \
                                const RinShaderInfoV1* info,                 \
                                uint64_t* cookie);                           \
    void (*destroy_shader_module)(void* context, uint64_t cookie);           \
    int (*create_compute_pipeline)(void* context,                            \
                                   uint64_t shader_module_cookie,            \
                                   const RinShaderInfoV1* shader_info,       \
                                   uint64_t* cookie);                        \
    void (*destroy_compute_pipeline)(void* context, uint64_t cookie);        \
    int (*create_graphics_pipeline)(                                         \
        void* context, uint64_t vertex_shader_cookie,                       \
        const RinShaderInfoV1* vertex_shader_info,                          \
        uint64_t fragment_shader_cookie,                                    \
        const RinShaderInfoV1* fragment_shader_info,                        \
        const graphics_pipeline_desc_type* desc, uint64_t* cookie);         \
    void (*destroy_graphics_pipeline)(void* context, uint64_t cookie);       \
    int (*create_compute_bind_group)(                                        \
        void* context, uint64_t pipeline_cookie,                            \
        const RinGpuBackendBufferBindingV1* bindings, uint32_t binding_count,\
        uint64_t* cookie);                                                   \
    void (*destroy_compute_bind_group)(void* context, uint64_t cookie);      \
    int (*create_graphics_bind_group)(                                       \
        void* context, uint64_t pipeline_cookie,                            \
        const RinGpuBackendGraphicsBindingV1* bindings,                     \
        uint32_t binding_count, uint64_t* cookie);                          \
    void (*destroy_graphics_bind_group)(void* context, uint64_t cookie);     \
    int (*submit_commands)(void* context, const command_type* commands,      \
                           uint32_t command_count);                         \
    int (*get_query_result)(void* context, uint64_t query_cookie,            \
                            uint32_t query_type,                             \
                            uint64_t values[query_value_count],              \
                            uint32_t* available);                            \
    int (*get_timestamp_period)(void* context, uint64_t* period_nanoseconds);\
    void (*destroy_query)(void* context, uint64_t query_cookie);             \
    int (*wait_for_completion)(void* context, uint64_t timeout_ns);           \
    int (*readback_image)(void* context, uint64_t cookie,                    \
                          const RinGpuImageReadbackV1* readback,             \
                          void* destination, uint64_t destination_size);     \
    int (*create_memory)(void* context, const RinGpuMemoryDescV1* desc,      \
                         void** allocation_out);                            \
    void (*destroy_memory)(void* context, void* allocation,                  \
                           uint64_t size_bytes);                             \
    int (*bind_buffer_memory)(void* context, uint64_t buffer_cookie,         \
                              const RinGpuBufferDescV1* desc,                \
                              void* allocation, uint64_t allocation_size,    \
                              uint64_t offset_bytes,                         \
                              uint64_t* bound_cookie_out);                   \
    int (*bind_image_memory)(void* context, uint64_t image_cookie,          \
                             const RinGpuImageDescV1* desc,                  \
                             uint64_t resource_size, void* allocation,      \
                             uint64_t allocation_size, uint64_t offset_bytes,\
                             uint64_t* bound_cookie_out);                    \
} name

RIN_GPU_DECLARE_BACKEND_OPS_V1(
    RinGpuBackendOpsV1, RinGpuBackendGraphicsPipelineDescV1,
    RinGpuBackendCommandV1, RIN_GPU_QUERY_RESULT_VALUE_COUNT);

#endif /* RINGPU_PUBLIC_BACKEND_OPS_V1_H */
