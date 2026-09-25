// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_COMMAND_DRAW_H
#define RIN_GPU_COMMAND_DRAW_H

#include "../core/core.h"

void ringpu_release_graphics_bind_group_reference(
    RinGpuCore* core, RinGpuHandle bind_group);
int ringpu_graphics_draw_has_hazard(
    RinGpuCore* core, const RinGpuObjectSlot* list,
    RinGpuHandle new_bind_group_handle, uint32_t command_limit);
int ringpu_vertex_bindings_for_draw(
    RinGpuCore* core, const RinGpuObjectSlot* pipeline,
    const RinGpuVertexBufferBindingV1* bindings, uint32_t binding_count,
    uint32_t first_vertex, uint32_t vertex_count, uint32_t first_instance,
    uint32_t instance_count,
    RinGpuObjectSlot* resolved[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS]);
int ringpu_command_draw_indirect(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawIndirectV1* draw);
int ringpu_command_draw_indexed_indirect(
    RinGpuCore* core, RinGpuHandle command_list,
    const RinGpuDrawIndexedIndirectV1* draw);

#endif /* RIN_GPU_COMMAND_DRAW_H */
