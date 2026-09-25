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
#include "../presentation/render_pass.h"
#include "../resource/resources.h"
#include "../software/software_backend.h"
#include "../shader/modules.h"
#include "../pipeline/pipelines.h"
#include "../command/commands.h"
#include "../command/draw.h"

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
        } else if (slot->type == RIN_GPU_OBJECT_QUERY &&
                   core->backend.destroy_query != NULL) {
            uint64_t raw = ((uint64_t)slot->generation << 32) |
                           ((uint64_t)RIN_GPU_OBJECT_QUERY << 16) |
                           (uint64_t)(index + 1u);
            core->backend.destroy_query(
                core->backend_context, raw ^ core->handle_secret);
        }
    }
    /* Resource cookies are released before memory objects so a bound
     * resource can still refer to its backing while its software wrapper is
     * being destroyed. Core shutdown is unconditional, so reference counts
     * do not need to be decremented in this final pass. */
    for (uint32_t index = 0; index < RIN_GPU_CORE_MAX_OBJECTS; index++) {
        RinGpuObjectSlot* slot = &core->objects[index];
        if (!slot->occupied || slot->type != RIN_GPU_OBJECT_MEMORY)
            continue;
        ringpu_software_backend_destroy_memory(
            core->backend_context, slot->value.memory.bytes,
            slot->value.memory.size_bytes);
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



static int ringpu_create_queue_owner(RinGpuCore* core, uint32_t capabilities,
                                     uint32_t flags, uint32_t family_index,
                                     uint32_t engine_index,
                                     RinGpuHandle* queue) {
    RinGpuObjectSlot* slot;
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!queue || capabilities == 0u ||
        (capabilities & ~core->adapter.queue_capabilities) != 0u ||
        flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = ringpu_allocate(core, RIN_GPU_OBJECT_QUEUE, queue, &slot);
    if (result == RIN_GPU_OK) {
        slot->value.queue.capabilities = capabilities;
        slot->value.queue.family_index = family_index;
        slot->value.queue.engine_index = engine_index;
        ringpu_core_diagnostic(core, RIN_GPU_DIAGNOSTIC_QUEUE, *queue, *queue,
                               capabilities, 0u, RIN_GPU_OK);
    }
    return result;
}

int ringpu_create_queue(RinGpuCore* core, const RinGpuQueueDescV1* desc,
                        RinGpuHandle* queue) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !queue ||
        !ringpu_versioned(desc->abi_version, desc->struct_size, sizeof(*desc)))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return ringpu_create_queue_owner(core, desc->capabilities, desc->flags, 0u,
                                     0u, queue);
}

int ringpu_create_queue_v2(RinGpuCore* core, const RinGpuQueueDescV2* desc,
                           RinGpuHandle* queue) {
    int result = ringpu_core_ready(core);
    if (result != RIN_GPU_OK) return result;
    if (!desc || !queue ||
        !ringpu_versioned(desc->base.abi_version, desc->base.struct_size,
                          sizeof(desc->base)) ||
        desc->reserved0 != 0u || desc->reserved1 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return ringpu_create_queue_owner(core, desc->base.capabilities,
                                     desc->base.flags, desc->family_index,
                                     desc->engine_index, queue);
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
