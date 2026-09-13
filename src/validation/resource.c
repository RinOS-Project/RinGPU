// SPDX-License-Identifier: MIT
#include "resource.h"

int ringpu_range(uint64_t offset, uint64_t size, uint64_t limit)
{
    return size != 0u && offset <= limit && size <= limit - offset;
}

int ringpu_versioned(uint32_t version, uint32_t size, uint32_t required_size)
{
    return version == RIN_GPU_ABI_VERSION && size >= required_size;
}

int ringpu_buffer_upload_ready(const RinGpuObjectSlot* slot)
{
    return slot != NULL && slot->value.buffer.cpu_upload_pending == 0u;
}

int ringpu_image_upload_ready(const RinGpuObjectSlot* slot)
{
    return slot != NULL && slot->value.image.cpu_upload_pending == 0u;
}

int ringpu_multiply_u64(uint64_t left, uint64_t right, uint64_t* result)
{
    if (result == NULL || (left != 0u && right > UINT64_MAX / left))
        return 0;
    *result = left * right;
    return 1;
}

uint32_t ringpu_image_format_bytes(uint32_t format)
{
    switch (format) {
    case RIN_GPU_FORMAT_R8_UNORM:
    case RIN_GPU_FORMAT_S8_UINT:
        return 1u;
    case RIN_GPU_FORMAT_RGB565_UNORM:
    case RIN_GPU_FORMAT_RGBA4_UNORM:
    case RIN_GPU_FORMAT_RGB5_A1_UNORM:
        return 2u;
    case RIN_GPU_FORMAT_RGBA8_UNORM:
    case RIN_GPU_FORMAT_BGRA8_UNORM:
    case RIN_GPU_FORMAT_D32_FLOAT:
        return 4u;
    case RIN_GPU_FORMAT_D32_FLOAT_S8_UINT:
    case RIN_GPU_FORMAT_RGBA16_FLOAT:
        return 8u;
    case RIN_GPU_FORMAT_RGBA32_FLOAT:
        return 16u;
    default:
        return 0u;
    }
}

int ringpu_color_format(uint32_t format)
{
    return format == RIN_GPU_FORMAT_R8_UNORM ||
           format == RIN_GPU_FORMAT_RGB565_UNORM ||
           format == RIN_GPU_FORMAT_RGBA4_UNORM ||
           format == RIN_GPU_FORMAT_RGB5_A1_UNORM ||
           format == RIN_GPU_FORMAT_RGBA8_UNORM ||
           format == RIN_GPU_FORMAT_BGRA8_UNORM ||
           format == RIN_GPU_FORMAT_RGBA16_FLOAT ||
           format == RIN_GPU_FORMAT_RGBA32_FLOAT;
}

int ringpu_sampled_image_format(uint32_t format)
{
    return ringpu_color_format(format);
}

int ringpu_primitive_topology_valid(uint32_t topology)
{
    return topology == RIN_GPU_PRIMITIVE_TRIANGLE_LIST ||
           topology == RIN_GPU_PRIMITIVE_POINT_LIST ||
           topology == RIN_GPU_PRIMITIVE_LINE_LIST ||
           topology == RIN_GPU_PRIMITIVE_LINE_STRIP ||
           topology == RIN_GPU_PRIMITIVE_LINE_LOOP ||
           topology == RIN_GPU_PRIMITIVE_TRIANGLE_STRIP ||
           topology == RIN_GPU_PRIMITIVE_TRIANGLE_FAN;
}

int ringpu_depth_stencil_format(uint32_t format)
{
    return format == RIN_GPU_FORMAT_D32_FLOAT ||
           format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT ||
           format == RIN_GPU_FORMAT_S8_UINT;
}

int ringpu_depth_aspect_format(uint32_t format)
{
    return format == RIN_GPU_FORMAT_D32_FLOAT ||
           format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

int ringpu_stencil_aspect_format(uint32_t format)
{
    return format == RIN_GPU_FORMAT_S8_UINT ||
           format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

int ringpu_scanout_format(uint32_t format)
{
    return format == RIN_GPU_FORMAT_RGBA8_UNORM ||
           format == RIN_GPU_FORMAT_BGRA8_UNORM;
}
