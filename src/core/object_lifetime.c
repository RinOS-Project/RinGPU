// SPDX-License-Identifier: MIT
#include "core.h"

#include "../command/record.h"
#include "../pipeline/pipelines.h"
#include "../software/software_backend.h"
#include "../shader/modules.h"
#include "object_table.h"

#include <stdlib.h>

static int ringpu_release_bound_memory(RinGpuCore* core,
                                       RinGpuHandle memory)
{
    RinGpuObjectSlot* memory_slot;
    int result;

    if (memory == 0u) return RIN_GPU_OK;
    result = ringpu_slot_retained(core, memory, RIN_GPU_OBJECT_MEMORY, NULL,
                                  &memory_slot);
    if (result != RIN_GPU_OK || memory_slot->value.memory.reference_count == 0u)
        return RIN_GPU_ERROR_STATE;
    memory_slot->value.memory.reference_count--;
    return RIN_GPU_OK;
}

static int ringpu_destroy_slot(RinGpuCore* core, RinGpuHandle object,
                               RinGpuObjectSlot* slot) {
    int result;
    if (!core || !slot || !slot->occupied) return RIN_GPU_ERROR_STATE;
    if (slot->type == RIN_GPU_OBJECT_BUFFER) {
        if (slot->value.buffer.reference_count != 0u) return RIN_GPU_ERROR_BUSY;
        if (slot->value.buffer.memory_handle != 0u) {
            result = ringpu_release_bound_memory(
                core, slot->value.buffer.memory_handle);
            if (result != RIN_GPU_OK) return result;
        }
        core->backend.destroy_buffer(core->backend_context,
                                     slot->value.buffer.backend_cookie);
        if (slot->value.buffer.memory_handle == 0u)
            core->allocated_bytes -= slot->value.buffer.size_bytes;
    } else if (slot->type == RIN_GPU_OBJECT_IMAGE) {
        if (slot->value.image.reference_count != 0u) return RIN_GPU_ERROR_BUSY;
        if (slot->value.image.memory_handle != 0u) {
            result = ringpu_release_bound_memory(
                core, slot->value.image.memory_handle);
            if (result != RIN_GPU_OK) return result;
        }
        core->backend.destroy_image(core->backend_context,
                                    slot->value.image.backend_cookie);
        free(slot->value.image.subresource_states);
        free(slot->value.image.cpu_upload_complete);
        if (slot->value.image.memory_handle == 0u)
            core->allocated_bytes -= slot->value.image.allocation_bytes;
    } else if (slot->type == RIN_GPU_OBJECT_MEMORY) {
        if (slot->value.memory.reference_count != 0u)
            return RIN_GPU_ERROR_BUSY;
        ringpu_software_backend_destroy_memory(
            core->backend_context, slot->value.memory.bytes,
            slot->value.memory.size_bytes);
        core->allocated_bytes -= slot->value.memory.size_bytes;
    } else if (slot->type == RIN_GPU_OBJECT_SAMPLER) {
        if (slot->value.sampler.reference_count != 0u) {
            return RIN_GPU_ERROR_BUSY;
        }
        core->backend.destroy_sampler(core->backend_context,
                                      slot->value.sampler.backend_cookie);
    } else if (slot->type == RIN_GPU_OBJECT_SHADER_MODULE) {
        if (slot->value.shader_module.reference_count != 0u) {
            return RIN_GPU_ERROR_BUSY;
        }
        if (slot->value.shader_module.cache_index !=
            RIN_GPU_SHADER_CACHE_INDEX_NONE) {
            ringpu_shader_cache_release(
                core, slot->value.shader_module.cache_index);
        } else {
            core->backend.destroy_shader_module(
                core->backend_context,
                slot->value.shader_module.backend_cookie);
            free(slot->value.shader_module.rin_shader_ir);
        }
    } else if (slot->type == RIN_GPU_OBJECT_COMPUTE_PIPELINE) {
        RinGpuObjectSlot* shader;
        if (slot->value.compute_pipeline.reference_count != 0u) {
            return RIN_GPU_ERROR_BUSY;
        }
        result = ringpu_slot_retained(
            core, slot->value.compute_pipeline.shader_module,
            RIN_GPU_OBJECT_SHADER_MODULE, NULL, &shader);
        if (result != RIN_GPU_OK ||
            shader->value.shader_module.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        core->backend.destroy_compute_pipeline(
            core->backend_context,
            slot->value.compute_pipeline.backend_cookie);
        shader->value.shader_module.reference_count--;
    } else if (slot->type == RIN_GPU_OBJECT_GRAPHICS_PIPELINE) {
        RinGpuObjectSlot* vertex_shader;
        RinGpuObjectSlot* fragment_shader;
        if (slot->value.graphics_pipeline.reference_count != 0u) {
            return RIN_GPU_ERROR_BUSY;
        }
        result = ringpu_slot_retained(
            core, slot->value.graphics_pipeline.vertex_shader,
            RIN_GPU_OBJECT_SHADER_MODULE, NULL, &vertex_shader);
        if (result != RIN_GPU_OK ||
            vertex_shader->value.shader_module.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        result = ringpu_slot_retained(
            core, slot->value.graphics_pipeline.fragment_shader,
            RIN_GPU_OBJECT_SHADER_MODULE, NULL, &fragment_shader);
        if (result != RIN_GPU_OK ||
            fragment_shader->value.shader_module.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        core->backend.destroy_graphics_pipeline(
            core->backend_context,
            slot->value.graphics_pipeline.backend_cookie);
        vertex_shader->value.shader_module.reference_count--;
        fragment_shader->value.shader_module.reference_count--;
    } else if (slot->type == RIN_GPU_OBJECT_COMPUTE_BIND_GROUP) {
        RinGpuObjectSlot* pipeline;
        RinGpuObjectSlot* buffers[RIN_SHADER_MAX_RESOURCES] = {0};
        RinGpuBufferBindingV1* bindings =
            slot->value.compute_bind_group.bindings;
        uint32_t binding_count =
            slot->value.compute_bind_group.binding_count;
        uint64_t backend_cookie =
            slot->value.compute_bind_group.backend_cookie;
        if (slot->value.compute_bind_group.reference_count != 0u) {
            return RIN_GPU_ERROR_BUSY;
        }
        if (binding_count > RIN_SHADER_MAX_RESOURCES ||
            (binding_count != 0u && !bindings)) {
            return RIN_GPU_ERROR_STATE;
        }
        result = ringpu_slot_retained(
            core, slot->value.compute_bind_group.pipeline,
            RIN_GPU_OBJECT_COMPUTE_PIPELINE, NULL, &pipeline);
        if (result != RIN_GPU_OK ||
            pipeline->value.compute_pipeline.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        for (uint32_t binding = 0u; binding < binding_count; binding++) {
            RinGpuObjectSlot* buffer;
            result = ringpu_slot_retained(core, bindings[binding].buffer,
                                          RIN_GPU_OBJECT_BUFFER, NULL,
                                          &buffer);
            if (result != RIN_GPU_OK ||
                buffer->value.buffer.reference_count == 0u) {
                return RIN_GPU_ERROR_STATE;
            }
            buffers[binding] = buffer;
        }
        core->backend.destroy_compute_bind_group(core->backend_context,
                                                 backend_cookie);
        pipeline->value.compute_pipeline.reference_count--;
        for (uint32_t binding = 0u; binding < binding_count; binding++) {
            buffers[binding]->value.buffer.reference_count--;
        }
        free(bindings);
    } else if (slot->type == RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP) {
        RinGpuObjectSlot* pipeline;
        RinGpuObjectSlot* resources[RIN_SHADER_MAX_RESOURCES] = {0};
        uint16_t resource_types[RIN_SHADER_MAX_RESOURCES] = {0};
        RinGpuGraphicsBindingV1* bindings =
            slot->value.graphics_bind_group.bindings;
        uint32_t binding_count =
            slot->value.graphics_bind_group.binding_count;
        uint64_t backend_cookie =
            slot->value.graphics_bind_group.backend_cookie;
        if (slot->value.graphics_bind_group.reference_count != 0u) {
            return RIN_GPU_ERROR_BUSY;
        }
        if (binding_count > RIN_SHADER_MAX_RESOURCES ||
            (binding_count != 0u && !bindings)) {
            return RIN_GPU_ERROR_STATE;
        }
        result = ringpu_slot_retained(
            core, slot->value.graphics_bind_group.pipeline,
            RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
        if (result != RIN_GPU_OK ||
            pipeline->value.graphics_pipeline.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        for (uint32_t binding = 0u; binding < binding_count; binding++) {
            const RinGpuGraphicsBindingV1* resource_binding =
                &bindings[binding];
            RinGpuObjectSlot* resource;
            uint32_t references;
            result = ringpu_graphics_binding_slot_retained(
                core, resource_binding, &resource);
            if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
            if (resource_binding->kind ==
                RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
                references = resource->value.buffer.reference_count;
            } else if (ringpu_graphics_image_kind(
                           resource_binding->kind)) {
                references = resource->value.image.reference_count;
            } else {
                references = resource->value.sampler.reference_count;
            }
            if (references == 0u) {
                return RIN_GPU_ERROR_STATE;
            }
            resources[binding] = resource;
            resource_types[binding] = resource->type;
        }
        core->backend.destroy_graphics_bind_group(core->backend_context,
                                                  backend_cookie);
        pipeline->value.graphics_pipeline.reference_count--;
        for (uint32_t binding = 0u; binding < binding_count; binding++) {
            if (resource_types[binding] == RIN_GPU_OBJECT_BUFFER) {
                resources[binding]->value.buffer.reference_count--;
            } else if (resource_types[binding] == RIN_GPU_OBJECT_IMAGE) {
                resources[binding]->value.image.reference_count--;
            } else {
                resources[binding]->value.sampler.reference_count--;
            }
        }
        free(bindings);
    } else if (slot->type == RIN_GPU_OBJECT_COMMAND_LIST) {
        ringpu_release_command_references(core, slot);
        free(slot->value.command_list.commands);
    } else if (slot->type == RIN_GPU_OBJECT_QUERY) {
        if (core->backend.destroy_query != NULL) {
            core->backend.destroy_query(core->backend_context, object);
        }
    }
    ringpu_release_slot(slot);
    return RIN_GPU_OK;
}

static uint32_t ringpu_slot_reference_count(const RinGpuObjectSlot* slot)
{
    if (!slot || !slot->occupied) return 0u;
    switch (slot->type) {
    case RIN_GPU_OBJECT_BUFFER: return slot->value.buffer.reference_count;
    case RIN_GPU_OBJECT_IMAGE: return slot->value.image.reference_count;
    case RIN_GPU_OBJECT_MEMORY: return slot->value.memory.reference_count;
    case RIN_GPU_OBJECT_SAMPLER: return slot->value.sampler.reference_count;
    case RIN_GPU_OBJECT_SHADER_MODULE:
        return slot->value.shader_module.reference_count;
    case RIN_GPU_OBJECT_COMPUTE_PIPELINE:
        return slot->value.compute_pipeline.reference_count;
    case RIN_GPU_OBJECT_GRAPHICS_PIPELINE:
        return slot->value.graphics_pipeline.reference_count;
    case RIN_GPU_OBJECT_COMPUTE_BIND_GROUP:
        return slot->value.compute_bind_group.reference_count;
    case RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP:
        return slot->value.graphics_bind_group.reference_count;
    case RIN_GPU_OBJECT_QUERY: return slot->value.query.reference_count;
    default: return 0u;
    }
}

static RinGpuHandle ringpu_handle_for_slot(const RinGpuCore* core,
                                           uint32_t index,
                                           const RinGpuObjectSlot* slot)
{
    uint64_t raw = ((uint64_t)slot->generation << 32u) |
                   ((uint64_t)slot->type << 16u) |
                   (uint64_t)(index + 1u);
    return raw ^ core->handle_secret;
}

int ringpu_collect_deferred(RinGpuCore* core)
{
    int result = ringpu_core_ready(core);
    int made_progress;

    if (result != RIN_GPU_OK) return result;
    do {
        made_progress = 0;
        for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
            RinGpuObjectSlot* slot = &core->objects[index];
            RinGpuHandle handle;

            if (!slot->occupied || slot->destroy_pending == 0u ||
                slot->last_use_serial > core->completed_serial ||
                ringpu_slot_reference_count(slot) != 0u) {
                continue;
            }
            handle = ringpu_handle_for_slot(core, index, slot);
            result = ringpu_destroy_slot(core, handle, slot);
            if (result == RIN_GPU_ERROR_BUSY) continue;
            if (result != RIN_GPU_OK) return result;
            made_progress = 1;
        }
    } while (made_progress);
    return RIN_GPU_OK;
}

int ringpu_destroy(RinGpuCore* core, RinGpuHandle object)
{
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, object, RIN_GPU_OBJECT_NONE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if (slot->last_use_serial > core->completed_serial) {
        slot->destroy_pending = 1u;
        return RIN_GPU_OK;
    }
    if (ringpu_slot_reference_count(slot) != 0u)
        return RIN_GPU_ERROR_BUSY;
    result = ringpu_destroy_slot(core, object, slot);
    if (result != RIN_GPU_OK) return result;
    return ringpu_collect_deferred(core);
}
