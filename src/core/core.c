// SPDX-License-Identifier: MIT
#include "core.h"
#include "../command/record.h"
#include "object_table.h"
#include "../validation/graphics_layout.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"
#include "../validation/shader.h"
#include "../sync/barriers.h"
#include "../sync/fences.h"
#include "../presentation/present.h"
#include "../resource/resources.h"
#include "../shader/modules.h"
#include "../pipeline/pipelines.h"
#include "../command/commands.h"

#include <stdlib.h>
#include <string.h>

int ringpu_core_ready(const RinGpuCore* core) {
    if (!core || !core->initialized) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (core->device_lost) return RIN_GPU_ERROR_DEVICE_LOST;
    return RIN_GPU_OK;
}

void ringpu_core_diagnostic(RinGpuCore* core, uint32_t type,
                            uint64_t resource_cookie,
                            uint64_t queue_cookie, uint64_t value0,
                            uint64_t value1, int status) {
    if (!core || !core->diagnostics) return;
    (void)rin_gpu_diagnostics_record_simple(
        core->diagnostics, type, 0u, resource_cookie, queue_cookie, value0,
        value1, (uint32_t)status);
}

static int ringpu_core_diagnostics_valid(
    RinGpuDiagnosticsRuntime* diagnostics) {
    RinGpuDiagnosticsStatsV1 stats;
    if (!diagnostics) return 1;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    stats.version = RIN_GPU_DIAGNOSTICS_VERSION;
    return rin_gpu_diagnostics_get_stats(diagnostics, &stats) == 0 &&
           stats.device_generation != 0u;
}

uint64_t ringpu_core_device_generation(
    const RinGpuDiagnosticsRuntime* diagnostics) {
    RinGpuDiagnosticsStatsV1 stats;
    if (!diagnostics) return 0u;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    stats.version = RIN_GPU_DIAGNOSTICS_VERSION;
    if (rin_gpu_diagnostics_get_stats(
            (RinGpuDiagnosticsRuntime*)diagnostics, &stats) != 0)
        return 0u;
    return stats.device_generation;
}

static int ringpu_fixed_name_valid(const char* name, uint32_t capacity) {
    uint32_t length = 0u;
    uint32_t index = 0u;

    if (!name || capacity == 0u || name[0] == '\0') return 0;
    while (length < capacity && name[length] != '\0') ++length;
    if (length == capacity) return 0;
    for (uint32_t tail = length + 1u; tail < capacity; ++tail)
        if (name[tail] != '\0') return 0;

    while (index < length) {
        unsigned first = (unsigned char)name[index];
        unsigned code_point;
        uint32_t byte_count;

        if (first < 0x80u) {
            code_point = first;
            byte_count = 1u;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            code_point = first & 0x1Fu;
            byte_count = 2u;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            code_point = first & 0x0Fu;
            byte_count = 3u;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            code_point = first & 0x07u;
            byte_count = 4u;
        } else {
            return 0;
        }
        if (byte_count > length - index) return 0;
        for (uint32_t offset = 1u; offset < byte_count; ++offset) {
            unsigned continuation = (unsigned char)name[index + offset];
            if ((continuation & 0xC0u) != 0x80u) return 0;
            code_point = (code_point << 6u) | (continuation & 0x3Fu);
        }
        if ((byte_count == 2u && code_point < 0x80u) ||
            (byte_count == 3u && code_point < 0x800u) ||
            (byte_count == 4u && code_point < 0x10000u) ||
            code_point > 0x10FFFFu ||
            (code_point >= 0xD800u && code_point <= 0xDFFFu) ||
            code_point < 0x20u || code_point == 0x7Fu) {
            return 0;
        }
        index += byte_count;
    }
    return 1;
}

static int ringpu_display_info_valid(
    const RinGpuCoreConfigV1* config, const RinGpuDisplayInfoV1* display) {
    if (!config || !display ||
        display->abi_version != RIN_GPU_ABI_VERSION ||
        display->struct_size != sizeof(*display) ||
        display->flags == 0u ||
        (display->flags & ~RIN_GPU_DISPLAY_KNOWN_FLAGS) != 0u ||
        (display->flags & RIN_GPU_DISPLAY_CONNECTED) == 0u ||
        display->width == 0u || display->height == 0u ||
        display->width > config->max_image_dimension ||
        display->height > config->max_image_dimension ||
        display->refresh_millihertz <
            RIN_GPU_DISPLAY_MIN_REFRESH_MILLIHERTZ ||
        display->refresh_millihertz >
            RIN_GPU_DISPLAY_MAX_REFRESH_MILLIHERTZ ||
        !ringpu_scanout_format(display->format) ||
        ((display->physical_width_mm == 0u) !=
         (display->physical_height_mm == 0u)) ||
        display->scale_milli < RIN_GPU_DISPLAY_MIN_SCALE_MILLI ||
        display->scale_milli > RIN_GPU_DISPLAY_MAX_SCALE_MILLI ||
        display->reserved0 != 0u ||
        !ringpu_fixed_name_valid(display->name,
                                 RIN_GPU_DISPLAY_NAME_MAX)) {
        return 0;
    }
    if (display->display_id == RIN_GPU_PRIMARY_DISPLAY) {
        return (display->flags & RIN_GPU_DISPLAY_PRIMARY) != 0u;
    }
    return (display->flags & RIN_GPU_DISPLAY_PRIMARY) == 0u;
}

static void ringpu_release_graphics_bind_group_reference(
    RinGpuCore* core, RinGpuHandle bind_group) {
    RinGpuObjectSlot* slot;
    if (bind_group != 0u &&
        ringpu_slot(core, bind_group, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP,
                    NULL, &slot) == RIN_GPU_OK &&
        slot->value.graphics_bind_group.reference_count != 0u) {
        slot->value.graphics_bind_group.reference_count--;
    }
}

static int ringpu_graphics_image_kind(uint32_t kind) {
    return kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
           kind == RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE;
}

static int ringpu_graphics_sampler_kind(uint32_t kind) {
    return kind == RIN_SHADER_RESOURCE_SAMPLER ||
           kind == RIN_SHADER_RESOURCE_COMPARISON_SAMPLER;
}

static uint32_t ringpu_graphics_binding_mip_count(
    const RinGpuGraphicsBindingV1* binding, const RinGpuImageDescV1* desc);

static int ringpu_graphics_binding_slot(
    RinGpuCore* core, const RinGpuGraphicsBindingV1* binding,
    RinGpuObjectSlot** slot) {
    uint16_t type;
    if (!core || !binding || !slot) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
        type = RIN_GPU_OBJECT_BUFFER;
    } else if (ringpu_graphics_image_kind(binding->kind)) {
        type = RIN_GPU_OBJECT_IMAGE;
    } else if (ringpu_graphics_sampler_kind(binding->kind)) {
        type = RIN_GPU_OBJECT_SAMPLER;
    } else {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_slot(core, binding->resource, type, NULL, slot);
}

static int ringpu_graphics_binding_reference(
    RinGpuCore* core, const RinGpuGraphicsBindingV1* binding, int acquire) {
    RinGpuObjectSlot* slot;
    uint32_t* references;
    int result = ringpu_graphics_binding_slot(core, binding, &slot);
    if (result != RIN_GPU_OK) return result;
    if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
        references = &slot->value.buffer.reference_count;
    } else if (ringpu_graphics_image_kind(binding->kind)) {
        references = &slot->value.image.reference_count;
    } else {
        references = &slot->value.sampler.reference_count;
    }
    if (acquire) {
        if (*references == UINT32_MAX) return RIN_GPU_ERROR_LIMIT;
        (*references)++;
    } else {
        if (*references == 0u) return RIN_GPU_ERROR_STATE;
        (*references)--;
    }
    return RIN_GPU_OK;
}

int ringpu_core_init(RinGpuCore* core, const RinGpuCoreConfigV1* config) {
    uint32_t primary_count = 0u;
    if (!core || !config ||
        !ringpu_versioned(config->abi_version, config->struct_size,
                          sizeof(*config)) ||
        !ringpu_versioned(config->adapter.abi_version,
                          config->adapter.struct_size,
                          sizeof(config->adapter)) ||
        !ringpu_versioned(config->backend.abi_version,
                          config->backend.struct_size,
                          sizeof(config->backend)) ||
        config->handle_secret == 0u || config->max_buffer_size == 0u ||
        config->max_image_size == 0u ||
        config->max_total_allocation_size == 0u ||
        config->max_buffer_size > config->max_total_allocation_size ||
        config->max_image_size > config->max_total_allocation_size ||
        config->max_image_dimension == 0u ||
        config->max_image_layers == 0u ||
        config->max_image_mip_levels == 0u ||
        config->max_image_sample_count == 0u ||
        (config->max_image_sample_count &
         (config->max_image_sample_count - 1u)) != 0u ||
        !config->backend.create_buffer || !config->backend.destroy_buffer ||
        !config->backend.create_image || !config->backend.destroy_image ||
        !config->backend.create_sampler || !config->backend.destroy_sampler ||
        !config->backend.create_shader_module ||
        !config->backend.destroy_shader_module ||
        !config->backend.create_compute_pipeline ||
        !config->backend.destroy_compute_pipeline ||
        !config->backend.create_graphics_pipeline ||
        !config->backend.destroy_graphics_pipeline ||
        !config->backend.create_compute_bind_group ||
        !config->backend.destroy_compute_bind_group ||
        !config->backend.create_graphics_bind_group ||
        !config->backend.destroy_graphics_bind_group ||
        !config->backend.submit_commands ||
        !config->displays || config->display_count == 0u ||
        config->display_count > RIN_GPU_MAX_DISPLAYS ||
        config->reserved0 != 0u ||
        config->backend_family > RIN_GPU_BACKEND_FAMILY_VIRTIO ||
        config->reserved1 != 0u ||
        !ringpu_core_diagnostics_valid(config->diagnostics) ||
        !ringpu_fixed_name_valid(config->adapter.name,
                                 RIN_GPU_ADAPTER_NAME_MAX) ||
        (config->adapter.queue_capabilities &
         ~RIN_GPU_QUEUE_KNOWN_CAPABILITIES) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0u; index < config->display_count; index++) {
        if (!ringpu_display_info_valid(config, &config->displays[index])) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        if ((config->displays[index].flags & RIN_GPU_DISPLAY_PRIMARY) != 0u) {
            primary_count++;
        }
        for (uint32_t prior = 0u; prior < index; prior++) {
            if (config->displays[prior].display_id ==
                config->displays[index].display_id) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (primary_count != 1u) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(core, 0, sizeof(*core));
    core->handle_secret = config->handle_secret;
    core->max_buffer_size = config->max_buffer_size;
    core->max_image_size = config->max_image_size;
    core->max_total_allocation_size = config->max_total_allocation_size;
    core->max_image_dimension = config->max_image_dimension;
    core->max_image_layers = config->max_image_layers;
    core->max_image_mip_levels = config->max_image_mip_levels;
    core->max_image_sample_count = config->max_image_sample_count;
    core->adapter = config->adapter;
    memcpy(core->displays, config->displays,
           (size_t)config->display_count * sizeof(core->displays[0]));
    core->display_count = config->display_count;
    core->backend = config->backend;
    core->backend_context = config->backend_context;
    core->diagnostics = config->diagnostics;
    core->device_generation =
        ringpu_core_device_generation(config->diagnostics);
    core->backend_family = config->backend_family;
    core->initialized = 1u;
    return RIN_GPU_OK;
}

void ringpu_core_shutdown(RinGpuCore* core) {
    if (!core || !core->initialized) return;
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        if (!slot->occupied || slot->type != RIN_GPU_OBJECT_COMMAND_LIST) continue;
        ringpu_release_command_references(core, slot);
    }
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        RinGpuObjectSlot* pipeline;
        if (!slot->occupied ||
            slot->type != RIN_GPU_OBJECT_COMPUTE_BIND_GROUP) {
            continue;
        }
        core->backend.destroy_compute_bind_group(
            core->backend_context,
            slot->value.compute_bind_group.backend_cookie);
        if (ringpu_slot(core, slot->value.compute_bind_group.pipeline,
                        RIN_GPU_OBJECT_COMPUTE_PIPELINE, NULL, &pipeline) ==
                RIN_GPU_OK &&
            pipeline->value.compute_pipeline.reference_count != 0u) {
            pipeline->value.compute_pipeline.reference_count--;
        }
        for (uint32_t binding = 0u;
             binding < slot->value.compute_bind_group.binding_count;
             binding++) {
            RinGpuObjectSlot* buffer;
            if (ringpu_slot(
                    core,
                    slot->value.compute_bind_group.bindings[binding].buffer,
                    RIN_GPU_OBJECT_BUFFER, NULL, &buffer) == RIN_GPU_OK &&
                buffer->value.buffer.reference_count != 0u) {
                buffer->value.buffer.reference_count--;
            }
        }
        free(slot->value.compute_bind_group.bindings);
        ringpu_release_slot(slot);
    }
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        RinGpuObjectSlot* pipeline;
        if (!slot->occupied ||
            slot->type != RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP) {
            continue;
        }
        core->backend.destroy_graphics_bind_group(
            core->backend_context,
            slot->value.graphics_bind_group.backend_cookie);
        if (ringpu_slot(core, slot->value.graphics_bind_group.pipeline,
                        RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline) ==
                RIN_GPU_OK &&
            pipeline->value.graphics_pipeline.reference_count != 0u) {
            pipeline->value.graphics_pipeline.reference_count--;
        }
        for (uint32_t binding = 0u;
             binding < slot->value.graphics_bind_group.binding_count;
             binding++) {
            (void)ringpu_graphics_binding_reference(
                core, &slot->value.graphics_bind_group.bindings[binding], 0);
        }
        free(slot->value.graphics_bind_group.bindings);
        ringpu_release_slot(slot);
    }
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        RinGpuObjectSlot* vertex_shader;
        RinGpuObjectSlot* fragment_shader;
        if (!slot->occupied ||
            slot->type != RIN_GPU_OBJECT_GRAPHICS_PIPELINE) {
            continue;
        }
        core->backend.destroy_graphics_pipeline(
            core->backend_context,
            slot->value.graphics_pipeline.backend_cookie);
        if (ringpu_slot(core, slot->value.graphics_pipeline.vertex_shader,
                        RIN_GPU_OBJECT_SHADER_MODULE, NULL, &vertex_shader) ==
                RIN_GPU_OK &&
            vertex_shader->value.shader_module.reference_count != 0u) {
            vertex_shader->value.shader_module.reference_count--;
        }
        if (ringpu_slot(core, slot->value.graphics_pipeline.fragment_shader,
                        RIN_GPU_OBJECT_SHADER_MODULE, NULL,
                        &fragment_shader) == RIN_GPU_OK &&
            fragment_shader->value.shader_module.reference_count != 0u) {
            fragment_shader->value.shader_module.reference_count--;
        }
        ringpu_release_slot(slot);
    }
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        RinGpuObjectSlot* shader;
        if (!slot->occupied ||
            slot->type != RIN_GPU_OBJECT_COMPUTE_PIPELINE) {
            continue;
        }
        core->backend.destroy_compute_pipeline(
            core->backend_context,
            slot->value.compute_pipeline.backend_cookie);
        if (ringpu_slot(core, slot->value.compute_pipeline.shader_module,
                        RIN_GPU_OBJECT_SHADER_MODULE, NULL, &shader) ==
                RIN_GPU_OK &&
            shader->value.shader_module.reference_count != 0u) {
            shader->value.shader_module.reference_count--;
        }
        ringpu_release_slot(slot);
    }
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        if (!slot->occupied) continue;
        if (slot->type == RIN_GPU_OBJECT_COMMAND_LIST) {
            free(slot->value.command_list.commands);
        } else if (slot->type == RIN_GPU_OBJECT_BUFFER) {
            core->backend.destroy_buffer(core->backend_context,
                                         slot->value.buffer.backend_cookie);
        } else if (slot->type == RIN_GPU_OBJECT_IMAGE) {
            core->backend.destroy_image(core->backend_context,
                                        slot->value.image.backend_cookie);
            free(slot->value.image.subresource_states);
            free(slot->value.image.cpu_upload_complete);
        } else if (slot->type == RIN_GPU_OBJECT_SAMPLER) {
            core->backend.destroy_sampler(core->backend_context,
                                          slot->value.sampler.backend_cookie);
        } else if (slot->type == RIN_GPU_OBJECT_SHADER_MODULE) {
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
        }
    }
    memset(core, 0, sizeof(*core));
}

void ringpu_core_mark_device_lost(RinGpuCore* core) {
    if (core && core->initialized) {
        core->device_lost = 1u;
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_DEVICE_LOST, 0u, 0u,
                               0u, 0u, RIN_GPU_ERROR_DEVICE_LOST);
    }
}

int ringpu_get_device_generation(const RinGpuCore* core,
                                 uint64_t* generation_out) {
    uint64_t generation;

    if (!core || !core->initialized || !generation_out)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    generation = ringpu_core_device_generation(core->diagnostics);
    if (generation == 0u) generation = core->device_generation;
    if (generation == 0u) return RIN_GPU_ERROR_STATE;
    *generation_out = generation;
    return RIN_GPU_OK;
}

int ringpu_get_adapter_info(const RinGpuCore* core, RinGpuAdapterInfoV1* info) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!info) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *info = core->adapter;
    return RIN_GPU_OK;
}

