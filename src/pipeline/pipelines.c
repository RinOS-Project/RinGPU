// SPDX-License-Identifier: MIT
#include "pipelines.h"

#include "../core/object_table.h"
#include "../validation/resource.h"
#include "../validation/shader.h"

#include <stdlib.h>
#include <string.h>

int ringpu_create_compute_pipeline(
    RinGpuCore* core, const RinGpuComputePipelineDescV1* desc,
    RinGpuHandle* pipeline)
{
    RinGpuObjectSlot* shader;
    RinGpuObjectSlot* slot;
    uint32_t resource_access[RIN_SHADER_MAX_RESOURCES];
    uint32_t resource_kinds[RIN_SHADER_MAX_RESOURCES];
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!desc || !pipeline ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        desc->flags != 0u || desc->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *pipeline = 0u;
    result = ringpu_slot(core, desc->shader_module,
                         RIN_GPU_OBJECT_SHADER_MODULE, NULL, &shader);
    if (result != RIN_GPU_OK) return result;
    if (shader->value.shader_module.info.stage != RIN_SHADER_STAGE_COMPUTE) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_shader_resource_layout(shader, resource_access,
                                            resource_kinds);
    if (result != RIN_GPU_OK) return result;
    for (uint32_t resource = 0u;
         resource < shader->value.shader_module.info.resource_count;
         resource++) {
        if (resource_kinds[resource] !=
            RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
    }
    result = core->backend.create_compute_pipeline(
        core->backend_context, shader->value.shader_module.backend_cookie,
        &shader->value.shader_module.info, &cookie);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_COMPUTE_PIPELINE,
                             pipeline, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_compute_pipeline(core->backend_context, cookie);
        *pipeline = 0u;
        return result;
    }
    slot->value.compute_pipeline.shader_module = desc->shader_module;
    slot->value.compute_pipeline.backend_cookie = cookie;
    slot->value.compute_pipeline.resource_count =
        shader->value.shader_module.info.resource_count;
    memcpy(slot->value.compute_pipeline.resource_access, resource_access,
           sizeof(resource_access));
    memcpy(slot->value.compute_pipeline.resource_kinds, resource_kinds,
           sizeof(resource_kinds));
    shader->value.shader_module.reference_count++;
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_COMPUTE_PIPELINE,
                           shader->value.shader_module.info.resource_count,
                           RIN_GPU_OK);
    return RIN_GPU_OK;
}

int ringpu_create_compute_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group)
{
    RinGpuObjectSlot* pipeline_slot;
    RinGpuObjectSlot* slot;
    RinGpuBufferBindingV1* snapshot = NULL;
    RinGpuBackendBufferBindingV1* backend_bindings = NULL;
    uint64_t seen = 0u;
    uint64_t cookie = 0u;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!bind_group || binding_count > RIN_SHADER_MAX_RESOURCES ||
        (binding_count == 0u && bindings != NULL) ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *bind_group = 0u;
    result = ringpu_slot(core, pipeline, RIN_GPU_OBJECT_COMPUTE_PIPELINE,
                         NULL, &pipeline_slot);
    if (result != RIN_GPU_OK) return result;
    if (binding_count != pipeline_slot->value.compute_pipeline.resource_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (binding_count != 0u) {
        snapshot = (RinGpuBufferBindingV1*)calloc(
            binding_count, sizeof(*snapshot));
        backend_bindings = (RinGpuBackendBufferBindingV1*)calloc(
            binding_count, sizeof(*backend_bindings));
        if (!snapshot || !backend_bindings) {
            result = RIN_GPU_ERROR_NO_MEMORY;
            goto fail;
        }
    }
    for (uint32_t index = 0u; index < binding_count; index++) {
        const RinGpuBufferBindingV1* binding = &bindings[index];
        RinGpuObjectSlot* buffer;
        if (!ringpu_versioned(binding->abi_version, binding->struct_size,
                              sizeof(*binding)) ||
            binding->binding >= binding_count ||
            (seen & (UINT64_C(1) << binding->binding)) != 0u ||
            binding->access == 0u ||
            (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
            binding->access != pipeline_slot->value.compute_pipeline
                .resource_access[binding->binding] ||
            binding->flags != 0u || binding->reserved != 0u ||
            (binding->offset & 3u) != 0u ||
            (binding->size_bytes & 3u) != 0u) {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        result = ringpu_slot(core, binding->buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &buffer);
        if (result != RIN_GPU_OK) goto fail;
        if ((buffer->value.buffer.usage & RIN_GPU_BUFFER_STORAGE) == 0u ||
            !ringpu_buffer_upload_ready(buffer)) {
            result = RIN_GPU_ERROR_STATE;
            goto fail;
        }
        if (!ringpu_range(binding->offset, binding->size_bytes,
                          buffer->value.buffer.size_bytes)) {
            result = RIN_GPU_ERROR_BOUNDS;
            goto fail;
        }
        seen |= UINT64_C(1) << binding->binding;
        snapshot[binding->binding] = *binding;
        snapshot[binding->binding].struct_size = sizeof(*snapshot);
        backend_bindings[binding->binding].buffer_cookie =
            buffer->value.buffer.backend_cookie;
        backend_bindings[binding->binding].offset = binding->offset;
        backend_bindings[binding->binding].size_bytes = binding->size_bytes;
        backend_bindings[binding->binding].access = binding->access;
    }
    result = core->backend.create_compute_bind_group(
        core->backend_context,
        pipeline_slot->value.compute_pipeline.backend_cookie,
        backend_bindings, binding_count, &cookie);
    if (result != RIN_GPU_OK) goto fail;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_COMPUTE_BIND_GROUP,
                             bind_group, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_compute_bind_group(core->backend_context,
                                                 cookie);
        goto fail;
    }
    slot->value.compute_bind_group.pipeline = pipeline;
    slot->value.compute_bind_group.backend_cookie = cookie;
    slot->value.compute_bind_group.bindings = snapshot;
    slot->value.compute_bind_group.binding_count = binding_count;
    pipeline_slot->value.compute_pipeline.reference_count++;
    for (uint32_t index = 0u; index < binding_count; index++) {
        RinGpuObjectSlot* buffer;
        if (ringpu_slot(core, snapshot[index].buffer, RIN_GPU_OBJECT_BUFFER,
                        NULL, &buffer) == RIN_GPU_OK) {
            buffer->value.buffer.reference_count++;
        }
    }
    free(backend_bindings);
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_COMPUTE_BIND_GROUP,
                           binding_count, RIN_GPU_OK);
    return RIN_GPU_OK;

fail:
    free(snapshot);
    free(backend_bindings);
    return result;
}
