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
    case RIN_GPU_FORMAT_RGBA8_SRGB:
    case RIN_GPU_FORMAT_BGRA8_SRGB:
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
           format == RIN_GPU_FORMAT_RGBA8_SRGB ||
           format == RIN_GPU_FORMAT_BGRA8_SRGB ||
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

int ringpu_image_allocation_size(const RinGpuCore* core,
                                 const RinGpuImageDescV1* desc,
                                 uint64_t* size_out)
{
    uint64_t total = 0u;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t format_bytes;
    uint32_t possible_mips = 1u;
    uint32_t largest;
    uint32_t forbidden_usage;

    if (size_out) *size_out = 0u;
    if (!core || !desc || !size_out ||
        !ringpu_versioned(desc->abi_version, desc->struct_size,
                          sizeof(*desc)) ||
        desc->dimension < RIN_GPU_IMAGE_DIMENSION_1D ||
        desc->dimension > RIN_GPU_IMAGE_DIMENSION_3D ||
        desc->width == 0u || desc->height == 0u || desc->depth == 0u ||
        desc->array_layers == 0u || desc->mip_levels == 0u ||
        desc->sample_count == 0u || desc->usage == 0u ||
        (desc->usage & ~RIN_GPU_IMAGE_KNOWN_USAGE) != 0u ||
        (desc->flags & ~RIN_GPU_IMAGE_KNOWN_FLAGS) != 0u ||
        desc->width > core->max_image_dimension ||
        desc->height > core->max_image_dimension ||
        desc->depth > core->max_image_dimension ||
        desc->array_layers > core->max_image_layers ||
        desc->mip_levels > core->max_image_mip_levels ||
        (uint64_t)desc->array_layers * desc->mip_levels >
            RIN_GPU_CORE_MAX_IMAGE_SUBRESOURCES ||
        desc->sample_count > core->max_image_sample_count ||
        (desc->sample_count & (desc->sample_count - 1u)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    format_bytes = ringpu_image_format_bytes(desc->format);
    if (format_bytes == 0u ||
        (desc->dimension == RIN_GPU_IMAGE_DIMENSION_1D &&
         (desc->height != 1u || desc->depth != 1u)) ||
        (desc->dimension == RIN_GPU_IMAGE_DIMENSION_2D &&
         desc->depth != 1u) ||
        (desc->dimension == RIN_GPU_IMAGE_DIMENSION_3D &&
         desc->array_layers != 1u) ||
        (desc->sample_count != 1u &&
         (desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
          desc->array_layers != 1u || desc->mip_levels != 1u)) ||
        ((desc->usage & RIN_GPU_IMAGE_STORAGE) != 0u &&
         desc->sample_count != 1u) ||
        ((desc->flags & RIN_GPU_IMAGE_CPU_VISIBLE) != 0u &&
          ((desc->usage & RIN_GPU_IMAGE_COPY_DESTINATION) == 0u ||
           desc->sample_count != 1u)) ||
        ((desc->flags & RIN_GPU_IMAGE_CPU_READABLE) != 0u &&
          ((desc->usage & RIN_GPU_IMAGE_COPY_SOURCE) == 0u ||
           desc->sample_count != 1u))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (ringpu_depth_stencil_format(desc->format)) {
        forbidden_usage = RIN_GPU_IMAGE_COLOR_TARGET |
            RIN_GPU_IMAGE_PRESENT | RIN_GPU_IMAGE_STORAGE;
        if ((desc->usage & forbidden_usage) != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        if (desc->format == RIN_GPU_FORMAT_S8_UINT &&
            (((desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) == 0u) ||
             (desc->usage & RIN_GPU_IMAGE_SAMPLED) != 0u)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else if (desc->format == RIN_GPU_FORMAT_RGBA16_FLOAT ||
               desc->format == RIN_GPU_FORMAT_RGBA32_FLOAT) {
        forbidden_usage = ~(RIN_GPU_IMAGE_COPY_DESTINATION |
            RIN_GPU_IMAGE_SAMPLED | RIN_GPU_IMAGE_COLOR_TARGET |
            RIN_GPU_IMAGE_COPY_SOURCE) & RIN_GPU_IMAGE_KNOWN_USAGE;
        if ((desc->usage & forbidden_usage) != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    } else if (desc->format == RIN_GPU_FORMAT_RGBA8_SRGB ||
               desc->format == RIN_GPU_FORMAT_BGRA8_SRGB) {
        forbidden_usage = ~(RIN_GPU_IMAGE_COPY_DESTINATION |
            RIN_GPU_IMAGE_SAMPLED | RIN_GPU_IMAGE_COLOR_TARGET |
            RIN_GPU_IMAGE_COPY_SOURCE) & RIN_GPU_IMAGE_KNOWN_USAGE;
        if ((desc->usage & forbidden_usage) != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    } else if ((desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if ((desc->usage & RIN_GPU_IMAGE_PRESENT) != 0u &&
        (desc->dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
         desc->array_layers != 1u || desc->mip_levels != 1u ||
         desc->sample_count != 1u ||
         (desc->format != RIN_GPU_FORMAT_RGBA8_UNORM &&
          desc->format != RIN_GPU_FORMAT_BGRA8_UNORM) ||
         (desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) == 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    largest = desc->width;
    if (desc->height > largest) largest = desc->height;
    if (desc->depth > largest) largest = desc->depth;
    while (largest > 1u) {
        largest >>= 1u;
        possible_mips++;
    }
    if (desc->mip_levels > possible_mips)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    width = desc->width;
    height = desc->height;
    depth = desc->depth;
    for (uint32_t mip = 0u; mip < desc->mip_levels; ++mip) {
        uint64_t level;
        if (!ringpu_multiply_u64(width, height, &level) ||
            !ringpu_multiply_u64(level, depth, &level) ||
            !ringpu_multiply_u64(level, desc->array_layers, &level) ||
            !ringpu_multiply_u64(level, desc->sample_count, &level) ||
            !ringpu_multiply_u64(level, format_bytes, &level) ||
            level > UINT64_MAX - total) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        total += level;
        if (width > 1u) width >>= 1u;
        if (height > 1u) height >>= 1u;
        if (depth > 1u) depth >>= 1u;
    }
    if (total == 0u || total > core->max_image_size)
        return RIN_GPU_ERROR_BOUNDS;
    *size_out = total;
    return RIN_GPU_OK;
}

uint32_t ringpu_image_mip_extent(uint32_t extent, uint32_t mip_level)
{
    while (mip_level != 0u && extent > 1u) {
        extent >>= 1u;
        mip_level--;
    }
    return extent;
}

int ringpu_image_copy_side_valid(
    const RinGpuImageDescV1* desc, uint32_t mip_level, uint32_t array_layer,
    uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height,
    uint32_t depth)
{
    uint32_t mip_width;
    uint32_t mip_height;
    uint32_t mip_depth;

    if (!desc || mip_level >= desc->mip_levels ||
        array_layer >= desc->array_layers || width == 0u || height == 0u ||
        depth == 0u) {
        return 0;
    }
    mip_width = ringpu_image_mip_extent(desc->width, mip_level);
    mip_height = ringpu_image_mip_extent(desc->height, mip_level);
    mip_depth = ringpu_image_mip_extent(desc->depth, mip_level);
    if (x > mip_width || width > mip_width - x ||
        y > mip_height || height > mip_height - y ||
        z > mip_depth || depth > mip_depth - z) {
        return 0;
    }
    if ((desc->dimension == RIN_GPU_IMAGE_DIMENSION_1D &&
         (y != 0u || z != 0u || height != 1u || depth != 1u)) ||
        (desc->dimension == RIN_GPU_IMAGE_DIMENSION_2D &&
         (z != 0u || depth != 1u)) ||
        (desc->dimension == RIN_GPU_IMAGE_DIMENSION_3D &&
         array_layer != 0u)) {
        return 0;
    }
    return 1;
}

static int image_transfer_size_valid(uint64_t width, uint64_t height,
                                     uint64_t depth, uint32_t format_bytes,
                                     uint64_t* row_pitch, uint64_t* slice_pitch,
                                     uint64_t source_size)
{
    uint64_t row_bytes;
    uint64_t minimum_slice_pitch;
    uint64_t required_size;
    uint64_t extra_slices;
    uint64_t extra_rows;

    if (!ringpu_multiply_u64(width, format_bytes, &row_bytes))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (*row_pitch == 0u) *row_pitch = row_bytes;
    if (*row_pitch < row_bytes ||
        !ringpu_multiply_u64(*row_pitch, height, &minimum_slice_pitch)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (*slice_pitch == 0u) *slice_pitch = minimum_slice_pitch;
    if (*slice_pitch < minimum_slice_pitch ||
        !ringpu_multiply_u64(depth - 1u, *slice_pitch, &extra_slices) ||
        !ringpu_multiply_u64(height - 1u, *row_pitch, &extra_rows) ||
        extra_slices > UINT64_MAX - extra_rows ||
        extra_slices + extra_rows > UINT64_MAX - row_bytes) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    required_size = extra_slices + extra_rows + row_bytes;
    return source_size < required_size ? RIN_GPU_ERROR_BOUNDS : RIN_GPU_OK;
}

int ringpu_image_upload_source_valid(
    const RinGpuImageDescV1* desc, RinGpuImageUploadV1* upload,
    uint64_t source_size)
{
    uint32_t format_bytes;
    if (!desc || !upload ||
        !ringpu_versioned(upload->abi_version, upload->struct_size,
                          sizeof(*upload)) || upload->flags != 0u ||
        upload->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!ringpu_image_copy_side_valid(
            desc, upload->mip_level, upload->array_layer, upload->x,
            upload->y, upload->z, upload->width, upload->height,
            upload->depth)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    format_bytes = ringpu_image_format_bytes(desc->format);
    if (format_bytes == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return image_transfer_size_valid(
            upload->width, upload->height, upload->depth, format_bytes,
            &upload->source_row_pitch_bytes,
            &upload->source_slice_pitch_bytes, source_size);
}

int ringpu_image_readback_destination_valid(
    const RinGpuImageDescV1* desc, RinGpuImageReadbackV1* readback,
    uint64_t destination_size)
{
    uint32_t format_bytes;
    if (!desc || !readback ||
        !ringpu_versioned(readback->abi_version, readback->struct_size,
                          sizeof(*readback)) || readback->flags != 0u ||
        readback->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!ringpu_image_copy_side_valid(
            desc, readback->mip_level, readback->array_layer, readback->x,
            readback->y, readback->z, readback->width, readback->height,
            readback->depth)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    format_bytes = ringpu_image_format_bytes(desc->format);
    if (format_bytes == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    return image_transfer_size_valid(
            readback->width, readback->height, readback->depth, format_bytes,
            &readback->destination_row_pitch_bytes,
            &readback->destination_slice_pitch_bytes, destination_size);
}

int ringpu_image_state_allowed(const RinGpuImageDescV1* desc, uint32_t state)
{
    if (!desc) return 0;
    if (state == RIN_GPU_IMAGE_STATE_UNDEFINED) return 1;
    if (state == RIN_GPU_IMAGE_STATE_COPY_SOURCE)
        return (desc->usage & RIN_GPU_IMAGE_COPY_SOURCE) != 0u;
    if (state == RIN_GPU_IMAGE_STATE_COPY_DESTINATION)
        return (desc->usage & RIN_GPU_IMAGE_COPY_DESTINATION) != 0u;
    if (state == RIN_GPU_IMAGE_STATE_COLOR_TARGET)
        return (desc->usage & RIN_GPU_IMAGE_COLOR_TARGET) != 0u &&
               ringpu_color_format(desc->format);
    if (state == RIN_GPU_IMAGE_STATE_PRESENT)
        return (desc->usage & RIN_GPU_IMAGE_PRESENT) != 0u;
    if (state == RIN_GPU_IMAGE_STATE_DEPTH_TARGET)
        return (desc->usage & RIN_GPU_IMAGE_DEPTH_STENCIL) != 0u &&
               ringpu_depth_stencil_format(desc->format);
    if (state == RIN_GPU_IMAGE_STATE_SHADER_READ)
        return (desc->usage & RIN_GPU_IMAGE_SAMPLED) != 0u &&
               desc->sample_count == 1u &&
               (ringpu_sampled_image_format(desc->format) ||
                ringpu_depth_stencil_format(desc->format));
    return 0;
}