int ringpu_get_display_count(const RinGpuCore* core, uint32_t* count) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!count) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *count = core->display_count;
    return RIN_GPU_OK;
}

int ringpu_get_display_info(const RinGpuCore* core, uint32_t index,
                            RinGpuDisplayInfoV1* info) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!info) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (index >= core->display_count) return RIN_GPU_ERROR_BOUNDS;
    *info = core->displays[index];
    return RIN_GPU_OK;
}


int ringpu_create_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuBufferBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group) {
    RinGpuGraphicsBindingV1* typed = NULL;
    int result;
    if (binding_count > RIN_SHADER_MAX_RESOURCES ||
        (binding_count == 0u && bindings != NULL) ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (binding_count != 0u) {
        typed = (RinGpuGraphicsBindingV1*)calloc(binding_count,
                                                  sizeof(*typed));
        if (!typed) return RIN_GPU_ERROR_NO_MEMORY;
    }
    for (uint32_t index = 0u; index < binding_count; index++) {
        const RinGpuBufferBindingV1* source = &bindings[index];
        if (!ringpu_versioned(source->abi_version, source->struct_size,
                              sizeof(*source))) {
            free(typed);
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        typed[index].abi_version = source->abi_version;
        typed[index].struct_size = sizeof(typed[index]);
        typed[index].binding = source->binding;
        typed[index].kind = RIN_SHADER_RESOURCE_STORAGE_BUFFER;
        typed[index].access = source->access;
        typed[index].flags = source->flags;
        typed[index].resource = source->buffer;
        typed[index].offset = source->offset;
        typed[index].size_bytes = source->size_bytes;
        typed[index].reserved0 = source->reserved;
    }
    result = ringpu_create_graphics_bind_group_typed(
        core, pipeline, typed, binding_count, bind_group);
    free(typed);
    return result;
}

int ringpu_create_graphics_bind_group_typed(
    RinGpuCore* core, RinGpuHandle pipeline,
    const RinGpuGraphicsBindingV1* bindings, uint32_t binding_count,
    RinGpuHandle* bind_group) {
    RinGpuObjectSlot* pipeline_slot;
    RinGpuObjectSlot* slot;
    RinGpuGraphicsBindingV1* snapshot = NULL;
    RinGpuBackendGraphicsBindingV1* backend_bindings = NULL;
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
    result = ringpu_slot(core, pipeline, RIN_GPU_OBJECT_GRAPHICS_PIPELINE,
                         NULL, &pipeline_slot);
    if (result != RIN_GPU_OK) return result;
    if (binding_count == 0u ||
        binding_count !=
            pipeline_slot->value.graphics_pipeline.resource_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    snapshot = (RinGpuGraphicsBindingV1*)calloc(binding_count,
                                                 sizeof(*snapshot));
    backend_bindings = (RinGpuBackendGraphicsBindingV1*)calloc(
        binding_count, sizeof(*backend_bindings));
    if (!snapshot || !backend_bindings) {
        result = RIN_GPU_ERROR_NO_MEMORY;
        goto fail;
    }
    for (uint32_t index = 0u; index < binding_count; index++) {
        const RinGpuGraphicsBindingV1* binding = &bindings[index];
        RinGpuObjectSlot* resource;
        if (!ringpu_versioned(binding->abi_version, binding->struct_size,
                              sizeof(*binding)) ||
            binding->binding >= binding_count ||
            (seen & (UINT64_C(1) << binding->binding)) != 0u ||
            binding->kind != pipeline_slot->value.graphics_pipeline
                .resource_kinds[binding->binding] ||
            binding->access != pipeline_slot->value.graphics_pipeline
                .resource_access[binding->binding] ||
            (binding->flags & ~RIN_GPU_GRAPHICS_BINDING_KNOWN_FLAGS) != 0u ||
            binding->reserved0 != 0u ||
            binding->reserved1 != 0u) {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        result = ringpu_graphics_binding_slot(core, binding, &resource);
        if (result != RIN_GPU_OK) goto fail;
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            if (binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->mip_level != 0u || binding->array_layer != 0u ||
                (binding->offset & 3u) != 0u ||
                (binding->size_bytes & 3u) != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            if ((resource->value.buffer.usage & RIN_GPU_BUFFER_STORAGE) == 0u ||
                !ringpu_buffer_upload_ready(resource)) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
            if (!ringpu_range(binding->offset, binding->size_bytes,
                              resource->value.buffer.size_bytes)) {
                result = RIN_GPU_ERROR_BOUNDS;
                goto fail;
            }
            backend_bindings[binding->binding].offset = binding->offset;
            backend_bindings[binding->binding].size_bytes =
                binding->size_bytes;
        } else if (ringpu_graphics_image_kind(binding->kind)) {
            const RinGpuImageDescV1* desc = &resource->value.image.descriptor;
            if (binding->access != RIN_GPU_RESOURCE_READ ||
                binding->offset != 0u || binding->size_bytes != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            if ((desc->usage & RIN_GPU_IMAGE_SAMPLED) == 0u ||
                desc->sample_count != 1u ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE
                     ? (!ringpu_sampled_image_format(desc->format) &&
                        !ringpu_depth_stencil_format(desc->format))
                     : desc->format != RIN_GPU_FORMAT_D32_FLOAT)) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
            if (binding->mip_level >= desc->mip_levels ||
                binding->array_layer >= desc->array_layers) {
                result = RIN_GPU_ERROR_BOUNDS;
                goto fail;
            }
            if ((binding->flags != 0u &&
                 (binding->kind != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
                  binding->flags !=
                      RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN)) ||
                ringpu_graphics_binding_mip_count(binding, desc) == 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            backend_bindings[binding->binding].mip_level =
                binding->mip_level;
            backend_bindings[binding->binding].array_layer =
                binding->array_layer;
        } else if (ringpu_graphics_sampler_kind(binding->kind)) {
            if (binding->access != 0u || binding->offset != 0u ||
                binding->size_bytes != 0u || binding->mip_level != 0u ||
                binding->array_layer != 0u || binding->flags != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
            if (binding->kind == RIN_SHADER_RESOURCE_SAMPLER
                    ? resource->value.sampler.descriptor.compare_op != 0u
                    : resource->value.sampler.descriptor.compare_op == 0u) {
                result = RIN_GPU_ERROR_STATE;
                goto fail;
            }
        } else {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        seen |= UINT64_C(1) << binding->binding;
        snapshot[binding->binding] = *binding;
        snapshot[binding->binding].struct_size = sizeof(*snapshot);
        backend_bindings[binding->binding].binding = binding->binding;
        backend_bindings[binding->binding].kind = binding->kind;
        backend_bindings[binding->binding].access = binding->access;
        backend_bindings[binding->binding].flags = binding->flags;
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            backend_bindings[binding->binding].resource_cookie =
                resource->value.buffer.backend_cookie;
        } else if (ringpu_graphics_image_kind(binding->kind)) {
            backend_bindings[binding->binding].resource_cookie =
                resource->value.image.backend_cookie;
        } else {
            backend_bindings[binding->binding].resource_cookie =
                resource->value.sampler.backend_cookie;
        }
    }
    for (uint32_t current = 0u; current < binding_count; current++) {
        for (uint32_t prior = 0u; prior < current; prior++) {
            if (snapshot[current].kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                snapshot[prior].kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                snapshot[current].resource == snapshot[prior].resource &&
                snapshot[current].offset <
                    snapshot[prior].offset + snapshot[prior].size_bytes &&
                snapshot[prior].offset <
                    snapshot[current].offset + snapshot[current].size_bytes &&
                ((snapshot[current].access | snapshot[prior].access) &
                 RIN_GPU_RESOURCE_WRITE) != 0u) {
                result = RIN_GPU_ERROR_INVALID_ARGUMENT;
                goto fail;
            }
        }
    }
    result = core->backend.create_graphics_bind_group(
        core->backend_context,
        pipeline_slot->value.graphics_pipeline.backend_cookie,
        backend_bindings, binding_count, &cookie);
    if (result != RIN_GPU_OK) goto fail;
    result = ringpu_allocate(core, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP,
                             bind_group, &slot);
    if (result != RIN_GPU_OK) {
        core->backend.destroy_graphics_bind_group(core->backend_context,
                                                  cookie);
        goto fail;
    }
    slot->value.graphics_bind_group.pipeline = pipeline;
    slot->value.graphics_bind_group.backend_cookie = cookie;
    slot->value.graphics_bind_group.bindings = snapshot;
    slot->value.graphics_bind_group.binding_count = binding_count;
    pipeline_slot->value.graphics_pipeline.reference_count++;
    for (uint32_t index = 0u; index < binding_count; index++) {
        (void)ringpu_graphics_binding_reference(core, &snapshot[index], 1);
    }
    free(backend_bindings);
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_RESOURCE_CREATE, cookie,
                           0u, RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP,
                           binding_count, RIN_GPU_OK);
    return RIN_GPU_OK;

fail:
    free(snapshot);
    free(backend_bindings);
    return result;
}

int ringpu_create_queue(RinGpuCore* core, const RinGpuQueueDescV1* desc,
                        RinGpuHandle* queue) {
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !queue ||
        !ringpu_versioned(desc->abi_version, desc->struct_size, sizeof(*desc)) ||
        desc->capabilities == 0u ||
        (desc->capabilities & ~core->adapter.queue_capabilities) != 0u ||
        desc->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_allocate(core, RIN_GPU_OBJECT_QUEUE, queue, &slot);
    if (result == RIN_GPU_OK) {
        slot->value.queue.capabilities = desc->capabilities;
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_QUEUE, *queue, *queue,
                               desc->capabilities, 0u, RIN_GPU_OK);
    }
    return result;
}

int ringpu_create_command_list(RinGpuCore* core,
                               const RinGpuCommandListDescV1* desc,
                               RinGpuHandle* command_list) {
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !command_list ||
        !ringpu_versioned(desc->abi_version, desc->struct_size, sizeof(*desc)) ||
        desc->capabilities == 0u ||
        (desc->capabilities & ~core->adapter.queue_capabilities) != 0u ||
        desc->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_allocate(core, RIN_GPU_OBJECT_COMMAND_LIST, command_list,
                             &slot);
    if (result == RIN_GPU_OK) {
        slot->value.command_list.capabilities = desc->capabilities;
        slot->value.command_list.state = RIN_GPU_COMMAND_RECORDING;
    }
    return result;
}

int ringpu_command_list_reset(RinGpuCore* core, RinGpuHandle command_list) {
    RinGpuObjectSlot* list;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    ringpu_release_command_references(core, list);
    list->value.command_list.state = RIN_GPU_COMMAND_RECORDING;
    return RIN_GPU_OK;
}

static int ringpu_render_color_clear_valid(uint32_t load_op, float red,
                                            float green, float blue,
                                            float alpha,
                                            uint32_t color_write_mask) {
    if ((color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u)
        return 0;
    if (load_op == RIN_GPU_RENDER_LOAD) {
        return red == 0.0f && green == 0.0f && blue == 0.0f &&
               alpha == 0.0f && color_write_mask == 0u;
    }
    /* Float color targets retain finite values outside [0, 1]. Fixed-point
     * backends quantize/clamp when they store the pass clear; keeping the
     * command value intact avoids losing Float32 render-target semantics. */
    return load_op == RIN_GPU_RENDER_CLEAR && ringpu_finite_float(red) &&
           ringpu_finite_float(green) && ringpu_finite_float(blue) &&
           ringpu_finite_float(alpha);
}

static int ringpu_render_color_clear_valid_for_format(
    uint32_t format, uint32_t load_op, float red, float green, float blue,
    float alpha, uint32_t color_write_mask) {
    if (!ringpu_render_color_clear_valid(load_op, red, green, blue, alpha,
                                         color_write_mask)) {
        return 0;
    }
    return load_op != RIN_GPU_RENDER_CLEAR ||
        format == RIN_GPU_FORMAT_RGBA16_FLOAT ||
        format == RIN_GPU_FORMAT_RGBA32_FLOAT ||
        (red >= 0.0f && red <= 1.0f && green >= 0.0f && green <= 1.0f &&
         blue >= 0.0f && blue <= 1.0f && alpha >= 0.0f && alpha <= 1.0f);
}

static int ringpu_clear_region_valid(const RinGpuClearRegionV1* region,
                                     uint32_t width, uint32_t height) {
    if (!region || region->enabled > 1u || region->reserved != 0u)
        return 0;
    if (region->enabled == 0u) {
        return region->x == 0 && region->y == 0 && region->width == 0u &&
               region->height == 0u;
    }
    return region->x >= 0 && region->y >= 0 &&
           (uint32_t)region->x <= width && region->width <= width -
               (uint32_t)region->x &&
           (uint32_t)region->y <= height && region->height <= height -
               (uint32_t)region->y;
}

static int ringpu_render_clear_valid(const RinGpuRenderPassDescV1* pass) {
    return pass && ringpu_render_color_clear_valid(
        pass->load_op, pass->clear_red, pass->clear_green,
        pass->clear_blue, pass->clear_alpha, pass->color_write_mask);
}

static int ringpu_render_depth_clear_valid(uint32_t load_op, float depth) {
    if (load_op == RIN_GPU_RENDER_LOAD) return depth == 0.0f;
    return load_op == RIN_GPU_RENDER_CLEAR && depth >= 0.0f && depth <= 1.0f;
}

static int ringpu_render_stencil_clear_valid(uint32_t format,
                                             uint32_t load_op,
                                             uint32_t store_op,
                                             uint32_t clear_stencil,
                                             uint32_t write_mask) {
    if (format == RIN_GPU_FORMAT_D32_FLOAT) {
        return load_op == 0u && store_op == 0u && clear_stencil == 0u &&
               write_mask == 0u;
    }
    if ((format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
         format != RIN_GPU_FORMAT_S8_UINT) ||
        store_op != RIN_GPU_RENDER_STORE || clear_stencil > 0xffu ||
        write_mask > 0xffu) {
        return 0;
    }
    return load_op == RIN_GPU_RENDER_CLEAR ||
           (load_op == RIN_GPU_RENDER_LOAD && clear_stencil == 0u &&
            write_mask == 0u);
}

static uint32_t ringpu_mip_dimension(uint32_t dimension,
                                     uint32_t mip_level) {
    dimension >>= mip_level;
    return dimension == 0u ? 1u : dimension;
}

int ringpu_command_begin_render_pass(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDescV1* render_pass) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* color_target;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!render_pass ||
        !ringpu_versioned(render_pass->abi_version,
                          render_pass->struct_size, sizeof(*render_pass)) ||
        render_pass->store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_clear_valid(render_pass) ||
        render_pass->flags != 0u || render_pass->reserved != 0u ||
        render_pass->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, render_pass->color_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (render_pass->mip_level >= target_desc->mip_levels ||
        render_pass->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        !ringpu_color_format(target_desc->format) ||
        !ringpu_render_color_clear_valid_for_format(
            target_desc->format, render_pass->load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(target_desc->width, render_pass->mip_level),
            ringpu_mip_dimension(target_desc->height, render_pass->mip_level))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS;
    command->destination = render_pass->color_target;
    command->value.render_pass = *render_pass;
    command->value.render_pass.struct_size =
        sizeof(command->value.render_pass);
    list->value.command_list.count++;
    list->value.command_list.render_target = render_pass->color_target;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_color_targets[0] = render_pass->color_target;
    list->value.command_list.render_depth_target = 0u;
    list->value.command_list.render_stencil_target = 0u;
    list->value.command_list.render_mip_level = render_pass->mip_level;
    list->value.command_list.render_array_layer = render_pass->array_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.render_color_mip_levels[0] = render_pass->mip_level;
    list->value.command_list.render_color_array_layers[0] = render_pass->array_layer;
    list->value.command_list.active_color_mask = 1u;
    list->value.command_list.render_depth_mip_level = 0u;
    list->value.command_list.render_depth_array_layer = 0u;
    list->value.command_list.render_stencil_mip_level = 0u;
    list->value.command_list.render_stencil_array_layer = 0u;
    list->value.command_list.render_pass_active = 1u;
    color_target->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_begin_render_pass_mrt(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassMrtDescV1* render_pass)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* first_color = NULL;
    RinGpuObjectSlot* depth_target = NULL;
    RinGpuObjectSlot* stencil_target = NULL;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* first_desc = NULL;
    RinGpuHandle first_handle = 0u;
    uint32_t first_mip = 0u;
    uint32_t first_layer = 0u;
    uint32_t color_index;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK)
        return result;
    if (render_pass == NULL ||
        !ringpu_versioned(render_pass->abi_version, render_pass->struct_size,
                          sizeof(*render_pass)) ||
        render_pass->active_color_mask == 0u ||
        (render_pass->active_color_mask & ~((1u << RIN_GPU_MAX_COLOR_TARGETS) - 1u)) != 0u ||
        render_pass->color_store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_color_clear_valid(
            render_pass->color_load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        render_pass->flags != 0u || render_pass->reserved0 != 0u ||
        render_pass->reserved1 != 0u || render_pass->reserved2 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST,
                         NULL, &list);
    if (result != RIN_GPU_OK)
        return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u) {
        return RIN_GPU_ERROR_STATE;
    }

    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        const RinGpuColorAttachmentV1* attachment =
            &render_pass->color_attachments[color_index];
        RinGpuObjectSlot* color_target;
        const RinGpuImageDescV1* color_desc;

        if ((render_pass->active_color_mask & (1u << color_index)) == 0u) {
            if (attachment->target != 0u || attachment->mip_level != 0u ||
                attachment->array_layer != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            continue;
        }
        result = ringpu_slot(core, attachment->target, RIN_GPU_OBJECT_IMAGE,
                             NULL, &color_target);
        if (result != RIN_GPU_OK)
            return result;
        color_desc = &color_target->value.image.descriptor;
        if (attachment->mip_level >= color_desc->mip_levels ||
            attachment->array_layer >= color_desc->array_layers ||
            (color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
            color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
            color_desc->sample_count != 1u || !ringpu_color_format(color_desc->format) ||
            !ringpu_render_color_clear_valid_for_format(
                color_desc->format, render_pass->color_load_op,
                render_pass->clear_red, render_pass->clear_green,
                render_pass->clear_blue, render_pass->clear_alpha,
                render_pass->color_write_mask)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        if (first_color == NULL) {
            first_color = color_target;
            first_desc = color_desc;
            first_handle = attachment->target;
            first_mip = attachment->mip_level;
            first_layer = attachment->array_layer;
        } else if (color_desc->format != first_desc->format ||
                   ringpu_mip_dimension(color_desc->width,
                                        attachment->mip_level) !=
                       ringpu_mip_dimension(first_desc->width, first_mip) ||
                   ringpu_mip_dimension(color_desc->height,
                                        attachment->mip_level) !=
                       ringpu_mip_dimension(first_desc->height, first_mip)) {
            /* The current graphics-pipeline ABI has one color format. Do not
             * pretend mixed-format MRT is executable until that ABI grows the
             * corresponding per-target pipeline description. */
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
        for (uint32_t previous = 0u; previous < color_index; ++previous) {
            const RinGpuColorAttachmentV1* prior =
                &render_pass->color_attachments[previous];

            if ((render_pass->active_color_mask & (1u << previous)) != 0u &&
                prior->target == attachment->target &&
                prior->mip_level == attachment->mip_level &&
                prior->array_layer == attachment->array_layer) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (first_color == NULL || first_desc == NULL ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(first_desc->width, first_mip),
            ringpu_mip_dimension(first_desc->height, first_mip))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }

    if (render_pass->depth_target == 0u) {
        if (render_pass->depth_mip_level != 0u ||
            render_pass->depth_array_layer != 0u ||
            render_pass->depth_load_op != 0u || render_pass->depth_store_op != 0u ||
            render_pass->clear_depth != 0.0f) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else {
        const RinGpuImageDescV1* depth_desc;

        result = ringpu_slot(core, render_pass->depth_target,
                             RIN_GPU_OBJECT_IMAGE, NULL, &depth_target);
        if (result != RIN_GPU_OK)
            return result;
        depth_desc = &depth_target->value.image.descriptor;
        if (render_pass->depth_store_op != RIN_GPU_RENDER_STORE ||
            !ringpu_render_depth_clear_valid(render_pass->depth_load_op,
                                             render_pass->clear_depth) ||
            render_pass->depth_mip_level >= depth_desc->mip_levels ||
            render_pass->depth_array_layer >= depth_desc->array_layers ||
            (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
            depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
            depth_desc->sample_count != 1u ||
            !ringpu_depth_stencil_format(depth_desc->format) ||
            (depth_desc->format == RIN_GPU_FORMAT_S8_UINT &&
             (render_pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
              render_pass->clear_depth != 0.0f)) ||
            ringpu_mip_dimension(depth_desc->width, render_pass->depth_mip_level) !=
                ringpu_mip_dimension(first_desc->width, first_mip) ||
            ringpu_mip_dimension(depth_desc->height, render_pass->depth_mip_level) !=
                ringpu_mip_dimension(first_desc->height, first_mip)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }
    if (render_pass->stencil_target == 0u) {
        if (render_pass->stencil_mip_level != 0u ||
            render_pass->stencil_array_layer != 0u ||
            render_pass->stencil_load_op != 0u ||
            render_pass->stencil_store_op != 0u || render_pass->clear_stencil != 0u ||
            render_pass->stencil_write_mask != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else {
        const RinGpuImageDescV1* stencil_desc;

        result = ringpu_slot(core, render_pass->stencil_target,
                             RIN_GPU_OBJECT_IMAGE, NULL, &stencil_target);
        if (result != RIN_GPU_OK)
            return result;
        stencil_desc = &stencil_target->value.image.descriptor;
        if (!ringpu_render_stencil_clear_valid(
                stencil_desc->format, render_pass->stencil_load_op,
                render_pass->stencil_store_op, render_pass->clear_stencil,
                render_pass->stencil_write_mask) ||
            render_pass->stencil_mip_level >= stencil_desc->mip_levels ||
            render_pass->stencil_array_layer >= stencil_desc->array_layers ||
            (stencil_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
            stencil_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
            stencil_desc->sample_count != 1u ||
            !ringpu_depth_stencil_format(stencil_desc->format) ||
            ringpu_mip_dimension(stencil_desc->width,
                                 render_pass->stencil_mip_level) !=
                ringpu_mip_dimension(first_desc->width, first_mip) ||
            ringpu_mip_dimension(stencil_desc->height,
                                 render_pass->stencil_mip_level) !=
                ringpu_mip_dimension(first_desc->height, first_mip)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }

    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK)
        return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT;
    command->destination = first_handle;
    command->source = render_pass->depth_target;
    command->auxiliary = render_pass->stencil_target;
    command->value.render_pass_mrt = *render_pass;
    command->value.render_pass_mrt.struct_size =
        sizeof(command->value.render_pass_mrt);
    list->value.command_list.count++;
    list->value.command_list.render_target = first_handle;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_depth_target = render_pass->depth_target;
    list->value.command_list.render_stencil_target = render_pass->stencil_target;
    list->value.command_list.render_mip_level = first_mip;
    list->value.command_list.render_array_layer = first_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.active_color_mask = render_pass->active_color_mask;
    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        if ((render_pass->active_color_mask & (1u << color_index)) == 0u)
            continue;
        list->value.command_list.render_color_targets[color_index] =
            render_pass->color_attachments[color_index].target;
        list->value.command_list.render_color_mip_levels[color_index] =
            render_pass->color_attachments[color_index].mip_level;
        list->value.command_list.render_color_array_layers[color_index] =
            render_pass->color_attachments[color_index].array_layer;
    }
    list->value.command_list.render_depth_mip_level = render_pass->depth_mip_level;
    list->value.command_list.render_depth_array_layer = render_pass->depth_array_layer;
    list->value.command_list.render_stencil_mip_level = render_pass->stencil_mip_level;
    list->value.command_list.render_stencil_array_layer = render_pass->stencil_array_layer;
    list->value.command_list.render_pass_active = 1u;
    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        if ((render_pass->active_color_mask & (1u << color_index)) == 0u)
            continue;
        result = ringpu_slot(core,
                             render_pass->color_attachments[color_index].target,
                             RIN_GPU_OBJECT_IMAGE, NULL, &first_color);
        if (result != RIN_GPU_OK)
            return RIN_GPU_ERROR_STATE;
        first_color->value.image.reference_count++;
    }
    if (depth_target != NULL)
        depth_target->value.image.reference_count++;
    if (stencil_target != NULL)
        stencil_target->value.image.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_begin_render_pass_depth(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDepthDescV1* render_pass) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* depth_target;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* color_desc;
    const RinGpuImageDescV1* depth_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!render_pass ||
        !ringpu_versioned(render_pass->abi_version,
                          render_pass->struct_size, sizeof(*render_pass)) ||
        render_pass->color_store_op != RIN_GPU_RENDER_STORE ||
        render_pass->depth_store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_color_clear_valid(
            render_pass->color_load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        !ringpu_render_depth_clear_valid(
            render_pass->depth_load_op, render_pass->clear_depth) ||
        render_pass->flags != 0u || render_pass->reserved0 != 0u ||
        render_pass->reserved1 != 0u || render_pass->reserved2 != 0u ||
        render_pass->reserved3 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, render_pass->color_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, render_pass->depth_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &depth_target);
    if (result != RIN_GPU_OK) return result;
    color_desc = &color_target->value.image.descriptor;
    depth_desc = &depth_target->value.image.descriptor;
    if (render_pass->color_mip_level >= color_desc->mip_levels ||
        render_pass->color_array_layer >= color_desc->array_layers ||
        render_pass->depth_mip_level >= depth_desc->mip_levels ||
        render_pass->depth_array_layer >= depth_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        color_desc->sample_count != 1u ||
        !ringpu_color_format(color_desc->format) ||
        !ringpu_render_color_clear_valid_for_format(
            color_desc->format, render_pass->color_load_op,
            render_pass->clear_red, render_pass->clear_green,
            render_pass->clear_blue, render_pass->clear_alpha,
            render_pass->color_write_mask) ||
        (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
        depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        depth_desc->sample_count != 1u ||
        !ringpu_depth_stencil_format(depth_desc->format) ||
        (depth_desc->format == RIN_GPU_FORMAT_S8_UINT &&
         (render_pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
          render_pass->clear_depth != 0.0f)) ||
        !ringpu_render_stencil_clear_valid(
            depth_desc->format, render_pass->stencil_load_op,
            render_pass->stencil_store_op, render_pass->clear_stencil,
            render_pass->stencil_write_mask) ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(color_desc->width,
                                 render_pass->color_mip_level),
            ringpu_mip_dimension(color_desc->height,
                                 render_pass->color_mip_level)) ||
        ringpu_mip_dimension(color_desc->width,
                             render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->width,
                                 render_pass->depth_mip_level) ||
        ringpu_mip_dimension(color_desc->height,
                             render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->height,
                                 render_pass->depth_mip_level)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH;
    command->destination = render_pass->color_target;
    command->source = render_pass->depth_target;
    command->value.render_pass_depth = *render_pass;
    command->value.render_pass_depth.struct_size =
        sizeof(command->value.render_pass_depth);
    list->value.command_list.count++;
    list->value.command_list.render_target = render_pass->color_target;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_color_targets[0] = render_pass->color_target;
    list->value.command_list.render_depth_target = render_pass->depth_target;
    list->value.command_list.render_stencil_target = 0u;
    list->value.command_list.render_mip_level =
        render_pass->color_mip_level;
    list->value.command_list.render_array_layer =
        render_pass->color_array_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.render_color_mip_levels[0] =
        render_pass->color_mip_level;
    list->value.command_list.render_color_array_layers[0] =
        render_pass->color_array_layer;
    list->value.command_list.active_color_mask = 1u;
    list->value.command_list.render_depth_mip_level =
        render_pass->depth_mip_level;
    list->value.command_list.render_depth_array_layer =
        render_pass->depth_array_layer;
    list->value.command_list.render_stencil_mip_level = 0u;
    list->value.command_list.render_stencil_array_layer = 0u;
    list->value.command_list.render_pass_active = 1u;
    color_target->value.image.reference_count++;
    depth_target->value.image.reference_count++;
    return RIN_GPU_OK;
}

static uint32_t ringpu_graphics_binding_mip_count(
    const RinGpuGraphicsBindingV1* binding, const RinGpuImageDescV1* desc)
{
    if (!binding || !desc || binding->mip_level >= desc->mip_levels)
        return 0u;
    return (binding->flags & RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN) != 0u
        ? desc->mip_levels - binding->mip_level : 1u;
}

int ringpu_command_begin_render_pass_depth_stencil(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRenderPassDepthStencilDescV1* render_pass) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* depth_target;
    RinGpuObjectSlot* stencil_target;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* color_desc;
    const RinGpuImageDescV1* depth_desc;
    const RinGpuImageDescV1* stencil_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!render_pass ||
        !ringpu_versioned(render_pass->abi_version, render_pass->struct_size,
                          sizeof(*render_pass)) ||
        render_pass->color_store_op != RIN_GPU_RENDER_STORE ||
        render_pass->depth_store_op != RIN_GPU_RENDER_STORE ||
        !ringpu_render_color_clear_valid(
            render_pass->color_load_op, render_pass->clear_red,
            render_pass->clear_green, render_pass->clear_blue,
            render_pass->clear_alpha, render_pass->color_write_mask) ||
        !ringpu_render_depth_clear_valid(render_pass->depth_load_op,
                                         render_pass->clear_depth) ||
        !ringpu_render_stencil_clear_valid(
            RIN_GPU_FORMAT_S8_UINT, render_pass->stencil_load_op,
            render_pass->stencil_store_op, render_pass->clear_stencil,
            render_pass->stencil_write_mask) ||
        render_pass->flags != 0u || render_pass->reserved0 != 0u ||
        render_pass->reserved1 != 0u || render_pass->reserved2 != 0u ||
        render_pass->reserved3 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (render_pass->color_target == render_pass->depth_target ||
        render_pass->color_target == render_pass->stencil_target ||
        render_pass->depth_target == render_pass->stencil_target) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, render_pass->color_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &color_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, render_pass->depth_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &depth_target);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, render_pass->stencil_target,
                         RIN_GPU_OBJECT_IMAGE, NULL, &stencil_target);
    if (result != RIN_GPU_OK) return result;
    color_desc = &color_target->value.image.descriptor;
    depth_desc = &depth_target->value.image.descriptor;
    stencil_desc = &stencil_target->value.image.descriptor;
    if (render_pass->color_mip_level >= color_desc->mip_levels ||
        render_pass->color_array_layer >= color_desc->array_layers ||
        render_pass->depth_mip_level >= depth_desc->mip_levels ||
        render_pass->depth_array_layer >= depth_desc->array_layers ||
        render_pass->stencil_mip_level >= stencil_desc->mip_levels ||
        render_pass->stencil_array_layer >= stencil_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        color_desc->sample_count != 1u || !ringpu_color_format(color_desc->format) ||
        !ringpu_render_color_clear_valid_for_format(
            color_desc->format, render_pass->color_load_op,
            render_pass->clear_red, render_pass->clear_green,
            render_pass->clear_blue, render_pass->clear_alpha,
            render_pass->color_write_mask) ||
        (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
        depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        depth_desc->sample_count != 1u ||
        !ringpu_depth_aspect_format(depth_desc->format) ||
        (stencil_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
        stencil_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        stencil_desc->sample_count != 1u ||
        !ringpu_stencil_aspect_format(stencil_desc->format) ||
        !ringpu_clear_region_valid(
            &render_pass->clear_region,
            ringpu_mip_dimension(color_desc->width,
                                 render_pass->color_mip_level),
            ringpu_mip_dimension(color_desc->height,
                                 render_pass->color_mip_level)) ||
        ringpu_mip_dimension(color_desc->width, render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->width, render_pass->depth_mip_level) ||
        ringpu_mip_dimension(color_desc->height, render_pass->color_mip_level) !=
            ringpu_mip_dimension(depth_desc->height, render_pass->depth_mip_level) ||
        ringpu_mip_dimension(color_desc->width, render_pass->color_mip_level) !=
            ringpu_mip_dimension(stencil_desc->width, render_pass->stencil_mip_level) ||
        ringpu_mip_dimension(color_desc->height, render_pass->color_mip_level) !=
            ringpu_mip_dimension(stencil_desc->height, render_pass->stencil_mip_level)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL;
    command->destination = render_pass->color_target;
    command->source = render_pass->depth_target;
    command->auxiliary = render_pass->stencil_target;
    command->value.render_pass_depth_stencil = *render_pass;
    command->value.render_pass_depth_stencil.struct_size =
        sizeof(command->value.render_pass_depth_stencil);
    list->value.command_list.count++;
    list->value.command_list.render_target = render_pass->color_target;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_color_targets[0] = render_pass->color_target;
    list->value.command_list.render_depth_target = render_pass->depth_target;
    list->value.command_list.render_stencil_target = render_pass->stencil_target;
    list->value.command_list.render_mip_level = render_pass->color_mip_level;
    list->value.command_list.render_array_layer = render_pass->color_array_layer;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.render_color_mip_levels[0] =
        render_pass->color_mip_level;
    list->value.command_list.render_color_array_layers[0] =
        render_pass->color_array_layer;
    list->value.command_list.active_color_mask = 1u;
    list->value.command_list.render_depth_mip_level = render_pass->depth_mip_level;
    list->value.command_list.render_depth_array_layer = render_pass->depth_array_layer;
    list->value.command_list.render_stencil_mip_level = render_pass->stencil_mip_level;
    list->value.command_list.render_stencil_array_layer = render_pass->stencil_array_layer;
    list->value.command_list.render_pass_active = 1u;
    color_target->value.image.reference_count++;
    depth_target->value.image.reference_count++;
    stencil_target->value.image.reference_count++;
    return RIN_GPU_OK;
}

static int ringpu_validate_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline_handle,
    const RinGpuObjectSlot* pipeline, RinGpuHandle bind_group_handle,
    RinGpuObjectSlot** bind_group_out) {
    RinGpuObjectSlot* bind_group;
    int result;
    if (!core || !pipeline ||
        pipeline->type != RIN_GPU_OBJECT_GRAPHICS_PIPELINE ||
        pipeline->value.graphics_pipeline.resource_count == 0u ||
        bind_group_handle == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, bind_group_handle,
                         RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                         &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (bind_group->value.graphics_bind_group.pipeline != pipeline_handle ||
        bind_group->value.graphics_bind_group.binding_count !=
            pipeline->value.graphics_pipeline.resource_count) {
        return RIN_GPU_ERROR_STATE;
    }
    for (uint32_t index = 0u;
         index < bind_group->value.graphics_bind_group.binding_count;
         index++) {
        const RinGpuGraphicsBindingV1* binding =
            &bind_group->value.graphics_bind_group.bindings[index];
        RinGpuObjectSlot* resource;
        result = ringpu_graphics_binding_slot(core, binding, &resource);
        if (result != RIN_GPU_OK) return result;
        if (binding->abi_version != RIN_GPU_ABI_VERSION ||
            binding->struct_size != sizeof(*binding) ||
            binding->binding != index ||
            binding->kind !=
                pipeline->value.graphics_pipeline.resource_kinds[index] ||
            binding->access !=
                pipeline->value.graphics_pipeline.resource_access[index] ||
            (binding->flags & ~RIN_GPU_GRAPHICS_BINDING_KNOWN_FLAGS) != 0u ||
            binding->reserved0 != 0u ||
            binding->reserved1 != 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_BUFFER) {
            if (binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->mip_level != 0u || binding->array_layer != 0u ||
                (binding->offset & 3u) != 0u ||
                (binding->size_bytes & 3u) != 0u ||
                (resource->value.buffer.usage & RIN_GPU_BUFFER_STORAGE) == 0u ||
                !ringpu_buffer_upload_ready(resource) ||
                !ringpu_range(binding->offset, binding->size_bytes,
                              resource->value.buffer.size_bytes)) {
                return RIN_GPU_ERROR_STATE;
            }
        } else if (ringpu_graphics_image_kind(binding->kind)) {
            const RinGpuImageDescV1* desc = &resource->value.image.descriptor;
            if (binding->access != RIN_GPU_RESOURCE_READ ||
                binding->offset != 0u || binding->size_bytes != 0u ||
                (desc->usage & RIN_GPU_IMAGE_SAMPLED) == 0u ||
                desc->sample_count != 1u ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE
                     ? (!ringpu_sampled_image_format(desc->format) &&
                        !ringpu_depth_stencil_format(desc->format))
                     : desc->format != RIN_GPU_FORMAT_D32_FLOAT) ||
                binding->mip_level >= desc->mip_levels ||
                binding->array_layer >= desc->array_layers ||
                (binding->flags != 0u &&
                 (binding->kind != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
                  binding->flags !=
                      RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN)) ||
                ringpu_graphics_binding_mip_count(binding, desc) == 0u) {
                return RIN_GPU_ERROR_STATE;
            }
        } else if (ringpu_graphics_sampler_kind(binding->kind)) {
            if (binding->access != 0u || binding->offset != 0u ||
                binding->size_bytes != 0u || binding->mip_level != 0u ||
                binding->array_layer != 0u || binding->flags != 0u ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLER
                     ? resource->value.sampler.descriptor.compare_op != 0u
                     : resource->value.sampler.descriptor.compare_op == 0u)) {
                return RIN_GPU_ERROR_STATE;
            }
        } else {
            return RIN_GPU_ERROR_STATE;
        }
    }
    for (uint32_t current = 0u;
         current < bind_group->value.graphics_bind_group.binding_count;
         current++) {
        const RinGpuGraphicsBindingV1* current_binding =
            &bind_group->value.graphics_bind_group.bindings[current];
        for (uint32_t prior = 0u; prior < current; prior++) {
            const RinGpuGraphicsBindingV1* prior_binding =
                &bind_group->value.graphics_bind_group.bindings[prior];
            if (current_binding->kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                prior_binding->kind ==
                    RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                current_binding->resource == prior_binding->resource &&
                current_binding->offset <
                    prior_binding->offset + prior_binding->size_bytes &&
                prior_binding->offset <
                    current_binding->offset + current_binding->size_bytes &&
                ((current_binding->access | prior_binding->access) &
                 RIN_GPU_RESOURCE_WRITE) != 0u) {
                return RIN_GPU_ERROR_STATE;
            }
        }
    }
    if (bind_group_out) *bind_group_out = bind_group;
    return RIN_GPU_OK;
}

int ringpu_command_bind_graphics_resources(RinGpuCore* core,
                                           RinGpuHandle command_list,
                                           RinGpuHandle bind_group) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* group = NULL;
    RinGpuHandle previous;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (bind_group != 0u) {
        result = ringpu_slot(core, bind_group,
                             RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                             &group);
        if (result != RIN_GPU_OK) return result;
    }
    previous = list->value.command_list.graphics_bind_group;
    if (previous == bind_group) return RIN_GPU_OK;
    if (group) group->value.graphics_bind_group.reference_count++;
    ringpu_release_graphics_bind_group_reference(core, previous);
    list->value.command_list.graphics_bind_group = bind_group;
    return RIN_GPU_OK;
}

int ringpu_command_set_raster_state(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuRasterStateV1* state) {
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!ringpu_raster_state_valid(state)) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST,
                         NULL, &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE;
    memset(&command->value.raster_state, 0,
           sizeof(command->value.raster_state));
    command->value.raster_state.base.base.base.base = *state;
    command->value.raster_state.base.base.line_width = 1.0f;
    command->value.raster_state.base.sample_coverage_value = 1.0f;
    if (state->struct_size == sizeof(RinGpuRasterStateV2) ||
        state->struct_size == sizeof(RinGpuRasterStateV3) ||
        state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV2* extended =
            (const RinGpuRasterStateV2*)(const void*)state;

        command->value.raster_state.base.base.base.polygon_offset_fill_enabled =
            extended->polygon_offset_fill_enabled;
        command->value.raster_state.base.base.base.polygon_offset_factor =
            extended->polygon_offset_factor;
        command->value.raster_state.base.base.base.polygon_offset_units =
            extended->polygon_offset_units;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV3) ||
        state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV3* extended =
            (const RinGpuRasterStateV3*)(const void*)state;

        command->value.raster_state.base.base.line_width = extended->line_width;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV4) ||
        state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV4* extended =
            (const RinGpuRasterStateV4*)(const void*)state;

        command->value.raster_state.base.sample_coverage_enabled =
            extended->sample_coverage_enabled;
        command->value.raster_state.base.sample_coverage_value =
            extended->sample_coverage_value;
        command->value.raster_state.base.sample_coverage_invert =
            extended->sample_coverage_invert;
    }
    if (state->struct_size == sizeof(RinGpuRasterStateV5)) {
        const RinGpuRasterStateV5* extended =
            (const RinGpuRasterStateV5*)(const void*)state;

        command->value.raster_state.dither_enabled = extended->dither_enabled;
    }
    command->value.raster_state.base.base.base.base.struct_size =
        sizeof(command->value.raster_state);
    command->value.raster_state.base.base.base.base.viewport.struct_size =
        sizeof(command->value.raster_state.base.base.base.base.viewport);
    command->value.raster_state.base.base.base.base.scissor.struct_size =
        sizeof(command->value.raster_state.base.base.base.base.scissor);
    list->value.command_list.count++;
    return RIN_GPU_OK;
}

static int ringpu_validate_graphics_sampled_record(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    const RinGpuObjectSlot* bind_group, uint32_t command_limit) {
    if (!core || !list || !bind_group ||
        command_limit > list->value.command_list.count) {
        return RIN_GPU_ERROR_STATE;
    }
    for (uint32_t binding_index = 0u;
         binding_index < bind_group->value.graphics_bind_group.binding_count;
         binding_index++) {
        const RinGpuGraphicsBindingV1* binding =
            &bind_group->value.graphics_bind_group.bindings[binding_index];
        RinGpuObjectSlot* image;
        uint32_t state;
        int result;
        if (!ringpu_graphics_image_kind(binding->kind)) continue;
        result = ringpu_slot(core, binding->resource, RIN_GPU_OBJECT_IMAGE,
                             NULL, &image);
        if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
        for (uint32_t mip = binding->mip_level;
             mip < binding->mip_level + ringpu_graphics_binding_mip_count(
                       binding, &image->value.image.descriptor);
             ++mip) {
            uint32_t color_index;
            int samples_active_color = 0;

            for (color_index = 0u;
                 color_index < RIN_GPU_MAX_COLOR_TARGETS; ++color_index) {
                if ((list->value.command_list.active_color_mask &
                     (1u << color_index)) != 0u &&
                    list->value.command_list.render_color_targets[color_index] ==
                        binding->resource &&
                    list->value.command_list.render_color_mip_levels[color_index] ==
                        mip &&
                    list->value.command_list
                        .render_color_array_layers[color_index] ==
                        binding->array_layer) {
                    samples_active_color = 1;
                    break;
                }
            }
            if (samples_active_color ||
                (list->value.command_list.render_depth_target == binding->resource &&
                 list->value.command_list.render_depth_mip_level == mip &&
                 list->value.command_list.render_depth_array_layer == binding->array_layer) ||
                (list->value.command_list.render_stencil_target == binding->resource &&
                 list->value.command_list.render_stencil_mip_level == mip &&
                 list->value.command_list.render_stencil_array_layer == binding->array_layer)) {
                return RIN_GPU_ERROR_STATE;
            }
            state = image->value.image.subresource_states[
                binding->array_layer * image->value.image.descriptor.mip_levels + mip];
            for (uint32_t command_index = 0u; command_index < command_limit;
                 command_index++) {
                const RinGpuRecordedCommand* command =
                    &list->value.command_list.commands[command_index];
                const RinGpuImageTransitionV1* transition;
                if (command->type != RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE ||
                    command->destination != binding->resource) {
                    continue;
                }
                transition = &command->value.image_transition;
                if (mip < transition->base_mip_level ||
                    mip >= transition->base_mip_level + transition->mip_level_count ||
                    binding->array_layer < transition->base_array_layer ||
                    binding->array_layer >= transition->base_array_layer +
                        transition->array_layer_count) {
                    continue;
                }
                if (state != transition->before_state)
                    return RIN_GPU_ERROR_STATE;
                state = transition->after_state;
            }
            if (state != RIN_GPU_IMAGE_STATE_SHADER_READ)
                return RIN_GPU_ERROR_STATE;
        }
    }
    return RIN_GPU_OK;
}

static int ringpu_graphics_resources_for_draw(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    RinGpuHandle pipeline_handle, const RinGpuObjectSlot* pipeline,
    RinGpuHandle* bind_group_handle, RinGpuObjectSlot** bind_group) {
    if (!core || !list || !pipeline || !bind_group_handle || !bind_group) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *bind_group_handle = 0u;
    *bind_group = NULL;
    if (pipeline->value.graphics_pipeline.resource_count == 0u) {
        return RIN_GPU_OK;
    }
    *bind_group_handle = list->value.command_list.graphics_bind_group;
    {
        int result = ringpu_validate_graphics_bind_group(
            core, pipeline_handle, pipeline, *bind_group_handle, bind_group);
        if (result != RIN_GPU_OK) return result;
        return ringpu_validate_graphics_sampled_record(
            core, list, *bind_group, list->value.command_list.count);
    }
}

static int ringpu_graphics_draw_has_hazard(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    RinGpuHandle new_bind_group_handle, uint32_t command_limit) {
    RinGpuObjectSlot* new_bind_group;
    if (!core || !list || command_limit > list->value.command_list.count) {
        return 1;
    }
    if (new_bind_group_handle == 0u) return 0;
    if (ringpu_slot(core, new_bind_group_handle,
                    RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                    &new_bind_group) != RIN_GPU_OK) {
        return 1;
    }
    for (uint32_t reverse = command_limit; reverse != 0u; reverse--) {
        const RinGpuRecordedCommand* previous =
            &list->value.command_list.commands[reverse - 1u];
        RinGpuObjectSlot* previous_group;
        if (previous->type == RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER) {
            return 0;
        }
        if (previous->type == RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS ||
            previous->type ==
                RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH ||
            previous->type ==
                RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL) {
            return 0;
        }
        if (previous->type != RIN_GPU_BACKEND_COMMAND_DRAW &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2 &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED &&
            previous->type != RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2 &&
            previous->type !=
                RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
            continue;
        }
        if (previous->resources == 0u) continue;
        if (ringpu_slot(core, previous->resources,
                        RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                        &previous_group) != RIN_GPU_OK) {
            return 1;
        }
        for (uint32_t current = 0u;
             current <
                 new_bind_group->value.graphics_bind_group.binding_count;
             current++) {
            const RinGpuGraphicsBindingV1* current_binding =
                &new_bind_group->value.graphics_bind_group.bindings[current];
            for (uint32_t prior = 0u;
                 prior <
                     previous_group->value.graphics_bind_group.binding_count;
                 prior++) {
                const RinGpuGraphicsBindingV1* prior_binding =
                    &previous_group->value.graphics_bind_group.bindings[prior];
                if (current_binding->kind ==
                        RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                    prior_binding->kind ==
                        RIN_SHADER_RESOURCE_STORAGE_BUFFER &&
                    current_binding->resource == prior_binding->resource &&
                    current_binding->offset <
                        prior_binding->offset + prior_binding->size_bytes &&
                    prior_binding->offset <
                        current_binding->offset +
                            current_binding->size_bytes &&
                    ((current_binding->access | prior_binding->access) &
                     RIN_GPU_RESOURCE_WRITE) != 0u) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int ringpu_command_draw(RinGpuCore* core, RinGpuHandle command_list,
                        const RinGpuDrawV1* draw) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    RinGpuHandle bind_group_handle;
    const RinGpuImageDescV1* target_desc;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->vertex_count == 0u || draw->instance_count == 0u ||
        draw->flags != 0u || draw->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count != 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw = *draw;
    command->value.draw.struct_size = sizeof(command->value.draw);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

static int ringpu_vertex_bindings_for_draw(
    RinGpuCore* core, const RinGpuObjectSlot* pipeline,
    const RinGpuVertexBufferBindingV1* bindings, uint32_t binding_count,
    uint32_t first_vertex, uint32_t vertex_count, uint32_t first_instance,
    uint32_t instance_count,
    RinGpuObjectSlot* resolved[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS])
{
    uint64_t vertex_end;

    if (!core || !pipeline || !resolved ||
        pipeline->type != RIN_GPU_OBJECT_GRAPHICS_PIPELINE ||
        binding_count != pipeline->value.graphics_pipeline.vertex_binding_count ||
        (binding_count != 0u && bindings == NULL)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0u || instance_count == 0u ||
        first_vertex > UINT32_MAX - vertex_count ||
        first_instance > UINT32_MAX - instance_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    vertex_end = (uint64_t)first_vertex + vertex_count;
    for (uint32_t index = 0u; index < binding_count; ++index) {
        const RinGpuVertexBufferBindingV1* binding = &bindings[index];
        const RinGpuVertexBufferLayoutV1* layout =
            &pipeline->value.graphics_pipeline.vertex_bindings[index];
        RinGpuObjectSlot* buffer;
        uint64_t required_bytes;
        int result;

        if (binding->binding != index || binding->reserved != 0u ||
            (binding->offset & (sizeof(uint32_t) - 1u)) != 0u ||
            layout->binding != index || layout->stride == 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        result = ringpu_slot(core, binding->buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &buffer);
        if (result != RIN_GPU_OK) return result;
        if ((buffer->value.buffer.usage & RIN_GPU_BUFFER_VERTEX) == 0u ||
            !ringpu_buffer_upload_ready(buffer)) {
            return RIN_GPU_ERROR_STATE;
        }
        uint64_t element_end = layout->flags == 0u
            ? vertex_end
            : ((uint64_t)first_instance + instance_count - 1u) /
                    layout->flags +
                1u;

        if (!ringpu_multiply_u64(element_end, layout->stride, &required_bytes) ||
            binding->offset > buffer->value.buffer.size_bytes ||
            required_bytes >
                buffer->value.buffer.size_bytes - binding->offset) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        resolved[index] = buffer;
    }
    return RIN_GPU_OK;
}

int ringpu_command_draw_vertices(RinGpuCore* core,
                                 RinGpuHandle command_list,
                                 const RinGpuDrawVerticesV1* draw) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* vertex_buffer = NULL;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    uint64_t vertex_end;
    uint64_t required_bytes;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->vertex_count == 0u || draw->instance_count == 0u ||
        draw->flags != 0u || draw->reserved != 0u ||
        (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (pipeline->value.graphics_pipeline.vertex_binding_count > 1u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->value.graphics_pipeline.vertex_stride == 0u) {
        if (draw->vertex_buffer != 0u || draw->vertex_offset != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    } else {
        result = ringpu_slot(core, draw->vertex_buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &vertex_buffer);
        if (result != RIN_GPU_OK) return result;
        if ((vertex_buffer->value.buffer.usage & RIN_GPU_BUFFER_VERTEX) == 0u ||
            !ringpu_buffer_upload_ready(vertex_buffer)) {
            return RIN_GPU_ERROR_STATE;
        }
        vertex_end = (uint64_t)draw->first_vertex + draw->vertex_count;
        if (!ringpu_multiply_u64(
                vertex_end, pipeline->value.graphics_pipeline.vertex_stride,
                &required_bytes) ||
            draw->vertex_offset > vertex_buffer->value.buffer.size_bytes ||
            required_bytes >
                vertex_buffer->value.buffer.size_bytes - draw->vertex_offset) {
            return RIN_GPU_ERROR_BOUNDS;
        }
    }
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_vertices = *draw;
    command->value.draw_vertices.struct_size =
        sizeof(command->value.draw_vertices);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    if (vertex_buffer != NULL)
        vertex_buffer->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_vertices_v2(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawVerticesV2* draw)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!draw || !ringpu_versioned(draw->abi_version, draw->struct_size,
                                   sizeof(*draw)) ||
        draw->vertex_count == 0u || draw->instance_count == 0u ||
        draw->flags != 0u || draw->reserved0 != 0u ||
        draw->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u ||
        list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_vertex_bindings_for_draw(
        core, pipeline, draw->vertex_buffers, draw->binding_count,
        draw->first_vertex, draw->vertex_count, draw->first_instance,
        draw->instance_count, vertex_buffers);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_vertices_v2 = *draw;
    command->value.draw_vertices_v2.struct_size =
        sizeof(command->value.draw_vertices_v2);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    for (uint32_t index = 0u; index < draw->binding_count; ++index)
        vertex_buffers[index]->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

static int ringpu_command_draw_indexed_internal(
        RinGpuCore* core, RinGpuHandle command_list,
        const RinGpuDrawIndexedV1* draw, int32_t base_vertex,
        uint32_t backend_command_type) {
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* vertex_buffer = NULL;
    RinGpuObjectSlot* index_buffer;
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    uint64_t vertex_bytes;
    uint64_t index_end;
    uint64_t index_bytes;
    uint32_t index_stride;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->index_count == 0u || draw->instance_count == 0u ||
        draw->vertex_count == 0u ||
        draw->flags != 0u || draw->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    index_stride = ringpu_index_format_bytes(draw->index_format);
    if (index_stride == 0u ||
        (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u ||
        (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!ringpu_base_vertex_has_valid_index(
            base_vertex, draw->vertex_count, index_stride)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (backend_command_type != RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED &&
        backend_command_type !=
            RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->index_count > RIN_GPU_MAX_DRAW_INDICES ||
        draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_index > UINT32_MAX - draw->index_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (pipeline->value.graphics_pipeline.vertex_binding_count > 1u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, draw->index_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &index_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((index_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDEX) == 0u ||
        !ringpu_buffer_upload_ready(index_buffer)) {
        return RIN_GPU_ERROR_STATE;
    }
    index_end = (uint64_t)draw->first_index + draw->index_count;
    if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
        draw->index_offset > index_buffer->value.buffer.size_bytes ||
        index_bytes >
            index_buffer->value.buffer.size_bytes - draw->index_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (pipeline->value.graphics_pipeline.vertex_stride == 0u) {
        if (draw->vertex_buffer != 0u || draw->vertex_offset != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    } else {
        result = ringpu_slot(core, draw->vertex_buffer, RIN_GPU_OBJECT_BUFFER,
                             NULL, &vertex_buffer);
        if (result != RIN_GPU_OK) return result;
        if ((vertex_buffer->value.buffer.usage & RIN_GPU_BUFFER_VERTEX) == 0u ||
            !ringpu_buffer_upload_ready(vertex_buffer)) {
            return RIN_GPU_ERROR_STATE;
        }
        if (!ringpu_multiply_u64(
                draw->vertex_count,
                pipeline->value.graphics_pipeline.vertex_stride,
                &vertex_bytes) ||
            draw->vertex_offset > vertex_buffer->value.buffer.size_bytes ||
            vertex_bytes >
                vertex_buffer->value.buffer.size_bytes - draw->vertex_offset) {
            return RIN_GPU_ERROR_BOUNDS;
        }
    }
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = backend_command_type;
    command->base_vertex = base_vertex;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_indexed = *draw;
    command->value.draw_indexed.struct_size =
        sizeof(command->value.draw_indexed);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    if (vertex_buffer != NULL)
        vertex_buffer->value.buffer.reference_count++;
    index_buffer->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_indexed(RinGpuCore* core,
                                RinGpuHandle command_list,
                                const RinGpuDrawIndexedV1* draw) {
    return ringpu_command_draw_indexed_internal(
        core, command_list, draw, 0,
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED);
}

int ringpu_command_draw_indexed_v2(RinGpuCore* core,
                                   RinGpuHandle command_list,
                                   const RinGpuDrawIndexedV2* draw)
{
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* pipeline;
    RinGpuObjectSlot* color_target;
    RinGpuObjectSlot* index_buffer;
    RinGpuObjectSlot* vertex_buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
    RinGpuObjectSlot* bind_group;
    RinGpuRecordedCommand* command;
    const RinGpuImageDescV1* target_desc;
    RinGpuHandle bind_group_handle;
    uint64_t index_end;
    uint64_t index_bytes;
    uint32_t index_stride;
    int result = ringpu_core_ready(core);

    if (result != RIN_GPU_OK) return result;
    if (!draw || !ringpu_versioned(draw->abi_version, draw->struct_size,
                                   sizeof(*draw)) ||
        draw->index_count == 0u || draw->vertex_count == 0u ||
        draw->instance_count == 0u || draw->flags != 0u ||
        draw->reserved0 != 0u || draw->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    index_stride = ringpu_index_format_bytes(draw->index_format);
    if (index_stride == 0u ||
        (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (draw->index_count > RIN_GPU_MAX_DRAW_INDICES ||
        draw->vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_index > UINT32_MAX - draw->index_count ||
        draw->first_instance > UINT32_MAX - draw->instance_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) == 0u ||
        list->value.command_list.render_pass_active == 0u ||
        list->value.command_list.render_target != draw->color_target ||
        list->value.command_list.render_mip_level != draw->mip_level ||
        list->value.command_list.render_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->pipeline,
                         RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL, &pipeline);
    if (result != RIN_GPU_OK) return result;
    if (pipeline->value.graphics_pipeline.vertex_input_count == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (pipeline->value.graphics_pipeline.depth_format != 0u &&
        list->value.command_list.render_depth_target == 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_slot(core, draw->color_target, RIN_GPU_OBJECT_IMAGE, NULL,
                         &color_target);
    if (result != RIN_GPU_OK) return result;
    target_desc = &color_target->value.image.descriptor;
    if (draw->mip_level >= target_desc->mip_levels ||
        draw->array_layer >= target_desc->array_layers) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if ((target_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
        target_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        target_desc->sample_count != 1u ||
        target_desc->format != pipeline->value.graphics_pipeline.color_format) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, draw->index_buffer, RIN_GPU_OBJECT_BUFFER,
                         NULL, &index_buffer);
    if (result != RIN_GPU_OK) return result;
    if ((index_buffer->value.buffer.usage & RIN_GPU_BUFFER_INDEX) == 0u ||
        !ringpu_buffer_upload_ready(index_buffer)) {
        return RIN_GPU_ERROR_STATE;
    }
    index_end = (uint64_t)draw->first_index + draw->index_count;
    if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
        draw->index_offset > index_buffer->value.buffer.size_bytes ||
        index_bytes > index_buffer->value.buffer.size_bytes - draw->index_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    result = ringpu_vertex_bindings_for_draw(
        core, pipeline, draw->vertex_buffers, draw->binding_count,
        0u, draw->vertex_count, draw->first_instance, draw->instance_count,
        vertex_buffers);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_graphics_resources_for_draw(
        core, list, draw->pipeline, pipeline, &bind_group_handle,
        &bind_group);
    if (result != RIN_GPU_OK) return result;
    if (ringpu_graphics_draw_has_hazard(
            core, list, bind_group_handle,
            list->value.command_list.count)) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2;
    command->destination = draw->pipeline;
    command->source = draw->color_target;
    command->resources = bind_group_handle;
    command->value.draw_indexed_v2 = *draw;
    command->value.draw_indexed_v2.struct_size =
        sizeof(command->value.draw_indexed_v2);
    list->value.command_list.count++;
    pipeline->value.graphics_pipeline.reference_count++;
    color_target->value.image.reference_count++;
    index_buffer->value.buffer.reference_count++;
    for (uint32_t index = 0u; index < draw->binding_count; ++index)
        vertex_buffers[index]->value.buffer.reference_count++;
    if (bind_group) bind_group->value.graphics_bind_group.reference_count++;
    return RIN_GPU_OK;
}

int ringpu_command_draw_indexed_base_vertex(
        RinGpuCore* core, RinGpuHandle command_list,
        const RinGpuDrawIndexedBaseVertexV1* draw) {
    RinGpuDrawIndexedV1 canonical;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!draw ||
        !ringpu_versioned(draw->abi_version, draw->struct_size,
                          sizeof(*draw)) ||
        draw->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(&canonical, 0, sizeof(canonical));
    canonical.abi_version = draw->abi_version;
    canonical.struct_size = sizeof(canonical);
    canonical.pipeline = draw->pipeline;
    canonical.color_target = draw->color_target;
    canonical.vertex_buffer = draw->vertex_buffer;
    canonical.index_buffer = draw->index_buffer;
    canonical.vertex_offset = draw->vertex_offset;
    canonical.index_offset = draw->index_offset;
    canonical.index_format = draw->index_format;
    canonical.mip_level = draw->mip_level;
    canonical.array_layer = draw->array_layer;
    canonical.index_count = draw->index_count;
    canonical.instance_count = draw->instance_count;
    canonical.first_index = draw->first_index;
    canonical.vertex_count = draw->vertex_count;
    canonical.first_instance = draw->first_instance;
    canonical.flags = draw->flags;
    return ringpu_command_draw_indexed_internal(
        core, command_list, &canonical, draw->base_vertex,
        RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX);
}

int ringpu_command_end_render_pass(RinGpuCore* core,
                                   RinGpuHandle command_list) {
    RinGpuObjectSlot* list;
    RinGpuRecordedCommand* command;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active == 0u ||
        (list->value.command_list.capabilities & RIN_GPU_QUEUE_GRAPHICS) ==
            0u) {
        return RIN_GPU_ERROR_STATE;
    }
    result = ringpu_record_command(list, &command);
    if (result != RIN_GPU_OK) return result;
    command->type = RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS;
    list->value.command_list.count++;
    ringpu_release_graphics_bind_group_reference(
        core, list->value.command_list.graphics_bind_group);
    list->value.command_list.graphics_bind_group = 0u;
    list->value.command_list.render_target = 0u;
    memset(list->value.command_list.render_color_targets, 0,
           sizeof(list->value.command_list.render_color_targets));
    list->value.command_list.render_depth_target = 0u;
    list->value.command_list.render_stencil_target = 0u;
    list->value.command_list.render_mip_level = 0u;
    list->value.command_list.render_array_layer = 0u;
    memset(list->value.command_list.render_color_mip_levels, 0,
           sizeof(list->value.command_list.render_color_mip_levels));
    memset(list->value.command_list.render_color_array_layers, 0,
           sizeof(list->value.command_list.render_color_array_layers));
    list->value.command_list.active_color_mask = 0u;
    list->value.command_list.render_depth_mip_level = 0u;
    list->value.command_list.render_depth_array_layer = 0u;
    list->value.command_list.render_stencil_mip_level = 0u;
    list->value.command_list.render_stencil_array_layer = 0u;
    list->value.command_list.render_pass_active = 0u;
    return RIN_GPU_OK;
}

int ringpu_command_list_close(RinGpuCore* core, RinGpuHandle command_list) {
    RinGpuObjectSlot* list;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, command_list, RIN_GPU_OBJECT_COMMAND_LIST, NULL,
                         &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_RECORDING ||
        list->value.command_list.render_pass_active != 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    list->value.command_list.state = RIN_GPU_COMMAND_EXECUTABLE;
    return RIN_GPU_OK;
}

static int ringpu_backend_graphics_bind_group(
    RinGpuCore* core, RinGpuHandle pipeline_handle,
    const RinGpuObjectSlot* pipeline, RinGpuHandle bind_group_handle,
    uint64_t* cookie) {
    RinGpuObjectSlot* bind_group;
    int result;
    if (!core || !pipeline || !cookie) return RIN_GPU_ERROR_STATE;
    *cookie = 0u;
    if (pipeline->value.graphics_pipeline.resource_count == 0u) {
        return bind_group_handle == 0u ? RIN_GPU_OK : RIN_GPU_ERROR_STATE;
    }
    result = ringpu_validate_graphics_bind_group(
        core, pipeline_handle, pipeline, bind_group_handle, &bind_group);
    if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
    *cookie = bind_group->value.graphics_bind_group.backend_cookie;
    return RIN_GPU_OK;
}

static int ringpu_stage_image_states(RinGpuObjectSlot* image,
                                     uint32_t object_index,
                                     uint32_t** staged_states) {
    uint32_t* states;
    if (!image || !staged_states ||
        object_index >= RIN_GPU_CORE_MAX_OBJECTS ||
        image->type != RIN_GPU_OBJECT_IMAGE ||
        !ringpu_image_upload_ready(image) ||
        image->value.image.subresource_count == 0u ||
        !image->value.image.subresource_states) {
        return RIN_GPU_ERROR_STATE;
    }
    states = staged_states[object_index];
    if (!states) {
        states = (uint32_t*)malloc(
            (size_t)image->value.image.subresource_count * sizeof(*states));
        if (!states) return RIN_GPU_ERROR_NO_MEMORY;
        memcpy(states, image->value.image.subresource_states,
               (size_t)image->value.image.subresource_count * sizeof(*states));
        staged_states[object_index] = states;
    }
    return RIN_GPU_OK;
}

static int ringpu_validate_graphics_sampled_submit(
    RinGpuCore* core, RinGpuHandle bind_group_handle,
    uint32_t** staged_states, RinGpuHandle color_target,
    uint32_t color_mip_level, uint32_t color_array_layer,
    RinGpuHandle depth_target, uint32_t depth_mip_level,
    uint32_t depth_array_layer, RinGpuHandle stencil_target,
    uint32_t stencil_mip_level, uint32_t stencil_array_layer) {
    RinGpuObjectSlot* bind_group;
    int result;
    if (bind_group_handle == 0u) return RIN_GPU_OK;
    result = ringpu_slot(core, bind_group_handle,
                         RIN_GPU_OBJECT_GRAPHICS_BIND_GROUP, NULL,
                         &bind_group);
    if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
    for (uint32_t binding_index = 0u;
         binding_index < bind_group->value.graphics_bind_group.binding_count;
         binding_index++) {
        const RinGpuGraphicsBindingV1* binding =
            &bind_group->value.graphics_bind_group.bindings[binding_index];
        RinGpuObjectSlot* image;
        uint32_t object_index;
        uint32_t* states;
        if (!ringpu_graphics_image_kind(binding->kind)) continue;
        result = ringpu_slot(core, binding->resource, RIN_GPU_OBJECT_IMAGE,
                             &object_index, &image);
        if (result != RIN_GPU_OK) return RIN_GPU_ERROR_STATE;
        result = ringpu_stage_image_states(image, object_index,
                                           staged_states);
        if (result != RIN_GPU_OK) return result;
        states = staged_states[object_index];
        for (uint32_t mip = binding->mip_level;
             mip < binding->mip_level + ringpu_graphics_binding_mip_count(
                       binding, &image->value.image.descriptor);
             ++mip) {
            if ((color_target == binding->resource &&
                 color_mip_level == mip && color_array_layer == binding->array_layer) ||
                (depth_target == binding->resource &&
                 depth_mip_level == mip && depth_array_layer == binding->array_layer) ||
                (stencil_target == binding->resource &&
                 stencil_mip_level == mip && stencil_array_layer == binding->array_layer) ||
                states[binding->array_layer *
                           image->value.image.descriptor.mip_levels + mip] !=
                    RIN_GPU_IMAGE_STATE_SHADER_READ) {
                return RIN_GPU_ERROR_STATE;
            }
        }
    }
    return RIN_GPU_OK;
}

int ringpu_queue_submit(RinGpuCore* core, RinGpuHandle queue,
                        const RinGpuSubmitInfoV1* submit) {
    RinGpuObjectSlot* queue_slot;
    RinGpuObjectSlot* list;
    RinGpuObjectSlot* fence = NULL;
    RinGpuBackendCommandV1* commands = NULL;
    uint32_t* staged_states[RIN_GPU_CORE_MAX_OBJECTS] = {0};
    RinGpuHandle active_render_target = 0u;
    RinGpuHandle active_depth_target = 0u;
    RinGpuHandle active_stencil_target = 0u;
    uint32_t active_render_mip_level = 0u;
    uint32_t active_render_array_layer = 0u;
    uint32_t active_depth_mip_level = 0u;
    uint32_t active_depth_array_layer = 0u;
    uint32_t active_stencil_mip_level = 0u;
    uint32_t active_stencil_array_layer = 0u;
    uint32_t active_depth_format = 0u;
    uint32_t present_count = 0u;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!submit ||
        !ringpu_versioned(submit->abi_version, submit->struct_size,
                          sizeof(*submit))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_slot(core, queue, RIN_GPU_OBJECT_QUEUE, NULL, &queue_slot);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, submit->command_list,
                         RIN_GPU_OBJECT_COMMAND_LIST, NULL, &list);
    if (result != RIN_GPU_OK) return result;
    if (list->value.command_list.state != RIN_GPU_COMMAND_EXECUTABLE ||
        (list->value.command_list.capabilities &
         ~queue_slot->value.queue.capabilities) != 0u) {
        return RIN_GPU_ERROR_STATE;
    }
    if (submit->signal_fence != 0u) {
        result = ringpu_slot(core, submit->signal_fence, RIN_GPU_OBJECT_FENCE,
                             NULL, &fence);
        if (result != RIN_GPU_OK) return result;
        if (submit->signal_value <= fence->value.fence.value) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else if (submit->signal_value != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (list->value.command_list.count != 0u) {
        commands = (RinGpuBackendCommandV1*)calloc(
            list->value.command_list.count, sizeof(*commands));
        if (!commands) return RIN_GPU_ERROR_NO_MEMORY;
    }
    for (uint32_t index = 0; index < list->value.command_list.count; index++) {
        RinGpuRecordedCommand* command =
            &list->value.command_list.commands[index];
        RinGpuObjectSlot* destination;
        RinGpuObjectSlot* source;
        RinGpuObjectSlot* auxiliary;
        uint32_t destination_index;
        uint32_t source_index;
        uint32_t auxiliary_index;
        uint32_t* destination_states;
        uint32_t* source_states;
        uint32_t* auxiliary_states;
        commands[index].type = command->type;
        if (command->type == RIN_GPU_BACKEND_COMMAND_COPY_BUFFER) {
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &source);
            if (result != RIN_GPU_OK) break;
            commands[index].value.buffer_copy.destination_cookie =
                destination->value.buffer.backend_cookie;
            commands[index].value.buffer_copy.destination_offset =
                command->value.buffer_copy.destination_offset;
            commands[index].value.buffer_copy.source_cookie =
                source->value.buffer.backend_cookie;
            commands[index].value.buffer_copy.source_offset =
                command->value.buffer_copy.source_offset;
            commands[index].value.buffer_copy.size_bytes =
                command->value.buffer_copy.size_bytes;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_COPY_IMAGE) {
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (source_index == destination_index) {
                source_states = destination_states;
            } else {
                result = ringpu_stage_image_states(
                    source, source_index, staged_states);
                if (result != RIN_GPU_OK) break;
                source_states = staged_states[source_index];
            }
            if (destination_states[
                    command->value.image_copy.destination_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    command->value.image_copy.destination_mip_level] !=
                RIN_GPU_IMAGE_STATE_COPY_DESTINATION) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            if (source_states[
                    command->value.image_copy.source_array_layer *
                        source->value.image.descriptor.mip_levels +
                    command->value.image_copy.source_mip_level] !=
                    RIN_GPU_IMAGE_STATE_COPY_SOURCE) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.image_copy.destination_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.image_copy.source_cookie =
                source->value.image.backend_cookie;
            commands[index].value.image_copy.region =
                command->value.image_copy;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE) {
            const RinGpuImageTransitionV1* transition =
                &command->value.image_transition;
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            for (uint32_t layer = 0u;
                 layer < transition->array_layer_count; layer++) {
                for (uint32_t mip = 0u;
                     mip < transition->mip_level_count; mip++) {
                    uint32_t subresource =
                        (transition->base_array_layer + layer) *
                            destination->value.image.descriptor.mip_levels +
                        transition->base_mip_level + mip;
                    if (destination_states[subresource] !=
                        transition->before_state) {
                        result = RIN_GPU_ERROR_STATE;
                        break;
                    }
                }
                if (result != RIN_GPU_OK) break;
            }
            if (result != RIN_GPU_OK) break;
            for (uint32_t layer = 0u;
                 layer < transition->array_layer_count; layer++) {
                for (uint32_t mip = 0u;
                     mip < transition->mip_level_count; mip++) {
                    uint32_t subresource =
                        (transition->base_array_layer + layer) *
                            destination->value.image.descriptor.mip_levels +
                        transition->base_mip_level + mip;
                    destination_states[subresource] = transition->after_state;
                }
            }
            commands[index].value.image_transition.image_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.image_transition.transition = *transition;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_DISPATCH) {
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_COMPUTE_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_COMPUTE_BIND_GROUP, NULL,
                                 &source);
            if (result != RIN_GPU_OK) break;
            if (source->value.compute_bind_group.pipeline !=
                command->destination) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.dispatch.pipeline_cookie =
                destination->value.compute_pipeline.backend_cookie;
            commands[index].value.dispatch.bind_group_cookie =
                source->value.compute_bind_group.backend_cookie;
            commands[index].value.dispatch.group_count_x =
                command->value.dispatch.group_count_x;
            commands[index].value.dispatch.group_count_y =
                command->value.dispatch.group_count_y;
            commands[index].value.dispatch.group_count_z =
                command->value.dispatch.group_count_z;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER) {
            commands[index].value.compute_barrier.source_access =
                command->value.compute_barrier.source_access;
            commands[index].value.compute_barrier.destination_access =
                command->value.compute_barrier.destination_access;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER) {
            const RinGpuGraphicsBarrierV1* barrier =
                &command->value.graphics_barrier;
            if (active_render_target == 0u ||
                barrier->source_access != RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                barrier->destination_access !=
                    RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                barrier->flags != 0u || barrier->reserved != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.graphics_barrier.source_access =
                barrier->source_access;
            commands[index].value.graphics_barrier.destination_access =
                barrier->destination_access;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT) {
            const RinGpuRenderPassMrtDescV1* pass =
                &command->value.render_pass_mrt;
            RinGpuBackendRenderPassMrtBeginV1* backend_pass =
                &commands[index].value.render_pass_mrt_begin;
            const RinGpuImageDescV1* first_desc = NULL;
            RinGpuHandle first_target = 0u;
            uint32_t first_mip = 0u;
            uint32_t first_layer = 0u;
            uint32_t color_index;

            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u ||
                !ringpu_versioned(pass->abi_version, pass->struct_size,
                                  sizeof(*pass)) ||
                pass->active_color_mask == 0u ||
                (pass->active_color_mask &
                 ~((1u << RIN_GPU_MAX_COLOR_TARGETS) - 1u)) != 0u ||
                pass->color_store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_color_clear_valid(
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                pass->flags != 0u || pass->reserved0 != 0u ||
                pass->reserved1 != 0u || pass->reserved2 != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
                 ++color_index) {
                const RinGpuColorAttachmentV1* attachment =
                    &pass->color_attachments[color_index];
                RinGpuObjectSlot* color;
                const RinGpuImageDescV1* color_desc;
                uint32_t color_slot_index;
                uint32_t* color_states;

                if ((pass->active_color_mask & (1u << color_index)) == 0u) {
                    if (attachment->target != 0u || attachment->mip_level != 0u ||
                        attachment->array_layer != 0u) {
                        result = RIN_GPU_ERROR_STATE;
                        break;
                    }
                    continue;
                }
                result = ringpu_slot(core, attachment->target,
                                     RIN_GPU_OBJECT_IMAGE, &color_slot_index,
                                     &color);
                if (result != RIN_GPU_OK)
                    break;
                result = ringpu_stage_image_states(color, color_slot_index,
                                                   staged_states);
                if (result != RIN_GPU_OK)
                    break;
                color_states = staged_states[color_slot_index];
                color_desc = &color->value.image.descriptor;
                if (attachment->mip_level >= color_desc->mip_levels ||
                    attachment->array_layer >= color_desc->array_layers ||
                    (color_desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                    color_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                    color_desc->sample_count != 1u ||
                    !ringpu_color_format(color_desc->format) ||
                    !ringpu_render_color_clear_valid_for_format(
                        color_desc->format, pass->color_load_op,
                        pass->clear_red, pass->clear_green, pass->clear_blue,
                        pass->clear_alpha, pass->color_write_mask) ||
                    color_states[attachment->array_layer * color_desc->mip_levels +
                                 attachment->mip_level] !=
                        RIN_GPU_IMAGE_STATE_COLOR_TARGET) {
                    result = RIN_GPU_ERROR_STATE;
                    break;
                }
                if (first_desc == NULL) {
                    first_desc = color_desc;
                    first_target = attachment->target;
                    first_mip = attachment->mip_level;
                    first_layer = attachment->array_layer;
                } else if (color_desc->format != first_desc->format ||
                           ringpu_mip_dimension(color_desc->width,
                                                attachment->mip_level) !=
                               ringpu_mip_dimension(first_desc->width,
                                                    first_mip) ||
                           ringpu_mip_dimension(color_desc->height,
                                                attachment->mip_level) !=
                               ringpu_mip_dimension(first_desc->height,
                                                    first_mip)) {
                    result = RIN_GPU_ERROR_UNSUPPORTED;
                    break;
                }
                for (uint32_t prior_index = 0u; prior_index < color_index;
                     ++prior_index) {
                    const RinGpuColorAttachmentV1* prior =
                        &pass->color_attachments[prior_index];

                    if ((pass->active_color_mask & (1u << prior_index)) != 0u &&
                        prior->target == attachment->target &&
                        prior->mip_level == attachment->mip_level &&
                        prior->array_layer == attachment->array_layer) {
                        result = RIN_GPU_ERROR_STATE;
                        break;
                    }
                }
                if (result != RIN_GPU_OK)
                    break;
                backend_pass->color_target_cookies[color_index] =
                    color->value.image.backend_cookie;
                backend_pass->color_mip_levels[color_index] = attachment->mip_level;
                backend_pass->color_array_layers[color_index] =
                    attachment->array_layer;
            }
            if (result != RIN_GPU_OK)
                break;
            if (first_desc == NULL || command->destination != first_target ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(first_desc->width, first_mip),
                    ringpu_mip_dimension(first_desc->height, first_mip))) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            if (pass->depth_target != 0u) {
                const RinGpuImageDescV1* depth_desc;

                result = ringpu_slot(core, pass->depth_target,
                                     RIN_GPU_OBJECT_IMAGE, &source_index,
                                     &source);
                if (result != RIN_GPU_OK)
                    break;
                result = ringpu_stage_image_states(source, source_index,
                                                   staged_states);
                if (result != RIN_GPU_OK)
                    break;
                source_states = staged_states[source_index];
                depth_desc = &source->value.image.descriptor;
                if (command->source != pass->depth_target ||
                    pass->depth_store_op != RIN_GPU_RENDER_STORE ||
                    !ringpu_render_depth_clear_valid(pass->depth_load_op,
                                                     pass->clear_depth) ||
                    pass->depth_mip_level >= depth_desc->mip_levels ||
                    pass->depth_array_layer >= depth_desc->array_layers ||
                    (depth_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                    depth_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                    depth_desc->sample_count != 1u ||
                    !ringpu_depth_stencil_format(depth_desc->format) ||
                    (depth_desc->format == RIN_GPU_FORMAT_S8_UINT &&
                     (pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
                      pass->clear_depth != 0.0f)) ||
                    ringpu_mip_dimension(depth_desc->width,
                                         pass->depth_mip_level) !=
                        ringpu_mip_dimension(first_desc->width, first_mip) ||
                    ringpu_mip_dimension(depth_desc->height,
                                         pass->depth_mip_level) !=
                        ringpu_mip_dimension(first_desc->height, first_mip) ||
                    source_states[pass->depth_array_layer * depth_desc->mip_levels +
                                  pass->depth_mip_level] !=
                        RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                    result = RIN_GPU_ERROR_STATE;
                    break;
                }
                backend_pass->depth_target_cookie =
                    source->value.image.backend_cookie;
            } else if (command->source != 0u || pass->depth_mip_level != 0u ||
                       pass->depth_array_layer != 0u || pass->depth_load_op != 0u ||
                       pass->depth_store_op != 0u || pass->clear_depth != 0.0f) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            if (pass->stencil_target != 0u) {
                const RinGpuImageDescV1* stencil_desc;

                result = ringpu_slot(core, pass->stencil_target,
                                     RIN_GPU_OBJECT_IMAGE, &auxiliary_index,
                                     &auxiliary);
                if (result != RIN_GPU_OK)
                    break;
                result = ringpu_stage_image_states(auxiliary, auxiliary_index,
                                                   staged_states);
                if (result != RIN_GPU_OK)
                    break;
                auxiliary_states = staged_states[auxiliary_index];
                stencil_desc = &auxiliary->value.image.descriptor;
                if (command->auxiliary != pass->stencil_target ||
                    !ringpu_render_stencil_clear_valid(
                        stencil_desc->format, pass->stencil_load_op,
                        pass->stencil_store_op, pass->clear_stencil,
                        pass->stencil_write_mask) ||
                    pass->stencil_mip_level >= stencil_desc->mip_levels ||
                    pass->stencil_array_layer >= stencil_desc->array_layers ||
                    (stencil_desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                    stencil_desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                    stencil_desc->sample_count != 1u ||
                    !ringpu_depth_stencil_format(stencil_desc->format) ||
                    ringpu_mip_dimension(stencil_desc->width,
                                         pass->stencil_mip_level) !=
                        ringpu_mip_dimension(first_desc->width, first_mip) ||
                    ringpu_mip_dimension(stencil_desc->height,
                                         pass->stencil_mip_level) !=
                        ringpu_mip_dimension(first_desc->height, first_mip) ||
                    auxiliary_states[
                        pass->stencil_array_layer * stencil_desc->mip_levels +
                        pass->stencil_mip_level] != RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                    result = RIN_GPU_ERROR_STATE;
                    break;
                }
                backend_pass->stencil_target_cookie =
                    auxiliary->value.image.backend_cookie;
            } else if (command->auxiliary != 0u ||
                       pass->stencil_mip_level != 0u ||
                       pass->stencil_array_layer != 0u ||
                       pass->stencil_load_op != 0u ||
                       pass->stencil_store_op != 0u ||
                       pass->clear_stencil != 0u ||
                       pass->stencil_write_mask != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            backend_pass->active_color_mask = pass->active_color_mask;
            backend_pass->depth_mip_level = pass->depth_mip_level;
            backend_pass->depth_array_layer = pass->depth_array_layer;
            backend_pass->stencil_mip_level = pass->stencil_mip_level;
            backend_pass->stencil_array_layer = pass->stencil_array_layer;
            backend_pass->color_load_op = pass->color_load_op;
            backend_pass->color_store_op = pass->color_store_op;
            backend_pass->depth_load_op = pass->depth_load_op;
            backend_pass->depth_store_op = pass->depth_store_op;
            backend_pass->stencil_load_op = pass->stencil_load_op;
            backend_pass->stencil_store_op = pass->stencil_store_op;
            backend_pass->clear_red = pass->clear_red;
            backend_pass->clear_green = pass->clear_green;
            backend_pass->clear_blue = pass->clear_blue;
            backend_pass->clear_alpha = pass->clear_alpha;
            backend_pass->clear_depth = pass->clear_depth;
            backend_pass->clear_stencil = pass->clear_stencil;
            backend_pass->stencil_write_mask = pass->stencil_write_mask;
            backend_pass->color_write_mask = pass->color_write_mask;
            backend_pass->clear_region = pass->clear_region;
            active_render_target = first_target;
            active_render_mip_level = first_mip;
            active_render_array_layer = first_layer;
            active_depth_target = pass->depth_target;
            active_depth_mip_level = pass->depth_mip_level;
            active_depth_array_layer = pass->depth_array_layer;
            active_stencil_target = pass->stencil_target;
            active_stencil_mip_level = pass->stencil_mip_level;
            active_stencil_array_layer = pass->stencil_array_layer;
            active_depth_format = pass->depth_target != 0u
                ? source->value.image.descriptor.format
                : pass->stencil_target != 0u
                    ? auxiliary->value.image.descriptor.format : 0u;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS) {
            const RinGpuRenderPassDescV1* pass =
                &command->value.render_pass;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (pass->color_target != command->destination ||
                pass->store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_clear_valid(pass) || pass->flags != 0u ||
                !ringpu_render_color_clear_valid_for_format(
                    destination->value.image.descriptor.format, pass->load_op,
                    pass->clear_red, pass->clear_green, pass->clear_blue,
                    pass->clear_alpha, pass->color_write_mask) ||
                pass->reserved != 0u || pass->reserved1 != 0u ||
                pass->mip_level >=
                    destination->value.image.descriptor.mip_levels ||
                pass->array_layer >=
                    destination->value.image.descriptor.array_layers ||
                (destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.sample_count != 1u ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.width,
                        pass->mip_level),
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.height,
                        pass->mip_level)) ||
                destination_states[
                    pass->array_layer *
                        destination->value.image.descriptor.mip_levels +
                    pass->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.render_pass_begin.color_target_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.render_pass_begin.mip_level =
                pass->mip_level;
            commands[index].value.render_pass_begin.array_layer =
                pass->array_layer;
            commands[index].value.render_pass_begin.load_op = pass->load_op;
            commands[index].value.render_pass_begin.store_op = pass->store_op;
            commands[index].value.render_pass_begin.clear_red =
                pass->clear_red;
            commands[index].value.render_pass_begin.clear_green =
                pass->clear_green;
            commands[index].value.render_pass_begin.clear_blue =
                pass->clear_blue;
            commands[index].value.render_pass_begin.clear_alpha =
                pass->clear_alpha;
            commands[index].value.render_pass_begin.color_write_mask =
                pass->color_write_mask;
            commands[index].value.render_pass_begin.clear_region =
                pass->clear_region;
            active_render_target = command->destination;
            active_render_mip_level = pass->mip_level;
            active_render_array_layer = pass->array_layer;
            active_depth_target = 0u;
            active_stencil_target = 0u;
            active_stencil_mip_level = 0u;
            active_stencil_array_layer = 0u;
            active_depth_format = 0u;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH) {
            const RinGpuRenderPassDepthDescV1* pass =
                &command->value.render_pass_depth;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if (source_index == destination_index) {
                source_states = destination_states;
            } else {
                result = ringpu_stage_image_states(
                    source, source_index, staged_states);
                if (result != RIN_GPU_OK) break;
                source_states = staged_states[source_index];
            }
            if (pass->color_target != command->destination ||
                pass->depth_target != command->source ||
                pass->color_store_op != RIN_GPU_RENDER_STORE ||
                pass->depth_store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_color_clear_valid(
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_color_clear_valid_for_format(
                    destination->value.image.descriptor.format,
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_depth_clear_valid(
                    pass->depth_load_op, pass->clear_depth) ||
                (source->value.image.descriptor.format ==
                     RIN_GPU_FORMAT_S8_UINT &&
                 (pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
                  pass->clear_depth != 0.0f)) ||
                !ringpu_render_stencil_clear_valid(
                    source->value.image.descriptor.format,
                    pass->stencil_load_op, pass->stencil_store_op,
                    pass->clear_stencil, pass->stencil_write_mask) ||
                pass->flags != 0u || pass->reserved0 != 0u ||
                pass->reserved1 != 0u || pass->reserved2 != 0u ||
                pass->reserved3 != 0u ||
                pass->color_mip_level >=
                    destination->value.image.descriptor.mip_levels ||
                pass->color_array_layer >=
                    destination->value.image.descriptor.array_layers ||
                pass->depth_mip_level >=
                    source->value.image.descriptor.mip_levels ||
                pass->depth_array_layer >=
                    source->value.image.descriptor.array_layers ||
                (destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.sample_count != 1u ||
                !ringpu_color_format(
                    destination->value.image.descriptor.format) ||
                (source->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                source->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                source->value.image.descriptor.sample_count != 1u ||
                !ringpu_depth_stencil_format(
                    source->value.image.descriptor.format) ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.width,
                        pass->color_mip_level),
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.height,
                        pass->color_mip_level)) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.width,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.width,
                        pass->depth_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.height,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.height,
                        pass->depth_mip_level) ||
                destination_states[
                    pass->color_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    pass->color_mip_level] !=
                    RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source_states[
                    pass->depth_array_layer *
                        source->value.image.descriptor.mip_levels +
                    pass->depth_mip_level] !=
                    RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.render_pass_depth_begin.color_target_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.render_pass_depth_begin.depth_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.render_pass_depth_begin.color_mip_level =
                pass->color_mip_level;
            commands[index].value.render_pass_depth_begin.color_array_layer =
                pass->color_array_layer;
            commands[index].value.render_pass_depth_begin.depth_mip_level =
                pass->depth_mip_level;
            commands[index].value.render_pass_depth_begin.depth_array_layer =
                pass->depth_array_layer;
            commands[index].value.render_pass_depth_begin.color_load_op =
                pass->color_load_op;
            commands[index].value.render_pass_depth_begin.color_store_op =
                pass->color_store_op;
            commands[index].value.render_pass_depth_begin.depth_load_op =
                pass->depth_load_op;
            commands[index].value.render_pass_depth_begin.depth_store_op =
                pass->depth_store_op;
            commands[index].value.render_pass_depth_begin.clear_red =
                pass->clear_red;
            commands[index].value.render_pass_depth_begin.clear_green =
                pass->clear_green;
            commands[index].value.render_pass_depth_begin.clear_blue =
                pass->clear_blue;
            commands[index].value.render_pass_depth_begin.clear_alpha =
                pass->clear_alpha;
            commands[index].value.render_pass_depth_begin.clear_depth =
                pass->clear_depth;
            commands[index].value.render_pass_depth_begin.stencil_load_op =
                pass->stencil_load_op;
            commands[index].value.render_pass_depth_begin.stencil_store_op =
                pass->stencil_store_op;
            commands[index].value.render_pass_depth_begin.clear_stencil =
                pass->clear_stencil;
            commands[index].value.render_pass_depth_begin.stencil_write_mask =
                pass->stencil_write_mask;
            commands[index].value.render_pass_depth_begin.reserved2 =
                pass->reserved2;
            commands[index].value.render_pass_depth_begin.color_write_mask =
                pass->color_write_mask;
            commands[index].value.render_pass_depth_begin.clear_region =
                pass->clear_region;
            active_render_target = command->destination;
            active_depth_target = command->source;
            active_render_mip_level = pass->color_mip_level;
            active_render_array_layer = pass->color_array_layer;
            active_depth_mip_level = pass->depth_mip_level;
            active_depth_array_layer = pass->depth_array_layer;
            active_depth_format = source->value.image.descriptor.format;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL) {
            const RinGpuRenderPassDepthStencilDescV1* pass =
                &command->value.render_pass_depth_stencil;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source, RIN_GPU_OBJECT_IMAGE,
                                 &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->auxiliary,
                                 RIN_GPU_OBJECT_IMAGE, &auxiliary_index,
                                 &auxiliary);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(destination, destination_index,
                                               staged_states);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                               staged_states);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(auxiliary, auxiliary_index,
                                               staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            source_states = staged_states[source_index];
            auxiliary_states = staged_states[auxiliary_index];
            if (pass->color_target != command->destination ||
                pass->depth_target != command->source ||
                pass->stencil_target != command->auxiliary ||
                command->destination == command->source ||
                command->destination == command->auxiliary ||
                command->source == command->auxiliary ||
                pass->color_store_op != RIN_GPU_RENDER_STORE ||
                pass->depth_store_op != RIN_GPU_RENDER_STORE ||
                !ringpu_render_color_clear_valid(
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_color_clear_valid_for_format(
                    destination->value.image.descriptor.format,
                    pass->color_load_op, pass->clear_red, pass->clear_green,
                    pass->clear_blue, pass->clear_alpha,
                    pass->color_write_mask) ||
                !ringpu_render_depth_clear_valid(pass->depth_load_op,
                                                 pass->clear_depth) ||
                !ringpu_render_stencil_clear_valid(
                    auxiliary->value.image.descriptor.format,
                    pass->stencil_load_op,
                    pass->stencil_store_op, pass->clear_stencil,
                    pass->stencil_write_mask) ||
                pass->flags != 0u || pass->reserved0 != 0u ||
                pass->reserved1 != 0u || pass->reserved2 != 0u ||
                pass->reserved3 != 0u ||
                pass->color_mip_level >=
                    destination->value.image.descriptor.mip_levels ||
                pass->color_array_layer >=
                    destination->value.image.descriptor.array_layers ||
                pass->depth_mip_level >=
                    source->value.image.descriptor.mip_levels ||
                pass->depth_array_layer >=
                    source->value.image.descriptor.array_layers ||
                pass->stencil_mip_level >=
                    auxiliary->value.image.descriptor.mip_levels ||
                pass->stencil_array_layer >=
                    auxiliary->value.image.descriptor.array_layers ||
                (destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_COLOR_TARGET) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.sample_count != 1u ||
                !ringpu_color_format(
                    destination->value.image.descriptor.format) ||
                (source->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                source->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                source->value.image.descriptor.sample_count != 1u ||
                !ringpu_depth_aspect_format(
                    source->value.image.descriptor.format) ||
                (auxiliary->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u ||
                auxiliary->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                auxiliary->value.image.descriptor.sample_count != 1u ||
                !ringpu_stencil_aspect_format(
                    auxiliary->value.image.descriptor.format) ||
                !ringpu_clear_region_valid(
                    &pass->clear_region,
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.width,
                        pass->color_mip_level),
                    ringpu_mip_dimension(
                        destination->value.image.descriptor.height,
                        pass->color_mip_level)) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.width,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.width,
                        pass->depth_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.height,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        source->value.image.descriptor.height,
                        pass->depth_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.width,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        auxiliary->value.image.descriptor.width,
                        pass->stencil_mip_level) ||
                ringpu_mip_dimension(
                    destination->value.image.descriptor.height,
                    pass->color_mip_level) !=
                    ringpu_mip_dimension(
                        auxiliary->value.image.descriptor.height,
                        pass->stencil_mip_level) ||
                destination_states[
                    pass->color_array_layer *
                        destination->value.image.descriptor.mip_levels +
                    pass->color_mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source_states[
                    pass->depth_array_layer *
                        source->value.image.descriptor.mip_levels +
                    pass->depth_mip_level] != RIN_GPU_IMAGE_STATE_DEPTH_TARGET ||
                auxiliary_states[
                    pass->stencil_array_layer *
                        auxiliary->value.image.descriptor.mip_levels +
                    pass->stencil_mip_level] != RIN_GPU_IMAGE_STATE_DEPTH_TARGET) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.render_pass_depth_stencil_begin
                .color_target_cookie = destination->value.image.backend_cookie;
            commands[index].value.render_pass_depth_stencil_begin
                .depth_target_cookie = source->value.image.backend_cookie;
            commands[index].value.render_pass_depth_stencil_begin
                .stencil_target_cookie = auxiliary->value.image.backend_cookie;
            commands[index].value.render_pass_depth_stencil_begin.color_mip_level =
                pass->color_mip_level;
            commands[index].value.render_pass_depth_stencil_begin.color_array_layer =
                pass->color_array_layer;
            commands[index].value.render_pass_depth_stencil_begin.depth_mip_level =
                pass->depth_mip_level;
            commands[index].value.render_pass_depth_stencil_begin.depth_array_layer =
                pass->depth_array_layer;
            commands[index].value.render_pass_depth_stencil_begin.stencil_mip_level =
                pass->stencil_mip_level;
            commands[index].value.render_pass_depth_stencil_begin.stencil_array_layer =
                pass->stencil_array_layer;
            commands[index].value.render_pass_depth_stencil_begin.color_load_op =
                pass->color_load_op;
            commands[index].value.render_pass_depth_stencil_begin.color_store_op =
                pass->color_store_op;
            commands[index].value.render_pass_depth_stencil_begin.depth_load_op =
                pass->depth_load_op;
            commands[index].value.render_pass_depth_stencil_begin.depth_store_op =
                pass->depth_store_op;
            commands[index].value.render_pass_depth_stencil_begin.clear_red =
                pass->clear_red;
            commands[index].value.render_pass_depth_stencil_begin.clear_green =
                pass->clear_green;
            commands[index].value.render_pass_depth_stencil_begin.clear_blue =
                pass->clear_blue;
            commands[index].value.render_pass_depth_stencil_begin.clear_alpha =
                pass->clear_alpha;
            commands[index].value.render_pass_depth_stencil_begin.clear_depth =
                pass->clear_depth;
            commands[index].value.render_pass_depth_stencil_begin.stencil_load_op =
                pass->stencil_load_op;
            commands[index].value.render_pass_depth_stencil_begin.stencil_store_op =
                pass->stencil_store_op;
            commands[index].value.render_pass_depth_stencil_begin.clear_stencil =
                pass->clear_stencil;
            commands[index].value.render_pass_depth_stencil_begin.stencil_write_mask =
                pass->stencil_write_mask;
            commands[index].value.render_pass_depth_stencil_begin.color_write_mask =
                pass->color_write_mask;
            commands[index].value.render_pass_depth_stencil_begin.clear_region =
                pass->clear_region;
            active_render_target = command->destination;
            active_depth_target = command->source;
            active_stencil_target = command->auxiliary;
            active_render_mip_level = pass->color_mip_level;
            active_render_array_layer = pass->color_array_layer;
            active_depth_mip_level = pass->depth_mip_level;
            active_depth_array_layer = pass->depth_array_layer;
            active_stencil_mip_level = 0u;
            active_stencil_array_layer = 0u;
            active_stencil_mip_level = pass->stencil_mip_level;
            active_stencil_array_layer = pass->stencil_array_layer;
            active_depth_format = RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE) {
            const RinGpuRasterStateV5* state = &command->value.raster_state;
            uint32_t target_width;
            uint32_t target_height;
            if (active_render_target == 0u ||
                !ringpu_raster_state_valid(&state->base.base.base.base)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, active_render_target,
                                 RIN_GPU_OBJECT_IMAGE, NULL, &destination);
            if (result != RIN_GPU_OK) break;
            target_width = ringpu_mip_dimension(
                destination->value.image.descriptor.width,
                active_render_mip_level);
            target_height = ringpu_mip_dimension(
                destination->value.image.descriptor.height,
                active_render_mip_level);
            /* Viewports may extend beyond an attachment, including from a
             * negative lower-left origin. Backends clip coverage to the
             * attachment; only the integer scissor retains bounded target
             * coordinates in this profile. */
            if (state->base.base.base.base.scissor.enabled != 0u &&
                 ((uint32_t)state->base.base.base.base.scissor.x > target_width ||
                  state->base.base.base.base.scissor.width >
                      target_width - (uint32_t)state->base.base.base.base.scissor.x ||
                  (uint32_t)state->base.base.base.base.scissor.y > target_height ||
                  state->base.base.base.base.scissor.height >
                      target_height - (uint32_t)state->base.base.base.base.scissor.y)) {
                result = RIN_GPU_ERROR_BOUNDS;
                break;
            }
            commands[index].value.raster_state.viewport_x = state->base.base.base.base.viewport.x;
            commands[index].value.raster_state.viewport_y = state->base.base.base.base.viewport.y;
            commands[index].value.raster_state.viewport_width =
                state->base.base.base.base.viewport.width;
            commands[index].value.raster_state.viewport_height =
                state->base.base.base.base.viewport.height;
            commands[index].value.raster_state.min_depth =
                state->base.base.base.base.viewport.min_depth;
            commands[index].value.raster_state.max_depth =
                state->base.base.base.base.viewport.max_depth;
            commands[index].value.raster_state.scissor_x = state->base.base.base.base.scissor.x;
            commands[index].value.raster_state.scissor_y = state->base.base.base.base.scissor.y;
            commands[index].value.raster_state.scissor_width =
                state->base.base.base.base.scissor.width;
            commands[index].value.raster_state.scissor_height =
                state->base.base.base.base.scissor.height;
            commands[index].value.raster_state.scissor_enabled =
                state->base.base.base.base.scissor.enabled;
            commands[index].value.raster_state.polygon_offset_fill_enabled =
                state->base.base.base.polygon_offset_fill_enabled;
            commands[index].value.raster_state.polygon_offset_factor =
                state->base.base.base.polygon_offset_factor;
            commands[index].value.raster_state.polygon_offset_units =
                state->base.base.base.polygon_offset_units;
            commands[index].value.raster_state.line_width = state->base.base.line_width;
            commands[index].value.raster_state.sample_coverage_enabled =
                state->base.sample_coverage_enabled;
            commands[index].value.raster_state.sample_coverage_value =
                state->base.sample_coverage_value;
            commands[index].value.raster_state.sample_coverage_invert =
                state->base.sample_coverage_invert;
            commands[index].value.raster_state.dither_enabled =
                state->dither_enabled;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_DRAW) {
            uint64_t bind_group_cookie;
            if (active_render_target != command->source ||
                active_render_mip_level != command->value.draw.mip_level ||
                active_render_array_layer !=
                    command->value.draw.array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            if (source_states[
                    command->value.draw.array_layer *
                        source->value.image.descriptor.mip_levels +
                    command->value.draw.mip_level] !=
                    RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                destination->value.graphics_pipeline.vertex_input_count != 0u ||
                destination->value.graphics_pipeline.vertex_stride != 0u ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.draw.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw.mip_level =
                command->value.draw.mip_level;
            commands[index].value.draw.array_layer =
                command->value.draw.array_layer;
            commands[index].value.draw.vertex_count =
                command->value.draw.vertex_count;
            commands[index].value.draw.instance_count =
                command->value.draw.instance_count;
            commands[index].value.draw.first_vertex =
                command->value.draw.first_vertex;
            commands[index].value.draw.first_instance =
                command->value.draw.first_instance;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES) {
            const RinGpuDrawVerticesV1* draw =
                &command->value.draw_vertices;
            RinGpuObjectSlot* vertex_buffer = NULL;
            uint64_t bind_group_cookie;
            uint64_t vertex_end;
            uint64_t required_bytes;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            if (destination->value.graphics_pipeline.vertex_stride != 0u) {
                result = ringpu_slot(core, draw->vertex_buffer,
                                     RIN_GPU_OBJECT_BUFFER, NULL,
                                     &vertex_buffer);
                if (result != RIN_GPU_OK) break;
            }
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            vertex_end = (uint64_t)draw->first_vertex + draw->vertex_count;
            if (destination->value.graphics_pipeline.vertex_input_count == 0u ||
                (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u ||
                (destination->value.graphics_pipeline.vertex_stride == 0u
                     ? (draw->vertex_buffer != 0u ||
                        draw->vertex_offset != 0u)
                     : (vertex_buffer == NULL ||
                        (vertex_buffer->value.buffer.usage &
                         RIN_GPU_BUFFER_VERTEX) == 0u ||
                        !ringpu_buffer_upload_ready(vertex_buffer) ||
                        !ringpu_multiply_u64(
                            vertex_end,
                            destination->value.graphics_pipeline.vertex_stride,
                            &required_bytes) ||
                        draw->vertex_offset >
                            vertex_buffer->value.buffer.size_bytes ||
                        required_bytes >
                            vertex_buffer->value.buffer.size_bytes -
                                draw->vertex_offset)) ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.draw_vertices.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_vertices.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_vertices.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_vertices.vertex_buffer_cookie =
                vertex_buffer != NULL
                    ? vertex_buffer->value.buffer.backend_cookie : 0u;
            commands[index].value.draw_vertices.vertex_offset =
                draw->vertex_offset;
            commands[index].value.draw_vertices.vertex_stride =
                destination->value.graphics_pipeline.vertex_stride;
            commands[index].value.draw_vertices.mip_level = draw->mip_level;
            commands[index].value.draw_vertices.array_layer =
                draw->array_layer;
            commands[index].value.draw_vertices.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_vertices.instance_count =
                draw->instance_count;
            commands[index].value.draw_vertices.first_vertex =
                draw->first_vertex;
            commands[index].value.draw_vertices.first_instance =
                draw->first_instance;
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2) {
            const RinGpuDrawVerticesV2* draw =
                &command->value.draw_vertices_v2;
            RinGpuObjectSlot* vertex_buffers[
                RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
            uint64_t bind_group_cookie;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            if (destination->value.graphics_pipeline.vertex_input_count == 0u ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_vertex_bindings_for_draw(
                core, destination, draw->vertex_buffers, draw->binding_count,
                draw->first_vertex, draw->vertex_count, draw->first_instance,
                draw->instance_count, vertex_buffers);
            if (result != RIN_GPU_OK) break;
            commands[index].value.draw_vertices_v2.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_vertices_v2.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_vertices_v2.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_vertices_v2.mip_level = draw->mip_level;
            commands[index].value.draw_vertices_v2.array_layer =
                draw->array_layer;
            commands[index].value.draw_vertices_v2.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_vertices_v2.instance_count =
                draw->instance_count;
            commands[index].value.draw_vertices_v2.first_vertex =
                draw->first_vertex;
            commands[index].value.draw_vertices_v2.first_instance =
                draw->first_instance;
            commands[index].value.draw_vertices_v2.vertex_binding_count =
                draw->binding_count;
            for (uint32_t binding = 0u; binding < draw->binding_count;
                 ++binding) {
                commands[index].value.draw_vertices_v2.vertex_buffers[binding]
                    .binding = draw->vertex_buffers[binding].binding;
                commands[index].value.draw_vertices_v2.vertex_buffers[binding]
                    .buffer_cookie =
                        vertex_buffers[binding]->value.buffer.backend_cookie;
                commands[index].value.draw_vertices_v2.vertex_buffers[binding]
                    .offset = draw->vertex_buffers[binding].offset;
            }
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED ||
                   command->type ==
                       RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
            const RinGpuDrawIndexedV1* draw =
                &command->value.draw_indexed;
            RinGpuObjectSlot* vertex_buffer = NULL;
            RinGpuObjectSlot* index_buffer;
            uint64_t bind_group_cookie;
            uint64_t vertex_bytes;
            uint64_t index_end;
            uint64_t index_bytes;
            uint32_t index_stride;
            int32_t base_vertex = command->base_vertex;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            if (destination->value.graphics_pipeline.vertex_stride != 0u) {
                result = ringpu_slot(core, draw->vertex_buffer,
                                     RIN_GPU_OBJECT_BUFFER, NULL,
                                     &vertex_buffer);
                if (result != RIN_GPU_OK) break;
            }
            result = ringpu_slot(core, draw->index_buffer,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &index_buffer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            index_stride = ringpu_index_format_bytes(draw->index_format);
            index_end = (uint64_t)draw->first_index + draw->index_count;
            if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
                destination->value.graphics_pipeline.vertex_input_count == 0u ||
                index_stride == 0u || draw->index_count == 0u ||
                draw->vertex_count == 0u || draw->instance_count == 0u ||
                !ringpu_base_vertex_has_valid_index(
                    base_vertex, draw->vertex_count, index_stride) ||
                (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u ||
                (draw->vertex_offset & (sizeof(uint32_t) - 1u)) != 0u ||
                (index_buffer->value.buffer.usage &
                 RIN_GPU_BUFFER_INDEX) == 0u ||
                !ringpu_buffer_upload_ready(index_buffer) ||
                draw->index_offset > index_buffer->value.buffer.size_bytes ||
                index_bytes >
                    index_buffer->value.buffer.size_bytes -
                        draw->index_offset ||
                (destination->value.graphics_pipeline.vertex_stride == 0u
                     ? (draw->vertex_buffer != 0u ||
                        draw->vertex_offset != 0u)
                     : (vertex_buffer == NULL ||
                        (vertex_buffer->value.buffer.usage &
                         RIN_GPU_BUFFER_VERTEX) == 0u ||
                        !ringpu_buffer_upload_ready(vertex_buffer) ||
                        !ringpu_multiply_u64(
                            draw->vertex_count,
                            destination->value.graphics_pipeline.vertex_stride,
                            &vertex_bytes) ||
                        draw->vertex_offset >
                            vertex_buffer->value.buffer.size_bytes ||
                        vertex_bytes >
                            vertex_buffer->value.buffer.size_bytes -
                                draw->vertex_offset)) ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.draw_indexed.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_indexed.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_indexed.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_indexed.vertex_buffer_cookie =
                vertex_buffer != NULL
                    ? vertex_buffer->value.buffer.backend_cookie : 0u;
            commands[index].value.draw_indexed.index_buffer_cookie =
                index_buffer->value.buffer.backend_cookie;
            commands[index].value.draw_indexed.vertex_offset =
                draw->vertex_offset;
            commands[index].value.draw_indexed.index_offset =
                draw->index_offset;
            commands[index].value.draw_indexed.vertex_stride =
                destination->value.graphics_pipeline.vertex_stride;
            commands[index].value.draw_indexed.index_format =
                draw->index_format;
            commands[index].value.draw_indexed.mip_level = draw->mip_level;
            commands[index].value.draw_indexed.array_layer =
                draw->array_layer;
            commands[index].value.draw_indexed.index_count =
                draw->index_count;
            commands[index].value.draw_indexed.instance_count =
                draw->instance_count;
            commands[index].value.draw_indexed.first_index =
                draw->first_index;
            commands[index].value.draw_indexed.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_indexed.first_instance =
                draw->first_instance;
            if (command->type ==
                RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX) {
                commands[index].value.draw_indexed_base_vertex.base_vertex =
                    base_vertex;
            } else {
                commands[index].value.draw_indexed.reserved = 0u;
            }
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2) {
            const RinGpuDrawIndexedV2* draw =
                &command->value.draw_indexed_v2;
            RinGpuObjectSlot* vertex_buffers[
                RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS] = {0};
            RinGpuObjectSlot* index_buffer;
            uint64_t bind_group_cookie;
            uint64_t index_end;
            uint64_t index_bytes;
            uint32_t index_stride;
            if (active_render_target != command->source ||
                active_render_mip_level != draw->mip_level ||
                active_render_array_layer != draw->array_layer) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_backend_graphics_bind_group(
                core, command->destination, destination, command->resources,
                &bind_group_cookie);
            if (result != RIN_GPU_OK) break;
            if (ringpu_graphics_draw_has_hazard(
                    core, list, command->resources, index)) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_validate_graphics_sampled_submit(
                core, command->resources, staged_states,
                active_render_target, active_render_mip_level,
                active_render_array_layer, active_depth_target,
                active_depth_mip_level, active_depth_array_layer,
                active_stencil_target, active_stencil_mip_level,
                active_stencil_array_layer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, command->source,
                                 RIN_GPU_OBJECT_IMAGE, &source_index, &source);
            if (result != RIN_GPU_OK) break;
            result = ringpu_slot(core, draw->index_buffer,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &index_buffer);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(source, source_index,
                                                staged_states);
            if (result != RIN_GPU_OK) break;
            source_states = staged_states[source_index];
            index_stride = ringpu_index_format_bytes(draw->index_format);
            index_end = (uint64_t)draw->first_index + draw->index_count;
            if (!ringpu_multiply_u64(index_end, index_stride, &index_bytes) ||
                destination->value.graphics_pipeline.vertex_input_count == 0u ||
                index_stride == 0u || draw->index_count == 0u ||
                draw->vertex_count == 0u || draw->instance_count == 0u ||
                (draw->index_offset & (uint64_t)(index_stride - 1u)) != 0u ||
                (index_buffer->value.buffer.usage &
                 RIN_GPU_BUFFER_INDEX) == 0u ||
                !ringpu_buffer_upload_ready(index_buffer) ||
                draw->index_offset > index_buffer->value.buffer.size_bytes ||
                index_bytes >
                    index_buffer->value.buffer.size_bytes - draw->index_offset ||
                (destination->value.graphics_pipeline.depth_format != 0u &&
                 (active_depth_target == 0u ||
                  active_depth_format !=
                      destination->value.graphics_pipeline.depth_format)) ||
                source_states[
                    draw->array_layer *
                        source->value.image.descriptor.mip_levels +
                    draw->mip_level] != RIN_GPU_IMAGE_STATE_COLOR_TARGET ||
                source->value.image.descriptor.format !=
                    destination->value.graphics_pipeline.color_format) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_vertex_bindings_for_draw(
                core, destination, draw->vertex_buffers, draw->binding_count,
                0u, draw->vertex_count, draw->first_instance,
                draw->instance_count, vertex_buffers);
            if (result != RIN_GPU_OK) break;
            commands[index].value.draw_indexed_v2.pipeline_cookie =
                destination->value.graphics_pipeline.backend_cookie;
            commands[index].value.draw_indexed_v2.color_target_cookie =
                source->value.image.backend_cookie;
            commands[index].value.draw_indexed_v2.bind_group_cookie =
                bind_group_cookie;
            commands[index].value.draw_indexed_v2.index_buffer_cookie =
                index_buffer->value.buffer.backend_cookie;
            commands[index].value.draw_indexed_v2.index_offset =
                draw->index_offset;
            commands[index].value.draw_indexed_v2.index_format =
                draw->index_format;
            commands[index].value.draw_indexed_v2.mip_level = draw->mip_level;
            commands[index].value.draw_indexed_v2.array_layer =
                draw->array_layer;
            commands[index].value.draw_indexed_v2.index_count =
                draw->index_count;
            commands[index].value.draw_indexed_v2.instance_count =
                draw->instance_count;
            commands[index].value.draw_indexed_v2.first_index =
                draw->first_index;
            commands[index].value.draw_indexed_v2.vertex_count =
                draw->vertex_count;
            commands[index].value.draw_indexed_v2.first_instance =
                draw->first_instance;
            commands[index].value.draw_indexed_v2.vertex_binding_count =
                draw->binding_count;
            for (uint32_t binding = 0u; binding < draw->binding_count;
                 ++binding) {
                commands[index].value.draw_indexed_v2.vertex_buffers[binding]
                    .binding = draw->vertex_buffers[binding].binding;
                commands[index].value.draw_indexed_v2.vertex_buffers[binding]
                    .buffer_cookie =
                        vertex_buffers[binding]->value.buffer.backend_cookie;
                commands[index].value.draw_indexed_v2.vertex_buffers[binding]
                    .offset = draw->vertex_buffers[binding].offset;
            }
        } else if (command->type ==
                   RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS) {
            if (active_render_target == 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            active_render_target = 0u;
            active_depth_target = 0u;
            active_stencil_target = 0u;
            active_render_mip_level = 0u;
            active_render_array_layer = 0u;
            active_depth_mip_level = 0u;
            active_depth_array_layer = 0u;
            active_stencil_mip_level = 0u;
            active_stencil_array_layer = 0u;
            active_depth_format = 0u;
        } else if (command->type == RIN_GPU_BACKEND_COMMAND_PRESENT) {
            const RinGpuPresentV1* present = &command->value.present;
            const RinGpuDisplayInfoV1* display;
            if (active_render_target != 0u || active_depth_target != 0u ||
                active_stencil_target != 0u ||
                present->image != command->destination ||
                present->flags != 0u || present->reserved0 != 0u ||
                present->reserved1 != 0u) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            display = ringpu_find_display(core, present->display_id);
            if (!display) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            result = ringpu_slot(core, command->destination,
                                 RIN_GPU_OBJECT_IMAGE, &destination_index,
                                 &destination);
            if (result != RIN_GPU_OK) break;
            result = ringpu_stage_image_states(
                destination, destination_index, staged_states);
            if (result != RIN_GPU_OK) break;
            destination_states = staged_states[destination_index];
            if ((destination->value.image.descriptor.usage &
                 RIN_GPU_IMAGE_PRESENT) == 0u ||
                destination->value.image.descriptor.dimension !=
                    RIN_GPU_IMAGE_DIMENSION_2D ||
                destination->value.image.descriptor.array_layers != 1u ||
                destination->value.image.descriptor.mip_levels != 1u ||
                destination->value.image.descriptor.sample_count != 1u ||
                destination->value.image.descriptor.width != display->width ||
                destination->value.image.descriptor.height !=
                    display->height ||
                destination->value.image.descriptor.format !=
                    display->format ||
                destination_states[0] != RIN_GPU_IMAGE_STATE_PRESENT) {
                result = RIN_GPU_ERROR_STATE;
                break;
            }
            commands[index].value.present.image_cookie =
                destination->value.image.backend_cookie;
            commands[index].value.present.display_id = present->display_id;
        } else {
            result = RIN_GPU_ERROR_STATE;
            break;
        }
    }
    if (result == RIN_GPU_OK &&
        (active_render_target != 0u || active_depth_target != 0u ||
         active_stencil_target != 0u)) {
        result = RIN_GPU_ERROR_STATE;
    }
    if (result == RIN_GPU_OK) {
        for (uint32_t index = 0u;
             index < list->value.command_list.count; ++index) {
            if (commands[index].type == RIN_GPU_BACKEND_COMMAND_PRESENT)
                ++present_count;
        }
    }
    if (result == RIN_GPU_OK) {
        result = core->backend.submit_commands(
            core->backend_context, commands,
            list->value.command_list.count);
    }
    ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_SUBMISSION,
                           submit->command_list, queue,
                           list->value.command_list.count,
                           submit->signal_value, result);
    for (uint32_t index = 0u; index < present_count; ++index)
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_PRESENT, 0u, queue,
                               submit->signal_value, index, result);
    if (result == RIN_GPU_OK) {
        for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
            if (!staged_states[index]) continue;
            memcpy(core->objects[index].value.image.subresource_states,
                   staged_states[index],
                   (size_t)core->objects[index].value.image.subresource_count *
                       sizeof(*staged_states[index]));
        }
    }
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        free(staged_states[index]);
    }
    free(commands);
    if (result != RIN_GPU_OK) return result;
    if (fence) fence->value.fence.value = submit->signal_value;
    return RIN_GPU_OK;
}

int ringpu_destroy(RinGpuCore* core, RinGpuHandle object) {
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    result = ringpu_slot(core, object, RIN_GPU_OBJECT_NONE, NULL, &slot);
    if (result != RIN_GPU_OK) return result;
    if (slot->type == RIN_GPU_OBJECT_BUFFER) {
        if (slot->value.buffer.reference_count != 0u) return RIN_GPU_ERROR_BUSY;
        core->backend.destroy_buffer(core->backend_context,
                                     slot->value.buffer.backend_cookie);
        core->allocated_bytes -= slot->value.buffer.size_bytes;
    } else if (slot->type == RIN_GPU_OBJECT_IMAGE) {
        if (slot->value.image.reference_count != 0u) return RIN_GPU_ERROR_BUSY;
        core->backend.destroy_image(core->backend_context,
                                    slot->value.image.backend_cookie);
        free(slot->value.image.subresource_states);
        free(slot->value.image.cpu_upload_complete);
        core->allocated_bytes -= slot->value.image.allocation_bytes;
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
        result = ringpu_slot(core, slot->value.compute_pipeline.shader_module,
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
        result = ringpu_slot(core,
                             slot->value.graphics_pipeline.vertex_shader,
                             RIN_GPU_OBJECT_SHADER_MODULE, NULL,
                             &vertex_shader);
        if (result != RIN_GPU_OK ||
            vertex_shader->value.shader_module.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        result = ringpu_slot(core,
                             slot->value.graphics_pipeline.fragment_shader,
                             RIN_GPU_OBJECT_SHADER_MODULE, NULL,
                             &fragment_shader);
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
        result = ringpu_slot(core, slot->value.compute_bind_group.pipeline,
                             RIN_GPU_OBJECT_COMPUTE_PIPELINE, NULL, &pipeline);
        if (result != RIN_GPU_OK ||
            pipeline->value.compute_pipeline.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        for (uint32_t binding = 0u; binding < binding_count; binding++) {
            RinGpuObjectSlot* buffer;
            result = ringpu_slot(core, bindings[binding].buffer,
                                 RIN_GPU_OBJECT_BUFFER, NULL, &buffer);
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
        result = ringpu_slot(core, slot->value.graphics_bind_group.pipeline,
                             RIN_GPU_OBJECT_GRAPHICS_PIPELINE, NULL,
                             &pipeline);
        if (result != RIN_GPU_OK ||
            pipeline->value.graphics_pipeline.reference_count == 0u) {
            return RIN_GPU_ERROR_STATE;
        }
        for (uint32_t binding = 0u; binding < binding_count; binding++) {
            const RinGpuGraphicsBindingV1* resource_binding =
                &bindings[binding];
            RinGpuObjectSlot* resource;
            uint32_t references;
            result = ringpu_graphics_binding_slot(core, resource_binding,
                                                  &resource);
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
    }
    ringpu_release_slot(slot);
    return RIN_GPU_OK;
}
