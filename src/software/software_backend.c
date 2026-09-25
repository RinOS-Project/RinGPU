// SPDX-License-Identifier: MIT
#include "software_backend.h"
#include "../core/core.h"
#include "../validation/pipeline.h"
#include "../validation/resource.h"

#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <ringpu/rin_shader.h>

#define SW_MAX_SHADER_BYTES (1024u * 1024u)
#define SW_MAX_REGISTERS RIN_SHADER_MAX_REGISTERS
#define SW_MAX_IO RIN_SHADER_MAX_IO
/* Keep CPU work bounded even when this backend is selected outside the
 * browser embedding.  Fragment programs are run once for preflight and once
 * for publication, so this is a per-pass limit rather than an allocation. */
#define SW_MAX_DRAW_VERTICES 16384u
#define SW_MAX_DRAW_INDICES 49152u
/* Every graphics command performs a complete vertex/index preflight before
 * its two shader/raster phases. Bound the product, rather than only each
 * public dimension, so an otherwise valid instanced command cannot turn the
 * synchronous software backend into an unbounded CPU loop. */
#define SW_MAX_DRAW_VERTEX_INVOCATIONS 1048576u
#define SW_MAX_FRAGMENT_INVOCATIONS 1048576u
/* Compute execution is synchronous and deliberately bounded like fragment
 * execution. A failed invocation must not publish a prefix of its storage
 * writes, so writable buffers are shadowed before the first invocation. */
#define SW_MAX_COMPUTE_INVOCATIONS 1048576u
#define SW_MAX_COMPUTE_SHADOW_BYTES (64u * 1024u * 1024u)
/* A triangle becomes at most seven vertices after clipping against the six
 * homogeneous frustum planes. One extra slot keeps the bounded clipping
 * implementation explicit should the w safety plane create an intersection
 * at the same time as a frustum boundary. */
#define SW_MAX_CLIPPED_VERTICES 8u
#define SW_CLIP_W_EPSILON 0.000001f

typedef struct SwBuffer {
    uint8_t* bytes;
    uint64_t size_bytes;
    uint32_t owns_bytes;
} SwBuffer;

typedef struct SwImage {
    /* desc/bytes describe the selected mip/layer view used by the current
     * backend operation. allocation_* retain the immutable public image
     * descriptor and the complete backing allocation. */
    uint8_t* bytes;
    uint64_t size_bytes;
    uint64_t row_pitch_bytes;
    uint8_t* allocation_bytes;
    uint64_t allocation_size_bytes;
    float* depth_pixels;
    uint64_t depth_size_bytes;
    uint64_t depth_row_pitch_bytes;
    uint8_t* stencil_pixels;
    uint64_t stencil_size_bytes;
    uint64_t stencil_row_pitch_bytes;
    RinGpuImageDescV1 desc;
    RinGpuImageDescV1 allocation_desc;
    uint32_t active_mip_level;
    uint32_t active_array_layer;
    uint32_t external_storage;
    uint32_t owns_allocation;
} SwImage;

typedef struct SwShader {
    uint8_t* ir;
    uint64_t ir_size;
    RinShaderInfoV1 info;
} SwShader;

typedef struct SwSampler {
    RinGpuSamplerDescV1 desc;
} SwSampler;

typedef struct SwPipeline {
    SwShader* vertex;
    SwShader* fragment;
    RinGpuBackendGraphicsPipelineDescV1 desc;
} SwPipeline;

typedef struct SwComputePipeline {
    SwShader* shader;
} SwComputePipeline;

typedef struct SwComputeBindGroup {
    const SwComputePipeline* pipeline;
    SwBuffer* buffers[RIN_SHADER_MAX_RESOURCES];
    uint64_t offsets[RIN_SHADER_MAX_RESOURCES];
    uint64_t sizes[RIN_SHADER_MAX_RESOURCES];
    uint32_t access[RIN_SHADER_MAX_RESOURCES];
    uint32_t binding_count;
} SwComputeBindGroup;

typedef struct SwComputeExecution {
    const SwComputeBindGroup* bind_group;
    /* Entries name the execution view for each binding. During a dispatch a
     * writable buffer uses its private shadow; otherwise it is the live
     * buffer. This keeps aliasing coherent without exposing failed writes. */
    uint8_t* storage[RIN_SHADER_MAX_RESOURCES];
    uint32_t global_x;
    uint32_t global_y;
    uint32_t global_z;
    uint32_t local_x;
    uint32_t local_y;
    uint32_t local_z;
    uint32_t workgroup_x;
    uint32_t workgroup_y;
    uint32_t workgroup_z;
    uint8_t* shared_memory;
    uint32_t shared_memory_size;
    uint32_t shared_epoch;
    const uint8_t* push_constants;
    uint32_t push_constant_size;
} SwComputeExecution;

typedef struct SwVertexBindings {
    const SwBuffer* buffers[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
    uint64_t offsets[RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS];
    uint32_t binding_count;
} SwVertexBindings;

typedef struct SwGraphicsBindGroup {
    const SwPipeline* pipeline;
    const SwImage* images[RIN_SHADER_MAX_RESOURCES];
    const SwSampler* samplers[RIN_SHADER_MAX_RESOURCES];
    /* A sampled binding can name either one exact mip or, when the public
     * chain flag is present, the remaining contiguous mips.  Keep this
     * metadata in the backend object rather than changing SwImage's selected
     * view: one image may be used by several bindings in one submission. */
    uint32_t image_base_mip_levels[RIN_SHADER_MAX_RESOURCES];
    uint32_t image_mip_counts[RIN_SHADER_MAX_RESOURCES];
    uint32_t image_array_layers[RIN_SHADER_MAX_RESOURCES];
    uint32_t kinds[RIN_SHADER_MAX_RESOURCES];
    uint32_t binding_count;
} SwGraphicsBindGroup;

typedef struct SwClipVertex {
    float f32[SW_MAX_IO];
    int32_t i32[SW_MAX_IO];
    uint8_t types[SW_MAX_IO];
} SwClipVertex;

static int sw_f32_finite(float value);

/* Depth values have been checked finite before rasterization. Keeping this
 * tiny helper local avoids an accidental hosted libm dependency (and the
 * implicit fabsf declaration that freestanding builds rightfully reject). */
static float sw_absf(float value)
{
    return value < 0.0f ? -value : value;
}

/* Return the IEEE-754 binary32 floor without taking a hosted libm
 * dependency.  RSH1 accepts only finite values, but retain the explicit
 * finite check here because a backend must never turn malformed bytecode into
 * an implementation-defined float-to-integer conversion. */
static int sw_floorf(float value, float* result_out)
{
    uint32_t bits;
    uint32_t exponent;
    uint32_t fractional_mask;
    uint32_t integral_bits;
    int negative;

    if (result_out == NULL || !sw_f32_finite(value))
        return 0;
    memcpy(&bits, &value, sizeof(bits));
    exponent = (bits >> 23u) & 0xffu;
    negative = (bits & UINT32_C(0x80000000)) != 0u;
    if (exponent >= 150u) {
        /* All binary32 values with exponent >= 23 are already integral. */
        *result_out = value;
        return 1;
    }
    if (exponent < 127u) {
        /* Preserve signed zero. Every other negative value of magnitude
         * below one floors to -1, while positive values floor to +0. */
        if ((bits & UINT32_C(0x7fffffff)) == 0u) {
            *result_out = value;
        } else {
            uint32_t result_bits = negative ? UINT32_C(0xbf800000) : 0u;

            memcpy(result_out, &result_bits, sizeof(result_bits));
        }
        return 1;
    }
    fractional_mask = (UINT32_C(1) << (150u - exponent)) - 1u;
    if ((bits & fractional_mask) == 0u) {
        *result_out = value;
        return 1;
    }
    integral_bits = bits & ~fractional_mask;
    if (negative)
        integral_bits += UINT32_C(1) << (150u - exponent);
    memcpy(result_out, &integral_bits, sizeof(integral_bits));
    return sw_f32_finite(*result_out);
}

/* Return a finite IEEE-754 binary32 square root without depending on hosted
 * libm. A bit-derived estimate followed by fixed Newton iterations keeps the
 * result deterministic for the freestanding software backend. Subnormal
 * inputs are scaled into the normal range before iteration, then scaled back
 * by sqrt(2^24). */
static int sw_sqrtf(float value, float* result_out)
{
    uint32_t bits;
    uint32_t exponent;
    float estimate;
    uint32_t iteration;
    int scaled_subnormal = 0;

    if (result_out == NULL || !sw_f32_finite(value) || value < 0.0f)
        return 0;
    if (value == 0.0f) {
        /* Preserve the sign of zero, as required by the scalar operation. */
        *result_out = value;
        return 1;
    }
    memcpy(&bits, &value, sizeof(bits));
    exponent = (bits >> 23u) & 0xffu;
    if (exponent == 0u) {
        value *= 16777216.0f;
        if (!sw_f32_finite(value) || value == 0.0f)
            return 0;
        scaled_subnormal = 1;
        memcpy(&bits, &value, sizeof(bits));
    }
    bits = (bits >> 1u) + UINT32_C(0x1fc00000);
    memcpy(&estimate, &bits, sizeof(estimate));
    if (!sw_f32_finite(estimate) || estimate == 0.0f)
        return 0;
    for (iteration = 0u; iteration < 7u; ++iteration) {
        estimate = 0.5f * (estimate + value / estimate);
        if (!sw_f32_finite(estimate) || estimate == 0.0f)
            return 0;
    }
    if (scaled_subnormal != 0)
        estimate *= 0.000244140625f;
    if (!sw_f32_finite(estimate))
        return 0;
    *result_out = estimate;
    return 1;
}

/* The executable GLSL trigonometric profile is deliberately finite and
 * bounded: range reduction is accurate for ordinary shader angles but never
 * delegates an enormous argument to hosted libm.  Inputs outside this range
 * are rejected before target publication. */
static int sw_sincosf(float value, float* sine_out, float* cosine_out)
{
    const float pi = 3.14159265358979323846f;
    const float half_pi = 1.57079632679489661923f;
    const float quarter_pi = 0.78539816339744830962f;
    const float two_pi = 6.28318530717958647692f;
    const float inverse_two_pi = 0.15915494309189533577f;
    float turns;
    float reduced;
    float base;
    float base_squared;
    float sine;
    float cosine;
    int sine_sign = 1;
    int cosine_sign = 1;

    if (sine_out == NULL || cosine_out == NULL || !sw_f32_finite(value) ||
        sw_absf(value) > 1024.0f) {
        return 0;
    }
    if (!sw_floorf(value * inverse_two_pi + 0.5f, &turns))
        return 0;
    reduced = value - turns * two_pi;
    if (!sw_f32_finite(reduced))
        return 0;
    if (reduced > pi)
        reduced -= two_pi;
    else if (reduced < -pi)
        reduced += two_pi;
    if (reduced < 0.0f) {
        reduced = -reduced;
        sine_sign = -1;
    }
    if (reduced > half_pi) {
        reduced = pi - reduced;
        cosine_sign = -1;
    }
    if (reduced < 0.0f || reduced > half_pi)
        return 0;
    if (reduced > quarter_pi) {
        base = half_pi - reduced;
        base_squared = base * base;
        sine = 1.0f + base_squared *
            (-0.5f + base_squared *
             (0.04166666666666666667f + base_squared *
              (-0.00138888888888888889f + base_squared *
               0.00002480158730158730f)));
        cosine = base * (1.0f + base_squared *
            (-0.16666666666666666667f + base_squared *
             (0.00833333333333333333f + base_squared *
              (-0.00019841269841269841f + base_squared *
               0.00000275573192239859f))));
    } else {
        base = reduced;
        base_squared = base * base;
        sine = base * (1.0f + base_squared *
            (-0.16666666666666666667f + base_squared *
             (0.00833333333333333333f + base_squared *
              (-0.00019841269841269841f + base_squared *
               0.00000275573192239859f))));
        cosine = 1.0f + base_squared *
            (-0.5f + base_squared *
             (0.04166666666666666667f + base_squared *
              (-0.00138888888888888889f + base_squared *
               0.00002480158730158730f)));
    }
    if (sine_sign < 0)
        sine = -sine;
    if (cosine_sign < 0)
        cosine = -cosine;
    if (!sw_f32_finite(sine) || !sw_f32_finite(cosine))
        return 0;
    *sine_out = sine;
    *cosine_out = cosine;
    return 1;
}

static int sw_atanf(float value, float* result_out)
{
    const float half_pi = 1.57079632679489661923f;
    const float quarter_pi = 0.78539816339744830962f;
    const float tan_quarter_pi = 0.41421356237309504880f;
    float magnitude;
    float reduced;
    float squared;
    float result;
    int negative = 0;

    if (result_out == NULL || !sw_f32_finite(value))
        return 0;
    if (value == 0.0f) {
        *result_out = value;
        return 1;
    }
    if (value < 0.0f) {
        magnitude = -value;
        negative = 1;
    } else {
        magnitude = value;
    }
    if (magnitude > 1.0f) {
        if (!sw_atanf(1.0f / magnitude, &result))
            return 0;
        result = half_pi - result;
    } else {
        if (magnitude > tan_quarter_pi) {
            reduced = (magnitude - 1.0f) / (magnitude + 1.0f);
            if (!sw_atanf(reduced, &result))
                return 0;
            result += quarter_pi;
        } else {
            squared = magnitude * magnitude;
            result = magnitude * (1.0f + squared *
                (-0.33333333333333333333f + squared *
                 (0.2f + squared *
                  (-0.14285714285714285714f + squared *
                   (0.11111111111111111111f + squared *
                    (-0.09090909090909090909f + squared *
                     0.07692307692307692308f))))));
        }
    }
    if (negative)
        result = -result;
    if (!sw_f32_finite(result))
        return 0;
    *result_out = result;
    return 1;
}

static int sw_atan2f(float y, float x, float* result_out)
{
    const float pi = 3.14159265358979323846f;
    const float half_pi = 1.57079632679489661923f;
    float result;

    if (result_out == NULL || !sw_f32_finite(y) || !sw_f32_finite(x) ||
        (x == 0.0f && y == 0.0f)) {
        return 0;
    }
    if (x == 0.0f) {
        *result_out = y < 0.0f ? -half_pi : half_pi;
        return 1;
    }
    /* Divide the smaller magnitude by the larger one. This preserves a
     * defined atan2 result for finite values such as (FLT_MAX, FLT_MIN),
     * whose direct y/x quotient would overflow before sw_atanf sees it. */
    if (sw_absf(y) > sw_absf(x)) {
        if (!sw_atanf(x / y, &result))
            return 0;
        result = y < 0.0f ? -half_pi - result : half_pi - result;
    } else {
        if (!sw_atanf(y / x, &result))
            return 0;
        if (x < 0.0f)
            result += y < 0.0f ? -pi : pi;
    }
    if (!sw_f32_finite(result))
        return 0;
    *result_out = result;
    return 1;
}

static int sw_asinf(float value, float* result_out)
{
    float complement;
    float root;

    if (result_out == NULL || !sw_f32_finite(value) || value < -1.0f ||
        value > 1.0f) {
        return 0;
    }
    complement = 1.0f - value * value;
    if (!sw_f32_finite(complement) || complement < 0.0f ||
        !sw_sqrtf(complement, &root)) {
        return 0;
    }
    return sw_atan2f(value, root, result_out);
}

static int sw_acosf(float value, float* result_out)
{
    float complement;
    float root;

    if (result_out == NULL || !sw_f32_finite(value) || value < -1.0f ||
        value > 1.0f) {
        return 0;
    }
    complement = 1.0f - value * value;
    if (!sw_f32_finite(complement) || complement < 0.0f ||
        !sw_sqrtf(complement, &root)) {
        return 0;
    }
    return sw_atan2f(root, value, result_out);
}

/* The bounded exponential profile constructs a normal binary32 power-of-two
 * scale directly, then evaluates 2^fraction with a fixed polynomial. This
 * keeps the shader executor freestanding and prevents overflow from becoming
 * a host-libm-dependent result. */
static int sw_exp2f(float value, float* result_out)
{
    const float ln2 = 0.69314718055994530942f;
    float integer_part;
    float fraction;
    float fractional_scale;
    float binary_scale;
    uint32_t bits;
    int32_t exponent;

    if (result_out == NULL || !sw_f32_finite(value) || value < -126.0f ||
        value > 127.0f || !sw_floorf(value, &integer_part)) {
        return 0;
    }
    fraction = value - integer_part;
    if (!sw_f32_finite(fraction) || fraction < 0.0f || fraction >= 1.0f)
        return 0;
    exponent = (int32_t)integer_part + 127;
    if (exponent <= 0 || exponent >= 255)
        return 0;
    bits = (uint32_t)exponent << 23u;
    memcpy(&binary_scale, &bits, sizeof(binary_scale));
    fractional_scale = 1.0f + fraction *
        (ln2 + fraction *
         (0.24022650695910071233f + fraction *
          (0.05550410866482157995f + fraction *
           (0.00961812910762847716f + fraction *
            (0.00133335581464284434f + fraction *
             (0.00015403530393381609f))))));
    if (!sw_f32_finite(fractional_scale))
        return 0;
    *result_out = binary_scale * fractional_scale;
    return sw_f32_finite(*result_out);
}

static int sw_log2f(float value, float* result_out)
{
    const float inverse_ln2 = 1.44269504088896340736f;
    uint32_t bits;
    uint32_t exponent_bits;
    float normalized;
    float z;
    float z_squared;
    float series;
    float scaled;
    int32_t exponent;

    if (result_out == NULL || !sw_f32_finite(value) || value <= 0.0f)
        return 0;
    memcpy(&bits, &value, sizeof(bits));
    exponent_bits = (bits >> 23u) & 0xffu;
    if (exponent_bits == 0u) {
        scaled = value * 8388608.0f;
        if (!sw_f32_finite(scaled) || scaled == 0.0f)
            return 0;
        memcpy(&bits, &scaled, sizeof(bits));
        exponent_bits = (bits >> 23u) & 0xffu;
        if (exponent_bits == 0u || exponent_bits == 0xffu)
            return 0;
        exponent = (int32_t)exponent_bits - 127 - 23;
    } else {
        exponent = (int32_t)exponent_bits - 127;
    }
    bits = (bits & UINT32_C(0x007fffff)) | UINT32_C(0x3f800000);
    memcpy(&normalized, &bits, sizeof(normalized));
    z = (normalized - 1.0f) / (normalized + 1.0f);
    z_squared = z * z;
    series = z * (1.0f + z_squared *
        (0.33333333333333333333f + z_squared *
         (0.2f + z_squared *
          (0.14285714285714285714f + z_squared *
           (0.11111111111111111111f + z_squared *
            0.09090909090909090909f)))));
    *result_out = (float)exponent + 2.0f * inverse_ln2 * series;
    return sw_f32_finite(*result_out);
}

static int sw_powf(float base, float exponent, float* result_out)
{
    float base_log2;
    float scaled_exponent;

    if (result_out == NULL || !sw_f32_finite(base) ||
        !sw_f32_finite(exponent) || base <= 0.0f ||
        !sw_log2f(base, &base_log2)) {
        return 0;
    }
    scaled_exponent = exponent * base_log2;
    if (!sw_f32_finite(scaled_exponent))
        return 0;
    return sw_exp2f(scaled_exponent, result_out);
}

/* The backend keeps pass-local state in submission order.  Nothing here is
 * global: a caller can own more than one RinGPU software backend, and command
 * streams must not leak a viewport or attachment between them.  Coordinates
 * in the public raster-state ABI use a lower-left origin; image bytes remain
 * top-down, so conversion is performed only at the raster boundary. */
typedef struct SwRasterState {
    float viewport_x;
    float viewport_y;
    float viewport_width;
    float viewport_height;
    float min_depth;
    float max_depth;
    int32_t scissor_x;
    int32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    uint32_t scissor_enabled;
    uint32_t polygon_offset_fill_enabled;
    float polygon_offset_factor;
    float polygon_offset_units;
    float line_width;
    uint32_t sample_coverage_enabled;
    float sample_coverage_value;
    uint32_t sample_coverage_invert;
    uint32_t dither_enabled;
} SwRasterState;

typedef struct SwRenderPass {
    /* Each attachment owns a selected subresource view for the lifetime of
     * the pass.  Do not retain only the backing SwImage pointer here: a
     * texture may legitimately be attached more than once at different
     * mips/layers, and mutating that shared object's current selection would
     * redirect an earlier attachment to the later one. */
    SwImage color_views[RIN_GPU_MAX_COLOR_TARGETS];
    SwImage depth_view;
    SwImage stencil_view;
    SwImage* color;
    SwImage* color_targets[RIN_GPU_MAX_COLOR_TARGETS];
    SwImage* depth;
    SwImage* stencil;
    uint64_t color_cookie;
    uint64_t color_target_cookies[RIN_GPU_MAX_COLOR_TARGETS];
    uint64_t depth_cookie;
    uint64_t stencil_cookie;
    uint32_t active_color_mask;
    uint32_t color_mip_level;
    uint32_t color_array_layer;
    SwRasterState raster;
    const uint8_t* push_constants;
    uint32_t push_constant_size;
    struct RinGpuSoftwareBackend* backend;
} SwRenderPass;

typedef struct SwQueryState {
    uint64_t cookie;
    uint32_t query_type;
    uint32_t active;
    uint32_t available;
    uint32_t reserved;
    uint64_t values[RIN_GPU_QUERY_RESULT_VALUE_COUNT];
} SwQueryState;

struct RinGpuSoftwareBackend {
    uint64_t max_total_bytes;
    uint64_t allocated_bytes;
    RinGpuSoftwarePresentCallbackV1 present_callback;
    void* present_context;
    RinGpuSoftwareAcquireImageCallbackV1 acquire_image;
    void* image_context;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t deterministic_seed;
    RinGpuSoftwareBackendStatsV1 stats;
    uint64_t timestamp_ticks;
    SwQueryState queries[RIN_GPU_CORE_MAX_OBJECTS];
};

static uint32_t sw_image_bytes_per_pixel(uint32_t format);
static int sw_multiply_u64(uint64_t left, uint64_t right, uint64_t* value);
static int sw_add_u64(uint64_t left, uint64_t right, uint64_t* value);
static int sw_f32_finite(float value);
static int sw_image_select_subresource(SwImage* image, uint32_t mip_level,
                                       uint32_t array_layer);
static int sw_image_pixel_offset(const SwImage* image, uint32_t x,
                                 uint32_t y, uint32_t bytes_per_pixel,
                                 uint64_t* offset_out);
static int sw_depth_target_valid(const SwImage* image);
static int sw_stencil_target_valid(const SwImage* image);
static void sw_store_color_components(SwImage* image, uint32_t x,
                                      uint32_t y, const float color[4],
                                      uint32_t write_mask);
static int sw_load_color_components(const SwImage* image, uint32_t x,
                                    uint32_t y, float color[4]);
static int sw_store_depth(SwImage* image, uint32_t x, uint32_t y, float depth);
static int sw_store_stencil(SwImage* image, uint32_t x, uint32_t y,
                            uint8_t stencil);
static int sw_image_select_mip(SwImage* image, uint32_t mip_level);
static void sw_initialize_render_pass(SwRenderPass* destination,
                                      SwImage* color, SwImage* depth,
                                      SwImage* stencil);
static int sw_external_image_configure(
    SwImage* image, const RinGpuSoftwareExternalImageV1* storage);

static SwQueryState* sw_query_find(RinGpuSoftwareBackend* backend,
                                   uint64_t cookie, uint32_t create)
{
    SwQueryState* free_slot = NULL;

    if (!backend || cookie == 0u) return NULL;
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; ++index) {
        SwQueryState* query = &backend->queries[index];
        if (query->cookie == cookie) return query;
        if (free_slot == NULL && query->cookie == 0u) free_slot = query;
    }
    if (!create || free_slot == NULL) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->cookie = cookie;
    return free_slot;
}

static int sw_query_add(SwQueryState* query, uint64_t value)
{
    if (!query || UINT64_MAX - query->values[0] < value)
        return RIN_GPU_ERROR_LIMIT;
    query->values[0] += value;
    return RIN_GPU_OK;
}

static int sw_query_add_pipeline(RinGpuSoftwareBackend* backend,
                                 uint32_t value_index, uint64_t value)
{
    if (!backend || value_index >= RIN_GPU_QUERY_RESULT_VALUE_COUNT)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < RIN_GPU_CORE_MAX_OBJECTS; ++index) {
        SwQueryState* query = &backend->queries[index];
        if (query->cookie == 0u || !query->active ||
            query->query_type != RIN_GPU_QUERY_PIPELINE_STATISTICS)
            continue;
        if (UINT64_MAX - query->values[value_index] < value)
            return RIN_GPU_ERROR_LIMIT;
        query->values[value_index] += value;
    }
    return RIN_GPU_OK;
}

static int sw_query_record_draw(RinGpuSoftwareBackend* backend,
                                uint32_t vertex_count,
                                uint32_t instance_count)
{
    uint64_t invocations;
    int result;

    if (!sw_multiply_u64(vertex_count, instance_count, &invocations))
        return RIN_GPU_ERROR_LIMIT;
    result = sw_query_add_pipeline(
        backend, RIN_GPU_PIPELINE_STAT_INPUT_ASSEMBLY_VERTICES, invocations);
    if (result != RIN_GPU_OK) return result;
    result = sw_query_add_pipeline(
        backend, RIN_GPU_PIPELINE_STAT_VERTEX_SHADER_INVOCATIONS,
        invocations);
    if (result != RIN_GPU_OK) return result;
    return sw_query_add_pipeline(
        backend, RIN_GPU_PIPELINE_STAT_DRAW_CALLS, 1u);
}

static int sw_query_begin(RinGpuSoftwareBackend* backend,
                          const RinGpuBackendQueryV1* command)
{
    SwQueryState* query;

    if (!backend || !command || command->reserved != 0u ||
        (command->query_type != RIN_GPU_QUERY_TIMESTAMP &&
         command->query_type != RIN_GPU_QUERY_OCCLUSION &&
         command->query_type != RIN_GPU_QUERY_PIPELINE_STATISTICS)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    query = sw_query_find(backend, command->query_cookie, 1u);
    if (!query || query->active != 0u || query->available != 0u)
        return RIN_GPU_ERROR_STATE;
    memset(query->values, 0, sizeof(query->values));
    query->query_type = command->query_type;
    query->active = 1u;
    if (query->query_type == RIN_GPU_QUERY_TIMESTAMP)
        query->values[0] = backend->timestamp_ticks;
    return RIN_GPU_OK;
}

static int sw_query_end(RinGpuSoftwareBackend* backend,
                        const RinGpuBackendQueryV1* command)
{
    SwQueryState* query;

    if (!backend || !command || command->reserved != 0u ||
        (command->query_type != RIN_GPU_QUERY_TIMESTAMP &&
         command->query_type != RIN_GPU_QUERY_OCCLUSION &&
         command->query_type != RIN_GPU_QUERY_PIPELINE_STATISTICS))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    query = sw_query_find(backend, command->query_cookie, 0u);
    if (!query || query->query_type != command->query_type ||
        query->active == 0u)
        return RIN_GPU_ERROR_STATE;
    if (query->query_type == RIN_GPU_QUERY_TIMESTAMP)
        query->values[0] = backend->timestamp_ticks;
    query->active = 0u;
    query->available = 1u;
    return RIN_GPU_OK;
}

static int sw_query_reset(RinGpuSoftwareBackend* backend,
                          const RinGpuBackendQueryV1* command)
{
    SwQueryState* query;

    if (!backend || !command || command->reserved != 0u ||
        (command->query_type != RIN_GPU_QUERY_TIMESTAMP &&
         command->query_type != RIN_GPU_QUERY_OCCLUSION &&
         command->query_type != RIN_GPU_QUERY_PIPELINE_STATISTICS))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    query = sw_query_find(backend, command->query_cookie, 1u);
    if (!query || query->active != 0u)
        return RIN_GPU_ERROR_STATE;
    query->query_type = command->query_type;
    query->available = 0u;
    memset(query->values, 0, sizeof(query->values));
    return RIN_GPU_OK;
}

static int sw_query_result(void* opaque, uint64_t cookie, uint32_t query_type,
                           uint64_t values[RIN_GPU_QUERY_RESULT_VALUE_COUNT],
                           uint32_t* available)
{
    RinGpuSoftwareBackend* backend = (RinGpuSoftwareBackend*)opaque;
    SwQueryState* query;

    if (!backend || !values || !available) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(values, 0, sizeof(uint64_t) * RIN_GPU_QUERY_RESULT_VALUE_COUNT);
    *available = 0u;
    query = sw_query_find(backend, cookie, 0u);
    if (!query || query->query_type != query_type) return RIN_GPU_ERROR_BUSY;
    if (query->available == 0u) return RIN_GPU_OK;
    memcpy(values, query->values, sizeof(query->values));
    *available = 1u;
    return RIN_GPU_OK;
}

static int sw_timestamp_period(void* opaque, uint64_t* period_nanoseconds)
{
    if (!opaque || !period_nanoseconds) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *period_nanoseconds = 1u;
    return RIN_GPU_OK;
}

static void sw_query_destroy(void* opaque, uint64_t cookie)
{
    RinGpuSoftwareBackend* backend = (RinGpuSoftwareBackend*)opaque;
    SwQueryState* query = sw_query_find(backend, cookie, 0u);

    if (query != NULL) memset(query, 0, sizeof(*query));
}

static uint64_t sw_hash_bytes(uint64_t hash, const void* bytes, size_t size)
{
    const uint8_t* input = (const uint8_t*)bytes;

    if (!input && size != 0u)
        return 0u;
    for (size_t index = 0u; index < size; ++index) {
        hash ^= input[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t sw_hash_presented_image(
    const RinGpuSoftwareBackend* backend, const SwImage* image,
    uint64_t row_pitch_bytes)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t row_bytes;
    uint64_t last_row_offset;
    uint64_t span_bytes;
    uint64_t metadata[5];

    if (!backend || !image || !image->bytes ||
        image->desc.width == 0u || image->desc.height == 0u ||
        !sw_multiply_u64(image->desc.width,
                         sw_image_bytes_per_pixel(image->desc.format),
                         &row_bytes) || row_pitch_bytes < row_bytes ||
        !sw_multiply_u64((uint64_t)image->desc.height - 1u,
                         row_pitch_bytes, &last_row_offset) ||
        !sw_add_u64(last_row_offset, row_bytes, &span_bytes) ||
        span_bytes > image->size_bytes)
        return 0u;
    if ((backend->flags & RIN_GPU_SOFTWARE_BACKEND_FLAG_DETERMINISTIC) != 0u)
        hash ^= backend->deterministic_seed;
    metadata[0] = image->desc.format;
    metadata[1] = image->desc.width;
    metadata[2] = image->desc.height;
    metadata[3] = row_pitch_bytes;
    metadata[4] = image->desc.mip_levels;
    hash = sw_hash_bytes(hash, metadata, sizeof(metadata));
    for (uint32_t row = 0u; row < image->desc.height; ++row) {
        const uint8_t* source = image->bytes +
            (uint64_t)row * row_pitch_bytes;
        hash = sw_hash_bytes(hash, source, (size_t)row_bytes);
    }
    return hash;
}

static int sw_reserve(RinGpuSoftwareBackend* backend, uint64_t bytes)
{
    if (!backend || bytes == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (backend->allocated_bytes > backend->max_total_bytes ||
        bytes > backend->max_total_bytes - backend->allocated_bytes)
        return RIN_GPU_ERROR_NO_MEMORY;
    backend->allocated_bytes += bytes;
    return RIN_GPU_OK;
}

static void sw_release(RinGpuSoftwareBackend* backend, uint64_t bytes)
{
    if (!backend)
        return;
    if (bytes > backend->allocated_bytes)
        backend->allocated_bytes = 0u;
    else
        backend->allocated_bytes -= bytes;
}

/* Backend objects and their CPU shadows are part of the same bounded budget
 * as resource backing storage. A NULL backend is retained for direct unit
 * test adapters that intentionally exercise no budget. */
static void* sw_alloc(RinGpuSoftwareBackend* backend, uint64_t bytes,
                      int zeroed)
{
    void* allocation;

    if (bytes == 0u || bytes > SIZE_MAX ||
        (backend != NULL && sw_reserve(backend, bytes) != RIN_GPU_OK)) {
        return NULL;
    }
    allocation = zeroed ? calloc(1u, (size_t)bytes)
                        : malloc((size_t)bytes);
    if (allocation == NULL && backend != NULL)
        sw_release(backend, bytes);
    return allocation;
}

static void sw_free(RinGpuSoftwareBackend* backend, void* allocation,
                    uint64_t bytes)
{
    if (allocation != NULL)
        free(allocation);
    if (backend != NULL && bytes != 0u)
        sw_release(backend, bytes);
}

static int sw_create_buffer(void* opaque, const RinGpuBufferDescV1* desc,
                            uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwBuffer* buffer;

    if (!backend || !desc || !cookie || desc->size_bytes == 0u ||
        desc->size_bytes > SIZE_MAX)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    buffer = sw_alloc(backend, sizeof(*buffer), 1);
    if (!buffer) {
        return RIN_GPU_ERROR_NO_MEMORY;
    }
    buffer->bytes = sw_alloc(backend, desc->size_bytes, 1);
    if (!buffer->bytes) {
        sw_free(backend, buffer, sizeof(*buffer));
        return RIN_GPU_ERROR_NO_MEMORY;
    }
    buffer->size_bytes = desc->size_bytes;
    buffer->owns_bytes = 1u;
    *cookie = (uint64_t)(uintptr_t)buffer;
    return RIN_GPU_OK;
}

static void sw_destroy_buffer(void* opaque, uint64_t cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwBuffer* buffer = (SwBuffer*)(uintptr_t)cookie;
    if (!buffer)
        return;
    if (buffer->owns_bytes != 0u)
        sw_free(backend, buffer->bytes, buffer->size_bytes);
    sw_free(backend, buffer, sizeof(*buffer));
}

int ringpu_software_backend_create_memory(void* opaque, uint64_t size_bytes,
                                          uint8_t** bytes_out)
{
    RinGpuSoftwareBackend* backend = opaque;

    if (!backend || !bytes_out || size_bytes == 0u || size_bytes > SIZE_MAX)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *bytes_out = sw_alloc(backend, size_bytes, 1);
    return *bytes_out != NULL ? RIN_GPU_OK : RIN_GPU_ERROR_NO_MEMORY;
}

void ringpu_software_backend_destroy_memory(void* opaque, uint8_t* bytes,
                                            uint64_t size_bytes)
{
    sw_free((RinGpuSoftwareBackend*)opaque, bytes, size_bytes);
}

int ringpu_software_backend_bind_buffer(
    void* opaque, const RinGpuBufferDescV1* desc, uint8_t* memory,
    uint64_t memory_size, uint64_t offset_bytes, uint64_t* cookie_out)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwBuffer* buffer;

    if (!backend || !desc || !memory || !cookie_out ||
        desc->size_bytes == 0u || offset_bytes > memory_size ||
        desc->size_bytes > memory_size - offset_bytes ||
        offset_bytes > SIZE_MAX) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    buffer = sw_alloc(backend, sizeof(*buffer), 1);
    if (!buffer) return RIN_GPU_ERROR_NO_MEMORY;
    buffer->bytes = memory + (size_t)offset_bytes;
    buffer->size_bytes = desc->size_bytes;
    buffer->owns_bytes = 0u;
    *cookie_out = (uint64_t)(uintptr_t)buffer;
    return RIN_GPU_OK;
}

static int sw_upload_buffer(void* opaque, uint64_t cookie,
                            uint64_t destination_offset, const void* source,
                            uint64_t size_bytes)
{
    SwBuffer* buffer = (SwBuffer*)(uintptr_t)cookie;
    (void)opaque;
    if (!buffer || (!source && size_bytes != 0u) ||
        destination_offset > buffer->size_bytes ||
        size_bytes > buffer->size_bytes - destination_offset)
        return RIN_GPU_ERROR_BOUNDS;
    if (size_bytes != 0u)
        memcpy(buffer->bytes + destination_offset, source, (size_t)size_bytes);
    return RIN_GPU_OK;
}

static int sw_create_image(void* opaque, const RinGpuImageDescV1* desc,
                           uint64_t allocation_bytes, uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwImage* image;
    RinGpuSoftwareExternalImageV1 storage;
    uint32_t bytes_per_pixel;
    uint64_t row_pitch_bytes;
    int result;

    if (!backend || !desc || !cookie || allocation_bytes == 0u ||
        allocation_bytes > SIZE_MAX)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    image = sw_alloc(backend, sizeof(*image), 1);
    if (!image)
        return RIN_GPU_ERROR_NO_MEMORY;
    image->desc = *desc;
    image->allocation_desc = *desc;
    if (backend->acquire_image != NULL) {
        memset(&storage, 0, sizeof(storage));
        storage.struct_size = sizeof(storage);
        storage.version = RIN_GPU_SOFTWARE_EXTERNAL_IMAGE_VERSION;
        result = backend->acquire_image(backend->image_context, desc,
                                        allocation_bytes, &storage);
        if (result != RIN_GPU_OK) {
            sw_free(backend, image, sizeof(*image));
            return result;
        }
        result = sw_external_image_configure(image, &storage);
        if (result != RIN_GPU_OK) {
            sw_free(backend, image, sizeof(*image));
            return result;
        }
        if (image->external_storage != 0u) {
            result = sw_image_select_mip(image, 0u);
            if (result != RIN_GPU_OK) {
                sw_free(backend, image, sizeof(*image));
                return result;
            }
            *cookie = (uint64_t)(uintptr_t)image;
            return RIN_GPU_OK;
        }
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(desc->format);
    if (bytes_per_pixel == 0u ||
        !sw_multiply_u64(desc->width, bytes_per_pixel, &row_pitch_bytes)) {
        sw_free(backend, image, sizeof(*image));
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    image->allocation_bytes = sw_alloc(backend, allocation_bytes, 1);
    if (!image->allocation_bytes) {
        sw_free(backend, image, sizeof(*image));
        return RIN_GPU_ERROR_NO_MEMORY;
    }
    image->allocation_size_bytes = allocation_bytes;
    image->owns_allocation = 1u;
    image->bytes = image->allocation_bytes;
    image->size_bytes = allocation_bytes;
    image->row_pitch_bytes = row_pitch_bytes;
    result = sw_image_select_mip(image, 0u);
    if (result != RIN_GPU_OK) {
        sw_free(backend, image->allocation_bytes, allocation_bytes);
        sw_free(backend, image, sizeof(*image));
        return result;
    }
    *cookie = (uint64_t)(uintptr_t)image;
    return RIN_GPU_OK;
}

static void sw_destroy_image(void* opaque, uint64_t cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwImage* image = (SwImage*)(uintptr_t)cookie;
    if (!image)
        return;
    if (image->external_storage == 0u && image->owns_allocation != 0u) {
        sw_free(backend, image->allocation_bytes,
                image->allocation_size_bytes);
    }
    sw_free(backend, image, sizeof(*image));
}

int ringpu_software_backend_bind_image(
    void* opaque, const RinGpuImageDescV1* desc, uint64_t allocation_bytes,
    uint8_t* memory, uint64_t memory_size, uint64_t offset_bytes,
    uint64_t* cookie_out)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwImage* image;
    int result;

    if (!backend || !desc || !memory || !cookie_out || allocation_bytes == 0u ||
        offset_bytes > memory_size || allocation_bytes > memory_size - offset_bytes ||
        offset_bytes > SIZE_MAX) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    image = sw_alloc(backend, sizeof(*image), 1);
    if (!image) return RIN_GPU_ERROR_NO_MEMORY;
    image->desc = *desc;
    image->allocation_desc = *desc;
    image->allocation_bytes = memory + (size_t)offset_bytes;
    image->allocation_size_bytes = allocation_bytes;
    image->owns_allocation = 0u;
    result = sw_image_select_mip(image, 0u);
    if (result != RIN_GPU_OK) {
        sw_free(backend, image, sizeof(*image));
        return result;
    }
    *cookie_out = (uint64_t)(uintptr_t)image;
    return RIN_GPU_OK;
}

static int sw_readback_buffer(void* opaque, uint64_t cookie,
                              uint64_t source_offset, void* destination,
                              uint64_t size_bytes)
{
    SwBuffer* buffer = (SwBuffer*)(uintptr_t)cookie;
    (void)opaque;
    if (!buffer || !destination || size_bytes == 0u ||
        source_offset > buffer->size_bytes ||
        size_bytes > buffer->size_bytes - source_offset)
        return RIN_GPU_ERROR_BOUNDS;
    memcpy(destination, buffer->bytes + source_offset, (size_t)size_bytes);
    return RIN_GPU_OK;
}

static int sw_multiply_u64(uint64_t left, uint64_t right, uint64_t* value)
{
    if (!value || (left != 0u && right > UINT64_MAX / left))
        return 0;
    *value = left * right;
    return 1;
}

/* This is deliberately checked before the draw's first vertex fetch. The
 * product covers the indexed preflight as well as the two graphics phases;
 * each phase keeps its independent fragment budget below. */
static int sw_draw_work_is_bounded(uint32_t vertices_or_indices,
                                   uint32_t instance_count)
{
    uint64_t total;

    return sw_multiply_u64(vertices_or_indices, instance_count, &total) &&
        total <= SW_MAX_DRAW_VERTEX_INVOCATIONS;
}

static int sw_add_u64(uint64_t left, uint64_t right, uint64_t* value)
{
    if (!value || left > UINT64_MAX - right)
        return 0;
    *value = left + right;
    return 1;
}

/* The core has already validated the public descriptor. Keep the software
 * layout calculation separate and overflow-checked so a backend selected by
 * another embedding cannot turn a malformed allocation into an aliasing
 * subresource view. External storage is intentionally one mip/layer (the V3
 * contract), while private software images retain their complete tightly
 * packed mip/layer allocation. */
static int sw_image_select_subresource(SwImage* image, uint32_t mip_level,
                                       uint32_t array_layer)
{
    RinGpuImageDescV1 view;
    uint32_t bytes_per_pixel;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t level;
    uint64_t offset = 0u;
    uint64_t row_pitch;
    uint64_t layer_size;
    uint64_t layer_offset;

    if (!image)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    /* Unit tests and the direct executor helpers intentionally construct a
     * one-level SwImage on the stack. Preserve that internal test ABI while
     * production-created images always use allocation_desc. */
    if (image->allocation_desc.mip_levels == 0u) {
        if (mip_level != 0u || array_layer != 0u ||
            image->desc.mip_levels != 1u || image->desc.array_layers != 1u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        image->active_mip_level = 0u;
        image->active_array_layer = 0u;
        return RIN_GPU_OK;
    }
    if ((image->allocation_desc.dimension < RIN_GPU_IMAGE_DIMENSION_1D ||
         image->allocation_desc.dimension > RIN_GPU_IMAGE_DIMENSION_3D) ||
        image->allocation_desc.width == 0u || image->allocation_desc.height == 0u ||
        image->allocation_desc.depth == 0u ||
        image->allocation_desc.mip_levels == 0u ||
        mip_level >= image->allocation_desc.mip_levels ||
        array_layer >= image->allocation_desc.array_layers) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (image->allocation_desc.dimension == RIN_GPU_IMAGE_DIMENSION_1D &&
        image->allocation_desc.height != 1u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (image->external_storage != 0u &&
        (image->allocation_desc.mip_levels != 1u ||
         image->allocation_desc.array_layers != 1u)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (image->allocation_desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        image->depth_pixels != NULL && image->stencil_pixels != NULL) {
        view = image->allocation_desc;
        view.mip_levels = 1u;
        image->desc = view;
        image->bytes = NULL;
        image->size_bytes = 0u;
        image->row_pitch_bytes = 0u;
        image->active_mip_level = 0u;
        image->active_array_layer = 0u;
        return RIN_GPU_OK;
    }
    if (image->external_storage != 0u) {
        view = image->allocation_desc;
        view.mip_levels = 1u;
        image->desc = view;
        image->bytes = image->allocation_bytes;
        image->size_bytes = image->allocation_size_bytes;
        image->active_mip_level = 0u;
        image->active_array_layer = 0u;
        return RIN_GPU_OK;
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(image->allocation_desc.format);
    if (bytes_per_pixel == 0u || image->allocation_bytes == NULL ||
        image->allocation_size_bytes == 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    width = image->allocation_desc.width;
    height = image->allocation_desc.height;
    depth = image->allocation_desc.depth;
    for (level = 0u; level < mip_level; ++level) {
        uint64_t prior_row_pitch;
        uint64_t prior_height_size;
        uint64_t prior_depth_size;
        uint64_t prior_level_size;

        if (!sw_multiply_u64(width, bytes_per_pixel, &prior_row_pitch) ||
            !sw_multiply_u64(prior_row_pitch, height, &prior_height_size) ||
            !sw_multiply_u64(prior_height_size, depth, &prior_depth_size) ||
            !sw_multiply_u64(prior_depth_size,
                             image->allocation_desc.sample_count,
                             &prior_depth_size) ||
            !sw_multiply_u64(prior_depth_size,
                             image->allocation_desc.array_layers,
                             &prior_level_size) ||
            !sw_add_u64(offset, prior_level_size, &offset)) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        if (width > 1u)
            width >>= 1u;
        if (height > 1u)
            height >>= 1u;
        if (depth > 1u)
            depth >>= 1u;
    }
    if (!sw_multiply_u64(width, bytes_per_pixel, &row_pitch) ||
        !sw_multiply_u64(row_pitch, height, &layer_size) ||
        !sw_multiply_u64(layer_size, depth, &layer_size) ||
        !sw_multiply_u64(layer_size, image->allocation_desc.sample_count,
                         &layer_size) ||
        !sw_multiply_u64(array_layer, layer_size, &layer_offset) ||
        !sw_add_u64(offset, layer_offset, &offset) ||
        offset > image->allocation_size_bytes ||
        layer_size > image->allocation_size_bytes - offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }

    view = image->allocation_desc;
    view.width = width;
    view.height = height;
    view.depth = depth;
    view.array_layers = 1u;
    view.mip_levels = 1u;
    image->desc = view;
    image->bytes = image->allocation_bytes + offset;
    image->size_bytes = layer_size;
    image->row_pitch_bytes = row_pitch;
    image->active_mip_level = mip_level;
    image->active_array_layer = array_layer;
    return RIN_GPU_OK;
}

static int sw_image_select_mip(SwImage* image, uint32_t mip_level)
{
    return sw_image_select_subresource(image, mip_level, 0u);
}

static int sw_f32_finite(float value);
static float sw_f16_to_f32(uint16_t half);
static uint16_t sw_f32_to_f16(float value);
static int sw_srgb_decode(float encoded, float* linear_out);
static int sw_srgb_encode(float linear, float* encoded_out);

static uint32_t sw_image_bytes_per_pixel(uint32_t format)
{
    if (format == RIN_GPU_FORMAT_R8_UNORM)
        return 1u;
    if (format == RIN_GPU_FORMAT_RGB565_UNORM ||
        format == RIN_GPU_FORMAT_RGBA4_UNORM ||
        format == RIN_GPU_FORMAT_RGB5_A1_UNORM) {
        return 2u;
    }
    if (format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT)
        return 8u;
    if (format == RIN_GPU_FORMAT_RGBA16_FLOAT)
        return 8u;
    if (format == RIN_GPU_FORMAT_RGBA32_FLOAT)
        return 16u;
    if (format == RIN_GPU_FORMAT_RGBA8_UNORM ||
        format == RIN_GPU_FORMAT_BGRA8_UNORM ||
        format == RIN_GPU_FORMAT_RGBA8_SRGB ||
        format == RIN_GPU_FORMAT_BGRA8_SRGB ||
        format == RIN_GPU_FORMAT_BC1_RGBA_UNORM ||
        format == RIN_GPU_FORMAT_D32_FLOAT) {
        return 4u;
    }
    return 0u;
}

static int sw_external_plane_valid(const void* pixels, uint64_t size_bytes,
                                   uint64_t row_pitch_bytes,
                                   uint64_t row_bytes, uint32_t height)
{
    uint64_t trailing_rows;
    uint64_t minimum_bytes;

    if (!pixels || row_bytes == 0u || height == 0u ||
        row_pitch_bytes < row_bytes ||
        !sw_multiply_u64((uint64_t)height - 1u, row_pitch_bytes,
                         &trailing_rows) ||
        !sw_add_u64(trailing_rows, row_bytes, &minimum_bytes) ||
        size_bytes < minimum_bytes) {
        return 0;
    }
    return 1;
}

static int sw_external_image_configure(
    SwImage* image, const RinGpuSoftwareExternalImageV1* storage)
{
    uint32_t bytes_per_pixel;
    uint64_t color_row_bytes;
    uint64_t color_size_bytes;
    uint64_t depth_row_bytes;
    int any_storage;
    int planar_depth_stencil;

    if (!image || !storage) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    /* The acquire callback may return an entirely zeroed record to select the
     * ordinary private allocation.  This is an explicit V3/V4 choice, not an
     * invalid descriptor: the caller still owns no storage in this branch. */
    if (storage->struct_size == 0u && storage->version == 0u &&
        storage->pixels == NULL && storage->size_bytes == 0u &&
        storage->row_pitch_bytes == 0u && storage->depth_pixels == NULL &&
        storage->depth_size_bytes == 0u &&
        storage->depth_row_pitch_bytes == 0u &&
        storage->stencil_pixels == NULL &&
        storage->stencil_size_bytes == 0u &&
        storage->stencil_row_pitch_bytes == 0u)
        return RIN_GPU_OK;
    if (storage->struct_size != sizeof(*storage) ||
        storage->version != RIN_GPU_SOFTWARE_EXTERNAL_IMAGE_VERSION) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    any_storage = storage->pixels != NULL || storage->size_bytes != 0u ||
        storage->row_pitch_bytes != 0u || storage->depth_pixels != NULL ||
        storage->depth_size_bytes != 0u ||
        storage->depth_row_pitch_bytes != 0u || storage->stencil_pixels != NULL ||
        storage->stencil_size_bytes != 0u ||
        storage->stencil_row_pitch_bytes != 0u;
    if (!any_storage)
        return RIN_GPU_OK;
    if (image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        image->desc.width == 0u || image->desc.height == 0u ||
        image->desc.depth != 1u || image->desc.array_layers != 1u ||
        image->desc.mip_levels != 1u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u ||
        !sw_multiply_u64(image->desc.width, bytes_per_pixel,
                         &color_row_bytes) ||
        !sw_multiply_u64(color_row_bytes, image->desc.height,
                         &color_size_bytes) ||
        !sw_multiply_u64(color_size_bytes, image->desc.sample_count,
                         &color_size_bytes) ||
        !sw_multiply_u64(image->desc.width, sizeof(float),
                         &depth_row_bytes)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    planar_depth_stencil = image->desc.format ==
            RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        storage->depth_pixels != NULL && storage->stencil_pixels != NULL;
    if (planar_depth_stencil) {
        if (storage->pixels != NULL || storage->size_bytes != 0u ||
            storage->row_pitch_bytes != 0u ||
            !sw_external_plane_valid(storage->depth_pixels,
                                     storage->depth_size_bytes,
                                     storage->depth_row_pitch_bytes,
                                     depth_row_bytes,
                                     image->desc.height) ||
            !sw_external_plane_valid(storage->stencil_pixels,
                                     storage->stencil_size_bytes,
                                     storage->stencil_row_pitch_bytes,
                                     image->desc.width,
                                     image->desc.height)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        image->depth_pixels = storage->depth_pixels;
        image->depth_size_bytes = storage->depth_size_bytes;
        image->depth_row_pitch_bytes = storage->depth_row_pitch_bytes;
        image->stencil_pixels = storage->stencil_pixels;
        image->stencil_size_bytes = storage->stencil_size_bytes;
        image->stencil_row_pitch_bytes = storage->stencil_row_pitch_bytes;
        /* Planar D32/S8 storage is the only V3 form without a color byte
         * allocation. Its selected mip remains level zero and depth/stencil
         * accessors use the supplied planes directly. */
        image->allocation_bytes = (uint8_t*)storage->depth_pixels;
        image->allocation_size_bytes = storage->depth_size_bytes;
        image->external_storage = 1u;
        return RIN_GPU_OK;
    }
    if (storage->depth_pixels != NULL || storage->depth_size_bytes != 0u ||
        storage->depth_row_pitch_bytes != 0u || storage->stencil_pixels != NULL ||
        storage->stencil_size_bytes != 0u ||
        storage->stencil_row_pitch_bytes != 0u ||
        storage->row_pitch_bytes < color_row_bytes ||
        storage->size_bytes < color_size_bytes ||
        !sw_external_plane_valid(storage->pixels, storage->size_bytes,
                                 storage->row_pitch_bytes, color_row_bytes,
                                 image->desc.height)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    image->bytes = storage->pixels;
    image->size_bytes = storage->size_bytes;
    image->row_pitch_bytes = storage->row_pitch_bytes;
    image->allocation_bytes = storage->pixels;
    image->allocation_size_bytes = storage->size_bytes;
    image->external_storage = 1u;
    return RIN_GPU_OK;
}

static int sw_image_row_pitch(const SwImage* image, uint32_t bytes_per_pixel,
                              uint64_t* row_pitch_out)
{
    uint64_t tight_row_pitch;
    uint64_t row_pitch;

    if (!image || !row_pitch_out || bytes_per_pixel == 0u ||
        !sw_multiply_u64(image->desc.width, bytes_per_pixel,
                         &tight_row_pitch)) {
        return 0;
    }
    row_pitch = image->row_pitch_bytes != 0u ? image->row_pitch_bytes
                                              : tight_row_pitch;
    if (row_pitch < tight_row_pitch)
        return 0;
    *row_pitch_out = row_pitch;
    return 1;
}

static int sw_image_pixel_offset(const SwImage* image, uint32_t x, uint32_t y,
                                 uint32_t bytes_per_pixel,
                                 uint64_t* offset_out)
{
    uint64_t row_pitch;
    uint64_t row_offset;
    uint64_t x_offset;
    uint64_t offset;

    if (!image || !image->bytes || !offset_out || x >= image->desc.width ||
        y >= image->desc.height ||
        !sw_image_row_pitch(image, bytes_per_pixel, &row_pitch) ||
        !sw_multiply_u64(y, row_pitch, &row_offset) ||
        !sw_multiply_u64(x, bytes_per_pixel, &x_offset) ||
        !sw_add_u64(row_offset, x_offset, &offset) ||
        offset > image->size_bytes ||
        bytes_per_pixel > image->size_bytes - offset) {
        return 0;
    }
    *offset_out = offset;
    return 1;
}

static int sw_image_sample_view(const SwImage* image, uint32_t sample,
                                SwImage* view)
{
    uint32_t bytes_per_pixel;
    uint64_t offset;

    if (!image || !view || image->desc.sample_count <= 1u ||
        sample >= image->desc.sample_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u ||
        !sw_multiply_u64(image->desc.width, bytes_per_pixel, &offset) ||
        !sw_multiply_u64(offset, image->desc.height, &offset) ||
        !sw_multiply_u64(sample, offset, &offset) ||
        offset > image->size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    *view = *image;
    view->desc.sample_count = 1u;
    view->bytes = image->bytes + offset;
    view->size_bytes = image->size_bytes - offset;
    /* Keep the multisample row stride while the view exposes one sample. */
    view->row_pitch_bytes = image->row_pitch_bytes;
    return RIN_GPU_OK;
}

static int sw_depth_pixel_address(const SwImage* image, uint32_t x, uint32_t y,
                                  uint8_t** address_out)
{
    uint64_t offset;
    uint64_t row_bytes;
    uint64_t x_bytes;

    if (!image || !address_out || x >= image->desc.width ||
        y >= image->desc.height ||
        (image->desc.format != RIN_GPU_FORMAT_D32_FLOAT &&
         image->desc.format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT)) {
        return 0;
    }
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        image->depth_pixels != NULL) {
        if (!sw_multiply_u64(image->desc.width, sizeof(float), &row_bytes) ||
            !sw_multiply_u64(x, sizeof(float), &x_bytes) ||
            !sw_external_plane_valid(image->depth_pixels,
                                     image->depth_size_bytes,
                                     image->depth_row_pitch_bytes, row_bytes,
                                     image->desc.height) ||
            !sw_multiply_u64(y, image->depth_row_pitch_bytes, &offset) ||
            !sw_add_u64(offset, x_bytes, &offset) ||
            offset > image->depth_size_bytes ||
            sizeof(float) > image->depth_size_bytes - offset) {
            return 0;
        }
        *address_out = (uint8_t*)image->depth_pixels + offset;
        return 1;
    }
    if (!sw_image_pixel_offset(image, x, y,
                               image->desc.format == RIN_GPU_FORMAT_D32_FLOAT
                                   ? 4u : 8u,
                               &offset)) {
        return 0;
    }
    *address_out = image->bytes + offset;
    return 1;
}

static int sw_stencil_pixel_address(const SwImage* image, uint32_t x,
                                    uint32_t y, uint8_t** address_out)
{
    uint64_t offset;
    uint64_t row_bytes;

    if (!image || !address_out || x >= image->desc.width ||
        y >= image->desc.height ||
        (image->desc.format != RIN_GPU_FORMAT_S8_UINT &&
         image->desc.format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT)) {
        return 0;
    }
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        image->stencil_pixels != NULL) {
        row_bytes = image->desc.width;
        if (!sw_external_plane_valid(image->stencil_pixels,
                                     image->stencil_size_bytes,
                                     image->stencil_row_pitch_bytes, row_bytes,
                                     image->desc.height) ||
            !sw_multiply_u64(y, image->stencil_row_pitch_bytes, &offset) ||
            !sw_add_u64(offset, x, &offset) ||
            offset >= image->stencil_size_bytes) {
            return 0;
        }
        *address_out = image->stencil_pixels + offset;
        return 1;
    }
    if (!sw_image_pixel_offset(image, x, y,
                               image->desc.format == RIN_GPU_FORMAT_S8_UINT
                                   ? 1u : 8u,
                               &offset)) {
        return 0;
    }
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        !sw_add_u64(offset, 4u, &offset)) {
        return 0;
    }
    if (offset >= image->size_bytes)
        return 0;
    *address_out = image->bytes + offset;
    return 1;
}

static int sw_validate_sampled_image_2d(const SwImage* image)
{
    uint64_t row_bytes;
    uint64_t row_pitch_bytes;
    uint64_t required_bytes;
    uint64_t trailing_rows;
    uint32_t bytes_per_pixel;

    if (!image || !image->bytes ||
        image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        image->desc.width == 0u || image->desc.height == 0u ||
        image->desc.depth != 1u || image->desc.mip_levels != 1u ||
        image->desc.array_layers != 1u || image->desc.sample_count != 1u ||
        (image->desc.format != RIN_GPU_FORMAT_R8_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGB565_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA4_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGB5_A1_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA8_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_BGRA8_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA8_SRGB &&
         image->desc.format != RIN_GPU_FORMAT_BGRA8_SRGB &&
         image->desc.format != RIN_GPU_FORMAT_BC1_RGBA_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA16_FLOAT &&
         image->desc.format != RIN_GPU_FORMAT_RGBA32_FLOAT &&
         image->desc.format != RIN_GPU_FORMAT_D32_FLOAT &&
         image->desc.format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) ||
        image->desc.width > 4096u || image->desc.height > 4096u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u ||
        !sw_multiply_u64(image->desc.width, bytes_per_pixel, &row_bytes) ||
        !sw_image_row_pitch(image, bytes_per_pixel, &row_pitch_bytes) ||
        !sw_multiply_u64((uint64_t)image->desc.height - 1u,
                         row_pitch_bytes, &trailing_rows) ||
        !sw_add_u64(trailing_rows, row_bytes, &required_bytes) ||
        required_bytes > image->size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    return RIN_GPU_OK;
}

static void sw_bc1_decode_color(uint16_t packed, uint8_t color[4])
{
    color[0] = (uint8_t)((((packed >> 11u) & 0x1fu) * 255u + 15u) / 31u);
    color[1] = (uint8_t)((((packed >> 5u) & 0x3fu) * 255u + 31u) / 63u);
    color[2] = (uint8_t)(((packed & 0x1fu) * 255u + 15u) / 31u);
    color[3] = 255u;
}

/* BC1 upload is an explicit software profile: the input is block-compressed
 * DXT1 data, while the image backing is canonical RGBA8 so existing sample,
 * copy, blit, and readback code never guesses at block addressing. */
static int sw_upload_bc1_image(SwImage* image,
                               const RinGpuImageUploadV1* upload,
                               const void* source, uint64_t source_size)
{
    uint64_t block_width;
    uint64_t block_height;
    uint64_t row_bytes;
    uint64_t minimum_slice_pitch;
    uint64_t source_row_pitch;
    uint64_t source_slice_pitch;
    uint64_t required_size;

    if (!image || !upload || !source || source_size == 0u ||
        image->desc.format != RIN_GPU_FORMAT_BC1_RGBA_UNORM ||
        image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        image->desc.depth != 1u || upload->depth != 1u ||
        (upload->x & 3u) != 0u || (upload->y & 3u) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    block_width = ((uint64_t)upload->width + 3u) / 4u;
    block_height = ((uint64_t)upload->height + 3u) / 4u;
    if (block_width == 0u || block_height == 0u ||
        !sw_multiply_u64(block_width, 8u, &row_bytes)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    source_row_pitch = upload->source_row_pitch_bytes != 0u
        ? upload->source_row_pitch_bytes : row_bytes;
    if (!sw_multiply_u64(row_bytes, block_height, &minimum_slice_pitch))
        return RIN_GPU_ERROR_BOUNDS;
    source_slice_pitch = upload->source_slice_pitch_bytes != 0u
        ? upload->source_slice_pitch_bytes
        : minimum_slice_pitch;
    if (source_row_pitch < row_bytes ||
        source_slice_pitch < minimum_slice_pitch ||
        !sw_multiply_u64(block_height - 1u, source_row_pitch,
                         &required_size) ||
        !sw_add_u64(required_size, row_bytes, &required_size) ||
        source_size < required_size) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    for (uint64_t block_y = 0u; block_y < block_height; ++block_y) {
        for (uint64_t block_x = 0u; block_x < block_width; ++block_x) {
            const uint8_t* block = (const uint8_t*)source +
                block_y * source_row_pitch + block_x * 8u;
            uint16_t color0 = (uint16_t)block[0] |
                              ((uint16_t)block[1] << 8u);
            uint16_t color1 = (uint16_t)block[2] |
                              ((uint16_t)block[3] << 8u);
            uint32_t indices = (uint32_t)block[4] |
                               ((uint32_t)block[5] << 8u) |
                               ((uint32_t)block[6] << 16u) |
                               ((uint32_t)block[7] << 24u);
            uint8_t colors[4][4];

            sw_bc1_decode_color(color0, colors[0]);
            sw_bc1_decode_color(color1, colors[1]);
            if (color0 > color1) {
                for (uint32_t component = 0u; component < 4u; ++component) {
                    colors[2][component] = (uint8_t)(
                        (2u * colors[0][component] +
                         colors[1][component]) / 3u);
                    colors[3][component] = (uint8_t)(
                        (colors[0][component] +
                         2u * colors[1][component]) / 3u);
                }
            } else {
                for (uint32_t component = 0u; component < 4u; ++component)
                    colors[2][component] = (uint8_t)(
                        (colors[0][component] + colors[1][component]) / 2u);
                memset(colors[3], 0, sizeof(colors[3]));
            }
            for (uint32_t pixel_y = 0u; pixel_y < 4u; ++pixel_y) {
                for (uint32_t pixel_x = 0u; pixel_x < 4u; ++pixel_x) {
                    uint64_t x = upload->x + block_x * 4u + pixel_x;
                    uint64_t y = upload->y + block_y * 4u + pixel_y;
                    uint64_t offset;
                    uint32_t index;

                    if (x >= upload->x + upload->width ||
                        y >= upload->y + upload->height)
                        continue;
                    index = (indices >> (2u * (pixel_y * 4u + pixel_x))) &
                        3u;
                    if (!sw_image_pixel_offset(image, (uint32_t)x,
                                                (uint32_t)y, 4u, &offset))
                        return RIN_GPU_ERROR_BOUNDS;
                    memcpy(image->bytes + offset, colors[index], 4u);
                }
            }
        }
    }
    return RIN_GPU_OK;
}

static int sw_upload_image(void* opaque, uint64_t cookie,
                           const RinGpuImageUploadV1* upload,
                           const void* source, uint64_t source_size)
{
    SwImage* image = (SwImage*)(uintptr_t)cookie;
    uint64_t row_bytes;
    uint64_t target_row_pitch;
    uint64_t target_slice_pitch;
    uint64_t minimum_slice_pitch;
    uint64_t required_size;
    uint64_t source_row_pitch;
    uint64_t source_slice_pitch;
    uint64_t target_offset;
    uint64_t target_row_offset;
    uint64_t target_x_offset;
    uint64_t extra_slices;
    uint64_t extra_rows;
    uint32_t bytes_per_pixel;
    int result;
    (void)opaque;

    if (!image || !upload || !source || source_size == 0u ||
        upload->mip_level >= (image->allocation_desc.mip_levels != 0u
                                  ? image->allocation_desc.mip_levels
                                  : image->desc.mip_levels) ||
        upload->array_layer >= (image->allocation_desc.mip_levels != 0u
             ? image->allocation_desc.array_layers
             : image->desc.array_layers) ||
        (image->allocation_desc.mip_levels != 0u
             ? image->allocation_desc.sample_count
             : image->desc.sample_count) != 1u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_image_select_subresource(image, upload->mip_level,
                                         upload->array_layer);
    if (result != RIN_GPU_OK)
        return result;
    if (image->desc.array_layers != 1u || image->desc.sample_count != 1u ||
        upload->mip_level != image->active_mip_level ||
        upload->array_layer != image->active_array_layer ||
        upload->width == 0u ||
        upload->height == 0u || upload->depth == 0u ||
        upload->x > image->desc.width ||
        upload->width > image->desc.width - upload->x ||
        upload->y > image->desc.height ||
        upload->height > image->desc.height - upload->y ||
        upload->z > image->desc.depth ||
        upload->depth > image->desc.depth - upload->z ||
        (image->desc.dimension == RIN_GPU_IMAGE_DIMENSION_1D &&
         (upload->y != 0u || upload->z != 0u || upload->height != 1u ||
          upload->depth != 1u)) ||
        (image->desc.dimension == RIN_GPU_IMAGE_DIMENSION_2D &&
         (upload->z != 0u || upload->depth != 1u)) ||
        image->desc.dimension < RIN_GPU_IMAGE_DIMENSION_1D ||
        image->desc.dimension > RIN_GPU_IMAGE_DIMENSION_3D) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (image->desc.format == RIN_GPU_FORMAT_BC1_RGBA_UNORM)
        return sw_upload_bc1_image(image, upload, source, source_size);
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u ||
        !sw_multiply_u64(upload->width, bytes_per_pixel, &row_bytes) ||
        !sw_image_row_pitch(image, bytes_per_pixel, &target_row_pitch) ||
        !sw_multiply_u64(target_row_pitch, image->desc.height,
                         &target_slice_pitch)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    source_row_pitch = upload->source_row_pitch_bytes;
    if (source_row_pitch == 0u)
        source_row_pitch = row_bytes;
    if (source_row_pitch < row_bytes ||
        !sw_multiply_u64(source_row_pitch, upload->height,
                         &minimum_slice_pitch)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    source_slice_pitch = upload->source_slice_pitch_bytes;
    if (source_slice_pitch == 0u)
        source_slice_pitch = minimum_slice_pitch;
    if (source_slice_pitch < minimum_slice_pitch ||
        !sw_multiply_u64(upload->depth - 1u, source_slice_pitch,
                         &extra_slices) ||
        !sw_multiply_u64(upload->height - 1u, source_row_pitch,
                         &extra_rows) ||
        !sw_add_u64(extra_slices, extra_rows, &required_size) ||
        !sw_add_u64(required_size, row_bytes, &required_size) ||
        source_size < required_size ||
        !sw_multiply_u64(upload->z, target_slice_pitch, &target_offset) ||
        !sw_multiply_u64(upload->y, target_row_pitch, &target_row_offset) ||
        !sw_multiply_u64(upload->x, bytes_per_pixel, &target_x_offset) ||
        !sw_add_u64(target_offset, target_row_offset, &target_offset) ||
        !sw_add_u64(target_offset, target_x_offset, &target_offset)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (!sw_multiply_u64(upload->depth - 1u, target_slice_pitch,
                         &extra_slices) ||
        !sw_multiply_u64(upload->height - 1u, target_row_pitch,
                         &extra_rows) ||
        !sw_add_u64(extra_slices, extra_rows, &required_size) ||
        !sw_add_u64(required_size, row_bytes, &required_size) ||
        target_offset > image->size_bytes ||
        required_size > image->size_bytes - target_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    for (uint32_t slice = 0u; slice < upload->depth; ++slice) {
        for (uint32_t row = 0u; row < upload->height; ++row) {
            memcpy(image->bytes + target_offset +
                       (uint64_t)slice * target_slice_pitch +
                       (uint64_t)row * target_row_pitch,
                   (const uint8_t*)source +
                       (uint64_t)slice * source_slice_pitch +
                       (uint64_t)row * source_row_pitch,
                   (size_t)row_bytes);
        }
    }
    return RIN_GPU_OK;
}

static int sw_readback_image(void* opaque, uint64_t cookie,
                             const RinGpuImageReadbackV1* readback,
                             void* destination,
                             uint64_t destination_size)
{
    SwImage* image = (SwImage*)(uintptr_t)cookie;
    uint64_t row_bytes;
    uint64_t source_row_pitch;
    uint64_t source_slice_pitch;
    uint64_t source_offset;
    uint64_t destination_required;
    uint64_t destination_row_pitch;
    uint64_t destination_slice_pitch;
    uint64_t minimum_destination_slice_pitch;
    uint64_t extra_rows;
    uint64_t extra_slices;
    uint64_t source_extra_rows;
    uint64_t source_extra_slices;
    uint64_t source_required;
    uint64_t source_row_offset;
    uint64_t source_x_offset;
    uint32_t bytes_per_pixel;
    int result;
    (void)opaque;

    if (!image || !readback || !destination || destination_size == 0u ||
        readback->mip_level >= (image->allocation_desc.mip_levels != 0u
                                    ? image->allocation_desc.mip_levels
                                    : image->desc.mip_levels) ||
        readback->array_layer >= (image->allocation_desc.mip_levels != 0u
             ? image->allocation_desc.array_layers
             : image->desc.array_layers) ||
        (image->allocation_desc.mip_levels != 0u
             ? image->allocation_desc.sample_count
             : image->desc.sample_count) != 1u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_image_select_subresource(image, readback->mip_level,
                                         readback->array_layer);
    if (result != RIN_GPU_OK)
        return result;
    if (image->desc.array_layers != 1u || image->desc.sample_count != 1u ||
        readback->mip_level != image->active_mip_level ||
        readback->array_layer != image->active_array_layer ||
        readback->width == 0u ||
        readback->height == 0u || readback->depth == 0u ||
        readback->x > image->desc.width ||
        readback->width > image->desc.width - readback->x ||
        readback->y > image->desc.height ||
        readback->height > image->desc.height - readback->y ||
        readback->z > image->desc.depth ||
        readback->depth > image->desc.depth - readback->z ||
        (image->desc.dimension == RIN_GPU_IMAGE_DIMENSION_1D &&
         (readback->y != 0u || readback->z != 0u ||
          readback->height != 1u || readback->depth != 1u)) ||
        (image->desc.dimension == RIN_GPU_IMAGE_DIMENSION_2D &&
         (readback->z != 0u || readback->depth != 1u))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u ||
        !sw_multiply_u64(readback->width, bytes_per_pixel, &row_bytes)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination_row_pitch = readback->destination_row_pitch_bytes;
    if (destination_row_pitch == 0u)
        destination_row_pitch = row_bytes;
    if (destination_row_pitch < row_bytes ||
        !sw_multiply_u64(destination_row_pitch, readback->height,
                         &minimum_destination_slice_pitch)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination_slice_pitch = readback->destination_slice_pitch_bytes;
    if (destination_slice_pitch == 0u)
        destination_slice_pitch = minimum_destination_slice_pitch;
    if (destination_slice_pitch < minimum_destination_slice_pitch ||
        !sw_image_row_pitch(image, bytes_per_pixel, &source_row_pitch) ||
        !sw_multiply_u64(source_row_pitch, image->desc.height,
                         &source_slice_pitch) ||
        !sw_multiply_u64(readback->depth - 1u,
                         destination_slice_pitch,
                         &extra_slices) ||
        !sw_multiply_u64(readback->height - 1u,
                         destination_row_pitch,
                         &extra_rows) ||
        !sw_add_u64(extra_slices, extra_rows, &destination_required) ||
        !sw_add_u64(destination_required, row_bytes, &destination_required) ||
        destination_size < destination_required ||
        !sw_multiply_u64(readback->z, source_slice_pitch, &source_offset) ||
        !sw_multiply_u64(readback->y, source_row_pitch, &source_row_offset) ||
        !sw_multiply_u64(readback->x, bytes_per_pixel, &source_x_offset) ||
        !sw_add_u64(source_offset, source_row_offset, &source_offset) ||
        !sw_add_u64(source_offset, source_x_offset, &source_offset) ||
        !sw_multiply_u64(readback->depth - 1u, source_slice_pitch,
                         &source_extra_slices) ||
        !sw_multiply_u64(readback->height - 1u, source_row_pitch,
                         &source_extra_rows) ||
        !sw_add_u64(source_offset, source_extra_slices, &source_required) ||
        !sw_add_u64(source_required, source_extra_rows, &source_required) ||
        !sw_add_u64(source_required, row_bytes, &source_required) ||
        source_required > image->size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    for (uint32_t slice = 0u; slice < readback->depth; ++slice) {
        for (uint32_t row = 0u; row < readback->height; ++row) {
            uint64_t source_row;

            if (!sw_multiply_u64(slice, source_slice_pitch, &source_row) ||
                !sw_add_u64(source_offset, source_row, &source_row) ||
                !sw_multiply_u64(row, source_row_pitch, &source_row_offset) ||
                !sw_add_u64(source_row, source_row_offset, &source_row)) {
                return RIN_GPU_ERROR_BOUNDS;
            }
            memcpy((uint8_t*)destination +
                       (uint64_t)slice * destination_slice_pitch +
                       (uint64_t)row * destination_row_pitch,
                   image->bytes + source_row, (size_t)row_bytes);
        }
    }
    return RIN_GPU_OK;
}

/* The scalar SAMPLE_* RSH1 opcodes name a one-dimensional image. Keep this
 * validation separate from the WebGL-facing 2D path so a 1D image can never
 * be treated as a degenerate color attachment or gain a fabricated V axis. */
static int sw_validate_sampled_depth_image_1d(const SwImage* image)
{
    uint64_t row_bytes;
    uint64_t row_pitch_bytes;

    if (!image || !image->bytes ||
        image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_1D ||
        image->desc.width == 0u || image->desc.height != 1u ||
        image->desc.depth != 1u || image->desc.mip_levels != 1u ||
        image->desc.array_layers != 1u || image->desc.sample_count != 1u ||
        image->desc.format != RIN_GPU_FORMAT_D32_FLOAT ||
        image->desc.width > 4096u ||
        !sw_multiply_u64(image->desc.width, sizeof(float), &row_bytes) ||
        !sw_image_row_pitch(image, sizeof(float), &row_pitch_bytes) ||
        row_pitch_bytes < row_bytes || row_bytes > image->size_bytes) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    return RIN_GPU_OK;
}

/* Backend command streams are normally produced by RinGPU core, but the
 * software executor is also used directly by the RinGL embedding tests. Keep
 * the raw copy boundary independently bounded: a malformed command must not
 * turn a source/destination alias into memcpy() undefined behaviour. */
static int sw_copy_buffer(const RinGpuBackendBufferCopyV1* copy)
{
    SwBuffer* destination;
    SwBuffer* source;

    if (!copy)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    destination = (SwBuffer*)(uintptr_t)copy->destination_cookie;
    source = (SwBuffer*)(uintptr_t)copy->source_cookie;
    if (!destination || !source || !destination->bytes || !source->bytes)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (copy->size_bytes > SIZE_MAX || copy->destination_offset > SIZE_MAX ||
        copy->source_offset > SIZE_MAX ||
        copy->destination_offset > destination->size_bytes ||
        copy->size_bytes > destination->size_bytes - copy->destination_offset ||
        copy->source_offset > source->size_bytes ||
        copy->size_bytes > source->size_bytes - copy->source_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (copy->size_bytes == 0u)
        return RIN_GPU_OK;
    if (destination == source &&
        copy->destination_offset < copy->source_offset + copy->size_bytes &&
        copy->source_offset < copy->destination_offset + copy->size_bytes) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memcpy(destination->bytes + copy->destination_offset,
           source->bytes + copy->source_offset, (size_t)copy->size_bytes);
    return RIN_GPU_OK;
}

static int sw_clear_buffer(const RinGpuBackendBufferClearV1* clear)
{
    SwBuffer* destination;

    if (!clear || clear->reserved != 0u || clear->size_bytes == 0u ||
        (clear->offset & UINT64_C(3)) != 0u ||
        (clear->size_bytes & UINT64_C(3)) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination = (SwBuffer*)(uintptr_t)clear->destination_cookie;
    if (!destination || !destination->bytes)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (clear->offset > destination->size_bytes ||
        clear->size_bytes > destination->size_bytes - clear->offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    for (uint64_t offset = 0u; offset < clear->size_bytes; offset += 4u) {
        memcpy(destination->bytes + clear->offset + offset,
               &clear->pattern, sizeof(clear->pattern));
    }
    return RIN_GPU_OK;
}

static int sw_clear_color_valid(uint32_t format,
                                const RinGpuBackendImageClearV1* clear)
{
    if (!clear || !ringpu_color_format(format) ||
        !sw_f32_finite(clear->color_red) ||
        !sw_f32_finite(clear->color_green) ||
        !sw_f32_finite(clear->color_blue) ||
        !sw_f32_finite(clear->color_alpha)) {
        return 0;
    }
    if (format == RIN_GPU_FORMAT_RGBA16_FLOAT ||
        format == RIN_GPU_FORMAT_RGBA32_FLOAT) {
        return 1;
    }
    return clear->color_red >= 0.0f && clear->color_red <= 1.0f &&
           clear->color_green >= 0.0f && clear->color_green <= 1.0f &&
           clear->color_blue >= 0.0f && clear->color_blue <= 1.0f &&
           clear->color_alpha >= 0.0f && clear->color_alpha <= 1.0f;
}

static int sw_clear_image(const RinGpuBackendImageClearV1* clear)
{
    SwImage* destination;
    SwImage view;
    uint32_t known_aspects = RIN_GPU_IMAGE_CLEAR_KNOWN_ASPECTS;
    int color;
    int depth;
    int stencil;
    int result;

    if (!clear || clear->flags != 0u || clear->reserved != 0u ||
        clear->aspects == 0u || (clear->aspects & ~known_aspects) != 0u ||
        clear->stencil > UINT32_C(0xff)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination = (SwImage*)(uintptr_t)clear->destination_cookie;
    if (!destination)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    view = *destination;
    result = sw_image_select_subresource(&view, clear->mip_level,
                                         clear->array_layer);
    if (result != RIN_GPU_OK)
        return result;
    if (view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        view.desc.depth != 1u || view.desc.sample_count != 1u ||
        view.desc.width == 0u || view.desc.height == 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    color = (clear->aspects & RIN_GPU_IMAGE_CLEAR_COLOR) != 0u;
    depth = (clear->aspects & RIN_GPU_IMAGE_CLEAR_DEPTH) != 0u;
    stencil = (clear->aspects & RIN_GPU_IMAGE_CLEAR_STENCIL) != 0u;
    if (color && !sw_clear_color_valid(view.desc.format, clear))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (depth && (!ringpu_depth_aspect_format(view.desc.format) ||
                  !sw_f32_finite(clear->depth) || clear->depth < 0.0f ||
                  clear->depth > 1.0f)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (stencil && !ringpu_stencil_aspect_format(view.desc.format))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (color && sw_image_bytes_per_pixel(view.desc.format) == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (depth && sw_depth_target_valid(&view) != RIN_GPU_OK)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (stencil && sw_stencil_target_valid(&view) != RIN_GPU_OK)
        return RIN_GPU_ERROR_UNSUPPORTED;
    for (uint32_t y = 0u; y < view.desc.height; ++y) {
        for (uint32_t x = 0u; x < view.desc.width; ++x) {
            if (color) {
                uint64_t offset;

                if (!sw_image_pixel_offset(
                        &view, x, y,
                        sw_image_bytes_per_pixel(view.desc.format), &offset)) {
                    return RIN_GPU_ERROR_BOUNDS;
                }
                sw_store_color_components(
                    &view, x, y,
                    (const float[4]){clear->color_red, clear->color_green,
                                    clear->color_blue, clear->color_alpha},
                    RIN_GPU_COLOR_WRITE_ALL);
            }
            if (depth) {
                result = sw_store_depth(&view, x, y, clear->depth);
                if (result != RIN_GPU_OK) return result;
            }
            if (stencil) {
                result = sw_store_stencil(&view, x, y,
                                          (uint8_t)clear->stencil);
                if (result != RIN_GPU_OK) return result;
            }
        }
    }
    return RIN_GPU_OK;
}

static int sw_image_copy_region_span(const SwImage* image, uint32_t x,
                                     uint32_t y, uint32_t width,
                                     uint32_t height,
                                     uint32_t bytes_per_pixel,
                                     uint64_t* start_out,
                                     uint64_t* end_out)
{
    uint64_t row_pitch;
    uint64_t row_bytes;
    uint64_t row_offset;
    uint64_t x_offset;
    uint64_t trailing_rows;
    uint64_t start;
    uint64_t end;

    if (!image || !image->bytes || !start_out || !end_out || width == 0u ||
        height == 0u || x > image->desc.width ||
        width > image->desc.width - x || y > image->desc.height ||
        height > image->desc.height - y ||
        !sw_multiply_u64(width, bytes_per_pixel, &row_bytes) ||
        !sw_image_row_pitch(image, bytes_per_pixel, &row_pitch) ||
        !sw_multiply_u64(y, row_pitch, &row_offset) ||
        !sw_multiply_u64(x, bytes_per_pixel, &x_offset) ||
        !sw_add_u64(row_offset, x_offset, &start) ||
        !sw_multiply_u64(height - 1u, row_pitch, &trailing_rows) ||
        !sw_add_u64(start, trailing_rows, &end) ||
        !sw_add_u64(end, row_bytes, &end) || end > image->size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    *start_out = start;
    *end_out = end;
    return RIN_GPU_OK;
}

static int sw_image_copy_ranges_overlap(const SwImage* left,
                                        uint64_t left_start,
                                        uint64_t left_end,
                                        const SwImage* right,
                                        uint64_t right_start,
                                        uint64_t right_end,
                                        int* overlap_out)
{
    uintptr_t left_begin;
    uintptr_t left_finish;
    uintptr_t right_begin;
    uintptr_t right_finish;

    if (!left || !right || !left->bytes || !right->bytes || !overlap_out ||
        left_start > UINTPTR_MAX || left_end > UINTPTR_MAX ||
        right_start > UINTPTR_MAX || right_end > UINTPTR_MAX ||
        (uintptr_t)left->bytes > UINTPTR_MAX - (uintptr_t)left_start ||
        (uintptr_t)right->bytes > UINTPTR_MAX - (uintptr_t)right_start ||
        (uintptr_t)left->bytes > UINTPTR_MAX - (uintptr_t)left_end ||
        (uintptr_t)right->bytes > UINTPTR_MAX - (uintptr_t)right_end) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    left_begin = (uintptr_t)left->bytes + (uintptr_t)left_start;
    left_finish = (uintptr_t)left->bytes + (uintptr_t)left_end;
    right_begin = (uintptr_t)right->bytes + (uintptr_t)right_start;
    right_finish = (uintptr_t)right->bytes + (uintptr_t)right_end;
    *overlap_out = left_begin < right_finish && right_begin < left_finish;
    return RIN_GPU_OK;
}

static int sw_copy_image(const RinGpuBackendImageCopyV1* copy)
{
    SwImage* destination;
    SwImage* source;
    SwImage destination_view;
    SwImage source_view;
    const RinGpuImageCopyRegionV1* region;
    uint32_t bytes_per_pixel;
    uint64_t source_start;
    uint64_t source_end;
    uint64_t destination_start;
    uint64_t destination_end;
    uint64_t source_row_pitch;
    uint64_t destination_row_pitch;
    size_t copy_row_bytes;
    int same_subresource;
    int ranges_overlap;
    int result;

    if (!copy)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    destination = (SwImage*)(uintptr_t)copy->destination_cookie;
    source = (SwImage*)(uintptr_t)copy->source_cookie;
    region = &copy->region;
    if (!destination || !source ||
        region->abi_version != RIN_GPU_ABI_VERSION ||
        region->struct_size != sizeof(*region) || region->flags != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination_view = *destination;
    source_view = *source;
    result = sw_image_select_subresource(&destination_view,
                                         region->destination_mip_level,
                                         region->destination_array_layer);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_image_select_subresource(&source_view,
                                         region->source_mip_level,
                                         region->source_array_layer);
    if (result != RIN_GPU_OK)
        return result;
    if (destination_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        source_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        destination_view.desc.depth != 1u || source_view.desc.depth != 1u ||
        destination_view.desc.sample_count != 1u ||
        source_view.desc.sample_count != 1u ||
        region->destination_z != 0u ||
        region->source_z != 0u || region->depth != 1u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (destination_view.desc.format != source_view.desc.format)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bytes_per_pixel = sw_image_bytes_per_pixel(destination_view.desc.format);
    if (bytes_per_pixel == 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = sw_image_copy_region_span(
        &source_view, region->source_x, region->source_y, region->width,
        region->height, bytes_per_pixel, &source_start, &source_end);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_image_copy_region_span(
        &destination_view, region->destination_x, region->destination_y,
        region->width, region->height, bytes_per_pixel, &destination_start,
        &destination_end);
    if (result != RIN_GPU_OK)
        return result;
    if (region->width > SIZE_MAX / bytes_per_pixel ||
        source_start > SIZE_MAX || source_end > SIZE_MAX ||
        destination_start > SIZE_MAX || destination_end > SIZE_MAX) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    same_subresource = destination == source &&
        region->destination_mip_level == region->source_mip_level &&
        region->destination_array_layer == region->source_array_layer;
    if (same_subresource &&
        region->destination_x < region->source_x + region->width &&
        region->source_x < region->destination_x + region->width &&
        region->destination_y < region->source_y + region->height &&
        region->source_y < region->destination_y + region->height) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!same_subresource) {
        result = sw_image_copy_ranges_overlap(
            &destination_view, destination_start, destination_end,
            &source_view, source_start, source_end, &ranges_overlap);
        if (result != RIN_GPU_OK)
            return result;
        if (ranges_overlap)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!sw_image_row_pitch(&source_view, bytes_per_pixel, &source_row_pitch) ||
        !sw_image_row_pitch(&destination_view, bytes_per_pixel,
                            &destination_row_pitch)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (source_row_pitch > SIZE_MAX || destination_row_pitch > SIZE_MAX)
        return RIN_GPU_ERROR_BOUNDS;
    copy_row_bytes = (size_t)region->width * bytes_per_pixel;
    for (uint32_t row = 0u; row < region->height; ++row) {
        memcpy(destination_view.bytes + destination_start +
                   (uint64_t)row * destination_row_pitch,
               source_view.bytes + source_start +
                   (uint64_t)row * source_row_pitch,
               copy_row_bytes);
    }
    return RIN_GPU_OK;
}

static int sw_wait_for_completion(void* opaque, uint64_t timeout_ns)
{
    (void)timeout_ns;
    return opaque ? RIN_GPU_OK : RIN_GPU_ERROR_INVALID_ARGUMENT;
}

static int sw_compare_valid(uint32_t compare);

static int sw_sampler_valid(const RinGpuSamplerDescV1* desc)
{
    return desc != NULL &&
        (desc->min_filter == RIN_GPU_SAMPLER_FILTER_NEAREST ||
         desc->min_filter == RIN_GPU_SAMPLER_FILTER_LINEAR) &&
        (desc->mag_filter == RIN_GPU_SAMPLER_FILTER_NEAREST ||
         desc->mag_filter == RIN_GPU_SAMPLER_FILTER_LINEAR) &&
        (desc->mip_filter == RIN_GPU_SAMPLER_MIP_FILTER_NONE ||
         desc->mip_filter == RIN_GPU_SAMPLER_FILTER_NEAREST ||
         desc->mip_filter == RIN_GPU_SAMPLER_FILTER_LINEAR) &&
        desc->address_u >= RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE &&
        desc->address_u <= RIN_GPU_SAMPLER_ADDRESS_MIRRORED_REPEAT &&
        desc->address_v >= RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE &&
        desc->address_v <= RIN_GPU_SAMPLER_ADDRESS_MIRRORED_REPEAT &&
        desc->address_w == RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE &&
        sw_f32_finite(desc->mip_lod_bias) && sw_f32_finite(desc->min_lod) &&
        sw_f32_finite(desc->max_lod) && desc->min_lod <= desc->max_lod &&
        desc->max_anisotropy >= 1u &&
        desc->max_anisotropy <= RIN_GPU_MAX_SAMPLER_ANISOTROPY &&
        desc->flags == 0u &&
        (desc->compare_op == 0u || sw_compare_valid(desc->compare_op));
}

static int sw_create_sampler(void* opaque, const RinGpuSamplerDescV1* desc,
                             uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwSampler* sampler;
    if (!desc || !cookie)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie = 0u;
    /* The generic backend owns the nearest/linear, implicit-LOD mip, and
     * bounded anisotropic modes used by RinGL. A nonzero compare operation is
     * reserved for the typed scalar D32 comparison-sampling profile. */
    if (!sw_sampler_valid(desc)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    sampler = sw_alloc(backend, sizeof(*sampler), 1);
    if (!sampler) return RIN_GPU_ERROR_NO_MEMORY;
    sampler->desc = *desc;
    *cookie = (uint64_t)(uintptr_t)sampler;
    return RIN_GPU_OK;
}

static int sw_create_shader(void* opaque, const void* shader,
                            uint64_t shader_size,
                            const RinShaderInfoV1* shader_info,
                            uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwShader* object;

    if (!backend || !shader || !shader_info || !cookie || shader_size == 0u ||
        shader_size > SW_MAX_SHADER_BYTES)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    object = sw_alloc(backend, sizeof(*object), 1);
    if (!object) {
        return RIN_GPU_ERROR_NO_MEMORY;
    }
    object->ir = sw_alloc(backend, shader_size, 0);
    if (!object->ir) {
        sw_free(backend, object, sizeof(*object));
        return RIN_GPU_ERROR_NO_MEMORY;
    }
    memcpy(object->ir, shader, (size_t)shader_size);
    object->ir_size = shader_size;
    object->info = *shader_info;
    *cookie = (uint64_t)(uintptr_t)object;
    return RIN_GPU_OK;
}

static void sw_destroy_shader(void* opaque, uint64_t cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwShader* shader = (SwShader*)(uintptr_t)cookie;
    if (!shader)
        return;
    sw_free(backend, shader->ir, shader->ir_size);
    sw_free(backend, shader, sizeof(*shader));
}

static int sw_create_compute_pipeline(void* opaque, uint64_t shader_cookie,
                                      const RinShaderInfoV1* shader_info,
                                      uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwShader* shader = (SwShader*)(uintptr_t)shader_cookie;
    SwComputePipeline* pipeline;

    if (!shader || !shader_info || !cookie ||
        shader_info->stage != RIN_SHADER_STAGE_COMPUTE ||
        shader->info.stage != RIN_SHADER_STAGE_COMPUTE ||
        shader_info->resource_count != shader->info.resource_count ||
        shader_info->register_count != shader->info.register_count ||
        shader_info->instruction_count != shader->info.instruction_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *cookie = 0u;
    pipeline = sw_alloc(backend, sizeof(*pipeline), 1);
    if (!pipeline)
        return RIN_GPU_ERROR_NO_MEMORY;
    pipeline->shader = shader;
    *cookie = (uint64_t)(uintptr_t)pipeline;
    return RIN_GPU_OK;
}

static int sw_create_graphics_pipeline(
    void* opaque, uint64_t vertex_shader_cookie,
    const RinShaderInfoV1* vertex_shader_info, uint64_t fragment_shader_cookie,
    const RinShaderInfoV1* fragment_shader_info,
    const RinGpuBackendGraphicsPipelineDescV1* desc, uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    SwPipeline* pipeline;
    if (!vertex_shader_cookie || !fragment_shader_cookie ||
        !vertex_shader_info || !fragment_shader_info || !desc || !cookie)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if ((desc->primitive_topology != RIN_GPU_PRIMITIVE_POINT_LIST &&
         desc->primitive_topology != RIN_GPU_PRIMITIVE_LINE_LIST &&
         desc->primitive_topology != RIN_GPU_PRIMITIVE_LINE_STRIP &&
         desc->primitive_topology != RIN_GPU_PRIMITIVE_LINE_LOOP &&
         desc->primitive_topology != RIN_GPU_PRIMITIVE_TRIANGLE_LIST &&
         desc->primitive_topology != RIN_GPU_PRIMITIVE_TRIANGLE_STRIP &&
         desc->primitive_topology != RIN_GPU_PRIMITIVE_TRIANGLE_FAN) ||
        desc->vertex_input_count > RIN_GPU_MAX_VERTEX_ATTRIBUTES ||
        desc->resource_count > RIN_SHADER_MAX_RESOURCES)
        return RIN_GPU_ERROR_UNSUPPORTED;
    pipeline = sw_alloc(backend, sizeof(*pipeline), 1);
    if (!pipeline)
        return RIN_GPU_ERROR_NO_MEMORY;
    pipeline->vertex = (SwShader*)(uintptr_t)vertex_shader_cookie;
    pipeline->fragment = (SwShader*)(uintptr_t)fragment_shader_cookie;
    pipeline->desc = *desc;
    *cookie = (uint64_t)(uintptr_t)pipeline;
    return RIN_GPU_OK;
}

static void sw_destroy_compute_pipeline(void* opaque, uint64_t cookie)
{
    sw_free((RinGpuSoftwareBackend*)opaque,
            (void*)(uintptr_t)cookie, sizeof(SwComputePipeline));
}

static void sw_destroy_graphics_pipeline(void* opaque, uint64_t cookie)
{
    sw_free((RinGpuSoftwareBackend*)opaque,
            (void*)(uintptr_t)cookie, sizeof(SwPipeline));
}

/* Kept for source-including legacy unit adapters. Production callbacks use
 * the typed destroy functions above so the metadata size is unambiguous. */
static void __attribute__((unused)) sw_destroy_pipeline(void* opaque,
                                                        uint64_t cookie)
{
    if (opaque != NULL)
        return;
    free((void*)(uintptr_t)cookie);
}

static int sw_shader_storage_access(const SwShader* shader,
                                    uint32_t resource, uint32_t* access_out)
{
    const RinShaderHeaderV1* header;
    const RinShaderInstructionV1* instructions;
    uint32_t access = 0u;

    if (!shader || !shader->ir || !access_out ||
        shader->ir_size < sizeof(*header)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    header = (const RinShaderHeaderV1*)shader->ir;
    if (header->stage != RIN_SHADER_STAGE_COMPUTE ||
        resource >= header->resource_count ||
        header->header_size < sizeof(*header) ||
        header->instruction_count >
            (shader->ir_size - header->header_size) / sizeof(*instructions)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    instructions = (const RinShaderInstructionV1*)(shader->ir +
                                                    header->header_size);
    for (uint32_t index = 0u; index < header->instruction_count; ++index) {
        const RinShaderInstructionV1* instruction = &instructions[index];

        if (instruction->resource != resource)
            continue;
        if (instruction->opcode == RIN_SHADER_OP_LOAD_RESOURCE_I32 ||
            instruction->opcode == RIN_SHADER_OP_LOAD_RESOURCE_F32) {
            access |= RIN_GPU_RESOURCE_READ;
        } else if (instruction->opcode == RIN_SHADER_OP_STORE_RESOURCE_I32 ||
                   instruction->opcode == RIN_SHADER_OP_STORE_RESOURCE_F32) {
            access |= RIN_GPU_RESOURCE_WRITE;
        } else if (instruction->opcode == RIN_SHADER_OP_ATOMIC_ADD_I32 ||
                   instruction->opcode == RIN_SHADER_OP_ATOMIC_EXCHANGE_I32 ||
                   instruction->opcode == RIN_SHADER_OP_ATOMIC_MIN_I32 ||
                   instruction->opcode == RIN_SHADER_OP_ATOMIC_MAX_I32) {
            access |= RIN_GPU_RESOURCE_READ | RIN_GPU_RESOURCE_WRITE;
        }
    }
    if (access == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *access_out = access;
    return RIN_GPU_OK;
}

static int sw_create_compute_bind_group(
    void* opaque, uint64_t pipeline_cookie,
    const RinGpuBackendBufferBindingV1* bindings, uint32_t binding_count,
    uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    const SwComputePipeline* pipeline =
        (const SwComputePipeline*)(uintptr_t)pipeline_cookie;
    SwComputeBindGroup* group;

    if (!cookie || !pipeline || !pipeline->shader ||
        binding_count != pipeline->shader->info.resource_count ||
        binding_count > RIN_SHADER_MAX_RESOURCES ||
        (binding_count != 0u && !bindings)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *cookie = 0u;
    group = sw_alloc(backend, sizeof(*group), 1);
    if (!group)
        return RIN_GPU_ERROR_NO_MEMORY;
    group->pipeline = pipeline;
    group->binding_count = binding_count;
    for (uint32_t binding = 0u; binding < binding_count; ++binding) {
        const RinGpuBackendBufferBindingV1* source = &bindings[binding];
        SwBuffer* buffer = (SwBuffer*)(uintptr_t)source->buffer_cookie;
        uint32_t expected_access;
        int result;

        result = sw_shader_storage_access(pipeline->shader, binding,
                                          &expected_access);
        if (result != RIN_GPU_OK || !buffer || source->reserved != 0u ||
            source->access != expected_access ||
            (source->offset & 3u) != 0u || (source->size_bytes & 3u) != 0u ||
            source->offset > buffer->size_bytes ||
            source->size_bytes > buffer->size_bytes - source->offset) {
            sw_free(backend, group, sizeof(*group));
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        group->buffers[binding] = buffer;
        group->offsets[binding] = source->offset;
        group->sizes[binding] = source->size_bytes;
        group->access[binding] = source->access;
    }
    *cookie = (uint64_t)(uintptr_t)group;
    return RIN_GPU_OK;
}

static void sw_destroy_compute_bind_group(void* opaque, uint64_t cookie)
{
    sw_free((RinGpuSoftwareBackend*)opaque, (void*)(uintptr_t)cookie,
            sizeof(SwComputeBindGroup));
}

static int sw_create_graphics_bind_group(
    void* opaque, uint64_t pipeline_cookie,
    const RinGpuBackendGraphicsBindingV1* bindings, uint32_t binding_count,
    uint64_t* cookie)
{
    RinGpuSoftwareBackend* backend = opaque;
    const SwPipeline* pipeline = (const SwPipeline*)(uintptr_t)pipeline_cookie;
    SwGraphicsBindGroup* group;
    uint64_t seen = 0u;

    if (!cookie) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *cookie = 0u;
    if (!pipeline || binding_count == 0u ||
        binding_count != pipeline->desc.resource_count ||
        binding_count > RIN_SHADER_MAX_RESOURCES || !bindings) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    group = sw_alloc(backend, sizeof(*group), 1);
    if (!group) return RIN_GPU_ERROR_NO_MEMORY;
    group->pipeline = pipeline;
    group->binding_count = binding_count;
    for (uint32_t index = 0u; index < binding_count; ++index) {
        const RinGpuBackendGraphicsBindingV1* binding = &bindings[index];

        if (binding->binding >= binding_count ||
            (seen & (UINT64_C(1) << binding->binding)) != 0u ||
            binding->kind != pipeline->desc.resource_kinds[binding->binding] ||
            binding->resource_cookie == 0u) {
            sw_free(backend, group, sizeof(*group));
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        if (binding->kind == RIN_SHADER_RESOURCE_STORAGE_IMAGE) {
            const SwImage* image =
                (const SwImage*)(uintptr_t)binding->resource_cookie;
            SwImage image_view;
            int result;
            if (!image || binding->access == 0u ||
                (binding->access & ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                binding->offset != 0u || binding->size_bytes != 0u ||
                binding->flags != 0u) {
                sw_free(backend, group, sizeof(*group));
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            image_view = *image;
            result = sw_image_select_subresource(&image_view,
                                                 binding->mip_level,
                                                 binding->array_layer);
            if (result != RIN_GPU_OK ||
                image_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                image_view.desc.format != RIN_GPU_FORMAT_R8_UNORM ||
                image_view.desc.sample_count != 1u ||
                (image_view.desc.usage & RIN_GPU_IMAGE_STORAGE) == 0u) {
                sw_free(backend, group, sizeof(*group));
                return result != RIN_GPU_OK ? result : RIN_GPU_ERROR_UNSUPPORTED;
            }
            group->images[binding->binding] = image;
            group->image_base_mip_levels[binding->binding] = binding->mip_level;
            group->image_mip_counts[binding->binding] = 1u;
            group->image_array_layers[binding->binding] = binding->array_layer;
        } else if (binding->kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
            binding->kind == RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE) {
            const SwImage* image =
                (const SwImage*)(uintptr_t)binding->resource_cookie;
            SwImage image_view;
            int result;
            uint32_t total_mips;
            uint32_t total_layers;

            if (binding->access != RIN_GPU_RESOURCE_READ ||
                binding->offset != 0u || binding->size_bytes != 0u ||
                (binding->flags &
                 ~RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN) != 0u) {
                sw_free(backend, group, sizeof(*group));
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            if (binding->kind == RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE &&
                binding->flags != 0u) {
                sw_free(backend, group, sizeof(*group));
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            total_mips = image->allocation_desc.mip_levels != 0u
                ? image->allocation_desc.mip_levels : image->desc.mip_levels;
            total_layers = image->allocation_desc.mip_levels != 0u
                ? image->allocation_desc.array_layers : image->desc.array_layers;
            if (total_mips == 0u || total_layers == 0u ||
                binding->mip_level >= total_mips ||
                binding->array_layer >= total_layers ||
                (binding->flags != 0u &&
                 binding->flags != RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN)) {
                sw_free(backend, group, sizeof(*group));
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            image_view = *image;
            result = sw_image_select_subresource(&image_view,
                                                 binding->mip_level,
                                                 binding->array_layer);
            if (result != RIN_GPU_OK) {
                sw_free(backend, group, sizeof(*group));
                return result;
            }
            result = binding->kind == RIN_SHADER_RESOURCE_SAMPLED_IMAGE
                ? sw_validate_sampled_image_2d(&image_view)
                : sw_validate_sampled_depth_image_1d(&image_view);
            if (result != RIN_GPU_OK) {
                sw_free(backend, group, sizeof(*group));
                return result;
            }
            group->images[binding->binding] = image;
            group->image_base_mip_levels[binding->binding] = binding->mip_level;
            group->image_mip_counts[binding->binding] =
                (binding->flags & RIN_GPU_GRAPHICS_BINDING_SAMPLED_MIP_CHAIN)
                != 0u ? total_mips - binding->mip_level : 1u;
            group->image_array_layers[binding->binding] = binding->array_layer;
        } else if (binding->kind == RIN_SHADER_RESOURCE_SAMPLER ||
                   binding->kind == RIN_SHADER_RESOURCE_COMPARISON_SAMPLER) {
            const SwSampler* sampler =
                (const SwSampler*)(uintptr_t)binding->resource_cookie;

            if (binding->access != 0u || binding->offset != 0u ||
                binding->size_bytes != 0u || binding->mip_level != 0u ||
                binding->array_layer != 0u || binding->flags != 0u ||
                !sw_sampler_valid(
                    sampler ? &sampler->desc : NULL) ||
                (binding->kind == RIN_SHADER_RESOURCE_SAMPLER
                    ? sampler->desc.compare_op != 0u
                    : sampler->desc.compare_op == 0u ||
                      sampler->desc.mip_filter !=
                          RIN_GPU_SAMPLER_MIP_FILTER_NONE ||
                      sampler->desc.min_filter != sampler->desc.mag_filter)) {
                sw_free(backend, group, sizeof(*group));
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            group->samplers[binding->binding] = sampler;
        } else {
            sw_free(backend, group, sizeof(*group));
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
        group->kinds[binding->binding] = binding->kind;
        seen |= UINT64_C(1) << binding->binding;
    }
    if (seen != (binding_count == 64u
                     ? UINT64_MAX
                     : (UINT64_C(1) << binding_count) - 1u)) {
        sw_free(backend, group, sizeof(*group));
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    *cookie = (uint64_t)(uintptr_t)group;
    return RIN_GPU_OK;
}

static void sw_destroy_graphics_bind_group(void* opaque, uint64_t cookie)
{
    sw_free((RinGpuSoftwareBackend*)opaque, (void*)(uintptr_t)cookie,
            sizeof(SwGraphicsBindGroup));
}

enum SwShaderValueType {
    SW_SHADER_VALUE_NONE = 0,
    SW_SHADER_VALUE_I32,
    SW_SHADER_VALUE_F32,
};

/* The rasterizer owns these values. Keeping the window position and its
 * lower-left derivatives together prevents a fragment builtin from being
 * accidentally sourced from a user varying or from the target's top-left
 * memory coordinates. */
typedef struct SwFragmentCoordinate {
    uint32_t valid;
    float value[4];
    float dx[4];
    float dy[4];
} SwFragmentCoordinate;

typedef struct SwShaderIo {
    const float* f32_inputs;
    /* Fragment draws provide one finite screen-space delta per Float input.
     * Vertex work deliberately leaves these NULL: derivative opcodes are
     * fragment-only and reject an executor that cannot supply a gradient. */
    const float* f32_input_dx;
    const float* f32_input_dy;
    const int32_t* i32_inputs;
    const uint8_t* input_types;
    uint32_t input_count;
    float* f32_outputs;
    int32_t* i32_outputs;
    uint8_t* output_types;
    uint32_t output_count;
    /* Set only by a fragment RSH1 DISCARD. Callers must skip both output
     * validation and depth/stencil/color publication when it is true. */
    int* discarded;
    const SwGraphicsBindGroup* graphics_bind_group;
    SwComputeExecution* compute_execution;
    const uint8_t* push_constants;
    uint32_t push_constant_size;
    /* Point-sprite coordinates are generated by the rasterizer. They must
     * never be smuggled through a user varying, because clipping and partial
     * point coverage would then give them the wrong origin. */
    uint32_t point_coord_valid;
    float point_coord_x;
    float point_coord_y;
    float point_coord_dx;
    float point_coord_dy;
    const SwFragmentCoordinate* fragment_coordinate;
} SwShaderIo;

static int sw_make_fragment_coordinate(const SwRenderPass* pass, int32_t x,
                                       int32_t y, float depth,
                                       float inverse_w, const float dx[4],
                                       const float dy[4],
                                       SwFragmentCoordinate* out)
{
    if (!pass || !pass->color || !dx || !dy || !out || x < 0 || y < 0 ||
        (uint32_t)x >= pass->color->desc.width ||
        (uint32_t)y >= pass->color->desc.height || !sw_f32_finite(depth) ||
        !sw_f32_finite(inverse_w) || inverse_w == 0.0f || depth < 0.0f ||
        depth > 1.0f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    out->valid = 1u;
    out->value[0] = (float)x + 0.5f;
    out->value[1] = (float)pass->color->desc.height - (float)y - 0.5f;
    out->value[2] = depth;
    out->value[3] = inverse_w;
    for (uint32_t component = 0u; component < 4u; ++component) {
        if (!sw_f32_finite(out->value[component]) ||
            !sw_f32_finite(dx[component]) || !sw_f32_finite(dy[component])) {
            return RIN_GPU_ERROR_BACKEND;
        }
        out->dx[component] = dx[component];
        out->dy[component] = dy[component];
    }
    return RIN_GPU_OK;
}

static int32_t sw_i32_from_bits(uint32_t bits)
{
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void sw_destroy_sampler(void* opaque, uint64_t cookie)
{
    sw_free((RinGpuSoftwareBackend*)opaque, (void*)(uintptr_t)cookie,
            sizeof(SwSampler));
}

static int32_t sw_i32_add(int32_t left, int32_t right)
{
    return sw_i32_from_bits((uint32_t)left + (uint32_t)right);
}

static int32_t sw_i32_subtract(int32_t left, int32_t right)
{
    return sw_i32_from_bits((uint32_t)left - (uint32_t)right);
}

static int32_t sw_i32_multiply(int32_t left, int32_t right)
{
    return sw_i32_from_bits((uint32_t)left * (uint32_t)right);
}

static int32_t sw_i32_shift_right(int32_t value, uint32_t amount)
{
    uint32_t bits = (uint32_t)value;
    amount &= 31u;
    if (amount == 0u)
        return value;
    bits >>= amount;
    if (value < 0)
        bits |= ~(UINT32_MAX >> amount);
    return sw_i32_from_bits(bits);
}

static int sw_normalize_sample_coordinate(float coordinate, uint32_t address,
                                          float* normalized_out)
{
    float magnitude;
    float fraction;
    uint32_t integral;
    uint32_t floor_is_odd;

    if (!normalized_out || !sw_f32_finite(coordinate))
        return RIN_GPU_ERROR_BACKEND;
    if (address == RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE) {
        if (coordinate < 0.0f) coordinate = 0.0f;
        if (coordinate > 1.0f) coordinate = 1.0f;
        *normalized_out = coordinate;
        return RIN_GPU_OK;
    }
    /* Conversion to uint32_t is used only to determine a texel-period parity.
     * Keep it defined for adversarial finite shader values instead of relying
     * on an implementation-defined large float-to-integer conversion. */
    magnitude = coordinate < 0.0f ? -coordinate : coordinate;
    if (magnitude > 2147483520.0f)
        return RIN_GPU_ERROR_LIMIT;
    integral = (uint32_t)magnitude;
    fraction = magnitude - (float)integral;
    if (coordinate >= 0.0f) {
        floor_is_odd = integral & 1u;
    } else if (fraction == 0.0f) {
        floor_is_odd = integral & 1u;
        fraction = 0.0f;
    } else {
        floor_is_odd = (integral + 1u) & 1u;
        fraction = 1.0f - fraction;
    }
    if (address == RIN_GPU_SAMPLER_ADDRESS_REPEAT) {
        *normalized_out = fraction;
        return RIN_GPU_OK;
    }
    if (address == RIN_GPU_SAMPLER_ADDRESS_MIRRORED_REPEAT) {
        *normalized_out = floor_is_odd != 0u ? 1.0f - fraction : fraction;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_INVALID_ARGUMENT;
}

static int sw_address_sample_texel(int32_t coordinate, uint32_t dimension,
                                   uint32_t address, uint32_t* resolved_out)
{
    if (!resolved_out || dimension == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (coordinate >= 0 && (uint32_t)coordinate < dimension) {
        *resolved_out = (uint32_t)coordinate;
        return RIN_GPU_OK;
    }
    if (address == RIN_GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE) {
        *resolved_out = coordinate < 0 ? 0u : dimension - 1u;
        return RIN_GPU_OK;
    }
    if (address == RIN_GPU_SAMPLER_ADDRESS_REPEAT) {
        *resolved_out = coordinate < 0 ? dimension - 1u : 0u;
        return RIN_GPU_OK;
    }
    if (address == RIN_GPU_SAMPLER_ADDRESS_MIRRORED_REPEAT) {
        *resolved_out = coordinate < 0 ? 0u : dimension - 1u;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_INVALID_ARGUMENT;
}

static int sw_sample_image_texel_component(const SwImage* image,
                                           uint64_t texel_index,
                                           uint16_t component,
                                           float* value_out)
{
    uint64_t byte_offset;
    uint64_t texel_count;
    uint16_t packed;
    uint32_t bytes_per_pixel;
    uint32_t x;
    uint32_t y;
    uint8_t* depth_address;
    float depth;

    if (!image || !value_out || component > RIN_SHADER_SAMPLE_COMPONENT_MASK)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!sw_multiply_u64(image->desc.width, image->desc.height,
                         &texel_count) || texel_index >= texel_count) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    x = (uint32_t)(texel_index % image->desc.width);
    y = (uint32_t)(texel_index / image->desc.width);
    if (image->desc.format == RIN_GPU_FORMAT_R8_UNORM) {
        if (!sw_image_pixel_offset(image, x, y, 1u, &byte_offset))
            return RIN_GPU_ERROR_BOUNDS;
        *value_out = component == RIN_SHADER_SAMPLE_COMPONENT_RED
            ? (float)image->bytes[byte_offset] / 255.0f
            : component == RIN_SHADER_SAMPLE_COMPONENT_ALPHA ? 1.0f : 0.0f;
        return RIN_GPU_OK;
    }
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        image->depth_pixels != NULL) {
        if (!sw_depth_pixel_address(image, x, y, &depth_address))
            return RIN_GPU_ERROR_BOUNDS;
        memcpy(&depth, depth_address, sizeof(depth));
        if (!sw_f32_finite(depth))
            return RIN_GPU_ERROR_BACKEND;
        *value_out = component == RIN_SHADER_SAMPLE_COMPONENT_RED
            ? depth
            : component == RIN_SHADER_SAMPLE_COMPONENT_ALPHA ? 1.0f : 0.0f;
        return RIN_GPU_OK;
    }
    if (!sw_image_pixel_offset(image, x, y, bytes_per_pixel, &byte_offset)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGB565_UNORM ||
        image->desc.format == RIN_GPU_FORMAT_RGBA4_UNORM ||
        image->desc.format == RIN_GPU_FORMAT_RGB5_A1_UNORM) {
        memcpy(&packed, image->bytes + byte_offset, sizeof(packed));
        if (image->desc.format == RIN_GPU_FORMAT_RGB565_UNORM) {
            if (component == RIN_SHADER_SAMPLE_COMPONENT_RED)
                *value_out = (float)((packed >> 11u) & 0x1fu) / 31.0f;
            else if (component == RIN_SHADER_SAMPLE_COMPONENT_GREEN)
                *value_out = (float)((packed >> 5u) & 0x3fu) / 63.0f;
            else if (component == RIN_SHADER_SAMPLE_COMPONENT_BLUE)
                *value_out = (float)(packed & 0x1fu) / 31.0f;
            else
                *value_out = 1.0f;
        } else if (image->desc.format == RIN_GPU_FORMAT_RGBA4_UNORM) {
            *value_out = (float)((packed >> (12u - 4u * component)) & 0xfu) /
                15.0f;
        } else {
            if (component == RIN_SHADER_SAMPLE_COMPONENT_RED)
                *value_out = (float)((packed >> 11u) & 0x1fu) / 31.0f;
            else if (component == RIN_SHADER_SAMPLE_COMPONENT_GREEN)
                *value_out = (float)((packed >> 6u) & 0x1fu) / 31.0f;
            else if (component == RIN_SHADER_SAMPLE_COMPONENT_BLUE)
                *value_out = (float)((packed >> 1u) & 0x1fu) / 31.0f;
            else
                *value_out = (packed & 1u) != 0u ? 1.0f : 0.0f;
        }
        return RIN_GPU_OK;
    }
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT ||
        image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) {
        if (!sw_depth_pixel_address(image, x, y, &depth_address))
            return RIN_GPU_ERROR_BOUNDS;
        memcpy(&depth, depth_address, sizeof(depth));
        if (!sw_f32_finite(depth))
            return RIN_GPU_ERROR_BACKEND;
        *value_out = component == RIN_SHADER_SAMPLE_COMPONENT_RED
            ? depth
            : component == RIN_SHADER_SAMPLE_COMPONENT_ALPHA ? 1.0f : 0.0f;
        return RIN_GPU_OK;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA32_FLOAT) {
        memcpy(value_out, image->bytes + byte_offset +
                              (uint64_t)component * sizeof(*value_out),
               sizeof(*value_out));
        return sw_f32_finite(*value_out) ? RIN_GPU_OK : RIN_GPU_ERROR_BACKEND;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA16_FLOAT) {
        uint16_t component_bits;

        memcpy(&component_bits, image->bytes + byte_offset +
                   (uint64_t)component * sizeof(component_bits),
               sizeof(component_bits));
        *value_out = sw_f16_to_f32(component_bits);
        return sw_f32_finite(*value_out) ? RIN_GPU_OK : RIN_GPU_ERROR_BACKEND;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA8_UNORM ||
        image->desc.format == RIN_GPU_FORMAT_RGBA8_SRGB ||
        image->desc.format == RIN_GPU_FORMAT_BC1_RGBA_UNORM) {
        uint8_t encoded = image->bytes[byte_offset + component];

        *value_out = (float)encoded / 255.0f;
        if (image->desc.format == RIN_GPU_FORMAT_RGBA8_SRGB &&
            component != RIN_SHADER_SAMPLE_COMPONENT_ALPHA &&
            !sw_srgb_decode(*value_out, value_out))
            return RIN_GPU_ERROR_BACKEND;
    } else if (image->desc.format == RIN_GPU_FORMAT_BGRA8_UNORM ||
               image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB) {
        static const uint8_t component_offsets[4] = {2u, 1u, 0u, 3u};
        *value_out = (float)image->bytes[
            byte_offset + component_offsets[component]] / 255.0f;
        if (image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB &&
            component != RIN_SHADER_SAMPLE_COMPONENT_ALPHA &&
            !sw_srgb_decode(*value_out, value_out))
            return RIN_GPU_ERROR_BACKEND;
    } else {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    return RIN_GPU_OK;
}

static int sw_sample_image_2d_at_mip(
    const SwImage* source_image, uint32_t mip_level, uint32_t array_layer,
    const SwSampler* sampler, uint32_t spatial_filter, float u, float v,
    uint16_t component, float* value_out)
{
    SwImage view;
    const SwImage* image = &view;
    float sampled[4];
    float x_fraction;
    float y_fraction;
    float normalized_u;
    float normalized_v;
    float position;
    uint64_t texel_index;
    int32_t source_x;
    int32_t source_y;
    uint32_t x[2];
    uint32_t y[2];
    uint32_t nearest_x;
    uint32_t nearest_y;
    int result;

    if (!source_image || !sampler || !value_out ||
        (spatial_filter != RIN_GPU_SAMPLER_FILTER_NEAREST &&
         spatial_filter != RIN_GPU_SAMPLER_FILTER_LINEAR) ||
        component > RIN_SHADER_SAMPLE_COMPONENT_MASK ||
        !sw_f32_finite(u) || !sw_f32_finite(v)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    if (!sw_sampler_valid(&sampler->desc) || sampler->desc.compare_op != 0u) {
        return RIN_GPU_ERROR_BACKEND;
    }
    view = *source_image;
    result = sw_image_select_subresource(&view, mip_level, array_layer);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_validate_sampled_image_2d(image);
    if (result != RIN_GPU_OK) return result;
    result = sw_normalize_sample_coordinate(u, sampler->desc.address_u,
                                            &normalized_u);
    if (result != RIN_GPU_OK) return result;
    result = sw_normalize_sample_coordinate(v, sampler->desc.address_v,
                                            &normalized_v);
    if (result != RIN_GPU_OK) return result;
    if (spatial_filter == RIN_GPU_SAMPLER_FILTER_NEAREST) {
        nearest_x = (uint32_t)(normalized_u * (float)image->desc.width);
        nearest_y = (uint32_t)(normalized_v * (float)image->desc.height);
        if (nearest_x >= image->desc.width)
            nearest_x = image->desc.width - 1u;
        if (nearest_y >= image->desc.height)
            nearest_y = image->desc.height - 1u;
        if (!sw_multiply_u64(nearest_y, image->desc.width, &texel_index) ||
            !sw_add_u64(texel_index, nearest_x, &texel_index)) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        return sw_sample_image_texel_component(image, texel_index, component,
                                               value_out);
    }

    position = normalized_u * (float)image->desc.width - 0.5f;
    source_x = position < 0.0f ? -1 : (int32_t)position;
    x_fraction = position - (float)source_x;
    position = normalized_v * (float)image->desc.height - 0.5f;
    source_y = position < 0.0f ? -1 : (int32_t)position;
    y_fraction = position - (float)source_y;
    result = sw_address_sample_texel(source_x, image->desc.width,
                                     sampler->desc.address_u, &x[0]);
    if (result != RIN_GPU_OK) return result;
    result = sw_address_sample_texel(source_x + 1, image->desc.width,
                                     sampler->desc.address_u, &x[1]);
    if (result != RIN_GPU_OK) return result;
    result = sw_address_sample_texel(source_y, image->desc.height,
                                     sampler->desc.address_v, &y[0]);
    if (result != RIN_GPU_OK) return result;
    result = sw_address_sample_texel(source_y + 1, image->desc.height,
                                     sampler->desc.address_v, &y[1]);
    if (result != RIN_GPU_OK) return result;
    for (uint32_t sample = 0u; sample < 4u; ++sample) {
        if (!sw_multiply_u64(y[sample >> 1u], image->desc.width,
                             &texel_index) ||
            !sw_add_u64(texel_index, x[sample & 1u], &texel_index)) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        result = sw_sample_image_texel_component(image, texel_index,
                                                  component, &sampled[sample]);
        if (result != RIN_GPU_OK) return result;
    }
    sampled[0] += (sampled[1] - sampled[0]) * x_fraction;
    sampled[2] += (sampled[3] - sampled[2]) * x_fraction;
    *value_out = sampled[0] + (sampled[2] - sampled[0]) * y_fraction;
    return sw_f32_finite(*value_out) ? RIN_GPU_OK : RIN_GPU_ERROR_BACKEND;
}

static int sw_compare_float(uint32_t compare, float source, float target);

/* RSH1's scalar comparison operation has one normalized coordinate and one
 * reference value. It intentionally selects one explicit D32 mip/layer: the
 * typed bind-group contract does not expose a mip chain for depth comparison.
 */
static int sw_sample_depth_image_1d_compare(
    const SwGraphicsBindGroup* group, uint16_t image_binding,
    uint32_t sampler_binding, float coordinate, float reference,
    float* value_out)
{
    const SwImage* source_image;
    const SwSampler* sampler;
    SwImage image;
    float normalized_coordinate;
    float first_depth;
    float second_depth;
    float position;
    float fraction;
    uint32_t texel;
    uint32_t addressed_texel;
    int32_t source_texel;
    int result;

    if (!group || !value_out || image_binding >= group->binding_count ||
        sampler_binding >= group->binding_count ||
        group->kinds[image_binding] !=
            RIN_SHADER_RESOURCE_SAMPLED_DEPTH_IMAGE ||
        group->kinds[sampler_binding] !=
            RIN_SHADER_RESOURCE_COMPARISON_SAMPLER ||
        !sw_f32_finite(coordinate) || !sw_f32_finite(reference)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    source_image = group->images[image_binding];
    sampler = group->samplers[sampler_binding];
    if (!source_image || !sampler ||
        group->image_mip_counts[image_binding] != 1u ||
        sampler->desc.compare_op == 0u ||
        sampler->desc.mip_filter != RIN_GPU_SAMPLER_MIP_FILTER_NONE ||
        sampler->desc.min_filter != sampler->desc.mag_filter ||
        !sw_compare_valid(sampler->desc.compare_op) ||
        !sw_sampler_valid(&sampler->desc)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    image = *source_image;
    result = sw_image_select_subresource(
        &image, group->image_base_mip_levels[image_binding],
        group->image_array_layers[image_binding]);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_validate_sampled_depth_image_1d(&image);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_normalize_sample_coordinate(coordinate,
                                            sampler->desc.address_u,
                                            &normalized_coordinate);
    if (result != RIN_GPU_OK)
        return result;
    if (sampler->desc.mag_filter == RIN_GPU_SAMPLER_FILTER_NEAREST) {
        texel = (uint32_t)(normalized_coordinate * (float)image.desc.width);
        if (texel >= image.desc.width)
            texel = image.desc.width - 1u;
        result = sw_sample_image_texel_component(
            &image, texel, RIN_SHADER_SAMPLE_COMPONENT_RED, &first_depth);
    } else {
        position = normalized_coordinate * (float)image.desc.width - 0.5f;
        source_texel = position < 0.0f ? -1 : (int32_t)position;
        fraction = position - (float)source_texel;
        result = sw_address_sample_texel(source_texel, image.desc.width,
                                         sampler->desc.address_u,
                                         &addressed_texel);
        if (result == RIN_GPU_OK) {
            result = sw_sample_image_texel_component(
                &image, addressed_texel, RIN_SHADER_SAMPLE_COMPONENT_RED,
                &first_depth);
        }
        if (result == RIN_GPU_OK) {
            result = sw_address_sample_texel(source_texel + 1,
                                             image.desc.width,
                                             sampler->desc.address_u,
                                             &addressed_texel);
        }
        if (result == RIN_GPU_OK) {
            result = sw_sample_image_texel_component(
                &image, addressed_texel, RIN_SHADER_SAMPLE_COMPONENT_RED,
                &second_depth);
        }
        if (result == RIN_GPU_OK) {
            first_depth += (second_depth - first_depth) * fraction;
            if (!sw_f32_finite(first_depth))
                result = RIN_GPU_ERROR_BACKEND;
        }
    }
    if (result != RIN_GPU_OK)
        return result;
    *value_out = sw_compare_float(sampler->desc.compare_op, reference,
                                  first_depth) ? 1.0f : 0.0f;
    return RIN_GPU_OK;
}

/* The backend is freestanding, so derive log2 without libm.  Texture LODs
 * are positive here; the short atanh series is monotonic on [1, 2), and the
 * exponent provides the exact integer part.  It is sufficient for the
 * nearest-level boundary and the two-level trilinear weight while preserving
 * a defined result for every finite shader gradient. */
static int sw_log2_positive(float value, float* result_out)
{
    uint32_t bits;
    uint32_t exponent_bits;
    float mantissa;
    float z;
    float z_squared;
    float series;

    if (result_out == NULL || !sw_f32_finite(value) || value <= 0.0f)
        return 0;
    memcpy(&bits, &value, sizeof(bits));
    exponent_bits = (bits >> 23u) & 0xffu;
    if (exponent_bits == 0u || exponent_bits == 0xffu)
        return 0;
    mantissa = 1.0f + (float)(bits & UINT32_C(0x007fffff)) / 8388608.0f;
    z = (mantissa - 1.0f) / (mantissa + 1.0f);
    z_squared = z * z;
    series = z;
    series += z * z_squared / 3.0f;
    series += z * z_squared * z_squared / 5.0f;
    series += z * z_squared * z_squared * z_squared / 7.0f;
    series += z * z_squared * z_squared * z_squared * z_squared / 9.0f;
    series += z * z_squared * z_squared * z_squared * z_squared * z_squared /
        11.0f;
    series += z * z_squared * z_squared * z_squared * z_squared * z_squared *
        z_squared / 13.0f;
    *result_out = (float)((int32_t)exponent_bits - 127) +
        2.8853900817779268f * series;
    return sw_f32_finite(*result_out);
}

typedef struct SwTextureFootprint {
    float x_u;
    float x_v;
    float y_u;
    float y_v;
    float x_squared;
    float y_squared;
} SwTextureFootprint;

static int sw_texture_footprint(const SwImage* image, float dudx, float dudy,
                                float dvdx, float dvdy,
                                SwTextureFootprint* footprint_out)
{
    SwTextureFootprint footprint;

    if (image == NULL || footprint_out == NULL ||
        !sw_f32_finite(dudx) || !sw_f32_finite(dudy) ||
        !sw_f32_finite(dvdx) || !sw_f32_finite(dvdy)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    footprint.x_u = dudx * (float)image->desc.width;
    footprint.x_v = dvdx * (float)image->desc.height;
    footprint.y_u = dudy * (float)image->desc.width;
    footprint.y_v = dvdy * (float)image->desc.height;
    /* A finite shader derivative can still overflow while being converted to
     * texel space. Do not turn that malformed footprint into a successful
     * maximum-LOD sample: the caller's preflight/publication split relies on
     * this path reporting failure before it can alter a render target. */
    if (!sw_f32_finite(footprint.x_u) || !sw_f32_finite(footprint.x_v) ||
        !sw_f32_finite(footprint.y_u) || !sw_f32_finite(footprint.y_v)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    footprint.x_squared = footprint.x_u * footprint.x_u +
        footprint.x_v * footprint.x_v;
    footprint.y_squared = footprint.y_u * footprint.y_u +
        footprint.y_v * footprint.y_v;
    if (!sw_f32_finite(footprint.x_squared) ||
        !sw_f32_finite(footprint.y_squared)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    *footprint_out = footprint;
    return RIN_GPU_OK;
}

static int sw_mip_lod_from_rho_squared(
    const RinGpuSamplerDescV1* sampler, float rho_squared, float* lod_out)
{
    float lod;

    if (sampler == NULL || lod_out == NULL || !sw_f32_finite(rho_squared) ||
        rho_squared < 0.0f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    {
        if (rho_squared <= 1.0f)
            lod = 0.0f;
        else if (!sw_log2_positive(rho_squared, &lod))
            return RIN_GPU_ERROR_BACKEND;
        else
            lod *= 0.5f;
    }
    lod += sampler->mip_lod_bias;
    if (lod < sampler->min_lod)
        lod = sampler->min_lod;
    if (lod > sampler->max_lod)
        lod = sampler->max_lod;
    if (!sw_f32_finite(lod))
        return RIN_GPU_ERROR_BACKEND;
    *lod_out = lod;
    return RIN_GPU_OK;
}

static int sw_implicit_mip_lod(const SwImage* image,
                               const RinGpuSamplerDescV1* sampler,
                               float dudx, float dudy, float dvdx, float dvdy,
                               float* lod_out)
{
    SwTextureFootprint footprint;
    int result;

    if (sampler == NULL || lod_out == NULL)
        return RIN_GPU_ERROR_BACKEND;
    result = sw_texture_footprint(image, dudx, dudy, dvdx, dvdy, &footprint);
    if (result != RIN_GPU_OK)
        return result;
    return sw_mip_lod_from_rho_squared(
        sampler, footprint.x_squared > footprint.y_squared
            ? footprint.x_squared : footprint.y_squared,
        lod_out);
}

static int sw_sample_image_2d_at_lod(
    const SwImage* image, uint32_t base_mip_level, uint32_t mip_count,
    uint32_t array_layer,
    const SwSampler* sampler, float lod, float u, float v, uint16_t component,
    float* value_out)
{
    uint32_t lower_mip = 0u;
    uint32_t upper_mip = 0u;
    uint32_t spatial_filter;
    float mip_blend = 0.0f;
    float lower_value;
    int result;

    if (image == NULL || sampler == NULL || value_out == NULL ||
        mip_count == 0u || !sw_f32_finite(lod)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    spatial_filter = lod <= 0.0f ? sampler->desc.mag_filter
                                 : sampler->desc.min_filter;
    if (sampler->desc.mip_filter != RIN_GPU_SAMPLER_MIP_FILTER_NONE) {
        float maximum_lod;

        if (mip_count < 2u)
            return RIN_GPU_ERROR_UNSUPPORTED;
        maximum_lod = (float)(mip_count - 1u);
        if (lod < 0.0f)
            lod = 0.0f;
        if (lod > maximum_lod)
            lod = maximum_lod;
        if (sampler->desc.mip_filter == RIN_GPU_SAMPLER_FILTER_NEAREST) {
            lower_mip = (uint32_t)(lod + 0.5f);
        } else {
            lower_mip = (uint32_t)lod;
            upper_mip = lower_mip + 1u < mip_count ? lower_mip + 1u
                                                     : lower_mip;
            mip_blend = lod - (float)lower_mip;
        }
    }
    result = sw_sample_image_2d_at_mip(image, base_mip_level + lower_mip,
                                        array_layer,
                                        sampler, spatial_filter, u, v,
                                        component, &lower_value);
    if (result != RIN_GPU_OK || upper_mip == lower_mip) {
        if (result == RIN_GPU_OK)
            *value_out = lower_value;
        return result;
    }
    result = sw_sample_image_2d_at_mip(image, base_mip_level + upper_mip,
                                        array_layer,
                                        sampler, spatial_filter, u, v,
                                        component, value_out);
    if (result != RIN_GPU_OK)
        return result;
    *value_out = lower_value + (*value_out - lower_value) * mip_blend;
    return sw_f32_finite(*value_out) ? RIN_GPU_OK : RIN_GPU_ERROR_BACKEND;
}

static int sw_sample_image_2d_filtered_with_bias(
    const SwGraphicsBindGroup* group, uint16_t image_binding,
    uint32_t sampler_binding, float u, float v, float dudx, float dudy,
    float dvdx, float dvdy, float shader_lod_bias, uint16_t component,
    float* value_out)
{
    const SwImage* image;
    const SwSampler* sampler;
    RinGpuSamplerDescV1 effective_sampler_desc;
    SwImage base_view;
    uint32_t base_mip_level;
    uint32_t mip_count;
    uint32_t array_layer;
    SwTextureFootprint footprint;
    uint32_t sample_count;
    uint32_t sample_index;
    int major_is_x;
    float lod;
    float rho_squared;
    float major_u;
    float major_v;
    float sampled_value;
    float accumulated_value = 0.0f;
    int result;

    if (!group || !value_out || image_binding >= group->binding_count ||
        sampler_binding >= group->binding_count || component >
            RIN_SHADER_SAMPLE_COMPONENT_MASK ||
        group->kinds[image_binding] != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
        group->kinds[sampler_binding] != RIN_SHADER_RESOURCE_SAMPLER ||
        !sw_f32_finite(shader_lod_bias)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    image = group->images[image_binding];
    sampler = group->samplers[sampler_binding];
    base_mip_level = group->image_base_mip_levels[image_binding];
    mip_count = group->image_mip_counts[image_binding];
    array_layer = group->image_array_layers[image_binding];
    if (image == NULL || sampler == NULL || mip_count == 0u ||
        !sw_sampler_valid(&sampler->desc)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    effective_sampler_desc = sampler->desc;
    effective_sampler_desc.mip_lod_bias += shader_lod_bias;
    if (!sw_sampler_valid(&effective_sampler_desc))
        return RIN_GPU_ERROR_BACKEND;
    base_view = *image;
    result = sw_image_select_subresource(&base_view, base_mip_level,
                                         array_layer);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_validate_sampled_image_2d(&base_view);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_texture_footprint(&base_view, dudx, dudy, dvdx, dvdy,
                                  &footprint);
    if (result != RIN_GPU_OK)
        return result;
    major_is_x = footprint.x_squared >= footprint.y_squared;
    rho_squared = major_is_x ? footprint.x_squared : footprint.y_squared;
    major_u = major_is_x ? dudx : dudy;
    major_v = major_is_x ? dvdx : dvdy;
    sample_count = 1u;
    if (sampler->desc.max_anisotropy > 1u && rho_squared > 0.0f) {
        float minor_squared = major_is_x ? footprint.y_squared
                                         : footprint.x_squared;

        /* EXT_texture_filter_anisotropic uses the major/minor footprint
         * ratio, capped by the requested degree. Compare squared magnitudes
         * so this freestanding backend needs neither sqrt nor a lossy
         * division by a zero minor axis. */
        while (sample_count < sampler->desc.max_anisotropy &&
               (minor_squared == 0.0f ||
                rho_squared > minor_squared *
                    (float)(sample_count + 1u) *
                    (float)(sample_count + 1u))) {
            ++sample_count;
        }
    }
    if (sample_count == 1u) {
        result = sw_implicit_mip_lod(&base_view, &effective_sampler_desc,
                                     dudx, dudy, dvdx, dvdy, &lod);
    } else {
        rho_squared /= (float)sample_count * (float)sample_count;
        if (!sw_f32_finite(rho_squared))
            return RIN_GPU_ERROR_BACKEND;
        result = sw_mip_lod_from_rho_squared(&effective_sampler_desc,
                                             rho_squared, &lod);
    }
    if (result != RIN_GPU_OK)
        return result;
    for (sample_index = 0u; sample_index < sample_count; ++sample_index) {
        float factor = (float)(sample_index + 1u) /
            (float)(sample_count + 1u) - 0.5f;

        result = sw_sample_image_2d_at_lod(
            image, base_mip_level, mip_count, array_layer, sampler, lod,
            u + major_u * factor, v + major_v * factor, component,
            &sampled_value);
        if (result != RIN_GPU_OK)
            return result;
        accumulated_value += sampled_value;
        if (!sw_f32_finite(accumulated_value))
            return RIN_GPU_ERROR_BACKEND;
    }
    *value_out = accumulated_value / (float)sample_count;
    return sw_f32_finite(*value_out) ? RIN_GPU_OK : RIN_GPU_ERROR_BACKEND;
}

static int sw_sample_image_2d_filtered(
    const SwGraphicsBindGroup* group, uint16_t image_binding,
    uint32_t sampler_binding, float u, float v, float dudx, float dudy,
    float dvdx, float dvdy, uint16_t component, float* value_out)
{
    return sw_sample_image_2d_filtered_with_bias(
        group, image_binding, sampler_binding, u, v, dudx, dudy, dvdx, dvdy,
        0.0f, component, value_out);
}

/* OpenGL cube coordinates select the face whose axis has the greatest
 * magnitude. The six faces are stored as array layers in the canonical order
 * +X, -X, +Y, -Y, +Z, -Z. The temporary bind-group copy selects that layer
 * without mutating a live descriptor or image view. */
static int sw_sample_image_cube_filtered(
    const SwGraphicsBindGroup* group, uint16_t image_binding,
    uint32_t sampler_binding, float x, float y, float z, float dxdx,
    float dxdy, float dydx, float dydy, float dzdx, float dzdy,
    uint16_t component, float* value_out)
{
    SwGraphicsBindGroup face_group;
    const SwImage* image;
    float ax;
    float ay;
    float az;
    float major;
    float s;
    float t;
    uint32_t face;

    if (!group || !value_out || image_binding >= group->binding_count ||
        sampler_binding >= group->binding_count ||
        group->kinds[image_binding] != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
        group->kinds[sampler_binding] != RIN_SHADER_RESOURCE_SAMPLER ||
        !sw_f32_finite(x) || !sw_f32_finite(y) || !sw_f32_finite(z) ||
        !sw_f32_finite(dxdx) || !sw_f32_finite(dxdy) ||
        !sw_f32_finite(dydx) || !sw_f32_finite(dydy) ||
        !sw_f32_finite(dzdx) || !sw_f32_finite(dzdy)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    image = group->images[image_binding];
    if (!image || image->allocation_desc.array_layers < 6u ||
        group->image_array_layers[image_binding] != 0u ||
        (x == 0.0f && y == 0.0f && z == 0.0f)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    ax = sw_absf(x);
    ay = sw_absf(y);
    az = sw_absf(z);
    if (ax >= ay && ax >= az) {
        major = ax;
        if (x >= 0.0f) {
            face = 0u;
            s = -z / major;
            t = -y / major;
        } else {
            face = 1u;
            s = z / major;
            t = -y / major;
        }
    } else if (ay >= az) {
        major = ay;
        if (y >= 0.0f) {
            face = 2u;
            s = x / major;
            t = z / major;
        } else {
            face = 3u;
            s = x / major;
            t = -z / major;
        }
    } else {
        major = az;
        if (z >= 0.0f) {
            face = 4u;
            s = x / major;
            t = -y / major;
        } else {
            face = 5u;
            s = -x / major;
            t = -y / major;
        }
    }
    if (!sw_f32_finite(s) || !sw_f32_finite(t))
        return RIN_GPU_ERROR_BACKEND;
    face_group = *group;
    face_group.image_array_layers[image_binding] = face;
    /* The direction-to-face Jacobian is bounded by the source direction
     * derivatives. Passing those derivatives to the established 2D sampler
     * gives deterministic mip/aniso behavior while keeping face selection
     * independent of sampler state. */
    return sw_sample_image_2d_filtered(
        &face_group, image_binding, sampler_binding, s * 0.5f + 0.5f,
        t * 0.5f + 0.5f, dxdx, dxdy, dydx, dydy, component, value_out);
}

/* Explicit GLSL texture LOD deliberately bypasses derivative footprint and
 * anisotropy selection. The author-specified finite value is still constrained
 * by the sampler's visible LOD interval and by the actual mip chain, then the
 * same real per-mip sampler path as implicit texture2D() performs the read. */
static int sw_sample_image_2d_explicit_lod(
    const SwGraphicsBindGroup* group, uint16_t image_binding,
    uint16_t sampler_binding, float lod, float u, float v, uint16_t component,
    float* value_out)
{
    const SwImage* image;
    const SwSampler* sampler;
    SwImage base_view;
    uint32_t base_mip_level;
    uint32_t mip_count;
    uint32_t array_layer;
    int result;

    if (!group || !value_out || image_binding >= group->binding_count ||
        sampler_binding >= group->binding_count || component >
            RIN_SHADER_SAMPLE_COMPONENT_MASK ||
        group->kinds[image_binding] != RIN_SHADER_RESOURCE_SAMPLED_IMAGE ||
        group->kinds[sampler_binding] != RIN_SHADER_RESOURCE_SAMPLER ||
        !sw_f32_finite(lod)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    image = group->images[image_binding];
    sampler = group->samplers[sampler_binding];
    base_mip_level = group->image_base_mip_levels[image_binding];
    mip_count = group->image_mip_counts[image_binding];
    array_layer = group->image_array_layers[image_binding];
    if (image == NULL || sampler == NULL || mip_count == 0u ||
        !sw_sampler_valid(&sampler->desc)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    base_view = *image;
    result = sw_image_select_subresource(&base_view, base_mip_level,
                                         array_layer);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_validate_sampled_image_2d(&base_view);
    if (result != RIN_GPU_OK)
        return result;
    if (lod < sampler->desc.min_lod)
        lod = sampler->desc.min_lod;
    if (lod > sampler->desc.max_lod)
        lod = sampler->desc.max_lod;
    if (!sw_f32_finite(lod))
        return RIN_GPU_ERROR_BACKEND;
    return sw_sample_image_2d_at_lod(image, base_mip_level, mip_count,
                                     array_layer, sampler, lod, u, v,
                                     component, value_out);
}

/* RSH1 storage addresses are signed byte offsets relative to the bound range.
 * The public bind-group ABI requires a four-byte aligned range and scalar
 * storage opcodes transfer exactly one native-endian 32-bit value. Keep the
 * address check here, next to the interpreter, because indices are dynamic
 * shader data rather than descriptor-time constants. */
static int sw_compute_resource_address(const SwShaderIo* io,
                                       uint16_t resource,
                                       int32_t byte_offset,
                                       uint32_t required_access,
                                       uint8_t** address_out)
{
    const SwComputeExecution* execution;
    const SwComputeBindGroup* group;
    uint64_t offset;

    if (!io || !address_out || byte_offset < 0 ||
        ((uint32_t)byte_offset & 3u) != 0u)
        return RIN_GPU_ERROR_BOUNDS;
    execution = io->compute_execution;
    if (!execution || !execution->bind_group)
        return RIN_GPU_ERROR_UNSUPPORTED;
    group = execution->bind_group;
    if (resource >= group->binding_count ||
        (group->access[resource] & required_access) != required_access ||
        !group->buffers[resource] || !execution->storage[resource]) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    offset = (uint32_t)byte_offset;
    if (offset > group->sizes[resource] ||
        sizeof(uint32_t) > group->sizes[resource] - offset)
        return RIN_GPU_ERROR_BOUNDS;
    *address_out = execution->storage[resource] + group->offsets[resource] +
        offset;
    return RIN_GPU_OK;
}

static int sw_compute_shared_address(const SwShaderIo* io, int32_t byte_offset,
                                     uint8_t** address_out)
{
    const SwComputeExecution* execution;
    uint32_t offset;

    if (!io || !address_out || byte_offset < 0 ||
        ((uint32_t)byte_offset & 3u) != 0u) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    execution = io->compute_execution;
    if (!execution || !execution->shared_memory ||
        execution->shared_memory_size == 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    offset = (uint32_t)byte_offset;
    if (offset > execution->shared_memory_size ||
        sizeof(uint32_t) > execution->shared_memory_size - offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    *address_out = execution->shared_memory + offset;
    return RIN_GPU_OK;
}

static int sw_storage_image_address(const SwShaderIo* io, uint16_t resource,
                                    int32_t x, int32_t y, uint32_t access,
                                    uint8_t** address_out)
{
    const SwGraphicsBindGroup* group;
    const SwImage* image;
    SwImage view;
    uint64_t offset;
    int result;

    if (!io || !address_out || x < 0 || y < 0 || !io->graphics_bind_group)
        return RIN_GPU_ERROR_BOUNDS;
    group = io->graphics_bind_group;
    if (resource >= group->binding_count ||
        group->kinds[resource] != RIN_SHADER_RESOURCE_STORAGE_IMAGE ||
        (group->kinds[resource] == RIN_SHADER_RESOURCE_STORAGE_IMAGE &&
         (group->images[resource] == NULL ||
          (group->pipeline == NULL))) ||
        (group->pipeline != NULL && access == 0u))
        return RIN_GPU_ERROR_UNSUPPORTED;
    image = group->images[resource];
    view = *image;
    result = sw_image_select_subresource(
        &view, group->image_base_mip_levels[resource],
        group->image_array_layers[resource]);
    if (result != RIN_GPU_OK || view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        view.desc.format != RIN_GPU_FORMAT_R8_UNORM ||
        view.desc.sample_count != 1u ||
        (view.desc.usage & RIN_GPU_IMAGE_STORAGE) == 0u ||
        (uint32_t)x >= view.desc.width || (uint32_t)y >= view.desc.height ||
        !sw_image_pixel_offset(&view, (uint32_t)x, (uint32_t)y, 1u, &offset) ||
        offset >= view.size_bytes)
        return result != RIN_GPU_OK ? result : RIN_GPU_ERROR_BOUNDS;
    *address_out = view.bytes + offset;
    return RIN_GPU_OK;
}

static int sw_run_shader_typed(const SwShader* shader, const SwShaderIo* io)
{
    const RinShaderHeaderV1* header;
    const RinShaderInstructionV1* instructions;
    float f32_registers[SW_MAX_REGISTERS];
    float f32_register_dx[SW_MAX_REGISTERS];
    float f32_register_dy[SW_MAX_REGISTERS];
    int32_t i32_registers[SW_MAX_REGISTERS];
    uint8_t register_types[SW_MAX_REGISTERS];
    uint32_t pc;
    int result;

    if (!shader || !shader->ir || !io || !io->f32_outputs ||
        !io->i32_outputs || !io->output_types ||
        shader->ir_size < sizeof(*header))
        return RIN_GPU_ERROR_BACKEND;
    header = (const RinShaderHeaderV1*)shader->ir;
    if (header->header_size < sizeof(*header) ||
        header->header_size > shader->ir_size ||
        header->instruction_count >
            (shader->ir_size - header->header_size) / sizeof(*instructions) ||
        header->entry_instruction >= header->instruction_count ||
        header->register_count > SW_MAX_REGISTERS ||
        header->input_count > io->input_count ||
        header->output_count > io->output_count ||
        (header->input_count != 0u && (!io->f32_inputs || !io->i32_inputs ||
                                      !io->input_types)))
        return RIN_GPU_ERROR_BOUNDS;
    instructions = (const RinShaderInstructionV1*)(shader->ir + header->header_size);
    memset(f32_registers, 0, sizeof(f32_registers));
    memset(f32_register_dx, 0, sizeof(f32_register_dx));
    memset(f32_register_dy, 0, sizeof(f32_register_dy));
    memset(i32_registers, 0, sizeof(i32_registers));
    memset(register_types, SW_SHADER_VALUE_NONE, sizeof(register_types));
    memset(io->f32_outputs, 0, (size_t)io->output_count *
           sizeof(*io->f32_outputs));
    memset(io->i32_outputs, 0, (size_t)io->output_count *
           sizeof(*io->i32_outputs));
    memset(io->output_types, SW_SHADER_VALUE_NONE,
           (size_t)io->output_count * sizeof(*io->output_types));
    if (io->discarded != NULL)
        *io->discarded = 0;

    for (pc = header->entry_instruction; pc < header->instruction_count; ++pc) {
        const RinShaderInstructionV1* in = &instructions[pc];
        int32_t i32_a;
        int32_t i32_b;
        float f32_a;
        float f32_b;

        if ((in->destination != RIN_SHADER_UNUSED &&
             in->destination >= header->register_count) ||
            (in->source0 != RIN_SHADER_UNUSED &&
             in->source0 >= header->register_count) ||
            (in->source1 != RIN_SHADER_UNUSED &&
             in->source1 >= header->register_count))
            return RIN_GPU_ERROR_BOUNDS;

        switch (in->opcode) {
        case RIN_SHADER_OP_NOP:
            break;
        case RIN_SHADER_OP_CONST_I32:
            i32_registers[in->destination] = sw_i32_from_bits(in->immediate);
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        case RIN_SHADER_OP_CONST_F32:
            memcpy(&f32_registers[in->destination], &in->immediate,
                   sizeof(f32_registers[in->destination]));
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_LOAD_INPUT_F32:
            if (in->immediate >= io->input_count ||
                io->input_types[in->immediate] != SW_SHADER_VALUE_F32)
                return RIN_GPU_ERROR_BOUNDS;
            f32_registers[in->destination] = io->f32_inputs[in->immediate];
            f32_register_dx[in->destination] = io->f32_input_dx
                ? io->f32_input_dx[in->immediate] : 0.0f;
            f32_register_dy[in->destination] = io->f32_input_dy
                ? io->f32_input_dy[in->immediate] : 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_LOAD_INPUT:
            if (in->immediate >= io->input_count ||
                io->input_types[in->immediate] != SW_SHADER_VALUE_I32)
                return RIN_GPU_ERROR_BOUNDS;
            i32_registers[in->destination] = io->i32_inputs[in->immediate];
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        case RIN_SHADER_OP_LOAD_PUSH_CONSTANT_I32:
        case RIN_SHADER_OP_LOAD_PUSH_CONSTANT_F32:
            if ((header->flags & RIN_SHADER_FLAG_PUSH_CONSTANTS) == 0u ||
                !io->push_constants ||
                io->push_constant_size < sizeof(uint32_t) ||
                (in->immediate & 3u) != 0u ||
                in->immediate > io->push_constant_size - sizeof(uint32_t))
                return RIN_GPU_ERROR_UNSUPPORTED;
            if (in->opcode == RIN_SHADER_OP_LOAD_PUSH_CONSTANT_I32) {
                memcpy(&i32_registers[in->destination],
                       io->push_constants + in->immediate,
                       sizeof(i32_registers[in->destination]));
                register_types[in->destination] = SW_SHADER_VALUE_I32;
            } else {
                memcpy(&f32_registers[in->destination],
                       io->push_constants + in->immediate,
                       sizeof(f32_registers[in->destination]));
                f32_register_dx[in->destination] = 0.0f;
                f32_register_dy[in->destination] = 0.0f;
                register_types[in->destination] = SW_SHADER_VALUE_F32;
            }
            break;
        case RIN_SHADER_OP_LOAD_BUILTIN_I32:
            if (in->flags != 0u || in->destination == RIN_SHADER_UNUSED ||
                in->source0 != RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED ||
                header->stage != RIN_SHADER_STAGE_COMPUTE ||
                !io->compute_execution) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            switch (in->immediate) {
            case RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_X:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->global_x;
                break;
            case RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_Y:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->global_y;
                break;
            case RIN_SHADER_BUILTIN_GLOBAL_INVOCATION_Z:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->global_z;
                break;
            case RIN_SHADER_BUILTIN_LOCAL_INVOCATION_X:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->local_x;
                break;
            case RIN_SHADER_BUILTIN_LOCAL_INVOCATION_Y:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->local_y;
                break;
            case RIN_SHADER_BUILTIN_LOCAL_INVOCATION_Z:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->local_z;
                break;
            case RIN_SHADER_BUILTIN_WORKGROUP_X:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->workgroup_x;
                break;
            case RIN_SHADER_BUILTIN_WORKGROUP_Y:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->workgroup_y;
                break;
            case RIN_SHADER_BUILTIN_WORKGROUP_Z:
                i32_registers[in->destination] =
                    (int32_t)io->compute_execution->workgroup_z;
                break;
            default:
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        case RIN_SHADER_OP_LOAD_BUILTIN_F32:
            if (header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                in->destination == RIN_SHADER_UNUSED) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            if (in->immediate >= RIN_SHADER_BUILTIN_FRAG_COORD_X &&
                in->immediate <= RIN_SHADER_BUILTIN_FRAG_COORD_W) {
                uint32_t component = in->immediate -
                    RIN_SHADER_BUILTIN_FRAG_COORD_X;
                const SwFragmentCoordinate* coordinate =
                    io->fragment_coordinate;

                if (!coordinate || coordinate->valid != 1u ||
                    !sw_f32_finite(coordinate->value[component]) ||
                    !sw_f32_finite(coordinate->dx[component]) ||
                    !sw_f32_finite(coordinate->dy[component])) {
                    return RIN_GPU_ERROR_BACKEND;
                }
                f32_registers[in->destination] = coordinate->value[component];
                f32_register_dx[in->destination] = coordinate->dx[component];
                f32_register_dy[in->destination] = coordinate->dy[component];
            } else if (in->immediate == RIN_SHADER_BUILTIN_POINT_COORD_X) {
                if (io->point_coord_valid != 1u ||
                    !sw_f32_finite(io->point_coord_x) ||
                    !sw_f32_finite(io->point_coord_dx)) {
                    return RIN_GPU_ERROR_BACKEND;
                }
                f32_registers[in->destination] = io->point_coord_x;
                f32_register_dx[in->destination] = io->point_coord_dx;
                f32_register_dy[in->destination] = 0.0f;
            } else if (in->immediate == RIN_SHADER_BUILTIN_POINT_COORD_Y) {
                if (io->point_coord_valid != 1u ||
                    !sw_f32_finite(io->point_coord_y) ||
                    !sw_f32_finite(io->point_coord_dy)) {
                    return RIN_GPU_ERROR_BACKEND;
                }
                f32_registers[in->destination] = io->point_coord_y;
                f32_register_dx[in->destination] = 0.0f;
                f32_register_dy[in->destination] = io->point_coord_dy;
            } else {
                /* RSH1's verifier recognizes additional core builtins. Do
                 * not fabricate a value until this executor can source it. */
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_LOAD_RESOURCE_I32:
        case RIN_SHADER_OP_LOAD_RESOURCE_F32: {
            uint8_t* address;

            if (in->flags != 0u || in->destination == RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED || in->immediate != 0u ||
                in->resource >= header->resource_count ||
                register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                header->stage != RIN_SHADER_STAGE_COMPUTE) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_compute_resource_address(
                io, in->resource, i32_registers[in->source0],
                RIN_GPU_RESOURCE_READ, &address);
            if (result != RIN_GPU_OK)
                return result;
            if (in->opcode == RIN_SHADER_OP_LOAD_RESOURCE_I32) {
                memcpy(&i32_registers[in->destination], address,
                       sizeof(i32_registers[in->destination]));
                register_types[in->destination] = SW_SHADER_VALUE_I32;
            } else {
                memcpy(&f32_registers[in->destination], address,
                       sizeof(f32_registers[in->destination]));
                if (!sw_f32_finite(f32_registers[in->destination]))
                    return RIN_GPU_ERROR_BACKEND;
                f32_register_dx[in->destination] = 0.0f;
                f32_register_dy[in->destination] = 0.0f;
                register_types[in->destination] = SW_SHADER_VALUE_F32;
            }
            break;
        }
        case RIN_SHADER_OP_STORE_RESOURCE_I32:
        case RIN_SHADER_OP_STORE_RESOURCE_F32: {
            uint8_t* address;

            if (in->flags != 0u || in->destination != RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 == RIN_SHADER_UNUSED || in->immediate != 0u ||
                in->resource >= header->resource_count ||
                register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                header->stage != RIN_SHADER_STAGE_COMPUTE ||
                (in->opcode == RIN_SHADER_OP_STORE_RESOURCE_I32
                     ? register_types[in->source1] != SW_SHADER_VALUE_I32
                     : register_types[in->source1] != SW_SHADER_VALUE_F32)) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_compute_resource_address(
                io, in->resource, i32_registers[in->source0],
                RIN_GPU_RESOURCE_WRITE, &address);
            if (result != RIN_GPU_OK)
                return result;
            if (in->opcode == RIN_SHADER_OP_STORE_RESOURCE_I32) {
                memcpy(address, &i32_registers[in->source1],
                       sizeof(i32_registers[in->source1]));
            } else {
                if (!sw_f32_finite(f32_registers[in->source1]))
                    return RIN_GPU_ERROR_BACKEND;
                memcpy(address, &f32_registers[in->source1],
                       sizeof(f32_registers[in->source1]));
            }
            break;
        }
        case RIN_SHADER_OP_LOAD_IMAGE_2D_I32:
        case RIN_SHADER_OP_LOAD_IMAGE_2D_F32: {
            uint8_t* address;
            if (in->destination == RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 == RIN_SHADER_UNUSED ||
                in->resource >= header->resource_count || in->immediate != 0u ||
                in->flags != 0u || header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                register_types[in->source1] != SW_SHADER_VALUE_I32)
                return RIN_GPU_ERROR_UNSUPPORTED;
            result = sw_storage_image_address(
                io, in->resource, i32_registers[in->source0],
                i32_registers[in->source1], RIN_GPU_RESOURCE_READ, &address);
            if (result != RIN_GPU_OK) return result;
            if (in->opcode == RIN_SHADER_OP_LOAD_IMAGE_2D_I32) {
                i32_registers[in->destination] = (int32_t)*address;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
            } else {
                f32_registers[in->destination] = (float)*address / 255.0f;
                f32_register_dx[in->destination] = 0.0f;
                f32_register_dy[in->destination] = 0.0f;
                register_types[in->destination] = SW_SHADER_VALUE_F32;
            }
            break;
        }
        case RIN_SHADER_OP_STORE_IMAGE_2D_I32:
        case RIN_SHADER_OP_STORE_IMAGE_2D_F32: {
            uint8_t* address;
            if (in->destination == RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 == RIN_SHADER_UNUSED ||
                in->resource >= header->resource_count || in->immediate != 0u ||
                in->flags != 0u || header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                register_types[in->source1] != SW_SHADER_VALUE_I32)
                return RIN_GPU_ERROR_UNSUPPORTED;
            result = sw_storage_image_address(
                io, in->resource, i32_registers[in->source0],
                i32_registers[in->source1], RIN_GPU_RESOURCE_WRITE, &address);
            if (result != RIN_GPU_OK) return result;
            if (in->opcode == RIN_SHADER_OP_STORE_IMAGE_2D_I32) {
                if (register_types[in->destination] != SW_SHADER_VALUE_I32 ||
                    i32_registers[in->destination] < 0 ||
                    i32_registers[in->destination] > 255)
                    return RIN_GPU_ERROR_BOUNDS;
                *address = (uint8_t)i32_registers[in->destination];
            } else {
                float value;
                if (register_types[in->destination] != SW_SHADER_VALUE_F32 ||
                    !sw_f32_finite(f32_registers[in->destination]) ||
                    f32_registers[in->destination] < 0.0f ||
                    f32_registers[in->destination] > 1.0f)
                    return RIN_GPU_ERROR_BOUNDS;
                value = f32_registers[in->destination] * 255.0f + 0.5f;
                if (value < 0.0f || value > 255.0f)
                    return RIN_GPU_ERROR_BOUNDS;
                *address = (uint8_t)value;
            }
            break;
        }
        case RIN_SHADER_OP_ATOMIC_ADD_I32:
        case RIN_SHADER_OP_ATOMIC_EXCHANGE_I32:
        case RIN_SHADER_OP_ATOMIC_MIN_I32:
        case RIN_SHADER_OP_ATOMIC_MAX_I32: {
            uint8_t* address;
            int32_t observed;
            int32_t replacement;

            if (in->destination == RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 == RIN_SHADER_UNUSED ||
                in->resource >= header->resource_count ||
                in->flags != 0u || in->immediate != 0u ||
                header->stage != RIN_SHADER_STAGE_COMPUTE ||
                register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                register_types[in->source1] != SW_SHADER_VALUE_I32) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_compute_resource_address(
                io, in->resource, i32_registers[in->source0],
                RIN_GPU_RESOURCE_READ | RIN_GPU_RESOURCE_WRITE, &address);
            if (result != RIN_GPU_OK)
                return result;
            memcpy(&observed, address, sizeof(observed));
            switch (in->opcode) {
            case RIN_SHADER_OP_ATOMIC_ADD_I32:
                replacement = sw_i32_add(observed,
                                         i32_registers[in->source1]);
                break;
            case RIN_SHADER_OP_ATOMIC_EXCHANGE_I32:
                replacement = i32_registers[in->source1];
                break;
            case RIN_SHADER_OP_ATOMIC_MIN_I32:
                replacement = observed < i32_registers[in->source1]
                    ? observed : i32_registers[in->source1];
                break;
            case RIN_SHADER_OP_ATOMIC_MAX_I32:
                replacement = observed > i32_registers[in->source1]
                    ? observed : i32_registers[in->source1];
                break;
            default:
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            memcpy(address, &replacement, sizeof(replacement));
            i32_registers[in->destination] = observed;
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        }
        case RIN_SHADER_OP_LOAD_SHARED_I32: {
            uint8_t* address;

            if (in->destination == RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                in->flags != 0u || header->stage != RIN_SHADER_STAGE_COMPUTE ||
                (header->flags & RIN_SHADER_FLAG_WORKGROUP_SHARED) == 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_I32) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_compute_shared_address(
                io, i32_registers[in->source0], &address);
            if (result != RIN_GPU_OK) return result;
            memcpy(&i32_registers[in->destination], address,
                   sizeof(i32_registers[in->destination]));
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        }
        case RIN_SHADER_OP_STORE_SHARED_I32: {
            uint8_t* address;

            if (in->destination != RIN_SHADER_UNUSED ||
                in->source0 == RIN_SHADER_UNUSED ||
                in->source1 == RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                in->flags != 0u || header->stage != RIN_SHADER_STAGE_COMPUTE ||
                (header->flags & RIN_SHADER_FLAG_WORKGROUP_SHARED) == 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                register_types[in->source1] != SW_SHADER_VALUE_I32) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_compute_shared_address(
                io, i32_registers[in->source0], &address);
            if (result != RIN_GPU_OK) return result;
            memcpy(address, &i32_registers[in->source1],
                   sizeof(i32_registers[in->source1]));
            break;
        }
        case RIN_SHADER_OP_WORKGROUP_BARRIER:
            if (in->destination != RIN_SHADER_UNUSED ||
                in->source0 != RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                in->flags != 0u || header->stage != RIN_SHADER_STAGE_COMPUTE ||
                (header->flags & RIN_SHADER_FLAG_WORKGROUP_SHARED) == 0u ||
                !io->compute_execution ||
                io->compute_execution->shared_epoch == UINT32_MAX) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            /* The validated profile has one invocation per workgroup, so
             * entering the barrier is a concrete shared-memory ordering
             * boundary rather than a dropped command. */
            io->compute_execution->shared_epoch++;
            break;
        case RIN_SHADER_OP_SAMPLE_IMAGE_2D_F32:
            if (in->destination == RIN_SHADER_UNUSED ||
                header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                !io->graphics_bind_group ||
                in->resource >= header->resource_count ||
                in->immediate >= header->resource_count ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                (in->flags & ~RIN_SHADER_SAMPLE_COMPONENT_MASK) != 0u) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_sample_image_2d_filtered(
                io->graphics_bind_group, in->resource, in->immediate,
                f32_registers[in->source0], f32_registers[in->source1],
                f32_register_dx[in->source0], f32_register_dy[in->source0],
                f32_register_dx[in->source1], f32_register_dy[in->source1],
                in->flags, &f32_registers[in->destination]);
            if (result != RIN_GPU_OK) return result;
            /* An implicit texture lookup does not expose the sampled
             * neighbor values to RSH1. Derivatives of those values are not
             * generated by RinGL's bounded derivative profile. */
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_SAMPLE_IMAGE_CUBE_F32: {
            uint16_t image_binding = RIN_SHADER_SAMPLE_CUBE_IMAGE_BINDING(
                in->resource);
            uint16_t sampler_binding = RIN_SHADER_SAMPLE_CUBE_SAMPLER_BINDING(
                in->resource);

            if (in->destination == RIN_SHADER_UNUSED ||
                header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                !io->graphics_bind_group ||
                (in->resource >> (RIN_SHADER_SAMPLE_CUBE_BINDING_BITS * 2u)) !=
                    0u ||
                image_binding >= header->resource_count ||
                sampler_binding >= header->resource_count ||
                image_binding == sampler_binding ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                in->immediate >= header->register_count ||
                register_types[in->immediate] != SW_SHADER_VALUE_F32 ||
                (in->flags & ~RIN_SHADER_SAMPLE_COMPONENT_MASK) != 0u) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_sample_image_cube_filtered(
                io->graphics_bind_group, image_binding, sampler_binding,
                f32_registers[in->source0], f32_registers[in->source1],
                f32_registers[in->immediate],
                f32_register_dx[in->source0], f32_register_dy[in->source0],
                f32_register_dx[in->source1], f32_register_dy[in->source1],
                f32_register_dx[in->immediate],
                f32_register_dy[in->immediate], in->flags,
                &f32_registers[in->destination]);
            if (result != RIN_GPU_OK)
                return result;
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_SAMPLE_IMAGE_2D_LOD_F32:
        case RIN_SHADER_OP_SAMPLE_IMAGE_2D_BIAS_F32: {
            uint16_t image_binding = RIN_SHADER_SAMPLE_2D_LOD_IMAGE_BINDING(
                in->resource);
            uint16_t sampler_binding = RIN_SHADER_SAMPLE_2D_LOD_SAMPLER_BINDING(
                in->resource);
            if (in->destination == RIN_SHADER_UNUSED ||
                header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                !io->graphics_bind_group ||
                (in->resource >>
                 (RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS * 2u)) != 0u ||
                image_binding >= header->resource_count ||
                sampler_binding >= header->resource_count ||
                image_binding == sampler_binding ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                in->immediate >= header->register_count ||
                register_types[in->immediate] != SW_SHADER_VALUE_F32 ||
                (in->flags & ~RIN_SHADER_SAMPLE_COMPONENT_MASK) != 0u ||
                !sw_f32_finite(f32_registers[in->immediate])) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            if (in->opcode == RIN_SHADER_OP_SAMPLE_IMAGE_2D_LOD_F32) {
                result = sw_sample_image_2d_explicit_lod(
                    io->graphics_bind_group, image_binding, sampler_binding,
                    f32_registers[in->immediate],
                    f32_registers[in->source0], f32_registers[in->source1],
                    in->flags, &f32_registers[in->destination]);
            } else {
                result = sw_sample_image_2d_filtered_with_bias(
                    io->graphics_bind_group, image_binding, sampler_binding,
                    f32_registers[in->source0], f32_registers[in->source1],
                    f32_register_dx[in->source0], f32_register_dy[in->source0],
                    f32_register_dx[in->source1], f32_register_dy[in->source1],
                    f32_registers[in->immediate], in->flags,
                    &f32_registers[in->destination]);
            }
            if (result != RIN_GPU_OK)
                return result;
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_SAMPLE_IMAGE_2D_GRAD_F32: {
            uint16_t image_binding = RIN_SHADER_SAMPLE_2D_GRAD_IMAGE_BINDING(
                in->resource);
            uint16_t sampler_binding = RIN_SHADER_SAMPLE_2D_GRAD_SAMPLER_BINDING(
                in->resource);
            uint32_t gradient_base = in->immediate;

            if (in->destination == RIN_SHADER_UNUSED ||
                header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                !io->graphics_bind_group ||
                (in->resource >>
                 (RIN_SHADER_SAMPLE_2D_LOD_BINDING_BITS * 2u)) != 0u ||
                image_binding >= header->resource_count ||
                sampler_binding >= header->resource_count ||
                image_binding == sampler_binding ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                gradient_base >= header->register_count ||
                header->register_count - gradient_base < 4u ||
                register_types[gradient_base] != SW_SHADER_VALUE_F32 ||
                register_types[gradient_base + 1u] != SW_SHADER_VALUE_F32 ||
                register_types[gradient_base + 2u] != SW_SHADER_VALUE_F32 ||
                register_types[gradient_base + 3u] != SW_SHADER_VALUE_F32 ||
                (in->flags & ~RIN_SHADER_SAMPLE_COMPONENT_MASK) != 0u ||
                !sw_f32_finite(f32_registers[gradient_base]) ||
                !sw_f32_finite(f32_registers[gradient_base + 1u]) ||
                !sw_f32_finite(f32_registers[gradient_base + 2u]) ||
                !sw_f32_finite(f32_registers[gradient_base + 3u])) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_sample_image_2d_filtered(
                io->graphics_bind_group, image_binding, sampler_binding,
                f32_registers[in->source0], f32_registers[in->source1],
                f32_registers[gradient_base], f32_registers[gradient_base + 1u],
                f32_registers[gradient_base + 2u],
                f32_registers[gradient_base + 3u], in->flags,
                &f32_registers[in->destination]);
            if (result != RIN_GPU_OK)
                return result;
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_SAMPLE_COMPARE_F32:
            if (in->destination == RIN_SHADER_UNUSED ||
                header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                !io->graphics_bind_group ||
                in->resource >= header->resource_count ||
                in->immediate >= header->resource_count ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                in->flags != 0u) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            result = sw_sample_depth_image_1d_compare(
                io->graphics_bind_group, in->resource, in->immediate,
                f32_registers[in->source0], f32_registers[in->source1],
                &f32_registers[in->destination]);
            if (result != RIN_GPU_OK)
                return result;
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_MOV:
            if (register_types[in->source0] == SW_SHADER_VALUE_NONE)
                return RIN_GPU_ERROR_BACKEND;
            if (register_types[in->source0] == SW_SHADER_VALUE_I32)
                i32_registers[in->destination] = i32_registers[in->source0];
            else {
                f32_registers[in->destination] = f32_registers[in->source0];
                f32_register_dx[in->destination] =
                    f32_register_dx[in->source0];
                f32_register_dy[in->destination] =
                    f32_register_dy[in->source0];
            }
            register_types[in->destination] = register_types[in->source0];
            break;
        case RIN_SHADER_OP_JUMP:
            if (in->flags != 0u || in->destination != RIN_SHADER_UNUSED ||
                in->source0 != RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate <= pc ||
                in->immediate >= header->instruction_count) {
                return RIN_GPU_ERROR_BACKEND;
            }
            /* The loop increments pc after this case. The RSH1 verifier
             * permits only forward targets, so subtracting one cannot wrap or
             * create an executor loop. */
            pc = in->immediate - 1u;
            break;
        case RIN_SHADER_OP_JUMP_IF:
            if (in->flags != 0u || in->destination != RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate <= pc ||
                in->immediate >= header->instruction_count ||
                register_types[in->source0] != SW_SHADER_VALUE_I32) {
                return RIN_GPU_ERROR_BACKEND;
            }
            if (i32_registers[in->source0] != 0)
                pc = in->immediate - 1u;
            break;
        case RIN_SHADER_OP_ADD_I32:
        case RIN_SHADER_OP_SUB_I32:
        case RIN_SHADER_OP_MUL_I32:
        case RIN_SHADER_OP_DIV_I32:
        case RIN_SHADER_OP_MOD_I32:
        case RIN_SHADER_OP_MIN_I32:
        case RIN_SHADER_OP_MAX_I32:
        case RIN_SHADER_OP_AND_I32:
        case RIN_SHADER_OP_OR_I32:
        case RIN_SHADER_OP_XOR_I32:
        case RIN_SHADER_OP_SHL_I32:
        case RIN_SHADER_OP_SHR_I32:
        case RIN_SHADER_OP_CMP_EQ_I32:
        case RIN_SHADER_OP_CMP_NE_I32:
        case RIN_SHADER_OP_CMP_LT_I32:
        case RIN_SHADER_OP_CMP_LE_I32:
        case RIN_SHADER_OP_CMP_GT_I32:
        case RIN_SHADER_OP_CMP_GE_I32:
            if (register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                register_types[in->source1] != SW_SHADER_VALUE_I32)
                return RIN_GPU_ERROR_BACKEND;
            i32_a = i32_registers[in->source0];
            i32_b = i32_registers[in->source1];
            switch (in->opcode) {
            case RIN_SHADER_OP_ADD_I32:
                i32_registers[in->destination] = sw_i32_add(i32_a, i32_b);
                break;
            case RIN_SHADER_OP_SUB_I32:
                i32_registers[in->destination] = sw_i32_subtract(i32_a, i32_b);
                break;
            case RIN_SHADER_OP_MUL_I32:
                i32_registers[in->destination] = sw_i32_multiply(i32_a, i32_b);
                break;
            case RIN_SHADER_OP_DIV_I32:
                if (i32_b == 0)
                    return RIN_GPU_ERROR_BACKEND;
                i32_registers[in->destination] =
                    (i32_a == INT32_MIN && i32_b == -1) ? INT32_MIN :
                    i32_a / i32_b;
                break;
            case RIN_SHADER_OP_MOD_I32:
                if (i32_b == 0)
                    return RIN_GPU_ERROR_BACKEND;
                i32_registers[in->destination] =
                    (i32_a == INT32_MIN && i32_b == -1) ? 0 : i32_a % i32_b;
                break;
            case RIN_SHADER_OP_MIN_I32:
                i32_registers[in->destination] =
                    i32_a < i32_b ? i32_a : i32_b;
                break;
            case RIN_SHADER_OP_MAX_I32:
                i32_registers[in->destination] =
                    i32_a > i32_b ? i32_a : i32_b;
                break;
            case RIN_SHADER_OP_AND_I32:
                i32_registers[in->destination] =
                    sw_i32_from_bits((uint32_t)i32_a & (uint32_t)i32_b);
                break;
            case RIN_SHADER_OP_OR_I32:
                i32_registers[in->destination] =
                    sw_i32_from_bits((uint32_t)i32_a | (uint32_t)i32_b);
                break;
            case RIN_SHADER_OP_XOR_I32:
                i32_registers[in->destination] =
                    sw_i32_from_bits((uint32_t)i32_a ^ (uint32_t)i32_b);
                break;
            case RIN_SHADER_OP_SHL_I32:
                i32_registers[in->destination] = sw_i32_from_bits(
                    (uint32_t)i32_a << ((uint32_t)i32_b & 31u));
                break;
            case RIN_SHADER_OP_SHR_I32:
                i32_registers[in->destination] =
                    sw_i32_shift_right(i32_a, (uint32_t)i32_b);
                break;
            case RIN_SHADER_OP_CMP_EQ_I32:
                i32_registers[in->destination] = i32_a == i32_b;
                break;
            case RIN_SHADER_OP_CMP_NE_I32:
                i32_registers[in->destination] = i32_a != i32_b;
                break;
            case RIN_SHADER_OP_CMP_LT_I32:
                i32_registers[in->destination] = i32_a < i32_b;
                break;
            case RIN_SHADER_OP_CMP_LE_I32:
                i32_registers[in->destination] = i32_a <= i32_b;
                break;
            case RIN_SHADER_OP_CMP_GT_I32:
                i32_registers[in->destination] = i32_a > i32_b;
                break;
            case RIN_SHADER_OP_CMP_GE_I32:
                i32_registers[in->destination] = i32_a >= i32_b;
                break;
            default:
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        case RIN_SHADER_OP_ADD_F32:
        case RIN_SHADER_OP_SUB_F32:
        case RIN_SHADER_OP_MUL_F32:
        case RIN_SHADER_OP_DIV_F32:
        case RIN_SHADER_OP_MIN_F32:
        case RIN_SHADER_OP_MAX_F32:
        case RIN_SHADER_OP_CMP_EQ_F32:
        case RIN_SHADER_OP_CMP_NE_F32:
        case RIN_SHADER_OP_CMP_LT_F32:
        case RIN_SHADER_OP_CMP_LE_F32:
        case RIN_SHADER_OP_CMP_GT_F32:
        case RIN_SHADER_OP_CMP_GE_F32:
            if (register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32)
                return RIN_GPU_ERROR_BACKEND;
            f32_a = f32_registers[in->source0];
            f32_b = f32_registers[in->source1];
            switch (in->opcode) {
            case RIN_SHADER_OP_ADD_F32:
                f32_registers[in->destination] = f32_a + f32_b;
                f32_register_dx[in->destination] =
                    f32_register_dx[in->source0] + f32_register_dx[in->source1];
                f32_register_dy[in->destination] =
                    f32_register_dy[in->source0] + f32_register_dy[in->source1];
                register_types[in->destination] = SW_SHADER_VALUE_F32;
                break;
            case RIN_SHADER_OP_SUB_F32:
                f32_registers[in->destination] = f32_a - f32_b;
                f32_register_dx[in->destination] =
                    f32_register_dx[in->source0] - f32_register_dx[in->source1];
                f32_register_dy[in->destination] =
                    f32_register_dy[in->source0] - f32_register_dy[in->source1];
                register_types[in->destination] = SW_SHADER_VALUE_F32;
                break;
            case RIN_SHADER_OP_MUL_F32:
                f32_registers[in->destination] = f32_a * f32_b;
                f32_register_dx[in->destination] =
                    f32_register_dx[in->source0] * f32_b +
                    f32_a * f32_register_dx[in->source1];
                f32_register_dy[in->destination] =
                    f32_register_dy[in->source0] * f32_b +
                    f32_a * f32_register_dy[in->source1];
                register_types[in->destination] = SW_SHADER_VALUE_F32;
                break;
            case RIN_SHADER_OP_DIV_F32:
                if (f32_b == 0.0f)
                    return RIN_GPU_ERROR_BACKEND;
                f32_registers[in->destination] = f32_a / f32_b;
                f32_register_dx[in->destination] =
                    (f32_register_dx[in->source0] * f32_b -
                     f32_a * f32_register_dx[in->source1]) / (f32_b * f32_b);
                f32_register_dy[in->destination] =
                    (f32_register_dy[in->source0] * f32_b -
                     f32_a * f32_register_dy[in->source1]) / (f32_b * f32_b);
                register_types[in->destination] = SW_SHADER_VALUE_F32;
                break;
            case RIN_SHADER_OP_MIN_F32:
                f32_registers[in->destination] =
                    f32_a < f32_b ? f32_a : f32_b;
                f32_register_dx[in->destination] = f32_a < f32_b
                    ? f32_register_dx[in->source0]
                    : f32_register_dx[in->source1];
                f32_register_dy[in->destination] = f32_a < f32_b
                    ? f32_register_dy[in->source0]
                    : f32_register_dy[in->source1];
                register_types[in->destination] = SW_SHADER_VALUE_F32;
                break;
            case RIN_SHADER_OP_MAX_F32:
                f32_registers[in->destination] =
                    f32_a > f32_b ? f32_a : f32_b;
                f32_register_dx[in->destination] = f32_a > f32_b
                    ? f32_register_dx[in->source0]
                    : f32_register_dx[in->source1];
                f32_register_dy[in->destination] = f32_a > f32_b
                    ? f32_register_dy[in->source0]
                    : f32_register_dy[in->source1];
                register_types[in->destination] = SW_SHADER_VALUE_F32;
                break;
            case RIN_SHADER_OP_CMP_EQ_F32:
                i32_registers[in->destination] = f32_a == f32_b;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
                break;
            case RIN_SHADER_OP_CMP_NE_F32:
                i32_registers[in->destination] = f32_a != f32_b;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
                break;
            case RIN_SHADER_OP_CMP_LT_F32:
                i32_registers[in->destination] = f32_a < f32_b;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
                break;
            case RIN_SHADER_OP_CMP_LE_F32:
                i32_registers[in->destination] = f32_a <= f32_b;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
                break;
            case RIN_SHADER_OP_CMP_GT_F32:
                i32_registers[in->destination] = f32_a > f32_b;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
                break;
            case RIN_SHADER_OP_CMP_GE_F32:
                i32_registers[in->destination] = f32_a >= f32_b;
                register_types[in->destination] = SW_SHADER_VALUE_I32;
                break;
            default:
                return RIN_GPU_ERROR_BACKEND;
            }
            if (register_types[in->destination] == SW_SHADER_VALUE_F32 &&
                (!sw_f32_finite(f32_registers[in->destination]) ||
                 !sw_f32_finite(f32_register_dx[in->destination]) ||
                 !sw_f32_finite(f32_register_dy[in->destination]))) {
                /* Keep a non-finite intermediate from being hidden by a
                 * later derivative instruction or branchless overwrite. */
                return RIN_GPU_ERROR_BACKEND;
            }
            break;
        case RIN_SHADER_OP_FLOOR_F32:
            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_floorf(f32_registers[in->source0],
                           &f32_registers[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            /* GLSL derivatives are undefined at floor discontinuities.  Keep
             * this scalar backend deterministic at those boundaries and use
             * the mathematically correct zero derivative everywhere else. */
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_SQRT_F32:
            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_sqrtf(f32_registers[in->source0],
                          &f32_registers[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            if (f32_registers[in->destination] == 0.0f) {
                /* GLSL leaves derivatives undefined at zero. Keep the
                 * backend deterministic instead of manufacturing infinity. */
                f32_register_dx[in->destination] = 0.0f;
                f32_register_dy[in->destination] = 0.0f;
            } else {
                float denominator = 2.0f * f32_registers[in->destination];

                f32_register_dx[in->destination] =
                    f32_register_dx[in->source0] / denominator;
                f32_register_dy[in->destination] =
                    f32_register_dy[in->source0] / denominator;
                if (!sw_f32_finite(f32_register_dx[in->destination]) ||
                    !sw_f32_finite(f32_register_dy[in->destination])) {
                    return RIN_GPU_ERROR_BACKEND;
                }
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_SIN_F32:
        case RIN_SHADER_OP_COS_F32: {
            float sine;
            float cosine;
            float derivative_scale;

            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_sincosf(f32_registers[in->source0], &sine, &cosine)) {
                return RIN_GPU_ERROR_BACKEND;
            }
            if (in->opcode == RIN_SHADER_OP_SIN_F32) {
                f32_registers[in->destination] = sine;
                derivative_scale = cosine;
            } else {
                f32_registers[in->destination] = cosine;
                derivative_scale = -sine;
            }
            f32_register_dx[in->destination] =
                derivative_scale * f32_register_dx[in->source0];
            f32_register_dy[in->destination] =
                derivative_scale * f32_register_dy[in->source0];
            if (!sw_f32_finite(f32_register_dx[in->destination]) ||
                !sw_f32_finite(f32_register_dy[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_ATAN_F32: {
            float denominator;
            float derivative_scale;

            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_atanf(f32_registers[in->source0],
                          &f32_registers[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            if (sw_absf(f32_registers[in->source0]) > 1.0f) {
                float inverse = 1.0f / f32_registers[in->source0];

                denominator = 1.0f + inverse * inverse;
                derivative_scale = inverse * inverse / denominator;
            } else {
                denominator = 1.0f + f32_registers[in->source0] *
                    f32_registers[in->source0];
                derivative_scale = 1.0f / denominator;
            }
            if (!sw_f32_finite(denominator) || denominator == 0.0f)
                return RIN_GPU_ERROR_BACKEND;
            f32_register_dx[in->destination] =
                derivative_scale * f32_register_dx[in->source0];
            f32_register_dy[in->destination] =
                derivative_scale * f32_register_dy[in->source0];
            if (!sw_f32_finite(f32_register_dx[in->destination]) ||
                !sw_f32_finite(f32_register_dy[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_ATAN2_F32: {
            float denominator;
            float derivative_x;
            float derivative_y;
            float scale;
            float normalized_y;
            float normalized_x;
            float scaled_source0_dx;
            float scaled_source0_dy;
            float scaled_source1_dx;
            float scaled_source1_dy;

            if (in->flags != 0u || in->resource != RIN_SHADER_UNUSED ||
                in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_registers[in->source1]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source1]) ||
                !sw_f32_finite(f32_register_dy[in->source1]) ||
                !sw_atan2f(f32_registers[in->source0],
                           f32_registers[in->source1],
                           &f32_registers[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            scale = sw_absf(f32_registers[in->source0]);
            if (sw_absf(f32_registers[in->source1]) > scale)
                scale = sw_absf(f32_registers[in->source1]);
            if (!sw_f32_finite(scale) || scale == 0.0f)
                return RIN_GPU_ERROR_BACKEND;
            normalized_y = f32_registers[in->source0] / scale;
            normalized_x = f32_registers[in->source1] / scale;
            scaled_source0_dx = f32_register_dx[in->source0] / scale;
            scaled_source0_dy = f32_register_dy[in->source0] / scale;
            scaled_source1_dx = f32_register_dx[in->source1] / scale;
            scaled_source1_dy = f32_register_dy[in->source1] / scale;
            denominator = normalized_y * normalized_y +
                normalized_x * normalized_x;
            derivative_x = normalized_x * scaled_source0_dx -
                normalized_y * scaled_source1_dx;
            derivative_y = normalized_x * scaled_source0_dy -
                normalized_y * scaled_source1_dy;
            if (!sw_f32_finite(denominator) || denominator == 0.0f ||
                !sw_f32_finite(derivative_x) ||
                !sw_f32_finite(derivative_y)) {
                return RIN_GPU_ERROR_BACKEND;
            }
            f32_register_dx[in->destination] = derivative_x / denominator;
            f32_register_dy[in->destination] = derivative_y / denominator;
            if (!sw_f32_finite(f32_register_dx[in->destination]) ||
                !sw_f32_finite(f32_register_dy[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_ASIN_F32:
        case RIN_SHADER_OP_ACOS_F32: {
            float complement;
            float denominator;
            float derivative_scale;

            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                (in->opcode == RIN_SHADER_OP_ASIN_F32
                     ? !sw_asinf(f32_registers[in->source0],
                                 &f32_registers[in->destination])
                     : !sw_acosf(f32_registers[in->source0],
                                 &f32_registers[in->destination]))) {
                return RIN_GPU_ERROR_BACKEND;
            }
            complement = 1.0f - f32_registers[in->source0] *
                f32_registers[in->source0];
            if (!sw_f32_finite(complement) || complement < 0.0f ||
                !sw_sqrtf(complement, &denominator)) {
                return RIN_GPU_ERROR_BACKEND;
            }
            if (denominator == 0.0f) {
                /* The analytic derivative diverges at the endpoints. Keep
                 * this otherwise defined scalar result deterministic. */
                f32_register_dx[in->destination] = 0.0f;
                f32_register_dy[in->destination] = 0.0f;
            } else {
                derivative_scale = 1.0f / denominator;
                if (in->opcode == RIN_SHADER_OP_ACOS_F32)
                    derivative_scale = -derivative_scale;
                f32_register_dx[in->destination] =
                    derivative_scale * f32_register_dx[in->source0];
                f32_register_dy[in->destination] =
                    derivative_scale * f32_register_dy[in->source0];
                if (!sw_f32_finite(f32_register_dx[in->destination]) ||
                    !sw_f32_finite(f32_register_dy[in->destination])) {
                    return RIN_GPU_ERROR_BACKEND;
                }
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_EXP2_F32: {
            const float ln2 = 0.69314718055994530942f;
            float derivative_scale;

            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_exp2f(f32_registers[in->source0],
                          &f32_registers[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            derivative_scale = ln2 * f32_registers[in->destination];
            f32_register_dx[in->destination] =
                derivative_scale * f32_register_dx[in->source0];
            f32_register_dy[in->destination] =
                derivative_scale * f32_register_dy[in->source0];
            if (!sw_f32_finite(derivative_scale) ||
                !sw_f32_finite(f32_register_dx[in->destination]) ||
                !sw_f32_finite(f32_register_dy[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_LOG2_F32: {
            const float ln2 = 0.69314718055994530942f;
            float denominator;
            float derivative_scale;

            if (in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_log2f(f32_registers[in->source0],
                          &f32_registers[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            denominator = f32_registers[in->source0] * ln2;
            if (!sw_f32_finite(denominator) || denominator == 0.0f)
                return RIN_GPU_ERROR_BACKEND;
            derivative_scale = 1.0f / denominator;
            f32_register_dx[in->destination] =
                derivative_scale * f32_register_dx[in->source0];
            f32_register_dy[in->destination] =
                derivative_scale * f32_register_dy[in->source0];
            if (!sw_f32_finite(derivative_scale) ||
                !sw_f32_finite(f32_register_dx[in->destination]) ||
                !sw_f32_finite(f32_register_dy[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_POW_F32: {
            const float ln2 = 0.69314718055994530942f;
            float base_log2;
            float base_scale;
            float exponent_scale;

            if (in->flags != 0u || in->resource != RIN_SHADER_UNUSED ||
                in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                register_types[in->source1] != SW_SHADER_VALUE_F32 ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_registers[in->source1]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source1]) ||
                !sw_f32_finite(f32_register_dy[in->source1]) ||
                !sw_powf(f32_registers[in->source0],
                         f32_registers[in->source1],
                         &f32_registers[in->destination]) ||
                !sw_log2f(f32_registers[in->source0], &base_log2)) {
                return RIN_GPU_ERROR_BACKEND;
            }
            base_scale = f32_registers[in->source1] *
                f32_registers[in->destination] / f32_registers[in->source0];
            exponent_scale = f32_registers[in->destination] * base_log2 * ln2;
            f32_register_dx[in->destination] =
                base_scale * f32_register_dx[in->source0] +
                exponent_scale * f32_register_dx[in->source1];
            f32_register_dy[in->destination] =
                base_scale * f32_register_dy[in->source0] +
                exponent_scale * f32_register_dy[in->source1];
            if (!sw_f32_finite(base_scale) || !sw_f32_finite(exponent_scale) ||
                !sw_f32_finite(f32_register_dx[in->destination]) ||
                !sw_f32_finite(f32_register_dy[in->destination])) {
                return RIN_GPU_ERROR_BACKEND;
            }
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        }
        case RIN_SHADER_OP_DFDX_F32:
        case RIN_SHADER_OP_DFDY_F32:
        case RIN_SHADER_OP_FWIDTH_F32:
            if (header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                in->flags != 0u || in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                io->f32_input_dx == NULL || io->f32_input_dy == NULL ||
                !sw_f32_finite(f32_registers[in->source0]) ||
                !sw_f32_finite(f32_register_dx[in->source0]) ||
                !sw_f32_finite(f32_register_dy[in->source0])) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
            if (in->opcode == RIN_SHADER_OP_DFDX_F32)
                f32_registers[in->destination] =
                    f32_register_dx[in->source0];
            else if (in->opcode == RIN_SHADER_OP_DFDY_F32)
                f32_registers[in->destination] =
                    f32_register_dy[in->source0];
            else
                f32_registers[in->destination] =
                    sw_absf(f32_register_dx[in->source0]) +
                    sw_absf(f32_register_dy[in->source0]);
            if (!sw_f32_finite(f32_registers[in->destination]))
                return RIN_GPU_ERROR_BACKEND;
            /* A first-order raster derivative has no further gradient in
             * this scalar execution profile. */
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_I32_TO_F32:
            if (register_types[in->source0] != SW_SHADER_VALUE_I32)
                return RIN_GPU_ERROR_BACKEND;
            f32_registers[in->destination] = (float)i32_registers[in->source0];
            f32_register_dx[in->destination] = 0.0f;
            f32_register_dy[in->destination] = 0.0f;
            register_types[in->destination] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_F32_TO_I32:
            if (register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                !(f32_registers[in->source0] >= (float)INT32_MIN) ||
                !(f32_registers[in->source0] < -(float)INT32_MIN))
                return RIN_GPU_ERROR_BACKEND;
            i32_registers[in->destination] = (int32_t)f32_registers[in->source0];
            register_types[in->destination] = SW_SHADER_VALUE_I32;
            break;
        case RIN_SHADER_OP_STORE_OUTPUT_F32:
            if (register_types[in->source0] != SW_SHADER_VALUE_F32 ||
                in->immediate >= io->output_count)
                return RIN_GPU_ERROR_BOUNDS;
            io->f32_outputs[in->immediate] = f32_registers[in->source0];
            io->output_types[in->immediate] = SW_SHADER_VALUE_F32;
            break;
        case RIN_SHADER_OP_STORE_OUTPUT:
            if (register_types[in->source0] != SW_SHADER_VALUE_I32 ||
                in->immediate >= io->output_count)
                return RIN_GPU_ERROR_BOUNDS;
            io->i32_outputs[in->immediate] = i32_registers[in->source0];
            io->output_types[in->immediate] = SW_SHADER_VALUE_I32;
            break;
        case RIN_SHADER_OP_DISCARD:
            if (header->stage != RIN_SHADER_STAGE_FRAGMENT ||
                in->flags != 0u || in->destination != RIN_SHADER_UNUSED ||
                in->source0 != RIN_SHADER_UNUSED ||
                in->source1 != RIN_SHADER_UNUSED ||
                in->resource != RIN_SHADER_UNUSED || in->immediate != 0u ||
                io->discarded == NULL) {
                return RIN_GPU_ERROR_BACKEND;
            }
            *io->discarded = 1;
            return RIN_GPU_OK;
        case RIN_SHADER_OP_RETURN:
            for (uint32_t output = 0u; output < header->output_count;
                 ++output) {
                if (io->output_types[output] == SW_SHADER_VALUE_NONE)
                    return RIN_GPU_ERROR_BACKEND;
            }
            return RIN_GPU_OK;
        default:
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
    }
    return RIN_GPU_ERROR_BACKEND;
}

static uint32_t sw_vertex_format_bytes(uint32_t format)
{
    switch (format) {
    case RIN_GPU_VERTEX_UINT8:
    case RIN_GPU_VERTEX_SINT8:
    case RIN_GPU_VERTEX_UNORM8:
    case RIN_GPU_VERTEX_SNORM8:
        return 1u;
    case RIN_GPU_VERTEX_UINT16:
    case RIN_GPU_VERTEX_SINT16:
    case RIN_GPU_VERTEX_UNORM16:
    case RIN_GPU_VERTEX_SNORM16:
        return 2u;
    case RIN_GPU_VERTEX_UINT32:
    case RIN_GPU_VERTEX_SINT32:
    case RIN_GPU_VERTEX_FLOAT32:
        return 4u;
    default:
        return 0u;
    }
}

/* RinGPU's vertex ABI deliberately converts every scalar numeric format to
 * Float32 before the RSH1 vertex program runs.  Use memcpy for multi-byte
 * formats: WebGL buffers may be merely component-aligned, not naturally
 * aligned for the host C type. */
static int sw_decode_vertex_component(uint32_t format, const uint8_t* source,
                                      float* value)
{
    int8_t signed8;
    uint16_t unsigned16;
    int16_t signed16;
    uint32_t unsigned32;
    int32_t signed32;

    if (!source || !value)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    switch (format) {
    case RIN_GPU_VERTEX_UINT8:
        *value = (float)source[0];
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_SINT8:
        memcpy(&signed8, source, sizeof(signed8));
        *value = (float)signed8;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_UNORM8:
        *value = (float)source[0] / 255.0f;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_SNORM8:
        if (source[0] == UINT8_C(0x80)) {
            *value = -1.0f;
            return RIN_GPU_OK;
        }
        memcpy(&signed8, source, sizeof(signed8));
        *value = (float)signed8 / 127.0f;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_UINT16:
        memcpy(&unsigned16, source, sizeof(unsigned16));
        *value = (float)unsigned16;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_SINT16:
        memcpy(&signed16, source, sizeof(signed16));
        *value = (float)signed16;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_UNORM16:
        memcpy(&unsigned16, source, sizeof(unsigned16));
        *value = (float)unsigned16 / 65535.0f;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_SNORM16:
        memcpy(&signed16, source, sizeof(signed16));
        *value = signed16 == INT16_MIN ? -1.0f : (float)signed16 / 32767.0f;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_UINT32:
        memcpy(&unsigned32, source, sizeof(unsigned32));
        *value = (float)unsigned32;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_SINT32:
        memcpy(&signed32, source, sizeof(signed32));
        *value = (float)signed32;
        return RIN_GPU_OK;
    case RIN_GPU_VERTEX_FLOAT32:
        memcpy(value, source, sizeof(*value));
        return RIN_GPU_OK;
    default:
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
}

static int sw_load_vertex_inputs(const SwPipeline* pipeline,
                                 const SwVertexBindings* bindings,
                                 uint32_t vertex_index,
                                 uint32_t instance_index,
                                 uint32_t first_instance,
                                 float* inputs)
{
    uint32_t index;
    if (!pipeline || !bindings || !inputs ||
        bindings->binding_count > RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(inputs, 0, SW_MAX_IO * sizeof(float));
    for (index = 0u; index < pipeline->desc.vertex_input_count; ++index) {
        const RinGpuBackendVertexAttributeV1* attribute =
            &pipeline->desc.vertex_attributes[index];
        const SwBuffer* buffer;
        uint32_t component_bytes;
        uint32_t stride;
        uint64_t element_index;
        uint64_t vertex_base;
        uint64_t offset;
        int result;

        if (attribute->location >= SW_MAX_IO)
            return RIN_GPU_ERROR_UNSUPPORTED;
        if (attribute->flags == RIN_GPU_VERTEX_ATTRIBUTE_CONSTANT_FLOAT32) {
            if (attribute->format != RIN_GPU_VERTEX_FLOAT32)
                return RIN_GPU_ERROR_UNSUPPORTED;
            memcpy(&inputs[attribute->location], &attribute->offset,
                   sizeof(inputs[attribute->location]));
            continue;
        }
        if (attribute->flags != 0u ||
            attribute->binding >= bindings->binding_count)
            return RIN_GPU_ERROR_UNSUPPORTED;
        component_bytes = sw_vertex_format_bytes(attribute->format);
        if (component_bytes == 0u)
            return RIN_GPU_ERROR_UNSUPPORTED;
        if (pipeline->desc.vertex_binding_count == 0u) {
            if (bindings->binding_count != 1u || attribute->binding != 0u ||
                pipeline->desc.vertex_stride == 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            stride = pipeline->desc.vertex_stride;
        } else {
            if (pipeline->desc.vertex_binding_count != bindings->binding_count ||
                pipeline->desc.vertex_bindings[attribute->binding].binding !=
                    attribute->binding ||
                pipeline->desc.vertex_bindings[attribute->binding].stride == 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            stride =
                pipeline->desc.vertex_bindings[attribute->binding].stride;
        }
        if (pipeline->desc.vertex_binding_count != 0u &&
            pipeline->desc.vertex_bindings[attribute->binding].flags != 0u) {
            uint32_t divisor =
                pipeline->desc.vertex_bindings[attribute->binding].flags;
            element_index = ((uint64_t)first_instance + instance_index) /
                divisor;
        } else {
            element_index = vertex_index;
        }
        buffer = bindings->buffers[attribute->binding];
        if (!buffer || !sw_multiply_u64(element_index, stride, &vertex_base) ||
            !sw_add_u64(bindings->offsets[attribute->binding], vertex_base,
                        &vertex_base) ||
            vertex_base > buffer->size_bytes) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        if (!sw_add_u64(vertex_base, attribute->offset, &offset) ||
            offset > buffer->size_bytes ||
            component_bytes > buffer->size_bytes - offset)
            return RIN_GPU_ERROR_BOUNDS;
        result = sw_decode_vertex_component(attribute->format,
                                            buffer->bytes + offset,
                                            &inputs[attribute->location]);
        if (result != RIN_GPU_OK) return result;
    }
    return RIN_GPU_OK;
}

static int sw_f32_finite(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

/* The portable backend owns byte-exact binary16 storage. Keep conversion
 * memcpy-based so unaligned upload/readback buffers never rely on host
 * aliasing or a hosted floating-point library. Finite binary32 stores
 * saturate to the largest finite binary16, rather than manufacturing an
 * infinity in an otherwise successful render pass. */
static float sw_f16_to_f32(uint16_t half)
{
    uint32_t sign = ((uint32_t)half & 0x8000u) << 16u;
    uint32_t exponent = ((uint32_t)half >> 10u) & 0x1fu;
    uint32_t mantissa = (uint32_t)half & 0x03ffu;
    uint32_t bits;
    float value;

    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            int32_t unbiased_exponent = -14;

            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1u;
                --unbiased_exponent;
            }
            bits = sign | ((uint32_t)(unbiased_exponent + 127) << 23u) |
                ((mantissa & 0x03ffu) << 13u);
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint16_t sw_f32_to_f16(float value)
{
    uint32_t bits;
    uint32_t sign;
    uint32_t exponent;
    uint32_t mantissa;
    int32_t half_exponent;

    memcpy(&bits, &value, sizeof(bits));
    sign = (bits >> 16u) & 0x8000u;
    exponent = (bits >> 23u) & 0xffu;
    mantissa = bits & 0x007fffffu;
    if (exponent == 0xffu)
        return (uint16_t)(sign | 0x7bffu);
    half_exponent = (int32_t)exponent - 127 + 15;
    if (half_exponent >= 31)
        return (uint16_t)(sign | 0x7bffu);
    if (half_exponent <= 0) {
        uint32_t shifted;
        uint32_t round_bit;

        if (half_exponent < -10)
            return (uint16_t)sign;
        mantissa |= 0x00800000u;
        shifted = mantissa >> (uint32_t)(14 - half_exponent);
        round_bit = UINT32_C(1) << (uint32_t)(13 - half_exponent);
        if ((mantissa & round_bit) != 0u &&
            ((mantissa & (round_bit - 1u)) != 0u || (shifted & 1u) != 0u)) {
            ++shifted;
        }
        return (uint16_t)(sign | shifted);
    }
    mantissa += 0x00001000u;
    if ((mantissa & 0x00800000u) != 0u) {
        mantissa = 0u;
        ++half_exponent;
        if (half_exponent >= 31)
            return (uint16_t)(sign | 0x7bffu);
    }
    return (uint16_t)(sign | ((uint32_t)half_exponent << 10u) |
                      (mantissa >> 13u));
}

static uint8_t sw_color_byte(float value)
{
    if (!(value >= 0.0f)) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return (uint8_t)(value * 255.0f + 0.5f);
}

/* sRGB conversion is kept in the freestanding software profile.  The
 * bounded shader power helper is deterministic and already rejects non-finite
 * inputs; valid normalized color values never take its unsupported branch. */
static int sw_srgb_decode(float encoded, float* linear_out)
{
    float power_input;

    if (!linear_out || !sw_f32_finite(encoded) || encoded < 0.0f ||
        encoded > 1.0f)
        return 0;
    if (encoded <= 0.04045f) {
        *linear_out = encoded / 12.92f;
        return sw_f32_finite(*linear_out);
    }
    power_input = (encoded + 0.055f) / 1.055f;
    if (!sw_powf(power_input, 2.4f, linear_out))
        return 0;
    return *linear_out >= 0.0f && *linear_out <= 1.0f;
}

static int sw_srgb_encode(float linear, float* encoded_out)
{
    float power;

    if (!encoded_out || !sw_f32_finite(linear))
        return 0;
    if (linear <= 0.0f) {
        *encoded_out = 0.0f;
        return 1;
    }
    if (linear >= 1.0f) {
        *encoded_out = 1.0f;
        return 1;
    }
    if (linear <= 0.0031308f) {
        *encoded_out = linear * 12.92f;
        return sw_f32_finite(*encoded_out);
    }
    if (!sw_powf(linear, 1.0f / 2.4f, &power))
        return 0;
    *encoded_out = 1.055f * power - 0.055f;
    return sw_f32_finite(*encoded_out) && *encoded_out >= 0.0f &&
           *encoded_out <= 1.0f;
}

static int sw_packed_color_format(uint32_t format)
{
    return format == RIN_GPU_FORMAT_RGB565_UNORM ||
           format == RIN_GPU_FORMAT_RGBA4_UNORM ||
           format == RIN_GPU_FORMAT_RGB5_A1_UNORM;
}

static uint16_t sw_pack_packed_color(uint32_t format, uint8_t red,
                                     uint8_t green, uint8_t blue,
                                     uint8_t alpha)
{
    if (format == RIN_GPU_FORMAT_RGB565_UNORM) {
        return (uint16_t)((((uint16_t)red * 31u + 127u) / 255u) << 11u |
                          (((uint16_t)green * 63u + 127u) / 255u) << 5u |
                          ((uint16_t)blue * 31u + 127u) / 255u);
    }
    if (format == RIN_GPU_FORMAT_RGBA4_UNORM) {
        return (uint16_t)(
            (((uint16_t)red * 15u + 127u) / 255u) << 12u |
            (((uint16_t)green * 15u + 127u) / 255u) << 8u |
            (((uint16_t)blue * 15u + 127u) / 255u) << 4u |
            ((uint16_t)alpha * 15u + 127u) / 255u);
    }
    return (uint16_t)(
        (((uint16_t)red * 31u + 127u) / 255u) << 11u |
        (((uint16_t)green * 31u + 127u) / 255u) << 6u |
        (((uint16_t)blue * 31u + 127u) / 255u) << 1u |
        ((uint16_t)alpha + 127u) / 255u);
}

static void sw_unpack_packed_color(uint32_t format, const uint8_t* pixel,
                                   float color[4])
{
    uint16_t packed;

    memcpy(&packed, pixel, sizeof(packed));
    if (format == RIN_GPU_FORMAT_RGB565_UNORM) {
        color[0] = (float)((packed >> 11u) & 0x1fu) / 31.0f;
        color[1] = (float)((packed >> 5u) & 0x3fu) / 63.0f;
        color[2] = (float)(packed & 0x1fu) / 31.0f;
        color[3] = 1.0f;
        return;
    }
    if (format == RIN_GPU_FORMAT_RGBA4_UNORM) {
        color[0] = (float)((packed >> 12u) & 0xfu) / 15.0f;
        color[1] = (float)((packed >> 8u) & 0xfu) / 15.0f;
        color[2] = (float)((packed >> 4u) & 0xfu) / 15.0f;
        color[3] = (float)(packed & 0xfu) / 15.0f;
        return;
    }
    color[0] = (float)((packed >> 11u) & 0x1fu) / 31.0f;
    color[1] = (float)((packed >> 6u) & 0x1fu) / 31.0f;
    color[2] = (float)((packed >> 1u) & 0x1fu) / 31.0f;
    color[3] = (packed & 1u) != 0u ? 1.0f : 0.0f;
}

static int sw_color_target_valid(const SwImage* image)
{
    uint32_t bytes_per_pixel;
    uint64_t row_bytes;
    uint64_t row_pitch;
    uint64_t trailing_rows;
    uint64_t required_bytes;

    if (!image || !image->bytes ||
        image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        image->desc.width == 0u || image->desc.height == 0u ||
        image->desc.depth != 1u || image->desc.mip_levels != 1u ||
        image->desc.array_layers != 1u || image->desc.sample_count != 1u ||
        (image->desc.format != RIN_GPU_FORMAT_RGB565_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA4_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGB5_A1_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA8_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_BGRA8_UNORM &&
         image->desc.format != RIN_GPU_FORMAT_RGBA8_SRGB &&
         image->desc.format != RIN_GPU_FORMAT_BGRA8_SRGB &&
        image->desc.format != RIN_GPU_FORMAT_RGBA16_FLOAT &&
        image->desc.format != RIN_GPU_FORMAT_RGBA32_FLOAT) ||
        (bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format)) == 0u ||
        !sw_multiply_u64(image->desc.width, bytes_per_pixel, &row_bytes) ||
        !sw_image_row_pitch(image, bytes_per_pixel, &row_pitch) ||
        !sw_multiply_u64((uint64_t)image->desc.height - 1u, row_pitch,
                         &trailing_rows) ||
        !sw_add_u64(trailing_rows, row_bytes, &required_bytes) ||
        required_bytes > image->size_bytes) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    return RIN_GPU_OK;
}

static void sw_default_raster_state(const SwImage* image,
                                    SwRasterState* raster)
{
    if (!image || !raster)
        return;
    memset(raster, 0, sizeof(*raster));
    raster->viewport_width = (float)image->desc.width;
    raster->viewport_height = (float)image->desc.height;
    raster->max_depth = 1.0f;
    raster->line_width = 1.0f;
    raster->sample_coverage_value = 1.0f;
}

static int sw_make_implicit_render_pass(SwImage* color, SwRenderPass* pass)
{
    uint64_t row_bytes;
    uint64_t row_pitch;
    uint64_t trailing_rows;
    uint64_t required_bytes;

    if (!pass) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    /* Direct draw helpers predate the full image descriptor checks and are
     * intentionally retained for focused executor tests.  Command submission
     * still goes through sw_color_target_valid() before it can create a pass. */
    if (!color || !color->bytes || color->desc.width == 0u ||
        color->desc.height == 0u ||
        (color->desc.format != RIN_GPU_FORMAT_RGB565_UNORM &&
         color->desc.format != RIN_GPU_FORMAT_RGBA4_UNORM &&
         color->desc.format != RIN_GPU_FORMAT_RGB5_A1_UNORM &&
         color->desc.format != RIN_GPU_FORMAT_RGBA8_UNORM &&
         color->desc.format != RIN_GPU_FORMAT_BGRA8_UNORM &&
         color->desc.format != RIN_GPU_FORMAT_RGBA8_SRGB &&
         color->desc.format != RIN_GPU_FORMAT_BGRA8_SRGB &&
         color->desc.format != RIN_GPU_FORMAT_RGBA16_FLOAT &&
         color->desc.format != RIN_GPU_FORMAT_RGBA32_FLOAT) ||
        !sw_multiply_u64(color->desc.width,
                         sw_image_bytes_per_pixel(color->desc.format),
                         &row_bytes) ||
        !sw_image_row_pitch(color,
                            sw_image_bytes_per_pixel(color->desc.format),
                            &row_pitch) ||
        !sw_multiply_u64((uint64_t)color->desc.height - 1u, row_pitch,
                         &trailing_rows) ||
        !sw_add_u64(trailing_rows, row_bytes, &required_bytes) ||
        color->size_bytes < required_bytes) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    memset(pass, 0, sizeof(*pass));
    pass->color_views[0] = *color;
    pass->color = &pass->color_views[0];
    pass->color_targets[0] = pass->color;
    pass->color_cookie = (uint64_t)(uintptr_t)color;
    pass->color_target_cookies[0] = pass->color_cookie;
    pass->active_color_mask = 1u;
    sw_default_raster_state(pass->color, &pass->raster);
    return RIN_GPU_OK;
}

static int sw_raster_state_valid(const SwRasterState* raster,
                                 const SwImage* color)
{
    if (!raster || !color || !sw_f32_finite(raster->viewport_x) ||
        !sw_f32_finite(raster->viewport_y) ||
        !sw_f32_finite(raster->viewport_width) ||
        !sw_f32_finite(raster->viewport_height) ||
        !sw_f32_finite(raster->min_depth) ||
        !sw_f32_finite(raster->max_depth) ||
        color->desc.width > INT32_MAX || color->desc.height > INT32_MAX ||
        raster->viewport_width <= 0.0f || raster->viewport_height <= 0.0f ||
        raster->min_depth < 0.0f || raster->min_depth > 1.0f ||
        raster->max_depth < 0.0f || raster->max_depth > 1.0f ||
        raster->min_depth > raster->max_depth ||
        raster->scissor_enabled > 1u ||
        raster->polygon_offset_fill_enabled > 1u ||
        raster->sample_coverage_enabled > 1u ||
        raster->sample_coverage_invert > 1u ||
        raster->dither_enabled > 1u ||
        !sw_f32_finite(raster->polygon_offset_factor) ||
        !sw_f32_finite(raster->polygon_offset_units) ||
        !sw_f32_finite(raster->line_width) ||
        raster->line_width < 1.0f || raster->line_width > 64.0f ||
        !sw_f32_finite(raster->sample_coverage_value) ||
        raster->sample_coverage_value < 0.0f ||
        raster->sample_coverage_value > 1.0f ||
        (raster->scissor_enabled != 0u &&
         (raster->scissor_x < 0 || raster->scissor_y < 0))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (raster->scissor_enabled == 0u &&
        (raster->scissor_x != 0 || raster->scissor_y != 0 ||
         raster->scissor_width != 0u || raster->scissor_height != 0u)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    return RIN_GPU_OK;
}

static int sw_raster_state_from_command(const RinGpuBackendRasterStateV1* source,
                                        const SwImage* color,
                                        SwRasterState* destination)
{
    if (!source || !destination || source->reserved != 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    destination->viewport_x = source->viewport_x;
    destination->viewport_y = source->viewport_y;
    destination->viewport_width = source->viewport_width;
    destination->viewport_height = source->viewport_height;
    destination->min_depth = source->min_depth;
    destination->max_depth = source->max_depth;
    destination->scissor_x = source->scissor_x;
    destination->scissor_y = source->scissor_y;
    destination->scissor_width = source->scissor_width;
    destination->scissor_height = source->scissor_height;
    destination->scissor_enabled = source->scissor_enabled;
    destination->polygon_offset_fill_enabled =
        source->polygon_offset_fill_enabled;
    destination->polygon_offset_factor = source->polygon_offset_factor;
    destination->polygon_offset_units = source->polygon_offset_units;
    destination->line_width = source->line_width;
    destination->sample_coverage_enabled = source->sample_coverage_enabled;
    destination->sample_coverage_value = source->sample_coverage_value;
    destination->sample_coverage_invert = source->sample_coverage_invert;
    destination->dither_enabled = source->dither_enabled;
    return sw_raster_state_valid(destination, color);
}

/* The public clear region uses a lower-left origin while the software image
 * allocation is row-major from the top.  Validate the complete rectangle
 * before publishing any component, including the canonical disabled form. */
static int sw_color_clear_bounds(const SwImage* image,
                                 const RinGpuClearRegionV1* region,
                                 uint32_t* x_out, uint32_t* y_out,
                                 uint32_t* width_out, uint32_t* height_out)
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    if (!image || !region || !x_out || !y_out || !width_out || !height_out ||
        region->enabled > 1u || region->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (region->enabled == 0u) {
        if (region->x != 0 || region->y != 0 || region->width != 0u ||
            region->height != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        *x_out = 0u;
        *y_out = 0u;
        *width_out = image->desc.width;
        *height_out = image->desc.height;
        return RIN_GPU_OK;
    }
    if (region->x < 0 || region->y < 0) return RIN_GPU_ERROR_BOUNDS;
    x = (uint32_t)region->x;
    y = (uint32_t)region->y;
    width = region->width;
    height = region->height;
    if (x > image->desc.width || width > image->desc.width - x ||
        y > image->desc.height || height > image->desc.height - y) {
        return RIN_GPU_ERROR_BOUNDS;
    }

    *x_out = x;
    *y_out = image->desc.height - (y + height);
    *width_out = width;
    *height_out = height;
    return RIN_GPU_OK;
}

static void sw_store_color_components(SwImage* image, uint32_t x, uint32_t y,
                                       const float color[4], uint32_t write_mask)
{
    uint32_t bytes_per_pixel;
    uint64_t offset;
    uint8_t* pixel;

    if (!image || !color || x >= image->desc.width || y >= image->desc.height)
        return;
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u) return;
    if (!sw_image_pixel_offset(image, x, y, bytes_per_pixel, &offset))
        return;
    pixel = image->bytes + offset;
    if (image->desc.format == RIN_GPU_FORMAT_R8_UNORM) {
        if ((write_mask & RIN_GPU_COLOR_WRITE_RED) != 0u)
            pixel[0] = sw_color_byte(color[0]);
        return;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA32_FLOAT) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            if ((write_mask & (RIN_GPU_COLOR_WRITE_RED << component)) != 0u) {
                memcpy(pixel + (uint64_t)component * sizeof(float),
                       &color[component], sizeof(float));
            }
        }
        return;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA16_FLOAT) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            uint16_t component_bits;

            if ((write_mask & (RIN_GPU_COLOR_WRITE_RED << component)) == 0u)
                continue;
            component_bits = sw_f32_to_f16(color[component]);
            memcpy(pixel + (uint64_t)component * sizeof(component_bits),
                   &component_bits, sizeof(component_bits));
        }
        return;
    }
    if (sw_packed_color_format(image->desc.format)) {
        float stored[4];
        uint16_t packed;

        sw_unpack_packed_color(image->desc.format, pixel, stored);
        if ((write_mask & RIN_GPU_COLOR_WRITE_RED) != 0u)
            stored[0] = color[0];
        if ((write_mask & RIN_GPU_COLOR_WRITE_GREEN) != 0u)
            stored[1] = color[1];
        if ((write_mask & RIN_GPU_COLOR_WRITE_BLUE) != 0u)
            stored[2] = color[2];
        /* RGB565 has no stored alpha plane. Its synthetic alpha remains one
         * regardless of the mask, as required for color read/modify/write. */
        if (image->desc.format != RIN_GPU_FORMAT_RGB565_UNORM &&
            (write_mask & RIN_GPU_COLOR_WRITE_ALPHA) != 0u) {
            stored[3] = color[3];
        }
        packed = sw_pack_packed_color(image->desc.format,
                                      sw_color_byte(stored[0]),
                                      sw_color_byte(stored[1]),
                                      sw_color_byte(stored[2]),
                                      sw_color_byte(stored[3]));
        memcpy(pixel, &packed, sizeof(packed));
        return;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA8_SRGB ||
        image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB) {
        float encoded[3];

        if (!sw_srgb_encode(color[0], &encoded[0]) ||
            !sw_srgb_encode(color[1], &encoded[1]) ||
            !sw_srgb_encode(color[2], &encoded[2]))
            return;
        if (image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB) {
            if ((write_mask & RIN_GPU_COLOR_WRITE_BLUE) != 0u)
                pixel[0] = sw_color_byte(encoded[2]);
            if ((write_mask & RIN_GPU_COLOR_WRITE_GREEN) != 0u)
                pixel[1] = sw_color_byte(encoded[1]);
            if ((write_mask & RIN_GPU_COLOR_WRITE_RED) != 0u)
                pixel[2] = sw_color_byte(encoded[0]);
        } else {
            if ((write_mask & RIN_GPU_COLOR_WRITE_RED) != 0u)
                pixel[0] = sw_color_byte(encoded[0]);
            if ((write_mask & RIN_GPU_COLOR_WRITE_GREEN) != 0u)
                pixel[1] = sw_color_byte(encoded[1]);
            if ((write_mask & RIN_GPU_COLOR_WRITE_BLUE) != 0u)
                pixel[2] = sw_color_byte(encoded[2]);
        }
        if ((write_mask & RIN_GPU_COLOR_WRITE_ALPHA) != 0u)
            pixel[3] = sw_color_byte(color[3]);
        return;
    }
    if (image->desc.format == RIN_GPU_FORMAT_BGRA8_UNORM) {
        if ((write_mask & RIN_GPU_COLOR_WRITE_BLUE) != 0u)
            pixel[0] = sw_color_byte(color[2]);
        if ((write_mask & RIN_GPU_COLOR_WRITE_GREEN) != 0u)
            pixel[1] = sw_color_byte(color[1]);
        if ((write_mask & RIN_GPU_COLOR_WRITE_RED) != 0u)
            pixel[2] = sw_color_byte(color[0]);
        if ((write_mask & RIN_GPU_COLOR_WRITE_ALPHA) != 0u)
            pixel[3] = sw_color_byte(color[3]);
    } else {
        if ((write_mask & RIN_GPU_COLOR_WRITE_RED) != 0u)
            pixel[0] = sw_color_byte(color[0]);
        if ((write_mask & RIN_GPU_COLOR_WRITE_GREEN) != 0u)
            pixel[1] = sw_color_byte(color[1]);
        if ((write_mask & RIN_GPU_COLOR_WRITE_BLUE) != 0u)
            pixel[2] = sw_color_byte(color[2]);
        if ((write_mask & RIN_GPU_COLOR_WRITE_ALPHA) != 0u)
            pixel[3] = sw_color_byte(color[3]);
    }
}

static int sw_load_color_components(const SwImage* image, uint32_t x,
                                    uint32_t y, float color[4])
{
    uint32_t bytes_per_pixel;
    uint64_t offset;
    const uint8_t* pixel;

    if (!image || !color || x >= image->desc.width || y >= image->desc.height)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u ||
        !sw_image_pixel_offset(image, x, y, bytes_per_pixel, &offset)) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    pixel = image->bytes + offset;
    color[0] = 0.0f;
    color[1] = 0.0f;
    color[2] = 0.0f;
    color[3] = 1.0f;
    if (image->desc.format == RIN_GPU_FORMAT_R8_UNORM) {
        color[0] = (float)pixel[0] / 255.0f;
        return RIN_GPU_OK;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA32_FLOAT) {
        memcpy(color, pixel, sizeof(float) * 4u);
        return sw_f32_finite(color[0]) && sw_f32_finite(color[1]) &&
                sw_f32_finite(color[2]) && sw_f32_finite(color[3])
            ? RIN_GPU_OK : RIN_GPU_ERROR_BACKEND;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA16_FLOAT) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            uint16_t bits;

            memcpy(&bits, pixel + component * sizeof(bits), sizeof(bits));
            color[component] = sw_f16_to_f32(bits);
        }
        return RIN_GPU_OK;
    }
    if (sw_packed_color_format(image->desc.format)) {
        sw_unpack_packed_color(image->desc.format, pixel, color);
        return RIN_GPU_OK;
    }
    if (image->desc.format == RIN_GPU_FORMAT_BGRA8_UNORM ||
        image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB) {
        color[0] = (float)pixel[2] / 255.0f;
        color[1] = (float)pixel[1] / 255.0f;
        color[2] = (float)pixel[0] / 255.0f;
        color[3] = (float)pixel[3] / 255.0f;
        if (image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB &&
            (!sw_srgb_decode(color[0], &color[0]) ||
             !sw_srgb_decode(color[1], &color[1]) ||
             !sw_srgb_decode(color[2], &color[2])))
            return RIN_GPU_ERROR_BACKEND;
        return RIN_GPU_OK;
    }
    if (image->desc.format == RIN_GPU_FORMAT_RGBA8_UNORM ||
        image->desc.format == RIN_GPU_FORMAT_RGBA8_SRGB ||
        image->desc.format == RIN_GPU_FORMAT_BC1_RGBA_UNORM) {
        for (uint32_t component = 0u; component < 4u; ++component)
            color[component] = (float)pixel[component] / 255.0f;
        if (image->desc.format == RIN_GPU_FORMAT_RGBA8_SRGB &&
            (!sw_srgb_decode(color[0], &color[0]) ||
             !sw_srgb_decode(color[1], &color[1]) ||
             !sw_srgb_decode(color[2], &color[2])))
            return RIN_GPU_ERROR_BACKEND;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_UNSUPPORTED;
}

static int sw_blit_image(const RinGpuBackendImageBlitV1* blit)
{
    SwImage* destination;
    SwImage* source;
    SwImage destination_view;
    SwImage source_view;
    int result;

    if (!blit || blit->blit.flags != 0u || blit->blit.reserved != 0u ||
        blit->blit.source_width == 0u || blit->blit.source_height == 0u ||
        blit->blit.destination_width == 0u ||
        blit->blit.destination_height == 0u ||
        blit->blit.filter > RIN_GPU_IMAGE_BLIT_LINEAR) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination = (SwImage*)(uintptr_t)blit->destination_cookie;
    source = (SwImage*)(uintptr_t)blit->source_cookie;
    if (!destination || !source)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    destination_view = *destination;
    source_view = *source;
    result = sw_image_select_subresource(
        &destination_view, blit->blit.destination_mip_level,
        blit->blit.destination_array_layer);
    if (result != RIN_GPU_OK) return result;
    result = sw_image_select_subresource(
        &source_view, blit->blit.source_mip_level,
        blit->blit.source_array_layer);
    if (result != RIN_GPU_OK) return result;
    if (destination_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        source_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        destination_view.desc.depth != 1u || source_view.desc.depth != 1u ||
        destination_view.desc.sample_count != 1u ||
        source_view.desc.sample_count != 1u ||
        destination_view.desc.format == RIN_GPU_FORMAT_BC1_RGBA_UNORM ||
        !ringpu_color_format(destination_view.desc.format) ||
        !ringpu_color_format(source_view.desc.format) ||
        sw_image_bytes_per_pixel(destination_view.desc.format) == 0u ||
        !destination_view.bytes || !source_view.bytes ||
        blit->blit.source_x > source_view.desc.width ||
        blit->blit.source_width >
            source_view.desc.width - blit->blit.source_x ||
        blit->blit.source_y > source_view.desc.height ||
        blit->blit.source_height >
            source_view.desc.height - blit->blit.source_y ||
        blit->blit.destination_x > destination_view.desc.width ||
        blit->blit.destination_width >
            destination_view.desc.width - blit->blit.destination_x ||
        blit->blit.destination_y > destination_view.desc.height ||
        blit->blit.destination_height >
            destination_view.desc.height - blit->blit.destination_y) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    for (uint32_t y = 0u; y < blit->blit.destination_height; ++y) {
        for (uint32_t x = 0u; x < blit->blit.destination_width; ++x) {
            float color[4];
            uint32_t source_x0;
            uint32_t source_x1;
            uint32_t source_y0;
            uint32_t source_y1;
            float x_fraction = 0.0f;
            float y_fraction = 0.0f;

            if (blit->blit.filter == RIN_GPU_IMAGE_BLIT_NEAREST) {
                source_x0 = blit->blit.source_x +
                    (uint32_t)(((uint64_t)x * blit->blit.source_width) /
                               blit->blit.destination_width);
                source_y0 = blit->blit.source_y +
                    (uint32_t)(((uint64_t)y * blit->blit.source_height) /
                               blit->blit.destination_height);
                source_x1 = source_x0;
                source_y1 = source_y0;
            } else {
                float source_x;
                float source_y;
                float source_x_floor;
                float source_y_floor;

                source_x = ((float)x + 0.5f) *
                    (float)blit->blit.source_width /
                    (float)blit->blit.destination_width - 0.5f;
                source_y = ((float)y + 0.5f) *
                    (float)blit->blit.source_height /
                    (float)blit->blit.destination_height - 0.5f;
                if (!sw_f32_finite(source_x) || !sw_f32_finite(source_y) ||
                    sw_floorf(source_x, &source_x_floor) == 0 ||
                    sw_floorf(source_y, &source_y_floor) == 0) {
                    return RIN_GPU_ERROR_BACKEND;
                }
                source_x0 = source_x_floor < 0.0f ? 0u :
                    source_x_floor >= (float)(blit->blit.source_width - 1u)
                        ? blit->blit.source_width - 1u
                        : (uint32_t)source_x_floor;
                source_y0 = source_y_floor < 0.0f ? 0u :
                    source_y_floor >= (float)(blit->blit.source_height - 1u)
                        ? blit->blit.source_height - 1u
                        : (uint32_t)source_y_floor;
                source_x1 = source_x0 + 1u < blit->blit.source_width
                    ? source_x0 + 1u : source_x0;
                source_y1 = source_y0 + 1u < blit->blit.source_height
                    ? source_y0 + 1u : source_y0;
                x_fraction = source_x - source_x_floor;
                y_fraction = source_y - source_y_floor;
                if (x_fraction < 0.0f) x_fraction = 0.0f;
                if (x_fraction > 1.0f) x_fraction = 1.0f;
                if (y_fraction < 0.0f) y_fraction = 0.0f;
                if (y_fraction > 1.0f) y_fraction = 1.0f;
                source_x0 += blit->blit.source_x;
                source_x1 += blit->blit.source_x;
                source_y0 += blit->blit.source_y;
                source_y1 += blit->blit.source_y;
            }
            if (blit->blit.filter == RIN_GPU_IMAGE_BLIT_NEAREST) {
                source_x0 = blit->blit.source_x +
                    (uint32_t)(((uint64_t)x * blit->blit.source_width) /
                               blit->blit.destination_width);
                source_y0 = blit->blit.source_y +
                    (uint32_t)(((uint64_t)y * blit->blit.source_height) /
                               blit->blit.destination_height);
            }
            if (blit->blit.filter == RIN_GPU_IMAGE_BLIT_NEAREST) {
                result = sw_load_color_components(&source_view, source_x0,
                                                  source_y0, color);
            } else {
                float top[4];
                float bottom[4];

                result = sw_load_color_components(&source_view, source_x0,
                                                  source_y0, top);
                if (result == RIN_GPU_OK)
                    result = sw_load_color_components(&source_view, source_x1,
                                                      source_y0, bottom);
                if (result != RIN_GPU_OK) return result;
                for (uint32_t component = 0u; component < 4u; ++component)
                    color[component] = top[component] +
                        (bottom[component] - top[component]) * x_fraction;
                result = sw_load_color_components(&source_view, source_x0,
                                                  source_y1, top);
                if (result == RIN_GPU_OK)
                    result = sw_load_color_components(&source_view, source_x1,
                                                      source_y1, bottom);
                if (result != RIN_GPU_OK) return result;
                for (uint32_t component = 0u; component < 4u; ++component) {
                    float lower = top[component] +
                        (bottom[component] - top[component]) * x_fraction;
                    color[component] += (lower - color[component]) * y_fraction;
                }
            }
            if (result != RIN_GPU_OK) return result;
            sw_store_color_components(
                &destination_view,
                blit->blit.destination_x + x,
                blit->blit.destination_y + y, color,
                RIN_GPU_COLOR_WRITE_ALL);
        }
    }
    return RIN_GPU_OK;
}

static int sw_resolve_image(const RinGpuBackendImageResolveV1* resolve)
{
    SwImage* destination;
    SwImage* source;
    SwImage destination_view;
    SwImage source_view;
    uint32_t sample_count;
    int result;

    if (!resolve || resolve->resolve.flags != 0u ||
        resolve->resolve.reserved != 0u || resolve->resolve.width == 0u ||
        resolve->resolve.height == 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    destination = (SwImage*)(uintptr_t)resolve->destination_cookie;
    source = (SwImage*)(uintptr_t)resolve->source_cookie;
    if (!destination || !source) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    destination_view = *destination;
    source_view = *source;
    result = sw_image_select_subresource(
        &destination_view, resolve->resolve.destination_mip_level,
        resolve->resolve.destination_array_layer);
    if (result != RIN_GPU_OK) return result;
    result = sw_image_select_subresource(
        &source_view, resolve->resolve.source_mip_level,
        resolve->resolve.source_array_layer);
    if (result != RIN_GPU_OK) return result;
    sample_count = source_view.desc.sample_count;
    if (destination_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        source_view.desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        destination_view.desc.depth != 1u || source_view.desc.depth != 1u ||
        destination_view.desc.sample_count != 1u || sample_count <= 1u ||
        destination_view.desc.format != source_view.desc.format ||
        !ringpu_color_format(destination_view.desc.format) ||
        sw_image_bytes_per_pixel(destination_view.desc.format) == 0u ||
        !destination_view.bytes || !source_view.bytes ||
        resolve->resolve.source_x > source_view.desc.width ||
        resolve->resolve.width >
            source_view.desc.width - resolve->resolve.source_x ||
        resolve->resolve.source_y > source_view.desc.height ||
        resolve->resolve.height >
            source_view.desc.height - resolve->resolve.source_y ||
        resolve->resolve.destination_x > destination_view.desc.width ||
        resolve->resolve.width >
            destination_view.desc.width - resolve->resolve.destination_x ||
        resolve->resolve.destination_y > destination_view.desc.height ||
        resolve->resolve.height >
            destination_view.desc.height - resolve->resolve.destination_y) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    for (uint32_t y = 0u; y < resolve->resolve.height; ++y) {
        for (uint32_t x = 0u; x < resolve->resolve.width; ++x) {
            float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            for (uint32_t sample = 0u; sample < sample_count; ++sample) {
                SwImage sample_view;
                float sample_color[4];

                result = sw_image_sample_view(&source_view, sample,
                                              &sample_view);
                if (result != RIN_GPU_OK) return result;
                result = sw_load_color_components(
                    &sample_view, resolve->resolve.source_x + x,
                    resolve->resolve.source_y + y, sample_color);
                if (result != RIN_GPU_OK) return result;
                for (uint32_t component = 0u; component < 4u; ++component)
                    color[component] += sample_color[component];
            }
            for (uint32_t component = 0u; component < 4u; ++component) {
                color[component] /= (float)sample_count;
                if (!sw_f32_finite(color[component]))
                    return RIN_GPU_ERROR_BACKEND;
            }
            sw_store_color_components(
                &destination_view, resolve->resolve.destination_x + x,
                resolve->resolve.destination_y + y, color,
                RIN_GPU_COLOR_WRITE_ALL);
        }
    }
    return RIN_GPU_OK;
}

/* Ordered 4x4 dither is applied after blending/write-mask resolution, only
 * while quantizing a fragment to a packed normalized color target. Clears do
 * not use this helper: GLES explicitly excludes clear operations from
 * dithering. Image rows are top-down, so convert to the public lower-left
 * origin before selecting the threshold. */
static float sw_dither_component(float value, uint32_t maximum, uint32_t x,
                                 uint32_t top_down_y, uint32_t height)
{
    static const uint8_t thresholds[16] = {
        0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u,
        3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u,
    };
    uint32_t y = height - 1u - top_down_y;
    float offset = ((float)thresholds[((y & 3u) << 2u) | (x & 3u)] + 0.5f) /
        16.0f - 0.5f;

    value += offset / (float)maximum;
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

static void sw_dither_packed_color(const SwImage* image, const SwRasterState* raster,
                                   uint32_t x, uint32_t y, float color[4])
{
    uint32_t red_maximum;
    uint32_t green_maximum;
    uint32_t blue_maximum;

    if (!image || !raster || raster->dither_enabled == 0u ||
        !sw_packed_color_format(image->desc.format)) {
        return;
    }
    red_maximum = 31u;
    green_maximum = image->desc.format == RIN_GPU_FORMAT_RGB565_UNORM ? 63u : 31u;
    blue_maximum = 31u;
    color[0] = sw_dither_component(color[0], red_maximum, x, y,
                                   image->desc.height);
    color[1] = sw_dither_component(color[1], green_maximum, x, y,
                                   image->desc.height);
    color[2] = sw_dither_component(color[2], blue_maximum, x, y,
                                   image->desc.height);
}

static int sw_validate_color_render_pass(
    const RinGpuBackendRenderPassBeginV1* pass, SwImage* image_out,
    uint32_t* x_out, uint32_t* y_out, uint32_t* width_out,
    uint32_t* height_out)
{
    SwImage* source_image;
    SwImage image;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    int result;

    if (!pass || !image_out || !x_out || !y_out || !width_out ||
        !height_out || pass->color_target_cookie == 0u ||
        pass->store_op != RIN_GPU_RENDER_STORE ||
        (pass->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    source_image = (SwImage*)(uintptr_t)pass->color_target_cookie;
    image = *source_image;
    result = sw_image_select_subresource(&image, pass->mip_level,
                                         pass->array_layer);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_color_target_valid(&image);
    if (result != RIN_GPU_OK) return result;
    if (pass->load_op == RIN_GPU_RENDER_LOAD) {
        if (pass->clear_red != 0.0f || pass->clear_green != 0.0f ||
            pass->clear_blue != 0.0f || pass->clear_alpha != 0.0f ||
            pass->color_write_mask != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        result = sw_color_clear_bounds(&image, &pass->clear_region, &x, &y,
                                       &width, &height);
        if (result != RIN_GPU_OK) return result;
        *image_out = image;
        *x_out = x;
        *y_out = y;
        *width_out = width;
        *height_out = height;
        return RIN_GPU_OK;
    }
    if (pass->load_op != RIN_GPU_RENDER_CLEAR ||
        !sw_f32_finite(pass->clear_red) ||
        !sw_f32_finite(pass->clear_green) ||
        !sw_f32_finite(pass->clear_blue) ||
        !sw_f32_finite(pass->clear_alpha) ||
        (image.desc.format != RIN_GPU_FORMAT_RGBA16_FLOAT &&
         image.desc.format != RIN_GPU_FORMAT_RGBA32_FLOAT &&
         (pass->clear_red < 0.0f || pass->clear_red > 1.0f ||
          pass->clear_green < 0.0f || pass->clear_green > 1.0f ||
          pass->clear_blue < 0.0f || pass->clear_blue > 1.0f ||
          pass->clear_alpha < 0.0f || pass->clear_alpha > 1.0f))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_color_clear_bounds(&image, &pass->clear_region, &x, &y,
                                   &width, &height);
    if (result != RIN_GPU_OK) return result;
    *image_out = image;
    *x_out = x;
    *y_out = y;
    *width_out = width;
    *height_out = height;
    return RIN_GPU_OK;
}

static void sw_apply_color_clear(const RinGpuBackendRenderPassBeginV1* pass,
                                 SwImage* image, uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height)
{
    const float clear_color[4] = {
        pass->clear_red, pass->clear_green, pass->clear_blue,
        pass->clear_alpha,
    };

    if (!pass || !image || pass->load_op != RIN_GPU_RENDER_CLEAR ||
        width == 0u || height == 0u || pass->color_write_mask == 0u)
        return;
    for (uint32_t row = y; row < y + height; ++row) {
        for (uint32_t column = x; column < x + width; ++column) {
            sw_store_color_components(image, column, row, clear_color,
                                      pass->color_write_mask);
        }
    }
}

static int sw_begin_render_pass_active(
    const RinGpuBackendRenderPassBeginV1* pass, SwRenderPass* active)
{
    SwImage image;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    int result;

    if (active == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = sw_validate_color_render_pass(pass, &image, &x, &y, &width,
                                           &height);
    if (result != RIN_GPU_OK)
        return result;
    sw_apply_color_clear(pass, &image, x, y, width, height);
    sw_initialize_render_pass(active, &image, NULL, NULL);
    active->color_cookie = pass->color_target_cookie;
    active->color_target_cookies[0] = pass->color_target_cookie;
    active->color_mip_level = pass->mip_level;
    active->color_array_layer = pass->array_layer;
    return RIN_GPU_OK;
}

static int sw_depth_target_valid(const SwImage* image)
{
    uint64_t bytes_per_pixel;
    uint64_t row_bytes;
    uint64_t row_pitch;
    uint64_t trailing_rows;
    uint64_t required_bytes;

    if (!image ||
        image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        image->desc.width == 0u || image->desc.height == 0u ||
        image->desc.depth != 1u || image->desc.array_layers != 1u ||
        image->desc.mip_levels != 1u || image->desc.sample_count != 1u ||
        (image->desc.format != RIN_GPU_FORMAT_D32_FLOAT &&
         image->desc.format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        image->depth_pixels != NULL) {
        if (!sw_multiply_u64(image->desc.width, sizeof(float), &row_bytes))
            return RIN_GPU_ERROR_BOUNDS;
        return sw_external_plane_valid(
                   image->depth_pixels, image->depth_size_bytes,
                   image->depth_row_pitch_bytes, row_bytes, image->desc.height)
            ? RIN_GPU_OK : RIN_GPU_ERROR_BOUNDS;
    }
    if (!image->bytes)
        return RIN_GPU_ERROR_BOUNDS;
    bytes_per_pixel = image->desc.format == RIN_GPU_FORMAT_D32_FLOAT ? 4u : 8u;
    if (!sw_multiply_u64(image->desc.width, bytes_per_pixel, &row_bytes) ||
        !sw_image_row_pitch(image, bytes_per_pixel, &row_pitch) ||
        !sw_multiply_u64((uint64_t)image->desc.height - 1u, row_pitch,
                         &trailing_rows) ||
        !sw_add_u64(trailing_rows, row_bytes, &required_bytes) ||
        required_bytes > image->size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    return RIN_GPU_OK;
}

static int sw_stencil_target_valid(const SwImage* image)
{
    uint64_t bytes_per_pixel;
    uint64_t row_bytes;
    uint64_t row_pitch;
    uint64_t trailing_rows;
    uint64_t required_bytes;

    if (!image ||
        image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
        image->desc.width == 0u || image->desc.height == 0u ||
        image->desc.depth != 1u || image->desc.array_layers != 1u ||
        image->desc.mip_levels != 1u || image->desc.sample_count != 1u ||
        (image->desc.format != RIN_GPU_FORMAT_S8_UINT &&
         image->desc.format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (image->desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT &&
        image->stencil_pixels != NULL) {
        return sw_external_plane_valid(
                   image->stencil_pixels, image->stencil_size_bytes,
                   image->stencil_row_pitch_bytes, image->desc.width,
                   image->desc.height)
            ? RIN_GPU_OK : RIN_GPU_ERROR_BOUNDS;
    }
    if (!image->bytes)
        return RIN_GPU_ERROR_BOUNDS;
    bytes_per_pixel = image->desc.format == RIN_GPU_FORMAT_S8_UINT ? 1u : 8u;
    if (!sw_multiply_u64(image->desc.width, bytes_per_pixel, &row_bytes) ||
        !sw_image_row_pitch(image, bytes_per_pixel, &row_pitch) ||
        !sw_multiply_u64((uint64_t)image->desc.height - 1u, row_pitch,
                         &trailing_rows) ||
        !sw_add_u64(trailing_rows, row_bytes, &required_bytes) ||
        required_bytes > image->size_bytes) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    return RIN_GPU_OK;
}

static int sw_attachment_dimensions_match(const SwImage* color,
                                          const SwImage* attachment)
{
    return color && attachment && color->desc.width == attachment->desc.width &&
           color->desc.height == attachment->desc.height;
}

static int sw_depth_load_valid(uint32_t load_op, float clear_depth)
{
    if (load_op == RIN_GPU_RENDER_LOAD)
        return clear_depth == 0.0f;
    return load_op == RIN_GPU_RENDER_CLEAR && sw_f32_finite(clear_depth) &&
           clear_depth >= 0.0f && clear_depth <= 1.0f;
}

static int sw_stencil_load_valid(uint32_t load_op, uint32_t store_op,
                                 uint32_t clear_stencil,
                                 uint32_t write_mask)
{
    if (store_op != RIN_GPU_RENDER_STORE || clear_stencil > 0xffu ||
        write_mask > 0xffu) {
        return 0;
    }
    if (load_op == RIN_GPU_RENDER_LOAD)
        return clear_stencil == 0u && write_mask == 0u;
    return load_op == RIN_GPU_RENDER_CLEAR;
}

static int sw_depth_storage(const SwImage* image, uint32_t x, uint32_t y,
                            float* depth_out)
{
    uint8_t* address;

    if (!image || !depth_out || x >= image->desc.width ||
        y >= image->desc.height || sw_depth_target_valid(image) != RIN_GPU_OK)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!sw_depth_pixel_address(image, x, y, &address))
        return RIN_GPU_ERROR_BOUNDS;
    memcpy(depth_out, address, sizeof(*depth_out));
    return sw_f32_finite(*depth_out) && *depth_out >= 0.0f &&
           *depth_out <= 1.0f
        ? RIN_GPU_OK
        : RIN_GPU_ERROR_BACKEND;
}

/* A depth attachment can be populated through image upload before a LOAD
 * pass.  Validate every stored value before any color/depth/stencil clear or
 * draw is published, so a late malformed depth sample cannot partially alter
 * an otherwise atomic command submission. */
static int sw_depth_contents_valid(const SwImage* image)
{
    if (sw_depth_target_valid(image) != RIN_GPU_OK) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t y = 0u; y < image->desc.height; ++y) {
        for (uint32_t x = 0u; x < image->desc.width; ++x) {
        float depth;

            if (sw_depth_storage(image, x, y, &depth) != RIN_GPU_OK ||
                !sw_f32_finite(depth) || depth < 0.0f || depth > 1.0f) {
            return RIN_GPU_ERROR_BACKEND;
            }
        }
    }
    return RIN_GPU_OK;
}

static int sw_store_depth(SwImage* image, uint32_t x, uint32_t y, float depth)
{
    uint8_t* address;

    if (!image || x >= image->desc.width || y >= image->desc.height ||
        !sw_f32_finite(depth) || depth < 0.0f || depth > 1.0f ||
        sw_depth_target_valid(image) != RIN_GPU_OK) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!sw_depth_pixel_address(image, x, y, &address))
        return RIN_GPU_ERROR_BOUNDS;
    memcpy(address, &depth, sizeof(depth));
    return RIN_GPU_OK;
}

static int sw_stencil_storage(const SwImage* image, uint32_t x, uint32_t y,
                              uint8_t* stencil_out)
{
    uint8_t* address;

    if (!image || !stencil_out || x >= image->desc.width ||
        y >= image->desc.height || sw_stencil_target_valid(image) != RIN_GPU_OK)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (!sw_stencil_pixel_address(image, x, y, &address))
        return RIN_GPU_ERROR_BOUNDS;
    *stencil_out = *address;
    return RIN_GPU_OK;
}

static int sw_store_stencil(SwImage* image, uint32_t x, uint32_t y,
                            uint8_t stencil)
{
    uint8_t* address;

    if (!image || x >= image->desc.width || y >= image->desc.height ||
        sw_stencil_target_valid(image) != RIN_GPU_OK) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (!sw_stencil_pixel_address(image, x, y, &address))
        return RIN_GPU_ERROR_BOUNDS;
    *address = stencil;
    return RIN_GPU_OK;
}

static int sw_apply_depth_clear(SwImage* image, uint32_t load_op,
                                float clear_depth, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height)
{
    if (load_op != RIN_GPU_RENDER_CLEAR) return RIN_GPU_OK;
    for (uint32_t row = y; row < y + height; ++row) {
        for (uint32_t column = x; column < x + width; ++column) {
            int result = sw_store_depth(image, column, row, clear_depth);

            if (result != RIN_GPU_OK) return result;
        }
    }
    return RIN_GPU_OK;
}

static int sw_apply_stencil_clear(SwImage* image, uint32_t load_op,
                                  uint32_t clear_stencil,
                                  uint32_t write_mask, uint32_t x,
                                  uint32_t y, uint32_t width, uint32_t height)
{
    if (load_op != RIN_GPU_RENDER_CLEAR) return RIN_GPU_OK;
    for (uint32_t row = y; row < y + height; ++row) {
        for (uint32_t column = x; column < x + width; ++column) {
            uint8_t old_stencil;
            uint8_t new_stencil;
            int result = sw_stencil_storage(image, column, row, &old_stencil);

            if (result != RIN_GPU_OK) return result;
            new_stencil = (uint8_t)((old_stencil & ~(uint8_t)write_mask) |
                ((uint8_t)clear_stencil & (uint8_t)write_mask));
            result = sw_store_stencil(image, column, row, new_stencil);
            if (result != RIN_GPU_OK) return result;
        }
    }
    return RIN_GPU_OK;
}

static void sw_initialize_render_pass(SwRenderPass* destination,
                                      SwImage* color, SwImage* depth,
                                      SwImage* stencil)
{
    memset(destination, 0, sizeof(*destination));
    if (color != NULL) {
        destination->color_views[0] = *color;
        destination->color = &destination->color_views[0];
        destination->color_targets[0] = destination->color;
    }
    if (depth != NULL) {
        destination->depth_view = *depth;
        destination->depth = &destination->depth_view;
    }
    if (stencil != NULL) {
        destination->stencil_view = *stencil;
        destination->stencil = &destination->stencil_view;
    }
    destination->color_cookie = (uint64_t)(uintptr_t)color;
    destination->color_target_cookies[0] = destination->color_cookie;
    destination->depth_cookie = (uint64_t)(uintptr_t)depth;
    destination->stencil_cookie = (uint64_t)(uintptr_t)stencil;
    destination->active_color_mask = 1u;
    sw_default_raster_state(color, &destination->raster);
}

static int sw_begin_render_pass_depth(
    const RinGpuBackendRenderPassDepthBeginV1* pass, SwRenderPass* active)
{
    RinGpuBackendRenderPassBeginV1 color_pass;
    SwImage color;
    SwImage depth;
    SwImage* depth_source;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    int result;

    if (!pass || !active || pass->color_target_cookie == 0u ||
        pass->depth_target_cookie == 0u ||
        pass->color_target_cookie == pass->depth_target_cookie ||
        pass->depth_store_op != RIN_GPU_RENDER_STORE || pass->flags != 0u ||
        pass->reserved0 != 0u || pass->reserved1 != 0u ||
        pass->reserved2 != 0u || pass->reserved3 != 0u ||
        !sw_depth_load_valid(pass->depth_load_op, pass->clear_depth)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(&color_pass, 0, sizeof(color_pass));
    color_pass.color_target_cookie = pass->color_target_cookie;
    color_pass.mip_level = pass->color_mip_level;
    color_pass.array_layer = pass->color_array_layer;
    color_pass.load_op = pass->color_load_op;
    color_pass.store_op = pass->color_store_op;
    color_pass.clear_red = pass->clear_red;
    color_pass.clear_green = pass->clear_green;
    color_pass.clear_blue = pass->clear_blue;
    color_pass.clear_alpha = pass->clear_alpha;
    color_pass.color_write_mask = pass->color_write_mask;
    color_pass.clear_region = pass->clear_region;
    result = sw_validate_color_render_pass(&color_pass, &color, &x, &y,
                                            &width, &height);
    if (result != RIN_GPU_OK) return result;
    depth_source = (SwImage*)(uintptr_t)pass->depth_target_cookie;
    depth = *depth_source;
    result = sw_image_select_subresource(&depth, pass->depth_mip_level,
                                         pass->depth_array_layer);
    if (result != RIN_GPU_OK) return result;
    result = sw_depth_target_valid(&depth);
    if (result != RIN_GPU_OK) return result;
    result = sw_depth_contents_valid(&depth);
    if (result != RIN_GPU_OK) return result;
    if (!sw_attachment_dimensions_match(&color, &depth))
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (depth.desc.format == RIN_GPU_FORMAT_D32_FLOAT) {
        if (pass->stencil_load_op != 0u || pass->stencil_store_op != 0u ||
            pass->clear_stencil != 0u || pass->stencil_write_mask != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else if (!sw_stencil_load_valid(pass->stencil_load_op,
                                      pass->stencil_store_op,
                                      pass->clear_stencil,
                                      pass->stencil_write_mask)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    sw_apply_color_clear(&color_pass, &color, x, y, width, height);
    result = sw_apply_depth_clear(&depth, pass->depth_load_op,
                                  pass->clear_depth, x, y, width, height);
    if (result != RIN_GPU_OK) return result;
    if (depth.desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) {
        result = sw_apply_stencil_clear(&depth, pass->stencil_load_op,
                                        pass->clear_stencil,
                                        pass->stencil_write_mask, x, y,
                                        width, height);
        if (result != RIN_GPU_OK) return result;
    }
    sw_initialize_render_pass(active, &color, &depth,
        depth.desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT ? &depth : NULL);
    active->color_cookie = pass->color_target_cookie;
    active->color_target_cookies[0] = pass->color_target_cookie;
    active->depth_cookie = pass->depth_target_cookie;
    active->stencil_cookie = depth.desc.format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT
        ? pass->depth_target_cookie : 0u;
    active->color_mip_level = pass->color_mip_level;
    active->color_array_layer = pass->color_array_layer;
    return RIN_GPU_OK;
}

static int sw_begin_render_pass_depth_stencil(
    const RinGpuBackendRenderPassDepthStencilBeginV1* pass,
    SwRenderPass* active)
{
    RinGpuBackendRenderPassBeginV1 color_pass;
    SwImage color;
    SwImage depth;
    SwImage stencil;
    SwImage* depth_source;
    SwImage* stencil_source;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    int result;

    if (!pass || !active || pass->color_target_cookie == 0u ||
        pass->depth_target_cookie == 0u || pass->stencil_target_cookie == 0u ||
        pass->color_target_cookie == pass->depth_target_cookie ||
        pass->color_target_cookie == pass->stencil_target_cookie ||
        pass->depth_target_cookie == pass->stencil_target_cookie ||
        pass->depth_store_op != RIN_GPU_RENDER_STORE || pass->flags != 0u ||
        pass->reserved0 != 0u || pass->reserved1 != 0u ||
        pass->reserved2 != 0u || pass->reserved3 != 0u ||
        !sw_depth_load_valid(pass->depth_load_op, pass->clear_depth) ||
        !sw_stencil_load_valid(pass->stencil_load_op, pass->stencil_store_op,
                               pass->clear_stencil, pass->stencil_write_mask)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(&color_pass, 0, sizeof(color_pass));
    color_pass.color_target_cookie = pass->color_target_cookie;
    color_pass.mip_level = pass->color_mip_level;
    color_pass.array_layer = pass->color_array_layer;
    color_pass.load_op = pass->color_load_op;
    color_pass.store_op = pass->color_store_op;
    color_pass.clear_red = pass->clear_red;
    color_pass.clear_green = pass->clear_green;
    color_pass.clear_blue = pass->clear_blue;
    color_pass.clear_alpha = pass->clear_alpha;
    color_pass.color_write_mask = pass->color_write_mask;
    color_pass.clear_region = pass->clear_region;
    result = sw_validate_color_render_pass(&color_pass, &color, &x, &y,
                                            &width, &height);
    if (result != RIN_GPU_OK) return result;
    depth_source = (SwImage*)(uintptr_t)pass->depth_target_cookie;
    stencil_source = (SwImage*)(uintptr_t)pass->stencil_target_cookie;
    depth = *depth_source;
    stencil = *stencil_source;
    result = sw_image_select_subresource(&depth, pass->depth_mip_level,
                                         pass->depth_array_layer);
    if (result != RIN_GPU_OK) return result;
    result = sw_image_select_subresource(&stencil, pass->stencil_mip_level,
                                         pass->stencil_array_layer);
    if (result != RIN_GPU_OK) return result;
    result = sw_depth_target_valid(&depth);
    if (result != RIN_GPU_OK) return result;
    result = sw_depth_contents_valid(&depth);
    if (result != RIN_GPU_OK) return result;
    result = sw_stencil_target_valid(&stencil);
    if (result != RIN_GPU_OK) return result;
    if (!sw_attachment_dimensions_match(&color, &depth) ||
        !sw_attachment_dimensions_match(&color, &stencil)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    sw_apply_color_clear(&color_pass, &color, x, y, width, height);
    result = sw_apply_depth_clear(&depth, pass->depth_load_op,
                                  pass->clear_depth, x, y, width, height);
    if (result != RIN_GPU_OK) return result;
    result = sw_apply_stencil_clear(&stencil, pass->stencil_load_op,
                                    pass->clear_stencil,
                                    pass->stencil_write_mask, x, y, width,
                                    height);
    if (result != RIN_GPU_OK) return result;
    sw_initialize_render_pass(active, &color, &depth, &stencil);
    active->color_cookie = pass->color_target_cookie;
    active->color_target_cookies[0] = pass->color_target_cookie;
    active->depth_cookie = pass->depth_target_cookie;
    active->stencil_cookie = pass->stencil_target_cookie;
    active->color_mip_level = pass->color_mip_level;
    active->color_array_layer = pass->color_array_layer;
    return RIN_GPU_OK;
}

static int sw_begin_render_pass_mrt(
    const RinGpuBackendRenderPassMrtBeginV1* pass, SwRenderPass* active)
{
    RinGpuBackendRenderPassBeginV1 color_pass;
    SwImage colors[RIN_GPU_MAX_COLOR_TARGETS];
    SwImage depth_view;
    SwImage stencil_view;
    SwImage* depth = NULL;
    SwImage* stencil = NULL;
    SwImage* first_color = NULL;
    uint32_t first_color_index = 0u;
    uint32_t first_x = 0u;
    uint32_t first_y = 0u;
    uint32_t first_width = 0u;
    uint32_t first_height = 0u;
    uint32_t color_index;
    int result;

    if (!pass || !active || pass->active_color_mask == 0u ||
        (pass->active_color_mask &
         ~((1u << RIN_GPU_MAX_COLOR_TARGETS) - 1u)) != 0u ||
        pass->color_store_op != RIN_GPU_RENDER_STORE || pass->flags != 0u ||
        pass->reserved0 != 0u || pass->reserved1 != 0u ||
        pass->reserved2 != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;

        if ((pass->active_color_mask & (1u << color_index)) == 0u) {
            if (pass->color_target_cookies[color_index] != 0u ||
                pass->color_mip_levels[color_index] != 0u ||
                pass->color_array_layers[color_index] != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            continue;
        }
        memset(&color_pass, 0, sizeof(color_pass));
        color_pass.color_target_cookie = pass->color_target_cookies[color_index];
        color_pass.mip_level = pass->color_mip_levels[color_index];
        color_pass.array_layer = pass->color_array_layers[color_index];
        color_pass.load_op = pass->color_load_op;
        color_pass.store_op = pass->color_store_op;
        color_pass.clear_red = pass->clear_red;
        color_pass.clear_green = pass->clear_green;
        color_pass.clear_blue = pass->clear_blue;
        color_pass.clear_alpha = pass->clear_alpha;
        color_pass.color_write_mask = pass->color_write_mask;
        color_pass.clear_region = pass->clear_region;
        result = sw_validate_color_render_pass(&color_pass, &colors[color_index],
                                                &x, &y, &width, &height);
        if (result != RIN_GPU_OK)
            return result;
        if (first_color == NULL) {
            first_color = &colors[color_index];
            first_color_index = color_index;
            first_x = x;
            first_y = y;
            first_width = width;
            first_height = height;
        } else if (!sw_attachment_dimensions_match(first_color,
                                                    &colors[color_index]) ||
                   colors[color_index].desc.format != first_color->desc.format) {
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
        for (uint32_t prior = 0u; prior < color_index; ++prior) {
            if ((pass->active_color_mask & (1u << prior)) != 0u &&
                pass->color_target_cookies[prior] ==
                    pass->color_target_cookies[color_index] &&
                pass->color_mip_levels[prior] == pass->color_mip_levels[color_index] &&
                pass->color_array_layers[prior] ==
                    pass->color_array_layers[color_index]) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (first_color == NULL)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;

    if (pass->depth_target_cookie == 0u) {
        if (pass->depth_mip_level != 0u || pass->depth_array_layer != 0u ||
            pass->depth_load_op != 0u || pass->depth_store_op != 0u ||
            pass->clear_depth != 0.0f) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else {
        depth_view = *(SwImage*)(uintptr_t)pass->depth_target_cookie;
        depth = &depth_view;
        if (pass->depth_store_op != RIN_GPU_RENDER_STORE ||
            !sw_depth_load_valid(pass->depth_load_op, pass->clear_depth)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        result = sw_image_select_subresource(depth, pass->depth_mip_level,
                                             pass->depth_array_layer);
        if (result != RIN_GPU_OK)
            return result;
        result = sw_depth_target_valid(depth);
        if (result != RIN_GPU_OK)
            return result;
        result = sw_depth_contents_valid(depth);
        if (result != RIN_GPU_OK)
            return result;
        if (!sw_attachment_dimensions_match(first_color, depth))
            return RIN_GPU_ERROR_UNSUPPORTED;
        if (depth->desc.format == RIN_GPU_FORMAT_S8_UINT &&
            (pass->depth_load_op != RIN_GPU_RENDER_LOAD ||
             pass->clear_depth != 0.0f)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }
    if (pass->stencil_target_cookie == 0u) {
        if (pass->stencil_mip_level != 0u ||
            pass->stencil_array_layer != 0u || pass->stencil_load_op != 0u ||
            pass->stencil_store_op != 0u || pass->clear_stencil != 0u ||
            pass->stencil_write_mask != 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    } else {
        stencil_view = *(SwImage*)(uintptr_t)pass->stencil_target_cookie;
        stencil = &stencil_view;
        if (!sw_stencil_load_valid(pass->stencil_load_op,
                                   pass->stencil_store_op,
                                   pass->clear_stencil,
                                   pass->stencil_write_mask)) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        result = sw_image_select_subresource(stencil, pass->stencil_mip_level,
                                             pass->stencil_array_layer);
        if (result != RIN_GPU_OK)
            return result;
        result = sw_stencil_target_valid(stencil);
        if (result != RIN_GPU_OK)
            return result;
        if (!sw_attachment_dimensions_match(first_color, stencil))
            return RIN_GPU_ERROR_UNSUPPORTED;
        if (pass->stencil_target_cookie == pass->depth_target_cookie &&
            stencil->desc.format !=
                RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
    }

    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        if ((pass->active_color_mask & (1u << color_index)) == 0u)
            continue;
        memset(&color_pass, 0, sizeof(color_pass));
        color_pass.load_op = pass->color_load_op;
        color_pass.clear_red = pass->clear_red;
        color_pass.clear_green = pass->clear_green;
        color_pass.clear_blue = pass->clear_blue;
        color_pass.clear_alpha = pass->clear_alpha;
        color_pass.color_write_mask = pass->color_write_mask;
        sw_apply_color_clear(&color_pass, &colors[color_index], first_x,
                             first_y, first_width, first_height);
    }
    if (depth != NULL) {
        result = sw_apply_depth_clear(depth, pass->depth_load_op,
                                      pass->clear_depth, first_x, first_y,
                                      first_width, first_height);
        if (result != RIN_GPU_OK)
            return result;
    }
    if (stencil != NULL) {
        result = sw_apply_stencil_clear(stencil, pass->stencil_load_op,
                                        pass->clear_stencil,
                                        pass->stencil_write_mask, first_x,
                                        first_y, first_width, first_height);
        if (result != RIN_GPU_OK)
            return result;
    }
    sw_initialize_render_pass(active, first_color, depth, stencil);
    active->active_color_mask = pass->active_color_mask;
    active->color_cookie = pass->color_target_cookies[first_color_index];
    active->depth_cookie = pass->depth_target_cookie;
    active->stencil_cookie = pass->stencil_target_cookie;
    active->color_mip_level = pass->color_mip_levels[first_color_index];
    active->color_array_layer = pass->color_array_layers[first_color_index];
    for (color_index = 0u; color_index < RIN_GPU_MAX_COLOR_TARGETS;
         ++color_index) {
        if ((pass->active_color_mask & (1u << color_index)) != 0u) {
            active->color_views[color_index] = colors[color_index];
            active->color_targets[color_index] =
                &active->color_views[color_index];
        } else {
            active->color_targets[color_index] = NULL;
        }
        active->color_target_cookies[color_index] =
            pass->color_target_cookies[color_index];
    }
    return RIN_GPU_OK;
}

static int sw_blend_state_valid(const SwPipeline* pipeline)
{
    const RinGpuBackendGraphicsPipelineDescV1* desc;

    if (!pipeline) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    desc = &pipeline->desc;
    if (desc->independent_blend_enabled != 0u) {
        if (desc->independent_blend_enabled != 1u ||
            desc->independent_blend_mask !=
                ((UINT32_C(1) << RIN_GPU_MAX_COLOR_TARGETS) - 1u))
            return RIN_GPU_ERROR_UNSUPPORTED;
        for (uint32_t target = 0u; target < RIN_GPU_MAX_COLOR_TARGETS;
             ++target) {
            const RinGpuBlendTargetV1* state = &desc->blend_targets[target];

            if (state->blend_enabled > 1u ||
                (state->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u ||
                state->reserved != 0u)
                return RIN_GPU_ERROR_UNSUPPORTED;
            if (state->blend_enabled == 0u) {
                if (state->source_color_factor != 0u ||
                    state->destination_color_factor != 0u ||
                    state->color_operation != 0u ||
                    state->source_alpha_factor != 0u ||
                    state->destination_alpha_factor != 0u ||
                    state->alpha_operation != 0u)
                    return RIN_GPU_ERROR_UNSUPPORTED;
            } else if (!ringpu_blend_source_factor_v2_valid(
                           state->source_color_factor) ||
                       !ringpu_blend_factor_v2_valid(
                           state->destination_color_factor) ||
                       !ringpu_blend_operation_valid(state->color_operation) ||
                       !ringpu_blend_source_factor_v2_valid(
                           state->source_alpha_factor) ||
                       !ringpu_blend_factor_v2_valid(
                           state->destination_alpha_factor) ||
                       !ringpu_blend_operation_valid(state->alpha_operation)) {
                return RIN_GPU_ERROR_UNSUPPORTED;
            }
        }
        return RIN_GPU_OK;
    }
    if (desc->independent_blend_mask != 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if ((desc->color_write_mask & ~RIN_GPU_COLOR_WRITE_ALL) != 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (desc->blend_enabled == 0u)
        return RIN_GPU_OK;
    if (desc->blend_enabled != 1u ||
        desc->source_color_factor < RIN_GPU_BLEND_ZERO ||
        desc->source_color_factor > RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA ||
        desc->destination_color_factor < RIN_GPU_BLEND_ZERO ||
        desc->destination_color_factor > RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA ||
        desc->destination_color_factor == RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE ||
        desc->source_alpha_factor < RIN_GPU_BLEND_ZERO ||
        desc->source_alpha_factor > RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA ||
        desc->source_alpha_factor == RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE ||
        desc->destination_alpha_factor < RIN_GPU_BLEND_ZERO ||
        desc->destination_alpha_factor > RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA ||
        desc->destination_alpha_factor == RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE ||
        desc->color_operation < RIN_GPU_BLEND_ADD ||
        desc->color_operation > RIN_GPU_BLEND_MAXIMUM ||
        desc->alpha_operation < RIN_GPU_BLEND_ADD ||
        desc->alpha_operation > RIN_GPU_BLEND_MAXIMUM ||
        !sw_f32_finite(desc->blend_constant_red) ||
        !sw_f32_finite(desc->blend_constant_green) ||
        !sw_f32_finite(desc->blend_constant_blue) ||
        !sw_f32_finite(desc->blend_constant_alpha) ||
        (desc->color_format != RIN_GPU_FORMAT_RGBA16_FLOAT &&
         desc->color_format != RIN_GPU_FORMAT_RGBA32_FLOAT &&
         (desc->blend_constant_red < 0.0f || desc->blend_constant_red > 1.0f ||
          desc->blend_constant_green < 0.0f || desc->blend_constant_green > 1.0f ||
          desc->blend_constant_blue < 0.0f || desc->blend_constant_blue > 1.0f ||
          desc->blend_constant_alpha < 0.0f || desc->blend_constant_alpha > 1.0f))) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    return RIN_GPU_OK;
}

static float sw_blend_factor(const SwPipeline* pipeline, uint32_t factor,
                             uint32_t component, const float source[4],
                             const float destination[4])
{
    const RinGpuBackendGraphicsPipelineDescV1* desc = &pipeline->desc;
    const float constants[4] = {
        desc->blend_constant_red, desc->blend_constant_green,
        desc->blend_constant_blue, desc->blend_constant_alpha,
    };

    switch (factor) {
    case RIN_GPU_BLEND_ZERO: return 0.0f;
    case RIN_GPU_BLEND_ONE: return 1.0f;
    case RIN_GPU_BLEND_SOURCE_ALPHA: return source[3];
    case RIN_GPU_BLEND_ONE_MINUS_SOURCE_ALPHA: return 1.0f - source[3];
    case RIN_GPU_BLEND_DESTINATION_ALPHA: return destination[3];
    case RIN_GPU_BLEND_ONE_MINUS_DESTINATION_ALPHA:
        return 1.0f - destination[3];
    case RIN_GPU_BLEND_SOURCE_COLOR: return source[component];
    case RIN_GPU_BLEND_ONE_MINUS_SOURCE_COLOR:
        return 1.0f - source[component];
    case RIN_GPU_BLEND_DESTINATION_COLOR: return destination[component];
    case RIN_GPU_BLEND_ONE_MINUS_DESTINATION_COLOR:
        return 1.0f - destination[component];
    case RIN_GPU_BLEND_SOURCE_ALPHA_SATURATE:
        return component == 3u ? 1.0f :
            (source[3] < 1.0f - destination[3]
                 ? source[3] : 1.0f - destination[3]);
    case RIN_GPU_BLEND_CONSTANT_COLOR: return constants[component];
    case RIN_GPU_BLEND_ONE_MINUS_CONSTANT_COLOR:
        return 1.0f - constants[component];
    case RIN_GPU_BLEND_CONSTANT_ALPHA: return constants[3];
    case RIN_GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA: return 1.0f - constants[3];
    default: return 0.0f;
    }
}

static float sw_blend_apply(uint32_t operation, float source,
                            float destination)
{
    switch (operation) {
    case RIN_GPU_BLEND_ADD: return source + destination;
    case RIN_GPU_BLEND_SUBTRACT: return source - destination;
    case RIN_GPU_BLEND_REVERSE_SUBTRACT: return destination - source;
    case RIN_GPU_BLEND_MINIMUM: return source < destination ? source : destination;
    case RIN_GPU_BLEND_MAXIMUM: return source > destination ? source : destination;
    default: return 0.0f;
    }
}

static int sw_compare_float(uint32_t compare, float source, float target)
{
    switch (compare) {
    case RIN_GPU_COMPARE_LESS: return source < target;
    case RIN_GPU_COMPARE_LESS_EQUAL: return source <= target;
    case RIN_GPU_COMPARE_ALWAYS: return 1;
    case RIN_GPU_COMPARE_NEVER: return 0;
    case RIN_GPU_COMPARE_EQUAL: return source == target;
    case RIN_GPU_COMPARE_GREATER: return source > target;
    case RIN_GPU_COMPARE_NOT_EQUAL: return source != target;
    case RIN_GPU_COMPARE_GREATER_EQUAL: return source >= target;
    default: return 0;
    }
}

static int sw_compare_u8(uint32_t compare, uint8_t source, uint8_t target)
{
    return sw_compare_float(compare, (float)source, (float)target);
}

static int sw_compare_valid(uint32_t compare)
{
    return compare >= RIN_GPU_COMPARE_LESS &&
           compare <= RIN_GPU_COMPARE_GREATER_EQUAL;
}

static int sw_stencil_operation_valid(uint32_t operation)
{
    return operation >= RIN_GPU_STENCIL_KEEP &&
           operation <= RIN_GPU_STENCIL_DECREMENT_WRAP;
}

static int sw_stencil_face_valid(uint32_t compare, uint32_t reference,
                                 uint32_t read_mask, uint32_t write_mask,
                                 uint32_t fail_operation,
                                 uint32_t depth_fail_operation,
                                 uint32_t pass_operation)
{
    return sw_compare_valid(compare) && reference <= 0xffu &&
           read_mask <= 0xffu && write_mask <= 0xffu &&
           sw_stencil_operation_valid(fail_operation) &&
           sw_stencil_operation_valid(depth_fail_operation) &&
           sw_stencil_operation_valid(pass_operation);
}

static int sw_pipeline_depth_stencil_valid(const SwPipeline* pipeline,
                                           const SwRenderPass* pass)
{
    const RinGpuBackendGraphicsPipelineDescV1* desc;
    int depth_enabled;

    if (!pipeline || !pass || !(desc = &pipeline->desc))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    depth_enabled = desc->depth_format == RIN_GPU_FORMAT_D32_FLOAT ||
                    desc->depth_format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
    if (desc->depth_format != 0u && !depth_enabled &&
        desc->depth_format != RIN_GPU_FORMAT_S8_UINT)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (!depth_enabled) {
        if (desc->depth_compare != 0u || desc->depth_write_enabled != 0u)
            return RIN_GPU_ERROR_UNSUPPORTED;
    } else if (!pass->depth ||
               (desc->depth_format != RIN_GPU_FORMAT_D32_FLOAT &&
                desc->depth_format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) ||
               (pass->depth->desc.format != RIN_GPU_FORMAT_D32_FLOAT &&
                pass->depth->desc.format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) ||
               !sw_compare_valid(desc->depth_compare) ||
               desc->depth_write_enabled > 1u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (desc->stencil_test_enabled == 0u) {
        if (desc->stencil_compare != 0u || desc->stencil_reference != 0u ||
            desc->stencil_read_mask != 0u || desc->stencil_write_mask != 0u ||
            desc->stencil_fail_operation != 0u ||
            desc->stencil_depth_fail_operation != 0u ||
            desc->stencil_pass_operation != 0u ||
            desc->separate_stencil_enabled != 0u ||
            desc->back_stencil_compare != 0u ||
            desc->back_stencil_reference != 0u ||
            desc->back_stencil_read_mask != 0u ||
            desc->back_stencil_write_mask != 0u ||
            desc->back_stencil_fail_operation != 0u ||
            desc->back_stencil_depth_fail_operation != 0u ||
            desc->back_stencil_pass_operation != 0u) {
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
    } else if (desc->stencil_test_enabled != 1u || !pass->stencil ||
               (desc->depth_format != RIN_GPU_FORMAT_S8_UINT &&
                desc->depth_format != RIN_GPU_FORMAT_D32_FLOAT_S8_UINT) ||
               !sw_stencil_face_valid(
                   desc->stencil_compare, desc->stencil_reference,
                   desc->stencil_read_mask, desc->stencil_write_mask,
                   desc->stencil_fail_operation,
                   desc->stencil_depth_fail_operation,
                   desc->stencil_pass_operation) ||
               (desc->separate_stencil_enabled == 0u &&
                (desc->back_stencil_compare != 0u ||
                 desc->back_stencil_reference != 0u ||
                 desc->back_stencil_read_mask != 0u ||
                 desc->back_stencil_write_mask != 0u ||
                 desc->back_stencil_fail_operation != 0u ||
                 desc->back_stencil_depth_fail_operation != 0u ||
                 desc->back_stencil_pass_operation != 0u)) ||
               (desc->separate_stencil_enabled == 1u &&
                !sw_stencil_face_valid(
                    desc->back_stencil_compare, desc->back_stencil_reference,
                    desc->back_stencil_read_mask, desc->back_stencil_write_mask,
                    desc->back_stencil_fail_operation,
                    desc->back_stencil_depth_fail_operation,
                    desc->back_stencil_pass_operation)) ||
               desc->separate_stencil_enabled > 1u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (desc->cull_mode != 0u &&
        (desc->cull_mode < RIN_GPU_CULL_NONE ||
         desc->cull_mode > RIN_GPU_CULL_BACK)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (desc->front_face != 0u &&
        (desc->front_face < RIN_GPU_FRONT_FACE_COUNTER_CLOCKWISE ||
         desc->front_face > RIN_GPU_FRONT_FACE_CLOCKWISE)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    return RIN_GPU_OK;
}

static uint8_t sw_apply_stencil_operation(uint32_t operation,
                                          uint8_t current,
                                          uint8_t reference)
{
    switch (operation) {
    case RIN_GPU_STENCIL_KEEP: return current;
    case RIN_GPU_STENCIL_ZERO: return 0u;
    case RIN_GPU_STENCIL_REPLACE: return reference;
    case RIN_GPU_STENCIL_INCREMENT_CLAMP:
        return current == UINT8_MAX ? UINT8_MAX : (uint8_t)(current + 1u);
    case RIN_GPU_STENCIL_DECREMENT_CLAMP:
        return current == 0u ? 0u : (uint8_t)(current - 1u);
    case RIN_GPU_STENCIL_INVERT: return (uint8_t)~current;
    case RIN_GPU_STENCIL_INCREMENT_WRAP: return (uint8_t)(current + 1u);
    case RIN_GPU_STENCIL_DECREMENT_WRAP: return (uint8_t)(current - 1u);
    default: return current;
    }
}

static int sw_write_stencil_operation(SwImage* image, uint32_t x, uint32_t y,
                                      uint32_t operation, uint32_t reference,
                                      uint32_t write_mask)
{
    uint8_t current;
    uint8_t operation_value;
    uint8_t next;
    int result = sw_stencil_storage(image, x, y, &current);

    if (result != RIN_GPU_OK) return result;
    operation_value = sw_apply_stencil_operation(operation, current,
                                                  (uint8_t)reference);
    next = (uint8_t)((current & ~(uint8_t)write_mask) |
        (operation_value & (uint8_t)write_mask));
    return sw_store_stencil(image, x, y, next);
}

static void sw_put_pixel_with_raster(const SwPipeline* pipeline, SwImage* image,
                                     const SwRasterState* raster, uint32_t
                                         color_index, int32_t x, int32_t y,
                                     const float color[4]);

static int sw_publish_fragment(const SwPipeline* pipeline,
                               const SwRenderPass* pass, int32_t x, int32_t y,
                               float depth, int front_facing,
                               const float color[4])
{
    const RinGpuBackendGraphicsPipelineDescV1* desc = &pipeline->desc;
    uint32_t stencil_compare;
    uint32_t stencil_reference;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_fail;
    uint32_t stencil_depth_fail;
    uint32_t stencil_pass;
    int depth_enabled = desc->depth_format == RIN_GPU_FORMAT_D32_FLOAT ||
                        desc->depth_format == RIN_GPU_FORMAT_D32_FLOAT_S8_UINT;
    int stencil_enabled = desc->stencil_test_enabled != 0u;
    int result;

    if (x < 0 || y < 0 || !pass->color ||
        (uint32_t)x >= pass->color->desc.width ||
        (uint32_t)y >= pass->color->desc.height || !sw_f32_finite(depth) ||
        depth < 0.0f || depth > 1.0f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    if (pass->backend != NULL) {
        result = sw_query_add_pipeline(
            pass->backend, RIN_GPU_PIPELINE_STAT_FRAGMENT_SHADER_INVOCATIONS,
            1u);
        if (result != RIN_GPU_OK) return result;
    }
    stencil_compare = desc->stencil_compare;
    stencil_reference = desc->stencil_reference;
    stencil_read_mask = desc->stencil_read_mask;
    stencil_write_mask = desc->stencil_write_mask;
    stencil_fail = desc->stencil_fail_operation;
    stencil_depth_fail = desc->stencil_depth_fail_operation;
    stencil_pass = desc->stencil_pass_operation;
    if (stencil_enabled && !front_facing &&
        desc->separate_stencil_enabled != 0u) {
        stencil_compare = desc->back_stencil_compare;
        stencil_reference = desc->back_stencil_reference;
        stencil_read_mask = desc->back_stencil_read_mask;
        stencil_write_mask = desc->back_stencil_write_mask;
        stencil_fail = desc->back_stencil_fail_operation;
        stencil_depth_fail = desc->back_stencil_depth_fail_operation;
        stencil_pass = desc->back_stencil_pass_operation;
    }
    if (stencil_enabled) {
        uint8_t stencil;

        result = sw_stencil_storage(pass->stencil, (uint32_t)x, (uint32_t)y,
                                    &stencil);
        if (result != RIN_GPU_OK) return result;
        if (!sw_compare_u8(stencil_compare,
                           (uint8_t)(stencil_reference & stencil_read_mask),
                           (uint8_t)(stencil & stencil_read_mask))) {
            return sw_write_stencil_operation(
                pass->stencil, (uint32_t)x, (uint32_t)y, stencil_fail,
                stencil_reference, stencil_write_mask);
        }
    }
    if (depth_enabled) {
        float stored_depth;

        result = sw_depth_storage(pass->depth, (uint32_t)x, (uint32_t)y,
                                  &stored_depth);
        if (result != RIN_GPU_OK) return result;
        if (!sw_compare_float(desc->depth_compare, depth, stored_depth)) {
            if (stencil_enabled) {
                return sw_write_stencil_operation(
                    pass->stencil, (uint32_t)x, (uint32_t)y,
                    stencil_depth_fail, stencil_reference, stencil_write_mask);
            }
            return RIN_GPU_OK;
        }
    }
    if (stencil_enabled) {
        result = sw_write_stencil_operation(pass->stencil, (uint32_t)x,
                                            (uint32_t)y, stencil_pass,
                                            stencil_reference,
                                            stencil_write_mask);
        if (result != RIN_GPU_OK) return result;
    }
    if (depth_enabled && desc->depth_write_enabled != 0u) {
        result = sw_store_depth(pass->depth, (uint32_t)x, (uint32_t)y, depth);
        if (result != RIN_GPU_OK) return result;
    }
    if (pass->backend != NULL) {
        for (uint32_t query_index = 0u;
             query_index < RIN_GPU_CORE_MAX_OBJECTS; ++query_index) {
            SwQueryState* query = &pass->backend->queries[query_index];
            if (query->cookie != 0u && query->active != 0u &&
                query->query_type == RIN_GPU_QUERY_OCCLUSION) {
                result = sw_query_add(query, 1u);
                if (result != RIN_GPU_OK) return result;
            }
        }
    }
    for (uint32_t color_index = 0u;
         color_index < RIN_GPU_MAX_COLOR_TARGETS; ++color_index) {
        if ((pass->active_color_mask & (1u << color_index)) == 0u)
            continue;
        if (pass->color_targets[color_index] == NULL)
            return RIN_GPU_ERROR_BACKEND;
        sw_put_pixel_with_raster(pipeline, pass->color_targets[color_index],
                                 &pass->raster, color_index, x, y,
                                 color + color_index * 4u);
    }
    return RIN_GPU_OK;
}

static void sw_put_pixel_with_raster(const SwPipeline* pipeline, SwImage* image,
                                     const SwRasterState* raster,
                                     uint32_t color_index, int32_t x, int32_t y,
                                     const float color[4])
{
    uint32_t bytes_per_pixel;
    uint64_t offset;
    uint8_t* pixel;
    float destination[4];
    float output[4];
    uint32_t write_mask;
    uint32_t blend_enabled;
    uint32_t source_color_factor;
    uint32_t destination_color_factor;
    uint32_t color_operation;
    uint32_t source_alpha_factor;
    uint32_t destination_alpha_factor;
    uint32_t alpha_operation;
    if (!image || x < 0 || y < 0 || (uint32_t)x >= image->desc.width ||
        (uint32_t)y >= image->desc.height)
        return;
    bytes_per_pixel = sw_image_bytes_per_pixel(image->desc.format);
    if (bytes_per_pixel == 0u)
        return;
    if (!sw_image_pixel_offset(image, (uint32_t)x, (uint32_t)y,
                               bytes_per_pixel, &offset))
        return;
    pixel = image->bytes + offset;
    if (image->desc.format == RIN_GPU_FORMAT_RGBA32_FLOAT) {
        memcpy(destination, pixel, sizeof(destination));
    } else if (image->desc.format == RIN_GPU_FORMAT_RGBA16_FLOAT) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            uint16_t component_bits;

            memcpy(&component_bits,
                   pixel + (uint64_t)component * sizeof(component_bits),
                   sizeof(component_bits));
            destination[component] = sw_f16_to_f32(component_bits);
        }
    } else if (sw_packed_color_format(image->desc.format)) {
        sw_unpack_packed_color(image->desc.format, pixel, destination);
    } else if (image->desc.format == RIN_GPU_FORMAT_BGRA8_UNORM ||
               image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB) {
        destination[0] = (float)pixel[2] / 255.0f;
        destination[1] = (float)pixel[1] / 255.0f;
        destination[2] = (float)pixel[0] / 255.0f;
        destination[3] = (float)pixel[3] / 255.0f;
        if (image->desc.format == RIN_GPU_FORMAT_BGRA8_SRGB &&
            (!sw_srgb_decode(destination[0], &destination[0]) ||
             !sw_srgb_decode(destination[1], &destination[1]) ||
             !sw_srgb_decode(destination[2], &destination[2])))
            return;
    } else if (image->desc.format == RIN_GPU_FORMAT_RGBA8_SRGB) {
        destination[0] = (float)pixel[0] / 255.0f;
        destination[1] = (float)pixel[1] / 255.0f;
        destination[2] = (float)pixel[2] / 255.0f;
        destination[3] = (float)pixel[3] / 255.0f;
        if (!sw_srgb_decode(destination[0], &destination[0]) ||
            !sw_srgb_decode(destination[1], &destination[1]) ||
            !sw_srgb_decode(destination[2], &destination[2]))
            return;
    } else {
        destination[0] = (float)pixel[0] / 255.0f;
        destination[1] = (float)pixel[1] / 255.0f;
        destination[2] = (float)pixel[2] / 255.0f;
        destination[3] = (float)pixel[3] / 255.0f;
    }
    blend_enabled = pipeline->desc.blend_enabled;
    source_color_factor = pipeline->desc.source_color_factor;
    destination_color_factor = pipeline->desc.destination_color_factor;
    color_operation = pipeline->desc.color_operation;
    source_alpha_factor = pipeline->desc.source_alpha_factor;
    destination_alpha_factor = pipeline->desc.destination_alpha_factor;
    alpha_operation = pipeline->desc.alpha_operation;
    write_mask = pipeline->desc.color_write_mask;
    if (pipeline->desc.independent_blend_enabled != 0u) {
        const RinGpuBlendTargetV1* target =
            &pipeline->desc.blend_targets[color_index];

        blend_enabled = target->blend_enabled;
        source_color_factor = target->source_color_factor;
        destination_color_factor = target->destination_color_factor;
        color_operation = target->color_operation;
        source_alpha_factor = target->source_alpha_factor;
        destination_alpha_factor = target->destination_alpha_factor;
        alpha_operation = target->alpha_operation;
        write_mask = target->color_write_mask;
    }
    memcpy(output, color, sizeof(output));
    if (blend_enabled != 0u) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            uint32_t source_factor = component == 3u
                ? source_alpha_factor : source_color_factor;
            uint32_t destination_factor = component == 3u
                ? destination_alpha_factor : destination_color_factor;
            uint32_t operation = component == 3u
                ? alpha_operation : color_operation;
            float source_value = color[component] *
                sw_blend_factor(pipeline, source_factor, component, color,
                                destination);
            float destination_value = destination[component] *
                sw_blend_factor(pipeline, destination_factor, component, color,
                                destination);
            output[component] = operation == RIN_GPU_BLEND_MINIMUM ||
                    operation == RIN_GPU_BLEND_MAXIMUM
                ? sw_blend_apply(operation, color[component],
                                 destination[component])
                : sw_blend_apply(operation, source_value, destination_value);
        }
    }
    /* Zero is a valid pipeline mask: depth/stencil work still executes but
     * no color component may be stored. */
    sw_dither_packed_color(image, raster, (uint32_t)x, (uint32_t)y, output);
    sw_store_color_components(image, (uint32_t)x, (uint32_t)y, output,
                              write_mask);
}

static float sw_edge(float ax, float ay, float bx, float by,
                     float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static int sw_pipeline_shader_io(const SwPipeline* pipeline,
                                 uint32_t* vertex_outputs,
                                 uint32_t* fragment_inputs,
                                 uint32_t* fragment_outputs)
{
    const RinShaderHeaderV1* vertex_header;
    const RinShaderHeaderV1* fragment_header;
    uint32_t fragment_seen = 0u;
    uint32_t color_outputs;

    if (!pipeline || !pipeline->vertex || !pipeline->fragment ||
        !vertex_outputs || !fragment_inputs || !fragment_outputs ||
        !pipeline->vertex->ir || !pipeline->fragment->ir ||
        pipeline->vertex->ir_size < sizeof(*vertex_header) ||
        pipeline->fragment->ir_size < sizeof(*fragment_header)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    vertex_header = (const RinShaderHeaderV1*)pipeline->vertex->ir;
    fragment_header = (const RinShaderHeaderV1*)pipeline->fragment->ir;
    if (vertex_header->stage != RIN_SHADER_STAGE_VERTEX ||
        fragment_header->stage != RIN_SHADER_STAGE_FRAGMENT ||
        (pipeline->desc.flags &
         ~RIN_GPU_GRAPHICS_PIPELINE_NATIVE_KNOWN_FLAGS) != 0u ||
        vertex_header->output_count > SW_MAX_IO ||
        fragment_header->input_count > SW_MAX_IO ||
        fragment_header->output_count == 0u ||
        fragment_header->output_count >
            RIN_GPU_MAX_COLOR_TARGETS * 4u + 1u ||
        pipeline->desc.position_output_location > vertex_header->output_count ||
        4u > vertex_header->output_count -
                 pipeline->desc.position_output_location ||
        pipeline->desc.varying_count != fragment_header->input_count ||
        pipeline->desc.varying_count > RIN_GPU_MAX_VARYINGS) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if ((pipeline->desc.flags &
         RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT) != 0u &&
        5u > vertex_header->output_count -
                 pipeline->desc.position_output_location) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    color_outputs = fragment_header->output_count;
    /* A single terminal scalar is the RSH1 representation of the optional
     * GLSL gl_FragDepthEXT output. It is deliberately not a color target. */
    if ((color_outputs & 3u) == 1u)
        color_outputs--;
    if (color_outputs == 0u || (color_outputs & 3u) != 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    for (uint32_t index = 0u; index < pipeline->desc.varying_count; ++index) {
        const RinGpuBackendVaryingV1* varying =
            &pipeline->desc.varyings[index];
        uint32_t bit;

        if ((varying->type != RIN_GPU_VARYING_FLOAT32 &&
             varying->type != RIN_GPU_VARYING_SINT32) ||
            (varying->interpolation != RIN_GPU_INTERPOLATION_PERSPECTIVE &&
             varying->interpolation != RIN_GPU_INTERPOLATION_NO_PERSPECTIVE &&
             varying->interpolation != RIN_GPU_INTERPOLATION_FLAT) ||
            (varying->type == RIN_GPU_VARYING_SINT32 &&
             varying->interpolation != RIN_GPU_INTERPOLATION_FLAT) ||
            varying->vertex_output_location >= vertex_header->output_count ||
            varying->fragment_input_location >= fragment_header->input_count ||
            (varying->vertex_output_location >=
                 pipeline->desc.position_output_location &&
             varying->vertex_output_location <
                 pipeline->desc.position_output_location + 4u) ||
            ((pipeline->desc.flags &
              RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT) != 0u &&
             varying->vertex_output_location ==
                 pipeline->desc.position_output_location + 4u)) {
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
        bit = UINT32_C(1) << varying->fragment_input_location;
        if ((fragment_seen & bit) != 0u)
            return RIN_GPU_ERROR_BACKEND;
        fragment_seen |= bit;
    }
    if (fragment_seen != (fragment_header->input_count == 32u
                              ? UINT32_MAX
                              : (UINT32_C(1) << fragment_header->input_count) -
                                    1u)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    *vertex_outputs = vertex_header->output_count;
    *fragment_inputs = fragment_header->input_count;
    *fragment_outputs = fragment_header->output_count;
    return RIN_GPU_OK;
}

static int sw_render_pass_fragment_outputs_supported(
    const SwRenderPass* pass, uint32_t fragment_outputs)
{
    uint32_t supported_mask;
    uint32_t color_outputs = fragment_outputs;

    if (pass == NULL || fragment_outputs == 0u ||
        fragment_outputs > RIN_GPU_MAX_COLOR_TARGETS * 4u + 1u ||
        pass->active_color_mask == 0u ||
        (pass->active_color_mask &
         ~((1u << RIN_GPU_MAX_COLOR_TARGETS) - 1u)) != 0u) {
        return RIN_GPU_ERROR_BACKEND;
    }
    if ((color_outputs & 3u) == 1u)
        color_outputs--;
    if (color_outputs == 0u || (color_outputs & 3u) != 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    supported_mask = (1u << (color_outputs / 4u)) - 1u;
    return (pass->active_color_mask & ~supported_mask) == 0u
        ? RIN_GPU_OK : RIN_GPU_ERROR_UNSUPPORTED;
}

static int sw_project_position(const SwRenderPass* pass,
                               const float position[4], float* x_out,
                               float* y_out, float* depth_out)
{
    const SwImage* image;
    float ndc_x;
    float ndc_y;
    float ndc_z;

    if (!pass || !(image = pass->color) || !position || !x_out || !y_out ||
        !depth_out || image->desc.width == 0u ||
        image->desc.height == 0u || image->desc.width > INT32_MAX ||
        image->desc.height > INT32_MAX || !sw_f32_finite(position[0]) ||
        !sw_f32_finite(position[1]) || !sw_f32_finite(position[2]) ||
        !sw_f32_finite(position[3]) || position[3] == 0.0f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    ndc_x = position[0] / position[3];
    ndc_y = position[1] / position[3];
    ndc_z = position[2] / position[3];
    if (!sw_f32_finite(ndc_x) || !sw_f32_finite(ndc_y) ||
        !sw_f32_finite(ndc_z) || ndc_x < -1.0001f || ndc_x > 1.0001f ||
        ndc_y < -1.0001f || ndc_y > 1.0001f || ndc_z < -1.0001f ||
        ndc_z > 1.0001f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    if (ndc_x < -1.0f) ndc_x = -1.0f;
    if (ndc_x > 1.0f) ndc_x = 1.0f;
    if (ndc_y < -1.0f) ndc_y = -1.0f;
    if (ndc_y > 1.0f) ndc_y = 1.0f;
    if (ndc_z < -1.0f) ndc_z = -1.0f;
    if (ndc_z > 1.0f) ndc_z = 1.0f;
    /* Transform clip-space boundaries to pixel boundaries, not to the last
     * pixel centre. A full-screen triangle must cover a 1x1 target and the
     * sample at x + 0.5/y + 0.5 must obey the same rule at every size. */
    *x_out = pass->raster.viewport_x +
        (ndc_x * 0.5f + 0.5f) * pass->raster.viewport_width;
    *y_out = (float)image->desc.height -
        (pass->raster.viewport_y +
         (ndc_y * 0.5f + 0.5f) * pass->raster.viewport_height);
    *depth_out = pass->raster.min_depth +
        (ndc_z * 0.5f + 0.5f) *
            (pass->raster.max_depth - pass->raster.min_depth);
    return sw_f32_finite(*x_out) && sw_f32_finite(*y_out) &&
           sw_f32_finite(*depth_out) && *depth_out >= 0.0f &&
           *depth_out <= 1.0f
        ? RIN_GPU_OK
        : RIN_GPU_ERROR_BACKEND;
}

static int sw_raster_covers_pixel(const SwRenderPass* pass, int32_t x,
                                  int32_t y)
{
    const SwImage* image;
    float lower_left_y;

    if (!pass || !(image = pass->color) || x < 0 || y < 0 ||
        (uint32_t)x >= image->desc.width || (uint32_t)y >= image->desc.height)
        return 0;
    lower_left_y = (float)(image->desc.height - 1u - (uint32_t)y) + 0.5f;
    if ((float)x + 0.5f < pass->raster.viewport_x ||
        (float)x + 0.5f >=
            pass->raster.viewport_x + pass->raster.viewport_width ||
        lower_left_y < pass->raster.viewport_y ||
        lower_left_y >= pass->raster.viewport_y + pass->raster.viewport_height) {
        return 0;
    }
    if (pass->raster.scissor_enabled != 0u &&
        ((float)x + 0.5f < (float)pass->raster.scissor_x ||
         (float)x + 0.5f >=
             (float)pass->raster.scissor_x + pass->raster.scissor_width ||
         lower_left_y < (float)pass->raster.scissor_y ||
         lower_left_y >=
             (float)pass->raster.scissor_y + pass->raster.scissor_height)) {
        return 0;
    }
    if (pass->raster.sample_coverage_enabled != 0u) {
        int covered = pass->raster.sample_coverage_value > 0.0f;

        if (pass->raster.sample_coverage_invert != 0u)
            covered = !covered;
        if (!covered)
            return 0;
    }
    return 1;
}

static int sw_interpolate_fragment_inputs(
    const SwPipeline* pipeline, float vertex_outputs[3][SW_MAX_IO],
    int32_t vertex_i32_outputs[3][SW_MAX_IO],
    uint8_t vertex_output_types[3][SW_MAX_IO], float positions[3][4],
    float barycentric0, float barycentric1, float barycentric2,
    float* f32_inputs, int32_t* i32_inputs, uint8_t* input_types,
    uint32_t input_count)
{
    float inverse_w_sum;

    if (!pipeline || !vertex_outputs || !vertex_i32_outputs ||
        !vertex_output_types || !positions || !f32_inputs || !i32_inputs ||
        !input_types || input_count != pipeline->desc.varying_count)
        return RIN_GPU_ERROR_BACKEND;
    memset(f32_inputs, 0, SW_MAX_IO * sizeof(*f32_inputs));
    memset(i32_inputs, 0, SW_MAX_IO * sizeof(*i32_inputs));
    memset(input_types, SW_SHADER_VALUE_NONE,
           SW_MAX_IO * sizeof(*input_types));
    inverse_w_sum = barycentric0 / positions[0][3] +
        barycentric1 / positions[1][3] + barycentric2 / positions[2][3];
    if (!sw_f32_finite(inverse_w_sum) || inverse_w_sum == 0.0f)
        return RIN_GPU_ERROR_BACKEND;
    for (uint32_t index = 0u; index < pipeline->desc.varying_count; ++index) {
        const RinGpuBackendVaryingV1* varying =
            &pipeline->desc.varyings[index];
        float value;

        if (vertex_output_types[0][varying->vertex_output_location] !=
                (varying->type == RIN_GPU_VARYING_FLOAT32
                     ? SW_SHADER_VALUE_F32
                     : SW_SHADER_VALUE_I32) ||
            vertex_output_types[1][varying->vertex_output_location] !=
                (varying->type == RIN_GPU_VARYING_FLOAT32
                     ? SW_SHADER_VALUE_F32
                     : SW_SHADER_VALUE_I32) ||
            vertex_output_types[2][varying->vertex_output_location] !=
                (varying->type == RIN_GPU_VARYING_FLOAT32
                     ? SW_SHADER_VALUE_F32
                     : SW_SHADER_VALUE_I32)) {
            return RIN_GPU_ERROR_BACKEND;
        }
        if (varying->type == RIN_GPU_VARYING_SINT32) {
            i32_inputs[varying->fragment_input_location] =
                vertex_i32_outputs[0][varying->vertex_output_location];
            input_types[varying->fragment_input_location] =
                SW_SHADER_VALUE_I32;
            continue;
        }

        if (varying->interpolation == RIN_GPU_INTERPOLATION_FLAT) {
            value = vertex_outputs[0][varying->vertex_output_location];
        } else if (varying->interpolation ==
                   RIN_GPU_INTERPOLATION_NO_PERSPECTIVE) {
            value = barycentric0 *
                    vertex_outputs[0][varying->vertex_output_location] +
                barycentric1 *
                    vertex_outputs[1][varying->vertex_output_location] +
                barycentric2 *
                    vertex_outputs[2][varying->vertex_output_location];
        } else {
            value = (barycentric0 *
                         vertex_outputs[0][varying->vertex_output_location] /
                         positions[0][3] +
                     barycentric1 *
                         vertex_outputs[1][varying->vertex_output_location] /
                         positions[1][3] +
                     barycentric2 *
                         vertex_outputs[2][varying->vertex_output_location] /
                         positions[2][3]) /
                inverse_w_sum;
        }
        if (!sw_f32_finite(value))
            return RIN_GPU_ERROR_BACKEND;
        f32_inputs[varying->fragment_input_location] = value;
        input_types[varying->fragment_input_location] = SW_SHADER_VALUE_F32;
    }
    return RIN_GPU_OK;
}

static int sw_raster_triangle(const SwPipeline* pipeline,
                              const SwRenderPass* pass,
                              float vertex_outputs[3][SW_MAX_IO],
                              int32_t vertex_i32_outputs[3][SW_MAX_IO],
                              uint8_t vertex_output_types[3][SW_MAX_IO],
                              const SwGraphicsBindGroup* bind_group,
                              uint32_t fragment_inputs,
                              uint32_t fragment_outputs, int publish,
                              uint64_t* fragment_invocations)
{
    const uint32_t position = pipeline->desc.position_output_location;
    SwImage* image;
    float positions[3][4];
    float depths[3];
    float x0, y0, x1, y1, x2, y2;
    float area;
    uint32_t color_outputs = fragment_outputs;
    int writes_fragment_depth;
    int32_t min_x, max_x, min_y, max_y;
    int32_t x, y;
    int result;

    if (!pipeline || !pass || !(image = pass->color) || !vertex_outputs ||
        !vertex_i32_outputs ||
        !vertex_output_types || !fragment_invocations)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    writes_fragment_depth = (color_outputs & 3u) == 1u;
    if (writes_fragment_depth)
        color_outputs--;
    if (color_outputs == 0u || (color_outputs & 3u) != 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = sw_blend_state_valid(pipeline);
    if (result != RIN_GPU_OK) return result;
    result = sw_pipeline_depth_stencil_valid(pipeline, pass);
    if (result != RIN_GPU_OK) return result;
    result = sw_raster_state_valid(&pass->raster, image);
    if (result != RIN_GPU_OK) return result;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            if (vertex_output_types[vertex][position + component] !=
                SW_SHADER_VALUE_F32) {
                return RIN_GPU_ERROR_BACKEND;
            }
        }
        memcpy(positions[vertex], vertex_outputs[vertex] + position,
               sizeof(positions[vertex]));
    }
    result = sw_project_position(pass, positions[0], &x0, &y0, &depths[0]);
    if (result != RIN_GPU_OK) return result;
    result = sw_project_position(pass, positions[1], &x1, &y1, &depths[1]);
    if (result != RIN_GPU_OK) return result;
    result = sw_project_position(pass, positions[2], &x2, &y2, &depths[2]);
    if (result != RIN_GPU_OK) return result;
    area = sw_edge(x0, y0, x1, y1, x2, y2);
    if (!sw_f32_finite(area)) return RIN_GPU_ERROR_BACKEND;
    if (area == 0.0f) return RIN_GPU_OK;
    {
        int front_facing = area < 0.0f;

        /* The software image has a top-left origin, so a counter-clockwise
         * NDC triangle has a negative signed screen-space area. */
        if (pipeline->desc.front_face == RIN_GPU_FRONT_FACE_CLOCKWISE)
            front_facing = !front_facing;
        if ((pipeline->desc.cull_mode == RIN_GPU_CULL_FRONT && front_facing) ||
            (pipeline->desc.cull_mode == RIN_GPU_CULL_BACK && !front_facing)) {
            return RIN_GPU_OK;
        }
        (void)front_facing;
    }
    {
        float minimum_x = (x0 < x1 ? x0 : x1) < x2
            ? (x0 < x1 ? x0 : x1) : x2;
        float maximum_x = (x0 > x1 ? x0 : x1) > x2
            ? (x0 > x1 ? x0 : x1) : x2;
        float minimum_y = (y0 < y1 ? y0 : y1) < y2
            ? (y0 < y1 ? y0 : y1) : y2;
        float maximum_y = (y0 > y1 ? y0 : y1) > y2
            ? (y0 > y1 ? y0 : y1) : y2;
        float last_x = (float)(image->desc.width - 1u);
        float last_y = (float)(image->desc.height - 1u);

        /* Clip in float space before converting to int32_t.  A public
         * viewport may legally lie far outside its target; casting those
         * values first is undefined and could turn an empty primitive into an
         * out-of-bounds raster loop. */
        if (maximum_x < 0.0f || maximum_y < 0.0f || minimum_x > last_x ||
            minimum_y > last_y) {
            return RIN_GPU_OK;
        }
        min_x = minimum_x <= 0.0f ? 0 : (int32_t)minimum_x;
        min_y = minimum_y <= 0.0f ? 0 : (int32_t)minimum_y;
        max_x = maximum_x >= last_x ? (int32_t)last_x :
            (int32_t)maximum_x + 1;
        max_y = maximum_y >= last_y ? (int32_t)last_y :
            (int32_t)maximum_y + 1;
    }
    for (y = min_y; y <= max_y; ++y) {
        for (x = min_x; x <= max_x; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = sw_edge(x1, y1, x2, y2, px, py);
            float w1 = sw_edge(x2, y2, x0, y0, px, py);
            float w2 = sw_edge(x0, y0, x1, y1, px, py);
            if ((area > 0.0f && w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                (area < 0.0f && w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f)) {
                float f32_inputs[SW_MAX_IO];
                float f32_input_dx[SW_MAX_IO];
                float f32_input_dy[SW_MAX_IO];
                float f32_neighbor[SW_MAX_IO];
                int32_t i32_inputs[SW_MAX_IO];
                int32_t i32_neighbor[SW_MAX_IO];
                uint8_t input_types[SW_MAX_IO];
                uint8_t neighbor_types[SW_MAX_IO];
                float color[SW_MAX_IO];
                int32_t i32_outputs[SW_MAX_IO];
                uint8_t output_types[SW_MAX_IO];
                SwFragmentCoordinate fragment_coordinate;
                SwShaderIo io;
                int discarded;
                float b0 = w0 / area;
                float b1 = w1 / area;
                float b2 = w2 / area;
                float fragment_depth =
                    b0 * depths[0] + b1 * depths[1] + b2 * depths[2];
                float fragment_coordinate_depth = fragment_depth;
                float b0_dx = (y1 - y2) / area;
                float b1_dx = (y2 - y0) / area;
                float b2_dx = (y0 - y1) / area;
                /* Raster storage is top-down, but GLSL dFdy and
                 * gl_FragCoord.y increase toward the lower-left origin. */
                float b0_dy = (x1 - x2) / area;
                float b1_dy = (x2 - x0) / area;
                float b2_dy = (x0 - x1) / area;
                float fragment_depth_dx = b0_dx * depths[0] +
                    b1_dx * depths[1] + b2_dx * depths[2];
                float fragment_depth_dy = b0_dy * depths[0] +
                    b1_dy * depths[1] + b2_dy * depths[2];
                float inverse_w = b0 / positions[0][3] +
                    b1 / positions[1][3] + b2 / positions[2][3];
                float inverse_w_dx = b0_dx / positions[0][3] +
                    b1_dx / positions[1][3] + b2_dx / positions[2][3];
                float inverse_w_dy = b0_dy / positions[0][3] +
                    b1_dy / positions[1][3] + b2_dy / positions[2][3];
                float fragment_coordinate_dx[4] = {
                    1.0f, 0.0f, fragment_depth_dx, inverse_w_dx,
                };
                float fragment_coordinate_dy[4] = {
                    0.0f, 1.0f, fragment_depth_dy, inverse_w_dy,
                };
                float polygon_depth_offset = 0.0f;
                int front_facing = area < 0.0f;

                if (!sw_raster_covers_pixel(pass, x, y))
                    continue;

                if (pipeline->desc.front_face == RIN_GPU_FRONT_FACE_CLOCKWISE)
                    front_facing = !front_facing;
                if (pass->raster.polygon_offset_fill_enabled != 0u) {
                    float slope = sw_absf(fragment_depth_dx) >
                            sw_absf(fragment_depth_dy)
                        ? sw_absf(fragment_depth_dx)
                        : sw_absf(fragment_depth_dy);

                    polygon_depth_offset =
                        slope * pass->raster.polygon_offset_factor +
                        pass->raster.polygon_offset_units / 16777215.0f;
                    fragment_depth += polygon_depth_offset;
                }
                if (!sw_f32_finite(fragment_depth) || fragment_depth < 0.0f ||
                    fragment_depth > 1.0f) {
                    return RIN_GPU_ERROR_BACKEND;
                }

                if (*fragment_invocations >= SW_MAX_FRAGMENT_INVOCATIONS)
                    return RIN_GPU_ERROR_LIMIT;
                (*fragment_invocations)++;
                result = sw_interpolate_fragment_inputs(
                    pipeline, vertex_outputs, vertex_i32_outputs,
                    vertex_output_types, positions, b0, b1, b2, f32_inputs,
                    i32_inputs, input_types, fragment_inputs);
                if (result != RIN_GPU_OK) return result;
                /* The software target is top-down, while the GL fragment
                 * coordinate system is lower-left. Compute dx at +x and dy
                 * at -y so dFdy observes the latter convention. These are
                 * interpolation values, not neighboring coverage tests: a
                 * fragment quad may straddle a primitive edge. */
                memset(f32_input_dx, 0, sizeof(f32_input_dx));
                memset(f32_input_dy, 0, sizeof(f32_input_dy));
                {
                    float dx0 = sw_edge(x1, y1, x2, y2, px + 1.0f, py) / area;
                    float dx1 = sw_edge(x2, y2, x0, y0, px + 1.0f, py) / area;
                    float dx2 = sw_edge(x0, y0, x1, y1, px + 1.0f, py) / area;
                    float dy0 = sw_edge(x1, y1, x2, y2, px, py - 1.0f) / area;
                    float dy1 = sw_edge(x2, y2, x0, y0, px, py - 1.0f) / area;
                    float dy2 = sw_edge(x0, y0, x1, y1, px, py - 1.0f) / area;

                    if (!sw_f32_finite(dx0) || !sw_f32_finite(dx1) ||
                        !sw_f32_finite(dx2) || !sw_f32_finite(dy0) ||
                        !sw_f32_finite(dy1) || !sw_f32_finite(dy2)) {
                        return RIN_GPU_ERROR_BACKEND;
                    }
                    result = sw_interpolate_fragment_inputs(
                        pipeline, vertex_outputs, vertex_i32_outputs,
                        vertex_output_types, positions, dx0, dx1, dx2,
                        f32_neighbor, i32_neighbor, neighbor_types,
                        fragment_inputs);
                    if (result != RIN_GPU_OK)
                        return result;
                    for (uint32_t input = 0u; input < fragment_inputs;
                         ++input) {
                        if (input_types[input] == SW_SHADER_VALUE_F32 &&
                            neighbor_types[input] == SW_SHADER_VALUE_F32) {
                            f32_input_dx[input] = f32_neighbor[input] -
                                f32_inputs[input];
                        }
                    }
                    result = sw_interpolate_fragment_inputs(
                        pipeline, vertex_outputs, vertex_i32_outputs,
                        vertex_output_types, positions, dy0, dy1, dy2,
                        f32_neighbor, i32_neighbor, neighbor_types,
                        fragment_inputs);
                    if (result != RIN_GPU_OK)
                        return result;
                    for (uint32_t input = 0u; input < fragment_inputs;
                         ++input) {
                        if (input_types[input] == SW_SHADER_VALUE_F32 &&
                            neighbor_types[input] == SW_SHADER_VALUE_F32) {
                            f32_input_dy[input] = f32_neighbor[input] -
                                f32_inputs[input];
                        }
                    }
                }
                result = sw_make_fragment_coordinate(
                    pass, x, y, fragment_coordinate_depth, inverse_w,
                    fragment_coordinate_dx, fragment_coordinate_dy,
                    &fragment_coordinate);
                if (result != RIN_GPU_OK)
                    return result;
                memset(&io, 0, sizeof(io));
                io.f32_inputs = f32_inputs;
                io.f32_input_dx = f32_input_dx;
                io.f32_input_dy = f32_input_dy;
                io.i32_inputs = i32_inputs;
                io.input_types = input_types;
                io.input_count = fragment_inputs;
                io.f32_outputs = color;
                io.i32_outputs = i32_outputs;
                io.output_types = output_types;
                io.output_count = SW_MAX_IO;
                io.discarded = &discarded;
                io.graphics_bind_group = bind_group;
                io.push_constants = pass->push_constants;
                io.push_constant_size = pass->push_constant_size;
                io.fragment_coordinate = &fragment_coordinate;
                result = sw_run_shader_typed(pipeline->fragment, &io);
                if (result != RIN_GPU_OK) return result;
                if (discarded)
                    continue;
                for (uint32_t component = 0u; component < fragment_outputs;
                     ++component) {
                    if (output_types[component] != SW_SHADER_VALUE_F32 ||
                        !sw_f32_finite(color[component]))
                        return RIN_GPU_ERROR_BACKEND;
                }
                if (writes_fragment_depth) {
                    fragment_depth = color[color_outputs] + polygon_depth_offset;
                    if (!sw_f32_finite(fragment_depth))
                        return RIN_GPU_ERROR_BACKEND;
                    /* EXT_frag_depth supplies a window-depth value. The GLES
                     * depth-write conversion clamps both fixed and floating
                     * depth storage to this interval. */
                    if (fragment_depth < 0.0f)
                        fragment_depth = 0.0f;
                    else if (fragment_depth > 1.0f)
                        fragment_depth = 1.0f;
                }
                if (publish) {
                    result = sw_publish_fragment(pipeline, pass, x, y,
                                                 fragment_depth, front_facing,
                                                 color);
                    if (result != RIN_GPU_OK) return result;
                }
            }
        }
    }
    return RIN_GPU_OK;
}

static int sw_clip_distance(const SwClipVertex* vertex, uint32_t position,
                            uint32_t plane, float* distance_out)
{
    float x;
    float y;
    float z;
    float w;
    float distance;

    if (!vertex || !distance_out || position > SW_MAX_IO - 4u || plane > 6u ||
        vertex->types[position] != SW_SHADER_VALUE_F32 ||
        vertex->types[position + 1u] != SW_SHADER_VALUE_F32 ||
        vertex->types[position + 2u] != SW_SHADER_VALUE_F32 ||
        vertex->types[position + 3u] != SW_SHADER_VALUE_F32) {
        return RIN_GPU_ERROR_BACKEND;
    }
    x = vertex->f32[position];
    y = vertex->f32[position + 1u];
    z = vertex->f32[position + 2u];
    w = vertex->f32[position + 3u];
    if (!sw_f32_finite(x) || !sw_f32_finite(y) || !sw_f32_finite(z) ||
        !sw_f32_finite(w)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    switch (plane) {
    case 0u: distance = x + w; break;
    case 1u: distance = w - x; break;
    case 2u: distance = y + w; break;
    case 3u: distance = w - y; break;
    case 4u: distance = z + w; break;
    case 5u: distance = w - z; break;
    default: distance = w - SW_CLIP_W_EPSILON; break;
    }
    if (!sw_f32_finite(distance))
        return RIN_GPU_ERROR_BACKEND;
    *distance_out = distance;
    return RIN_GPU_OK;
}

static int sw_clip_intersection(SwClipVertex* output,
                                const SwClipVertex* previous,
                                const SwClipVertex* current,
                                float previous_distance,
                                float current_distance,
                                uint32_t output_count)
{
    float denominator;
    float factor;

    if (!output || !previous || !current || output_count > SW_MAX_IO ||
        !sw_f32_finite(previous_distance) || !sw_f32_finite(current_distance)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    denominator = previous_distance - current_distance;
    if (!sw_f32_finite(denominator) || denominator == 0.0f)
        return RIN_GPU_ERROR_BACKEND;
    factor = previous_distance / denominator;
    if (!sw_f32_finite(factor) || factor < 0.0f || factor > 1.0f)
        return RIN_GPU_ERROR_BACKEND;
    for (uint32_t output_index = 0u; output_index < output_count;
         ++output_index) {
        if (previous->types[output_index] != current->types[output_index])
            return RIN_GPU_ERROR_BACKEND;
        output->types[output_index] = previous->types[output_index];
        if (output->types[output_index] == SW_SHADER_VALUE_F32) {
            float value = previous->f32[output_index] +
                (current->f32[output_index] - previous->f32[output_index]) *
                    factor;

            if (!sw_f32_finite(value))
                return RIN_GPU_ERROR_BACKEND;
            output->f32[output_index] = value;
        } else if (output->types[output_index] == SW_SHADER_VALUE_I32) {
            /* Integer varyings must be flat. The caller normalizes their
             * provoking value before clipping, so an intersection preserves
             * the original triangle's value rather than inventing one. */
            output->i32[output_index] = previous->i32[output_index];
        } else {
            return RIN_GPU_ERROR_BACKEND;
        }
    }
    return RIN_GPU_OK;
}

static int sw_clip_and_raster_triangle(
    const SwPipeline* pipeline, const SwRenderPass* pass,
    float vertex_outputs[3][SW_MAX_IO],
    int32_t vertex_i32_outputs[3][SW_MAX_IO],
    uint8_t vertex_output_types[3][SW_MAX_IO], uint32_t vertex_output_count,
    const SwGraphicsBindGroup* bind_group, uint32_t fragment_inputs,
    uint32_t fragment_outputs, int publish, uint64_t* fragment_invocations)
{
    SwClipVertex vertices[2][SW_MAX_CLIPPED_VERTICES];
    uint32_t source = 0u;
    uint32_t count = 3u;
    const uint32_t position = pipeline ? pipeline->desc.position_output_location :
                                        SW_MAX_IO;

    if (!pipeline || !pass || !vertex_outputs || !vertex_i32_outputs ||
        !vertex_output_types || !fragment_invocations ||
        vertex_output_count == 0u || vertex_output_count > SW_MAX_IO ||
        position > vertex_output_count || 4u > vertex_output_count - position) {
        return RIN_GPU_ERROR_BACKEND;
    }
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        for (uint32_t output_index = 0u; output_index < vertex_output_count;
             ++output_index) {
            if (vertex_output_types[vertex][output_index] !=
                    SW_SHADER_VALUE_F32 &&
                vertex_output_types[vertex][output_index] !=
                    SW_SHADER_VALUE_I32) {
                return RIN_GPU_ERROR_BACKEND;
            }
            vertices[source][vertex].types[output_index] =
                vertex_output_types[vertex][output_index];
            vertices[source][vertex].f32[output_index] =
                vertex_outputs[vertex][output_index];
            vertices[source][vertex].i32[output_index] =
                vertex_i32_outputs[vertex][output_index];
        }
    }
    for (uint32_t output_index = 0u; output_index < vertex_output_count;
         ++output_index) {
        if (vertices[source][0].types[output_index] == SW_SHADER_VALUE_I32) {
            for (uint32_t vertex = 1u; vertex < 3u; ++vertex) {
                if (vertices[source][vertex].types[output_index] !=
                    SW_SHADER_VALUE_I32) {
                    return RIN_GPU_ERROR_BACKEND;
                }
                vertices[source][vertex].i32[output_index] =
                    vertices[source][0].i32[output_index];
            }
        }
    }

    for (uint32_t plane = 0u; plane < 7u; ++plane) {
        uint32_t destination = source ^ 1u;
        uint32_t destination_count = 0u;
        const SwClipVertex* previous = &vertices[source][count - 1u];
        float previous_distance;
        int previous_inside;
        int result = sw_clip_distance(previous, position, plane,
                                      &previous_distance);

        if (result != RIN_GPU_OK)
            return result;
        previous_inside = previous_distance >= 0.0f;
        for (uint32_t current_index = 0u; current_index < count;
             ++current_index) {
            const SwClipVertex* current = &vertices[source][current_index];
            float current_distance;
            int current_inside;

            result = sw_clip_distance(current, position, plane,
                                      &current_distance);
            if (result != RIN_GPU_OK)
                return result;
            current_inside = current_distance >= 0.0f;
            if (current_inside != previous_inside) {
                if (destination_count >= SW_MAX_CLIPPED_VERTICES)
                    return RIN_GPU_ERROR_LIMIT;
                result = sw_clip_intersection(
                    &vertices[destination][destination_count++], previous,
                    current, previous_distance, current_distance,
                    vertex_output_count);
                if (result != RIN_GPU_OK)
                    return result;
            }
            if (current_inside) {
                if (destination_count >= SW_MAX_CLIPPED_VERTICES)
                    return RIN_GPU_ERROR_LIMIT;
                vertices[destination][destination_count++] = *current;
            }
            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }
        count = destination_count;
        if (count < 3u)
            return RIN_GPU_OK;
        source = destination;
    }

    for (uint32_t vertex = 1u; vertex + 1u < count; ++vertex) {
        float triangle_outputs[3][SW_MAX_IO];
        int32_t triangle_i32_outputs[3][SW_MAX_IO];
        uint8_t triangle_output_types[3][SW_MAX_IO];
        const uint32_t triangle_indices[3] = {0u, vertex, vertex + 1u};
        int result;

        for (uint32_t triangle_vertex = 0u; triangle_vertex < 3u;
             ++triangle_vertex) {
            const SwClipVertex* clipped =
                &vertices[source][triangle_indices[triangle_vertex]];

            memcpy(triangle_outputs[triangle_vertex], clipped->f32,
                   sizeof(triangle_outputs[triangle_vertex]));
            memcpy(triangle_i32_outputs[triangle_vertex], clipped->i32,
                   sizeof(triangle_i32_outputs[triangle_vertex]));
            memcpy(triangle_output_types[triangle_vertex], clipped->types,
                   sizeof(triangle_output_types[triangle_vertex]));
        }
        result = sw_raster_triangle(
            pipeline, pass, triangle_outputs, triangle_i32_outputs,
            triangle_output_types, bind_group, fragment_inputs,
            fragment_outputs, publish, fragment_invocations);
        if (result != RIN_GPU_OK)
            return result;
    }
    return RIN_GPU_OK;
}

/* Points and lines use the same fragment executor as triangles, but they do
 * not have a polygon area, winding, or polygon offset. Keeping this fragment
 * path in the generic backend makes the RinGL product surface execute its
 * declared non-triangle topology without falling back to an Aquamarine draw
 * command. */
static int sw_raster_fragment(const SwPipeline* pipeline,
                              const SwRenderPass* pass, int32_t x, int32_t y,
                              float fragment_depth, int front_facing,
                              uint32_t point_coord_valid,
                              float point_coord_x, float point_coord_y,
                              float point_coord_dx, float point_coord_dy,
                              const SwFragmentCoordinate* fragment_coordinate,
                              const float* f32_inputs,
                              const float* f32_input_dx,
                              const float* f32_input_dy,
                              const int32_t* i32_inputs,
                              const uint8_t* input_types,
                              uint32_t fragment_inputs,
                              uint32_t fragment_outputs,
                              const SwGraphicsBindGroup* bind_group,
                              int publish,
                              uint64_t* fragment_invocations)
{
    float color[SW_MAX_IO];
    int32_t i32_outputs[SW_MAX_IO];
    uint8_t output_types[SW_MAX_IO];
    SwShaderIo io;
    int discarded;
    uint32_t color_outputs = fragment_outputs;
    int writes_fragment_depth;
    int result;

    if (!pipeline || !pass || !f32_inputs || !f32_input_dx ||
        !f32_input_dy || !i32_inputs || !input_types ||
        !fragment_coordinate || fragment_coordinate->valid != 1u ||
        !fragment_invocations || point_coord_valid > 1u ||
        fragment_inputs != pipeline->desc.varying_count ||
        !sw_f32_finite(fragment_depth) || fragment_depth < 0.0f ||
        fragment_depth > 1.0f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    if (!sw_raster_covers_pixel(pass, x, y))
        return RIN_GPU_OK;
    writes_fragment_depth = (color_outputs & 3u) == 1u;
    if (writes_fragment_depth)
        color_outputs--;
    if (color_outputs == 0u || (color_outputs & 3u) != 0u)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (*fragment_invocations >= SW_MAX_FRAGMENT_INVOCATIONS)
        return RIN_GPU_ERROR_LIMIT;
    (*fragment_invocations)++;
    memset(&io, 0, sizeof(io));
    io.f32_inputs = f32_inputs;
    io.f32_input_dx = f32_input_dx;
    io.f32_input_dy = f32_input_dy;
    io.i32_inputs = i32_inputs;
    io.input_types = input_types;
    io.input_count = fragment_inputs;
    io.f32_outputs = color;
    io.i32_outputs = i32_outputs;
    io.output_types = output_types;
    io.output_count = SW_MAX_IO;
    io.discarded = &discarded;
    io.graphics_bind_group = bind_group;
    io.push_constants = pass->push_constants;
    io.push_constant_size = pass->push_constant_size;
    io.point_coord_valid = point_coord_valid;
    io.point_coord_x = point_coord_x;
    io.point_coord_y = point_coord_y;
    io.point_coord_dx = point_coord_dx;
    io.point_coord_dy = point_coord_dy;
    io.fragment_coordinate = fragment_coordinate;
    result = sw_run_shader_typed(pipeline->fragment, &io);
    if (result != RIN_GPU_OK)
        return result;
    if (discarded)
        return RIN_GPU_OK;
    for (uint32_t component = 0u; component < fragment_outputs; ++component) {
        if (output_types[component] != SW_SHADER_VALUE_F32 ||
            !sw_f32_finite(color[component])) {
            return RIN_GPU_ERROR_BACKEND;
        }
    }
    if (writes_fragment_depth) {
        fragment_depth = color[color_outputs];
        if (!sw_f32_finite(fragment_depth))
            return RIN_GPU_ERROR_BACKEND;
        if (fragment_depth < 0.0f)
            fragment_depth = 0.0f;
        else if (fragment_depth > 1.0f)
            fragment_depth = 1.0f;
    }
    return publish
        ? sw_publish_fragment(pipeline, pass, x, y, fragment_depth,
                              front_facing, color)
        : RIN_GPU_OK;
}

static int sw_point_fragment_inputs(
    const SwPipeline* pipeline, const float vertex_outputs[SW_MAX_IO],
    const int32_t vertex_i32_outputs[SW_MAX_IO],
    const uint8_t vertex_output_types[SW_MAX_IO], float* f32_inputs,
    int32_t* i32_inputs, uint8_t* input_types, uint32_t input_count)
{
    if (!pipeline || !vertex_outputs || !vertex_i32_outputs ||
        !vertex_output_types || !f32_inputs || !i32_inputs || !input_types ||
        input_count != pipeline->desc.varying_count) {
        return RIN_GPU_ERROR_BACKEND;
    }
    memset(f32_inputs, 0, SW_MAX_IO * sizeof(*f32_inputs));
    memset(i32_inputs, 0, SW_MAX_IO * sizeof(*i32_inputs));
    memset(input_types, SW_SHADER_VALUE_NONE,
           SW_MAX_IO * sizeof(*input_types));
    for (uint32_t index = 0u; index < input_count; ++index) {
        const RinGpuBackendVaryingV1* varying = &pipeline->desc.varyings[index];
        uint32_t output = varying->vertex_output_location;
        uint32_t input = varying->fragment_input_location;

        if (vertex_output_types[output] !=
            (varying->type == RIN_GPU_VARYING_FLOAT32
                 ? SW_SHADER_VALUE_F32
                 : SW_SHADER_VALUE_I32)) {
            return RIN_GPU_ERROR_BACKEND;
        }
        if (varying->type == RIN_GPU_VARYING_FLOAT32) {
            if (!sw_f32_finite(vertex_outputs[output]))
                return RIN_GPU_ERROR_BACKEND;
            f32_inputs[input] = vertex_outputs[output];
            input_types[input] = SW_SHADER_VALUE_F32;
        } else {
            i32_inputs[input] = vertex_i32_outputs[output];
            input_types[input] = SW_SHADER_VALUE_I32;
        }
    }
    return RIN_GPU_OK;
}

static int sw_raster_point(const SwPipeline* pipeline, const SwRenderPass* pass,
                           const float vertex_outputs[SW_MAX_IO],
                           const int32_t vertex_i32_outputs[SW_MAX_IO],
                           const uint8_t vertex_output_types[SW_MAX_IO],
                           uint32_t vertex_output_count,
                           int use_program_point_size,
                           float fixed_point_size,
                           const SwGraphicsBindGroup* bind_group,
                           uint32_t fragment_inputs,
                           uint32_t fragment_outputs, int publish,
                           uint64_t* fragment_invocations)
{
    SwClipVertex clip_vertex;
    float x, y, depth, point_size, inverse_w;
    float minimum_x, maximum_x, minimum_y, maximum_y;
    int32_t min_x, max_x, min_y, max_y;
    float f32_inputs[SW_MAX_IO];
    float f32_input_dx[SW_MAX_IO] = {0};
    float f32_input_dy[SW_MAX_IO] = {0};
    int32_t i32_inputs[SW_MAX_IO];
    uint8_t input_types[SW_MAX_IO];
    const uint32_t position = pipeline ? pipeline->desc.position_output_location :
                                         SW_MAX_IO;
    int result;

    if (!pipeline || !pass || !vertex_outputs || !vertex_i32_outputs ||
        !vertex_output_types || !fragment_invocations ||
        vertex_output_count == 0u || vertex_output_count > SW_MAX_IO ||
        (use_program_point_size != 0 && use_program_point_size != 1) ||
        position > vertex_output_count || 4u > vertex_output_count - position) {
        return RIN_GPU_ERROR_BACKEND;
    }
    result = sw_blend_state_valid(pipeline);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_pipeline_depth_stencil_valid(pipeline, pass);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_raster_state_valid(&pass->raster, pass->color);
    if (result != RIN_GPU_OK)
        return result;
    memset(&clip_vertex, 0, sizeof(clip_vertex));
    for (uint32_t output = 0u; output < vertex_output_count; ++output) {
        if (vertex_output_types[output] != SW_SHADER_VALUE_F32 &&
            vertex_output_types[output] != SW_SHADER_VALUE_I32) {
            return RIN_GPU_ERROR_BACKEND;
        }
        clip_vertex.types[output] = vertex_output_types[output];
        clip_vertex.f32[output] = vertex_outputs[output];
        clip_vertex.i32[output] = vertex_i32_outputs[output];
    }
    point_size = fixed_point_size;
    if (use_program_point_size != 0 &&
        (pipeline->desc.flags &
         RIN_GPU_GRAPHICS_PIPELINE_NATIVE_POINT_SIZE_OUTPUT) != 0u) {
        if (5u > vertex_output_count - position ||
            vertex_output_types[position + 4u] != SW_SHADER_VALUE_F32) {
            return RIN_GPU_ERROR_BACKEND;
        }
        point_size = vertex_outputs[position + 4u];
    }
    if (!sw_f32_finite(point_size))
        return RIN_GPU_ERROR_BACKEND;
    if (point_size < 1.0f)
        point_size = 1.0f;
    else if (point_size > 64.0f)
        point_size = 64.0f;
    for (uint32_t plane = 0u; plane < 7u; ++plane) {
        float distance;

        result = sw_clip_distance(&clip_vertex, position, plane, &distance);
        if (result != RIN_GPU_OK)
            return result;
        if (distance < 0.0f)
            return RIN_GPU_OK;
    }
    result = sw_project_position(pass, clip_vertex.f32 + position, &x, &y,
                                 &depth);
    if (result != RIN_GPU_OK)
        return result;
    inverse_w = 1.0f / clip_vertex.f32[position + 3u];
    if (!sw_f32_finite(inverse_w) || inverse_w == 0.0f)
        return RIN_GPU_ERROR_BACKEND;
    minimum_x = x - point_size * 0.5f;
    maximum_x = x + point_size * 0.5f;
    minimum_y = y - point_size * 0.5f;
    maximum_y = y + point_size * 0.5f;
    if (!sw_f32_finite(minimum_x) || !sw_f32_finite(maximum_x) ||
        !sw_f32_finite(minimum_y) || !sw_f32_finite(maximum_y)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    /* Points cover a half-open square centred at their window position.
     * Clip before conversion so a hostile viewport cannot turn into undefined
     * integer arithmetic. */
    if (maximum_x <= 0.5f || maximum_y <= 0.5f ||
        minimum_x >= (float)pass->color->desc.width + 0.5f ||
        minimum_y >= (float)pass->color->desc.height + 0.5f) {
        return RIN_GPU_OK;
    }
    result = sw_point_fragment_inputs(pipeline, clip_vertex.f32,
                                      clip_vertex.i32, clip_vertex.types,
                                      f32_inputs, i32_inputs, input_types,
                                      fragment_inputs);
    if (result != RIN_GPU_OK)
        return result;
    if (minimum_x < 0.5f) minimum_x = 0.5f;
    if (minimum_y < 0.5f) minimum_y = 0.5f;
    if (maximum_x > (float)pass->color->desc.width + 0.5f)
        maximum_x = (float)pass->color->desc.width + 0.5f;
    if (maximum_y > (float)pass->color->desc.height + 0.5f)
        maximum_y = (float)pass->color->desc.height + 0.5f;
    min_x = (int32_t)(minimum_x - 0.5f);
    min_y = (int32_t)(minimum_y - 0.5f);
    max_x = (int32_t)(maximum_x - 0.5f);
    max_y = (int32_t)(maximum_y - 0.5f);
    for (int32_t py = min_y; py <= max_y; ++py) {
        for (int32_t px = min_x; px <= max_x; ++px) {
            float sample_x = (float)px + 0.5f;
            float sample_y = (float)py + 0.5f;
            float point_coord_x;
            float point_coord_y;
            float point_coord_step;
            const float fragment_coordinate_dx[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
            const float fragment_coordinate_dy[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
            SwFragmentCoordinate fragment_coordinate;

            if (sample_x < minimum_x || sample_x >= maximum_x ||
                sample_y < minimum_y || sample_y >= maximum_y) {
                continue;
            }
            /* GLES point sprites have an immutable upper-left origin. Use the
             * unclipped point center rather than the clamped raster bounds,
             * so a point partly outside the surface retains its coordinate
             * space. dFdy follows GL's lower-left fragment convention. */
            point_coord_x = (sample_x - (x - point_size * 0.5f)) / point_size;
            point_coord_y = (sample_y - (y - point_size * 0.5f)) / point_size;
            point_coord_step = 1.0f / point_size;
            if (!sw_f32_finite(point_coord_x) || !sw_f32_finite(point_coord_y) ||
                !sw_f32_finite(point_coord_step) || point_coord_x < 0.0f ||
                point_coord_x > 1.0f || point_coord_y < 0.0f ||
                point_coord_y > 1.0f) {
                return RIN_GPU_ERROR_BACKEND;
            }
            result = sw_make_fragment_coordinate(
                pass, px, py, depth, inverse_w, fragment_coordinate_dx,
                fragment_coordinate_dy, &fragment_coordinate);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_raster_fragment(
                pipeline, pass, px, py, depth, 1, 1u, point_coord_x,
                point_coord_y, point_coord_step, -point_coord_step,
                &fragment_coordinate, f32_inputs, f32_input_dx,
                f32_input_dy, i32_inputs, input_types,
                fragment_inputs, fragment_outputs, bind_group, publish,
                fragment_invocations);
            if (result != RIN_GPU_OK)
                return result;
        }
    }
    return RIN_GPU_OK;
}

static int sw_line_fragment_inputs(
    const SwPipeline* pipeline, const SwClipVertex* first,
    const SwClipVertex* second, uint32_t vertex_output_count, float amount,
    float* f32_inputs, int32_t* i32_inputs, uint8_t* input_types,
    uint32_t input_count)
{
    float inverse_w;
    const uint32_t position = pipeline ? pipeline->desc.position_output_location :
                                         SW_MAX_IO;

    if (!pipeline || !first || !second || !f32_inputs || !i32_inputs ||
        !input_types || vertex_output_count == 0u ||
        position > vertex_output_count || 4u > vertex_output_count - position ||
        input_count != pipeline->desc.varying_count || !sw_f32_finite(amount) ||
        amount < 0.0f || amount > 1.0f ||
        !sw_f32_finite(first->f32[position + 3u]) ||
        !sw_f32_finite(second->f32[position + 3u]) ||
        first->f32[position + 3u] == 0.0f ||
        second->f32[position + 3u] == 0.0f) {
        return RIN_GPU_ERROR_BACKEND;
    }
    inverse_w = (1.0f - amount) / first->f32[position + 3u] +
        amount / second->f32[position + 3u];
    if (!sw_f32_finite(inverse_w) || inverse_w == 0.0f)
        return RIN_GPU_ERROR_BACKEND;
    memset(f32_inputs, 0, SW_MAX_IO * sizeof(*f32_inputs));
    memset(i32_inputs, 0, SW_MAX_IO * sizeof(*i32_inputs));
    memset(input_types, SW_SHADER_VALUE_NONE,
           SW_MAX_IO * sizeof(*input_types));
    for (uint32_t index = 0u; index < input_count; ++index) {
        const RinGpuBackendVaryingV1* varying = &pipeline->desc.varyings[index];
        uint32_t output = varying->vertex_output_location;
        uint32_t input = varying->fragment_input_location;
        float value;

        if (output >= vertex_output_count ||
            first->types[output] !=
                (varying->type == RIN_GPU_VARYING_FLOAT32
                     ? SW_SHADER_VALUE_F32
                     : SW_SHADER_VALUE_I32) ||
            second->types[output] != first->types[output]) {
            return RIN_GPU_ERROR_BACKEND;
        }
        if (varying->type == RIN_GPU_VARYING_SINT32) {
            i32_inputs[input] = first->i32[output];
            input_types[input] = SW_SHADER_VALUE_I32;
            continue;
        }
        if (varying->interpolation == RIN_GPU_INTERPOLATION_FLAT) {
            value = first->f32[output];
        } else if (varying->interpolation ==
                   RIN_GPU_INTERPOLATION_NO_PERSPECTIVE) {
            value = first->f32[output] +
                (second->f32[output] - first->f32[output]) * amount;
        } else {
            value = ((1.0f - amount) * first->f32[output] /
                         first->f32[position + 3u] +
                     amount * second->f32[output] /
                         second->f32[position + 3u]) /
                inverse_w;
        }
        if (!sw_f32_finite(value))
            return RIN_GPU_ERROR_BACKEND;
        f32_inputs[input] = value;
        input_types[input] = SW_SHADER_VALUE_F32;
    }
    return RIN_GPU_OK;
}

static int sw_raster_line(const SwPipeline* pipeline, const SwRenderPass* pass,
                          const float first_outputs[SW_MAX_IO],
                          const int32_t first_i32_outputs[SW_MAX_IO],
                          const uint8_t first_output_types[SW_MAX_IO],
                          const float second_outputs[SW_MAX_IO],
                          const int32_t second_i32_outputs[SW_MAX_IO],
                          const uint8_t second_output_types[SW_MAX_IO],
                          uint32_t vertex_output_count,
                          const SwGraphicsBindGroup* bind_group,
                          uint32_t fragment_inputs,
                          uint32_t fragment_outputs, int publish,
                          uint64_t* fragment_invocations)
{
    SwClipVertex first;
    SwClipVertex second;
    const uint32_t position = pipeline ? pipeline->desc.position_output_location :
                                         SW_MAX_IO;
    float x0, y0, depth0, x1, y1, depth1;
    float dx, dy, length_squared, half_width;
    float minimum_x, maximum_x, minimum_y, maximum_y;
    int32_t min_x, max_x, min_y, max_y;
    int result;

    if (!pipeline || !pass || !first_outputs || !first_i32_outputs ||
        !first_output_types || !second_outputs || !second_i32_outputs ||
        !second_output_types || !fragment_invocations ||
        vertex_output_count == 0u || vertex_output_count > SW_MAX_IO ||
        position > vertex_output_count || 4u > vertex_output_count - position) {
        return RIN_GPU_ERROR_BACKEND;
    }
    result = sw_blend_state_valid(pipeline);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_pipeline_depth_stencil_valid(pipeline, pass);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_raster_state_valid(&pass->raster, pass->color);
    if (result != RIN_GPU_OK)
        return result;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    for (uint32_t output = 0u; output < vertex_output_count; ++output) {
        if ((first_output_types[output] != SW_SHADER_VALUE_F32 &&
             first_output_types[output] != SW_SHADER_VALUE_I32) ||
            second_output_types[output] != first_output_types[output]) {
            return RIN_GPU_ERROR_BACKEND;
        }
        first.types[output] = first_output_types[output];
        first.f32[output] = first_outputs[output];
        first.i32[output] = first_i32_outputs[output];
        second.types[output] = second_output_types[output];
        second.f32[output] = second_outputs[output];
        second.i32[output] = second_i32_outputs[output];
        if (first.types[output] == SW_SHADER_VALUE_I32)
            second.i32[output] = first.i32[output];
    }
    for (uint32_t plane = 0u; plane < 7u; ++plane) {
        float first_distance;
        float second_distance;

        result = sw_clip_distance(&first, position, plane, &first_distance);
        if (result != RIN_GPU_OK)
            return result;
        result = sw_clip_distance(&second, position, plane, &second_distance);
        if (result != RIN_GPU_OK)
            return result;
        if (first_distance < 0.0f && second_distance < 0.0f)
            return RIN_GPU_OK;
        if ((first_distance < 0.0f) != (second_distance < 0.0f)) {
            SwClipVertex intersection;

            result = sw_clip_intersection(
                &intersection, &first, &second, first_distance,
                second_distance, vertex_output_count);
            if (result != RIN_GPU_OK)
                return result;
            if (first_distance < 0.0f)
                first = intersection;
            else
                second = intersection;
        }
    }
    result = sw_project_position(pass, first.f32 + position, &x0, &y0,
                                 &depth0);
    if (result != RIN_GPU_OK)
        return result;
    result = sw_project_position(pass, second.f32 + position, &x1, &y1,
                                 &depth1);
    if (result != RIN_GPU_OK)
        return result;
    dx = x1 - x0;
    dy = y1 - y0;
    length_squared = dx * dx + dy * dy;
    if (!sw_f32_finite(length_squared))
        return RIN_GPU_ERROR_BACKEND;
    if (length_squared == 0.0f) {
        return sw_raster_point(pipeline, pass, first.f32, first.i32,
                               first.types, vertex_output_count, 0,
                               pass->raster.line_width, bind_group,
                               fragment_inputs, fragment_outputs, publish,
                               fragment_invocations);
    }
    half_width = pass->raster.line_width * 0.5f;
    minimum_x = (x0 < x1 ? x0 : x1) - half_width - 0.5f;
    maximum_x = (x0 > x1 ? x0 : x1) + half_width - 0.5f;
    minimum_y = (y0 < y1 ? y0 : y1) - half_width - 0.5f;
    maximum_y = (y0 > y1 ? y0 : y1) + half_width - 0.5f;
    if (!sw_f32_finite(minimum_x) || !sw_f32_finite(maximum_x) ||
        !sw_f32_finite(minimum_y) || !sw_f32_finite(maximum_y)) {
        return RIN_GPU_ERROR_BACKEND;
    }
    if (maximum_x < 0.0f || maximum_y < 0.0f ||
        minimum_x > (float)(pass->color->desc.width - 1u) ||
        minimum_y > (float)(pass->color->desc.height - 1u)) {
        return RIN_GPU_OK;
    }
    min_x = minimum_x <= 0.0f ? 0 :
        (int32_t)minimum_x + ((float)(int32_t)minimum_x < minimum_x);
    min_y = minimum_y <= 0.0f ? 0 :
        (int32_t)minimum_y + ((float)(int32_t)minimum_y < minimum_y);
    max_x = maximum_x >= (float)(pass->color->desc.width - 1u)
        ? (int32_t)(pass->color->desc.width - 1u) : (int32_t)maximum_x;
    max_y = maximum_y >= (float)(pass->color->desc.height - 1u)
        ? (int32_t)(pass->color->desc.height - 1u) : (int32_t)maximum_y;
    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float amount = ((px - x0) * dx + (py - y0) * dy) /
                length_squared;
            float nearest_x;
            float nearest_y;
            float distance_squared;
            float fragment_depth;
            float amount_dx;
            float amount_dy;
            float inverse_w;
            float inverse_w_delta;
            float fragment_coordinate_dx[4];
            float fragment_coordinate_dy[4];
            SwFragmentCoordinate fragment_coordinate;
            float f32_inputs[SW_MAX_IO];
            float f32_input_dx[SW_MAX_IO] = {0};
            float f32_input_dy[SW_MAX_IO] = {0};
            float f32_neighbor[SW_MAX_IO];
            int32_t i32_inputs[SW_MAX_IO];
            int32_t i32_neighbor[SW_MAX_IO];
            uint8_t input_types[SW_MAX_IO];
            uint8_t neighbor_types[SW_MAX_IO];

            if (!sw_f32_finite(amount))
                return RIN_GPU_ERROR_BACKEND;
            if (amount < 0.0f)
                amount = 0.0f;
            else if (amount > 1.0f)
                amount = 1.0f;
            nearest_x = x0 + dx * amount;
            nearest_y = y0 + dy * amount;
            distance_squared = (px - nearest_x) * (px - nearest_x) +
                (py - nearest_y) * (py - nearest_y);
            if (!sw_f32_finite(distance_squared) ||
                distance_squared > half_width * half_width ||
                (distance_squared == half_width * half_width &&
                 (sw_absf(dx) >= sw_absf(dy) ? py > nearest_y
                                              : px > nearest_x))) {
                continue;
            }
            fragment_depth = depth0 + (depth1 - depth0) * amount;
            if (!sw_f32_finite(fragment_depth) || fragment_depth < 0.0f ||
                fragment_depth > 1.0f) {
                return RIN_GPU_ERROR_BACKEND;
            }
            result = sw_line_fragment_inputs(
                pipeline, &first, &second, vertex_output_count, amount,
                f32_inputs, i32_inputs, input_types, fragment_inputs);
            if (result != RIN_GPU_OK)
                return result;
            {
                amount_dx = (((px + 1.0f) - x0) * dx +
                             (py - y0) * dy) / length_squared;
                amount_dy = ((px - x0) * dx +
                             ((py - 1.0f) - y0) * dy) / length_squared;

                if (!sw_f32_finite(amount_dx) || !sw_f32_finite(amount_dy))
                    return RIN_GPU_ERROR_BACKEND;
                if (amount_dx < 0.0f) amount_dx = 0.0f;
                if (amount_dx > 1.0f) amount_dx = 1.0f;
                if (amount_dy < 0.0f) amount_dy = 0.0f;
                if (amount_dy > 1.0f) amount_dy = 1.0f;
                result = sw_line_fragment_inputs(
                    pipeline, &first, &second, vertex_output_count, amount_dx,
                    f32_neighbor, i32_neighbor, neighbor_types,
                    fragment_inputs);
                if (result != RIN_GPU_OK)
                    return result;
                for (uint32_t input = 0u; input < fragment_inputs; ++input) {
                    if (input_types[input] == SW_SHADER_VALUE_F32 &&
                        neighbor_types[input] == SW_SHADER_VALUE_F32) {
                        f32_input_dx[input] = f32_neighbor[input] -
                            f32_inputs[input];
                    }
                }
                result = sw_line_fragment_inputs(
                    pipeline, &first, &second, vertex_output_count, amount_dy,
                    f32_neighbor, i32_neighbor, neighbor_types, fragment_inputs);
                if (result != RIN_GPU_OK)
                    return result;
                for (uint32_t input = 0u; input < fragment_inputs; ++input) {
                    if (input_types[input] == SW_SHADER_VALUE_F32 &&
                        neighbor_types[input] == SW_SHADER_VALUE_F32) {
                        f32_input_dy[input] = f32_neighbor[input] -
                            f32_inputs[input];
                    }
                }
            }
            inverse_w = (1.0f - amount) / first.f32[position + 3u] +
                amount / second.f32[position + 3u];
            inverse_w_delta = 1.0f / second.f32[position + 3u] -
                1.0f / first.f32[position + 3u];
            fragment_coordinate_dx[0] = 1.0f;
            fragment_coordinate_dx[1] = 0.0f;
            fragment_coordinate_dx[2] = (depth1 - depth0) *
                (amount_dx - amount);
            fragment_coordinate_dx[3] = inverse_w_delta *
                (amount_dx - amount);
            fragment_coordinate_dy[0] = 0.0f;
            fragment_coordinate_dy[1] = 1.0f;
            fragment_coordinate_dy[2] = (depth1 - depth0) *
                (amount_dy - amount);
            fragment_coordinate_dy[3] = inverse_w_delta *
                (amount_dy - amount);
            result = sw_make_fragment_coordinate(
                pass, x, y, fragment_depth, inverse_w,
                fragment_coordinate_dx, fragment_coordinate_dy,
                &fragment_coordinate);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_raster_fragment(
                pipeline, pass, x, y, fragment_depth, 1, 0u, 0.0f, 0.0f,
                0.0f, 0.0f, &fragment_coordinate, f32_inputs, f32_input_dx, f32_input_dy,
                i32_inputs, input_types, fragment_inputs, fragment_outputs,
                bind_group, publish, fragment_invocations);
            if (result != RIN_GPU_OK)
                return result;
        }
    }
    return RIN_GPU_OK;
}

/* Assemble declared WebGL primitive topologies in the generic RinGPU
 * backend. Keeping this below RinGL is essential: validation/preflight must
 * cover exactly the primitives submitted by the browser, rather than a
 * triangle-only approximation. Strip winding alternates in
 * sw_triangle_vertices() so front-face and culling semantics remain identical
 * to the API topology. */
static int sw_primitive_count(uint32_t topology, uint32_t vertex_count,
                              uint32_t* primitive_count_out)
{
    if (!primitive_count_out)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (vertex_count == 0u) {
        *primitive_count_out = 0u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_POINT_LIST) {
        *primitive_count_out = vertex_count;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_LINE_LIST) {
        *primitive_count_out = vertex_count / 2u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_LINE_STRIP) {
        *primitive_count_out = vertex_count < 2u ? 0u : vertex_count - 1u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_LINE_LOOP) {
        *primitive_count_out = vertex_count < 2u ? 0u : vertex_count;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_TRIANGLE_LIST) {
        if ((vertex_count % 3u) != 0u)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        *primitive_count_out = vertex_count / 3u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_TRIANGLE_STRIP ||
        topology == RIN_GPU_PRIMITIVE_TRIANGLE_FAN) {
        *primitive_count_out = vertex_count < 3u ? 0u : vertex_count - 2u;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_UNSUPPORTED;
}

static int sw_triangle_vertices(uint32_t topology, uint32_t triangle,
                                uint32_t first_vertex,
                                uint32_t vertex_count,
                                uint32_t vertices_out[3])
{
    uint32_t triangle_count;

    if (!vertices_out ||
        sw_primitive_count(topology, vertex_count, &triangle_count) != RIN_GPU_OK ||
        triangle >= triangle_count || first_vertex > UINT32_MAX - vertex_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (topology == RIN_GPU_PRIMITIVE_TRIANGLE_LIST) {
        uint32_t first = first_vertex + triangle * 3u;

        vertices_out[0] = first;
        vertices_out[1] = first + 1u;
        vertices_out[2] = first + 2u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_TRIANGLE_STRIP) {
        uint32_t first = first_vertex + triangle;

        if ((triangle & 1u) == 0u) {
            vertices_out[0] = first;
            vertices_out[1] = first + 1u;
        } else {
            vertices_out[0] = first + 1u;
            vertices_out[1] = first;
        }
        vertices_out[2] = first + 2u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_TRIANGLE_FAN) {
        vertices_out[0] = first_vertex;
        vertices_out[1] = first_vertex + triangle + 1u;
        vertices_out[2] = first_vertex + triangle + 2u;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_UNSUPPORTED;
}

static int sw_line_vertices(uint32_t topology, uint32_t line,
                            uint32_t first_vertex, uint32_t vertex_count,
                            uint32_t vertices_out[2])
{
    uint32_t line_count;

    if (!vertices_out ||
        sw_primitive_count(topology, vertex_count, &line_count) != RIN_GPU_OK ||
        line >= line_count || first_vertex > UINT32_MAX - vertex_count) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (topology == RIN_GPU_PRIMITIVE_LINE_LIST) {
        uint32_t first = first_vertex + line * 2u;

        vertices_out[0] = first;
        vertices_out[1] = first + 1u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_LINE_STRIP) {
        vertices_out[0] = first_vertex + line;
        vertices_out[1] = first_vertex + line + 1u;
        return RIN_GPU_OK;
    }
    if (topology == RIN_GPU_PRIMITIVE_LINE_LOOP) {
        vertices_out[0] = first_vertex + line;
        vertices_out[1] = first_vertex +
            (line + 1u == vertex_count ? 0u : line + 1u);
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_UNSUPPORTED;
}

static int sw_draw_vertex_triangle(const SwPipeline* pipeline,
                                   const SwRenderPass* pass,
                                   const SwVertexBindings* vertex_bindings,
                                   const uint32_t vertex_indices[3],
                                   uint32_t instance_index,
                                   uint32_t first_instance,
                                   const SwGraphicsBindGroup* bind_group,
                                   uint32_t vertex_outputs,
                                   uint32_t fragment_inputs,
                                   uint32_t fragment_outputs, int publish,
                                   uint64_t* fragment_invocations)
{
    float outputs[3][SW_MAX_IO];
    int32_t i32_outputs[3][SW_MAX_IO];
    uint8_t output_types[3][SW_MAX_IO];
    int result;

    if (!pipeline || !pass || !pass->color || !vertex_bindings ||
        !vertex_indices ||
        !fragment_invocations) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0u; index < 3u; ++index) {
        float inputs[SW_MAX_IO];
        int32_t i32_inputs[SW_MAX_IO] = {0};
        uint8_t input_types[SW_MAX_IO];
        SwShaderIo io;

        result = sw_load_vertex_inputs(pipeline, vertex_bindings,
                                       vertex_indices[index], instance_index,
                                       first_instance, inputs);
        if (result != RIN_GPU_OK) return result;
        memset(input_types, SW_SHADER_VALUE_F32, sizeof(input_types));
        memset(&io, 0, sizeof(io));
        io.f32_inputs = inputs;
        io.i32_inputs = i32_inputs;
        io.input_types = input_types;
        io.input_count = SW_MAX_IO;
        io.f32_outputs = outputs[index];
        io.i32_outputs = i32_outputs[index];
        io.output_types = output_types[index];
        io.output_count = SW_MAX_IO;
        io.graphics_bind_group = bind_group;
        io.push_constants = pass->push_constants;
        io.push_constant_size = pass->push_constant_size;
        result = sw_run_shader_typed(pipeline->vertex, &io);
        if (result != RIN_GPU_OK) return result;
    }
    return sw_clip_and_raster_triangle(
        pipeline, pass, outputs, i32_outputs, output_types, vertex_outputs,
        bind_group, fragment_inputs, fragment_outputs, publish,
        fragment_invocations);
}

static int sw_draw_vertex_point(const SwPipeline* pipeline,
                                const SwRenderPass* pass,
                                const SwVertexBindings* vertex_bindings,
                                uint32_t vertex_index,
                                uint32_t instance_index,
                                uint32_t first_instance,
                                const SwGraphicsBindGroup* bind_group,
                                uint32_t vertex_outputs,
                                uint32_t fragment_inputs,
                                uint32_t fragment_outputs, int publish,
                                uint64_t* fragment_invocations)
{
    float inputs[SW_MAX_IO];
    float outputs[SW_MAX_IO];
    int32_t i32_inputs[SW_MAX_IO] = {0};
    int32_t i32_outputs[SW_MAX_IO];
    uint8_t input_types[SW_MAX_IO];
    uint8_t output_types[SW_MAX_IO];
    SwShaderIo io;
    int result;

    if (!pipeline || !pass || !vertex_bindings || !fragment_invocations)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    result = sw_load_vertex_inputs(pipeline, vertex_bindings, vertex_index,
                                   instance_index, first_instance, inputs);
    if (result != RIN_GPU_OK)
        return result;
    memset(input_types, SW_SHADER_VALUE_F32, sizeof(input_types));
    memset(&io, 0, sizeof(io));
    io.f32_inputs = inputs;
    io.i32_inputs = i32_inputs;
    io.input_types = input_types;
    io.input_count = SW_MAX_IO;
    io.f32_outputs = outputs;
    io.i32_outputs = i32_outputs;
    io.output_types = output_types;
    io.output_count = SW_MAX_IO;
    io.graphics_bind_group = bind_group;
    io.push_constants = pass->push_constants;
    io.push_constant_size = pass->push_constant_size;
    result = sw_run_shader_typed(pipeline->vertex, &io);
    if (result != RIN_GPU_OK)
        return result;
    return sw_raster_point(pipeline, pass, outputs, i32_outputs, output_types,
                           vertex_outputs, 1, 1.0f, bind_group, fragment_inputs,
                           fragment_outputs, publish, fragment_invocations);
}

static int sw_draw_vertex_line(const SwPipeline* pipeline,
                               const SwRenderPass* pass,
                               const SwVertexBindings* vertex_bindings,
                               const uint32_t vertex_indices[2],
                               uint32_t instance_index,
                               uint32_t first_instance,
                               const SwGraphicsBindGroup* bind_group,
                               uint32_t vertex_outputs,
                               uint32_t fragment_inputs,
                               uint32_t fragment_outputs, int publish,
                               uint64_t* fragment_invocations)
{
    float outputs[2][SW_MAX_IO];
    int32_t i32_outputs[2][SW_MAX_IO];
    uint8_t output_types[2][SW_MAX_IO];
    int result;

    if (!pipeline || !pass || !vertex_bindings || !vertex_indices ||
        !fragment_invocations) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0u; index < 2u; ++index) {
        float inputs[SW_MAX_IO];
        int32_t i32_inputs[SW_MAX_IO] = {0};
        uint8_t input_types[SW_MAX_IO];
        SwShaderIo io;

        result = sw_load_vertex_inputs(pipeline, vertex_bindings,
                                       vertex_indices[index], instance_index,
                                       first_instance, inputs);
        if (result != RIN_GPU_OK)
            return result;
        memset(input_types, SW_SHADER_VALUE_F32, sizeof(input_types));
        memset(&io, 0, sizeof(io));
        io.f32_inputs = inputs;
        io.i32_inputs = i32_inputs;
        io.input_types = input_types;
        io.input_count = SW_MAX_IO;
        io.f32_outputs = outputs[index];
        io.i32_outputs = i32_outputs[index];
        io.output_types = output_types[index];
        io.output_count = SW_MAX_IO;
        io.graphics_bind_group = bind_group;
        io.push_constants = pass->push_constants;
        io.push_constant_size = pass->push_constant_size;
        result = sw_run_shader_typed(pipeline->vertex, &io);
        if (result != RIN_GPU_OK)
            return result;
    }
    return sw_raster_line(
        pipeline, pass, outputs[0], i32_outputs[0], output_types[0],
        outputs[1], i32_outputs[1], output_types[1], vertex_outputs,
        bind_group, fragment_inputs, fragment_outputs, publish,
        fragment_invocations);
}

static int sw_draw_unindexed_primitive(
    const SwPipeline* pipeline, const SwRenderPass* pass,
    const SwVertexBindings* vertex_bindings, uint32_t primitive,
    uint32_t first_vertex, uint32_t vertex_count, uint32_t instance_index,
    uint32_t first_instance, const SwGraphicsBindGroup* bind_group,
    uint32_t vertex_outputs, uint32_t fragment_inputs,
    uint32_t fragment_outputs, int publish,
    uint64_t* fragment_invocations)
{
    if (!pipeline || !pass || !vertex_bindings || !fragment_invocations)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_POINT_LIST) {
        if (primitive >= vertex_count || first_vertex > UINT32_MAX - primitive)
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        return sw_draw_vertex_point(
            pipeline, pass, vertex_bindings, first_vertex + primitive,
            instance_index, first_instance, bind_group, vertex_outputs,
            fragment_inputs, fragment_outputs, publish, fragment_invocations);
    }
    if (pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_LINE_LIST ||
        pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_LINE_STRIP ||
        pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_LINE_LOOP) {
        uint32_t vertices[2];
        int result = sw_line_vertices(pipeline->desc.primitive_topology,
                                      primitive, first_vertex, vertex_count,
                                      vertices);

        if (result != RIN_GPU_OK)
            return result;
        return sw_draw_vertex_line(
            pipeline, pass, vertex_bindings, vertices, instance_index,
            first_instance, bind_group, vertex_outputs, fragment_inputs,
            fragment_outputs, publish, fragment_invocations);
    }
    {
        uint32_t vertices[3];
        int result = sw_triangle_vertices(pipeline->desc.primitive_topology,
                                          primitive, first_vertex,
                                          vertex_count, vertices);

        if (result != RIN_GPU_OK)
            return result;
        return sw_draw_vertex_triangle(
            pipeline, pass, vertex_bindings, vertices, instance_index,
            first_instance, bind_group, vertex_outputs, fragment_inputs,
            fragment_outputs, publish, fragment_invocations);
    }
}

static int sw_vertex_bindings_from_v2(
    const SwPipeline* pipeline,
    const RinGpuBackendVertexBufferBindingV1* source_bindings,
    uint32_t binding_count, SwVertexBindings* bindings_out)
{
    if (!pipeline || !bindings_out ||
        binding_count > RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS ||
        pipeline->desc.vertex_binding_count != binding_count ||
        (binding_count != 0u && !source_bindings)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    memset(bindings_out, 0, sizeof(*bindings_out));
    bindings_out->binding_count = binding_count;
    for (uint32_t binding = 0u; binding < binding_count; ++binding) {
        if (source_bindings[binding].binding != binding ||
            source_bindings[binding].reserved != 0u ||
            source_bindings[binding].buffer_cookie == 0u) {
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        bindings_out->buffers[binding] = (const SwBuffer*)(uintptr_t)
            source_bindings[binding].buffer_cookie;
        bindings_out->offsets[binding] = source_bindings[binding].offset;
    }
    return RIN_GPU_OK;
}

static int sw_draw_vertices_in_pass(const RinGpuBackendDrawVerticesV1* draw,
                                    const SwRenderPass* active_pass)
{
    SwPipeline* pipeline;
    SwImage* image;
    SwRenderPass implicit_pass;
    const SwRenderPass* render_pass = active_pass;
    SwBuffer* buffer;
    SwVertexBindings vertex_bindings;
    const SwGraphicsBindGroup* bind_group;
    uint32_t vertex_outputs;
    uint32_t fragment_inputs;
    uint32_t fragment_outputs;
    uint32_t triangle_count;
    uint32_t phase;
    uint32_t vertex;
    int result;

    if (!draw) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    pipeline = (SwPipeline*)(uintptr_t)draw->pipeline_cookie;
    image = (SwImage*)(uintptr_t)draw->color_target_cookie;
    buffer = (SwBuffer*)(uintptr_t)draw->vertex_buffer_cookie;
    bind_group = (const SwGraphicsBindGroup*)(uintptr_t)draw->bind_group_cookie;
    memset(&vertex_bindings, 0, sizeof(vertex_bindings));
    vertex_bindings.buffers[0] = buffer;
    vertex_bindings.offsets[0] = draw->vertex_offset;
    vertex_bindings.binding_count = pipeline != NULL &&
        pipeline->desc.vertex_binding_count == 0u &&
        pipeline->desc.vertex_stride == 0u ? 0u : 1u;
    if (render_pass == NULL) {
        if (draw->mip_level != 0u || draw->array_layer != 0u)
            return RIN_GPU_ERROR_UNSUPPORTED;
        result = sw_make_implicit_render_pass(image, &implicit_pass);
        if (result != RIN_GPU_OK) return result;
        render_pass = &implicit_pass;
    } else if (render_pass->color_cookie != draw->color_target_cookie ||
               render_pass->color_mip_level != draw->mip_level ||
               render_pass->color_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    if (!pipeline || !image ||
        (vertex_bindings.binding_count != 0u && !buffer) ||
        draw->instance_count == 0u ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_instance > UINT32_MAX - draw->instance_count ||
        draw->vertex_count > SW_MAX_DRAW_VERTICES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count)
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (!sw_draw_work_is_bounded(draw->vertex_count, draw->instance_count))
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = sw_primitive_count(pipeline->desc.primitive_topology,
                               draw->vertex_count, &triangle_count);
    if (result != RIN_GPU_OK)
        return result;
    if (image->allocation_desc.mip_levels != 0u) {
        result = sw_color_target_valid(render_pass->color);
        if (result != RIN_GPU_OK)
            return result;
    }
    if ((pipeline->desc.resource_count == 0u && bind_group != NULL) ||
        (pipeline->desc.resource_count != 0u &&
         (!bind_group || bind_group->pipeline != pipeline ||
          bind_group->binding_count != pipeline->desc.resource_count))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_pipeline_shader_io(pipeline, &vertex_outputs, &fragment_inputs,
                                   &fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    result = sw_render_pass_fragment_outputs_supported(render_pass,
                                                       fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    /* The preflight pass evaluates every vertex and every covered fragment
     * before publication.  RSH1 in this profile has no side effects, so the
     * second pass is deterministic and a late shader/input error cannot
     * leave an earlier triangle in the target. */
    for (phase = 0u; phase < 2u; ++phase) {
        uint64_t fragment_invocations = 0u;

        for (uint32_t instance = 0u; instance < draw->instance_count;
             ++instance) {
            for (vertex = 0u; vertex < triangle_count; ++vertex) {
                result = sw_draw_unindexed_primitive(
                    pipeline, render_pass, &vertex_bindings, vertex,
                    draw->first_vertex, draw->vertex_count, instance,
                    draw->first_instance, bind_group, vertex_outputs,
                    fragment_inputs, fragment_outputs, phase != 0u,
                    &fragment_invocations);
                if (result != RIN_GPU_OK) return result;
            }
        }
    }
    return RIN_GPU_OK;
}

static int sw_draw_vertices_v2_in_pass(const RinGpuBackendDrawVerticesV2* draw,
                                       const SwRenderPass* active_pass)
{
    SwPipeline* pipeline;
    SwImage* image;
    SwRenderPass implicit_pass;
    const SwRenderPass* render_pass = active_pass;
    SwVertexBindings vertex_bindings;
    const SwGraphicsBindGroup* bind_group;
    uint32_t vertex_outputs;
    uint32_t fragment_inputs;
    uint32_t fragment_outputs;
    uint32_t triangle_count;
    int result;

    if (!draw) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    pipeline = (SwPipeline*)(uintptr_t)draw->pipeline_cookie;
    image = (SwImage*)(uintptr_t)draw->color_target_cookie;
    bind_group = (const SwGraphicsBindGroup*)(uintptr_t)draw->bind_group_cookie;
    if (render_pass == NULL) {
        if (draw->mip_level != 0u || draw->array_layer != 0u)
            return RIN_GPU_ERROR_UNSUPPORTED;
        result = sw_make_implicit_render_pass(image, &implicit_pass);
        if (result != RIN_GPU_OK) return result;
        render_pass = &implicit_pass;
    } else if (render_pass->color_cookie != draw->color_target_cookie ||
               render_pass->color_mip_level != draw->mip_level ||
               render_pass->color_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    if (!pipeline || !image || draw->instance_count == 0u ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_instance > UINT32_MAX - draw->instance_count ||
        draw->reserved != 0u ||
        draw->vertex_count > SW_MAX_DRAW_VERTICES ||
        draw->first_vertex > UINT32_MAX - draw->vertex_count) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (!sw_draw_work_is_bounded(draw->vertex_count, draw->instance_count))
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = sw_primitive_count(pipeline->desc.primitive_topology,
                               draw->vertex_count, &triangle_count);
    if (result != RIN_GPU_OK)
        return result;
    if (image->allocation_desc.mip_levels != 0u) {
        result = sw_color_target_valid(render_pass->color);
        if (result != RIN_GPU_OK)
            return result;
    }
    if ((pipeline->desc.resource_count == 0u && bind_group != NULL) ||
        (pipeline->desc.resource_count != 0u &&
         (!bind_group || bind_group->pipeline != pipeline ||
          bind_group->binding_count != pipeline->desc.resource_count))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_vertex_bindings_from_v2(pipeline, draw->vertex_buffers,
                                        draw->vertex_binding_count,
                                        &vertex_bindings);
    if (result != RIN_GPU_OK) return result;
    result = sw_pipeline_shader_io(pipeline, &vertex_outputs, &fragment_inputs,
                                   &fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    result = sw_render_pass_fragment_outputs_supported(render_pass,
                                                       fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        uint64_t fragment_invocations = 0u;

        for (uint32_t instance = 0u; instance < draw->instance_count;
             ++instance) {
            for (uint32_t vertex = 0u; vertex < triangle_count; ++vertex) {
                result = sw_draw_unindexed_primitive(
                    pipeline, render_pass, &vertex_bindings, vertex,
                    draw->first_vertex, draw->vertex_count, instance,
                    draw->first_instance, bind_group, vertex_outputs,
                    fragment_inputs, fragment_outputs, pass != 0u,
                    &fragment_invocations);
                if (result != RIN_GPU_OK) return result;
            }
        }
    }
    return RIN_GPU_OK;
}

static uint32_t sw_index_format_bytes(uint32_t format)
{
    if (format == RIN_GPU_INDEX_UINT8)
        return 1u;
    if (format == RIN_GPU_INDEX_UINT16)
        return 2u;
    if (format == RIN_GPU_INDEX_UINT32)
        return 4u;
    return 0u;
}

static int sw_read_index(const SwBuffer* index_buffer, uint32_t index_format,
                         uint64_t index_offset, uint32_t first_index,
                         uint32_t index, uint32_t* value_out)
{
    uint64_t absolute_index;
    uint64_t byte_offset;
    uint32_t index_bytes;
    uint16_t u16_value;

    index_bytes = sw_index_format_bytes(index_format);
    if (!index_buffer || !value_out || index_bytes == 0u ||
        !sw_add_u64(first_index, index, &absolute_index) ||
        !sw_multiply_u64(absolute_index, index_bytes, &byte_offset) ||
        !sw_add_u64(index_offset, byte_offset, &byte_offset) ||
        byte_offset > index_buffer->size_bytes ||
        index_bytes > index_buffer->size_bytes - byte_offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    if (index_format == RIN_GPU_INDEX_UINT8) {
        *value_out = index_buffer->bytes[byte_offset];
    } else if (index_format == RIN_GPU_INDEX_UINT16) {
        memcpy(&u16_value, index_buffer->bytes + byte_offset,
               sizeof(u16_value));
        *value_out = u16_value;
    } else {
        memcpy(value_out, index_buffer->bytes + byte_offset,
               sizeof(*value_out));
    }
    return RIN_GPU_OK;
}

static int sw_index_restart_value(uint32_t index_format,
                                  uint32_t* value_out)
{
    if (!value_out) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (index_format == RIN_GPU_INDEX_UINT8) {
        *value_out = UINT8_MAX;
        return RIN_GPU_OK;
    }
    if (index_format == RIN_GPU_INDEX_UINT16) {
        *value_out = UINT16_MAX;
        return RIN_GPU_OK;
    }
    if (index_format == RIN_GPU_INDEX_UINT32) {
        *value_out = UINT32_MAX;
        return RIN_GPU_OK;
    }
    return RIN_GPU_ERROR_INVALID_ARGUMENT;
}

static int sw_rebase_vertex_index(uint32_t raw_index, int32_t base_vertex,
                                  uint32_t vertex_count,
                                  uint32_t* vertex_index_out)
{
    int64_t rebased;

    if (vertex_index_out == NULL || vertex_count == 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    rebased = (int64_t)raw_index + (int64_t)base_vertex;
    if (rebased < 0 || (uint64_t)rebased >= vertex_count)
        return RIN_GPU_ERROR_BOUNDS;
    *vertex_index_out = (uint32_t)rebased;
    return RIN_GPU_OK;
}

static int sw_draw_indexed_primitive(
    const SwPipeline* pipeline, const SwRenderPass* pass,
    const SwVertexBindings* vertex_bindings, const SwBuffer* index_buffer,
    uint32_t index_format, uint64_t index_offset, uint32_t first_index,
    uint32_t index_count, int32_t base_vertex, uint32_t vertex_count,
    uint32_t primitive, uint32_t instance_index, uint32_t first_instance,
    const SwGraphicsBindGroup* bind_group,
    uint32_t vertex_outputs, uint32_t fragment_inputs,
    uint32_t fragment_outputs, int publish,
    uint64_t* fragment_invocations)
{
    uint32_t index_positions[3];
    uint32_t vertex_indices[3];
    uint32_t component_count;
    int result;

    if (!pipeline || !pass || !vertex_bindings || !index_buffer ||
        !fragment_invocations) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_POINT_LIST) {
        index_positions[0] = primitive;
        component_count = 1u;
    } else if (pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_LINE_LIST ||
               pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_LINE_STRIP ||
               pipeline->desc.primitive_topology == RIN_GPU_PRIMITIVE_LINE_LOOP) {
        result = sw_line_vertices(pipeline->desc.primitive_topology, primitive,
                                  0u, index_count, index_positions);
        if (result != RIN_GPU_OK)
            return result;
        component_count = 2u;
    } else {
        result = sw_triangle_vertices(pipeline->desc.primitive_topology,
                                      primitive, 0u, index_count,
                                      index_positions);
        if (result != RIN_GPU_OK)
            return result;
        component_count = 3u;
    }
    for (uint32_t component = 0u; component < component_count; ++component) {
        result = sw_read_index(index_buffer, index_format, index_offset,
                               first_index, index_positions[component],
                               &vertex_indices[component]);
        if (result != RIN_GPU_OK)
            return result;
        result = sw_rebase_vertex_index(vertex_indices[component], base_vertex,
                                        vertex_count,
                                        &vertex_indices[component]);
        if (result != RIN_GPU_OK)
            return result;
    }
    if (component_count == 1u) {
        return sw_draw_vertex_point(
            pipeline, pass, vertex_bindings, vertex_indices[0],
            instance_index, first_instance, bind_group, vertex_outputs,
            fragment_inputs, fragment_outputs, publish, fragment_invocations);
    }
    if (component_count == 2u) {
        return sw_draw_vertex_line(
            pipeline, pass, vertex_bindings, vertex_indices, instance_index,
            first_instance, bind_group, vertex_outputs, fragment_inputs,
            fragment_outputs, publish, fragment_invocations);
    }
    return sw_draw_vertex_triangle(
        pipeline, pass, vertex_bindings, vertex_indices, instance_index,
        first_instance, bind_group, vertex_outputs, fragment_inputs,
        fragment_outputs, publish, fragment_invocations);
}

static int sw_draw_indexed_with_base_vertex_in_pass(
    const RinGpuBackendDrawIndexedV1* draw, int32_t base_vertex,
    const SwRenderPass* active_pass)
{
    SwPipeline* pipeline;
    SwImage* image;
    SwRenderPass implicit_pass;
    const SwRenderPass* render_pass = active_pass;
    SwBuffer* vertex_buffer;
    SwBuffer* index_buffer;
    SwVertexBindings vertex_bindings;
    const SwGraphicsBindGroup* bind_group;
    uint32_t vertex_outputs;
    uint32_t fragment_inputs;
    uint32_t fragment_outputs;
    uint32_t index_bytes;
    uint32_t triangle_count;
    uint32_t restart_index = 0u;
    int restart_enabled;
    int result;

    if (!draw) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    pipeline = (SwPipeline*)(uintptr_t)draw->pipeline_cookie;
    image = (SwImage*)(uintptr_t)draw->color_target_cookie;
    vertex_buffer = (SwBuffer*)(uintptr_t)draw->vertex_buffer_cookie;
    index_buffer = (SwBuffer*)(uintptr_t)draw->index_buffer_cookie;
    bind_group = (const SwGraphicsBindGroup*)(uintptr_t)draw->bind_group_cookie;
    memset(&vertex_bindings, 0, sizeof(vertex_bindings));
    vertex_bindings.buffers[0] = vertex_buffer;
    vertex_bindings.offsets[0] = draw->vertex_offset;
    vertex_bindings.binding_count = pipeline != NULL &&
        pipeline->desc.vertex_binding_count == 0u &&
        pipeline->desc.vertex_stride == 0u ? 0u : 1u;
    index_bytes = sw_index_format_bytes(draw->index_format);
    restart_enabled = pipeline != NULL &&
        (pipeline->desc.flags & RIN_GPU_GRAPHICS_PIPELINE_NATIVE_PRIMITIVE_RESTART) != 0u;
    if (restart_enabled &&
        sw_index_restart_value(draw->index_format, &restart_index) !=
            RIN_GPU_OK) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    if (render_pass == NULL) {
        if (draw->mip_level != 0u || draw->array_layer != 0u)
            return RIN_GPU_ERROR_UNSUPPORTED;
        result = sw_make_implicit_render_pass(image, &implicit_pass);
        if (result != RIN_GPU_OK) return result;
        render_pass = &implicit_pass;
    } else if (render_pass->color_cookie != draw->color_target_cookie ||
               render_pass->color_mip_level != draw->mip_level ||
               render_pass->color_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    if (!pipeline || !image ||
        (vertex_bindings.binding_count != 0u && !vertex_buffer) ||
        !index_buffer ||
        draw->instance_count == 0u ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_instance > UINT32_MAX - draw->instance_count ||
        index_bytes == 0u ||
        (draw->index_offset & (uint64_t)(index_bytes - 1u)) != 0u ||
        draw->index_count > SW_MAX_DRAW_INDICES ||
        draw->vertex_count == 0u || draw->vertex_count > SW_MAX_DRAW_VERTICES ||
        draw->first_index > UINT32_MAX - draw->index_count) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (!sw_draw_work_is_bounded(draw->index_count, draw->instance_count))
        return RIN_GPU_ERROR_UNSUPPORTED;
    if (!restart_enabled) {
        result = sw_primitive_count(pipeline->desc.primitive_topology,
                                   draw->index_count, &triangle_count);
        if (result != RIN_GPU_OK)
            return result;
    }
    if (image->allocation_desc.mip_levels != 0u) {
        result = sw_color_target_valid(render_pass->color);
        if (result != RIN_GPU_OK)
            return result;
    }
    if ((pipeline->desc.resource_count == 0u && bind_group != NULL) ||
        (pipeline->desc.resource_count != 0u &&
         (!bind_group || bind_group->pipeline != pipeline ||
          bind_group->binding_count != pipeline->desc.resource_count))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_pipeline_shader_io(pipeline, &vertex_outputs, &fragment_inputs,
                                   &fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    result = sw_render_pass_fragment_outputs_supported(render_pass,
                                                       fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    /* Check the complete indirection stream before accessing even one vertex.
     * The preflight/program pass below then preserves target atomicity for a
     * later vertex or fragment failure. */
    for (uint32_t instance = 0u; instance < draw->instance_count;
         ++instance) {
        for (uint32_t index = 0u; index < draw->index_count; ++index) {
            uint32_t vertex_index;
            float inputs[SW_MAX_IO];

            result = sw_read_index(index_buffer, draw->index_format,
                                   draw->index_offset, draw->first_index, index,
                                   &vertex_index);
            if (result != RIN_GPU_OK) return result;
            if (restart_enabled && vertex_index == restart_index)
                continue;
            result = sw_rebase_vertex_index(vertex_index, base_vertex,
                                            draw->vertex_count, &vertex_index);
            if (result != RIN_GPU_OK) return result;
            result = sw_load_vertex_inputs(pipeline, &vertex_bindings,
                                           vertex_index, instance,
                                           draw->first_instance, inputs);
            if (result != RIN_GPU_OK) return result;
        }
    }
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        uint64_t fragment_invocations = 0u;

        for (uint32_t instance = 0u; instance < draw->instance_count;
             ++instance) {
            if (!restart_enabled) {
                for (uint32_t index = 0u; index < triangle_count; ++index) {
                    result = sw_draw_indexed_primitive(
                        pipeline, render_pass, &vertex_bindings, index_buffer,
                        draw->index_format, draw->index_offset,
                        draw->first_index, draw->index_count, base_vertex,
                        draw->vertex_count, index, instance,
                        draw->first_instance, bind_group, vertex_outputs,
                        fragment_inputs, fragment_outputs, pass != 0u,
                        &fragment_invocations);
                    if (result != RIN_GPU_OK) return result;
                }
            } else {
                uint32_t segment_start = 0u;

                for (uint32_t scan = 0u; scan <= draw->index_count; ++scan) {
                    uint32_t raw_index = restart_index;
                    uint32_t segment_count;
                    int boundary = scan == draw->index_count;

                    if (!boundary) {
                        result = sw_read_index(
                            index_buffer, draw->index_format,
                            draw->index_offset, draw->first_index, scan,
                            &raw_index);
                        if (result != RIN_GPU_OK) return result;
                        boundary = raw_index == restart_index;
                    }
                    if (!boundary) continue;
                    if (scan > segment_start) {
                        result = sw_primitive_count(
                            pipeline->desc.primitive_topology,
                            scan - segment_start, &segment_count);
                        if (result != RIN_GPU_OK) return result;
                        for (uint32_t index = 0u; index < segment_count;
                             ++index) {
                            result = sw_draw_indexed_primitive(
                                pipeline, render_pass, &vertex_bindings,
                                index_buffer, draw->index_format,
                                draw->index_offset,
                                draw->first_index + segment_start,
                                scan - segment_start, base_vertex,
                                draw->vertex_count, index, instance,
                                draw->first_instance, bind_group,
                                vertex_outputs, fragment_inputs,
                                fragment_outputs, pass != 0u,
                                &fragment_invocations);
                            if (result != RIN_GPU_OK) return result;
                        }
                    }
                    segment_start = scan + 1u;
                }
            }
        }
    }
    return RIN_GPU_OK;
}

static int sw_draw_indexed_in_pass(const RinGpuBackendDrawIndexedV1* draw,
                                   const SwRenderPass* active_pass)
{
    return sw_draw_indexed_with_base_vertex_in_pass(draw, 0,
                                                     active_pass);
}

static int sw_draw_indexed_base_vertex_in_pass(
    const RinGpuBackendDrawIndexedBaseVertexV1* draw,
    const SwRenderPass* active_pass)
{
    RinGpuBackendDrawIndexedV1 indexed;

    if (!draw)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(&indexed, 0, sizeof(indexed));
    indexed.pipeline_cookie = draw->pipeline_cookie;
    indexed.color_target_cookie = draw->color_target_cookie;
    indexed.bind_group_cookie = draw->bind_group_cookie;
    indexed.vertex_buffer_cookie = draw->vertex_buffer_cookie;
    indexed.index_buffer_cookie = draw->index_buffer_cookie;
    indexed.vertex_offset = draw->vertex_offset;
    indexed.index_offset = draw->index_offset;
    indexed.vertex_stride = draw->vertex_stride;
    indexed.index_format = draw->index_format;
    indexed.mip_level = draw->mip_level;
    indexed.array_layer = draw->array_layer;
    indexed.index_count = draw->index_count;
    indexed.instance_count = draw->instance_count;
    indexed.first_index = draw->first_index;
    indexed.vertex_count = draw->vertex_count;
    indexed.first_instance = draw->first_instance;
    return sw_draw_indexed_with_base_vertex_in_pass(
        &indexed, draw->base_vertex, active_pass);
}

static int sw_draw_indexed_v2_in_pass(const RinGpuBackendDrawIndexedV2* draw,
                                      int32_t base_vertex,
                                      const SwRenderPass* active_pass)
{
    SwPipeline* pipeline;
    SwImage* image;
    SwRenderPass implicit_pass;
    const SwRenderPass* render_pass = active_pass;
    SwBuffer* index_buffer;
    SwVertexBindings vertex_bindings;
    const SwGraphicsBindGroup* bind_group;
    uint32_t vertex_outputs;
    uint32_t fragment_inputs;
    uint32_t fragment_outputs;
    uint32_t index_bytes;
    uint32_t triangle_count;
    int result;

    if (!draw) return RIN_GPU_ERROR_INVALID_ARGUMENT;
    pipeline = (SwPipeline*)(uintptr_t)draw->pipeline_cookie;
    image = (SwImage*)(uintptr_t)draw->color_target_cookie;
    index_buffer = (SwBuffer*)(uintptr_t)draw->index_buffer_cookie;
    bind_group = (const SwGraphicsBindGroup*)(uintptr_t)draw->bind_group_cookie;
    index_bytes = sw_index_format_bytes(draw->index_format);
    if (render_pass == NULL) {
        if (draw->mip_level != 0u || draw->array_layer != 0u)
            return RIN_GPU_ERROR_UNSUPPORTED;
        result = sw_make_implicit_render_pass(image, &implicit_pass);
        if (result != RIN_GPU_OK) return result;
        render_pass = &implicit_pass;
    } else if (render_pass->color_cookie != draw->color_target_cookie ||
               render_pass->color_mip_level != draw->mip_level ||
               render_pass->color_array_layer != draw->array_layer) {
        return RIN_GPU_ERROR_STATE;
    }
    if (!pipeline || !image || !index_buffer || draw->instance_count == 0u ||
        draw->instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
        draw->first_instance > UINT32_MAX - draw->instance_count ||
        draw->reserved != 0u ||
        index_bytes == 0u ||
        (draw->index_offset & (uint64_t)(index_bytes - 1u)) != 0u ||
        draw->index_count > SW_MAX_DRAW_INDICES ||
        draw->vertex_count == 0u || draw->vertex_count > SW_MAX_DRAW_VERTICES ||
        draw->first_index > UINT32_MAX - draw->index_count) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (!sw_draw_work_is_bounded(draw->index_count, draw->instance_count))
        return RIN_GPU_ERROR_UNSUPPORTED;
    result = sw_primitive_count(pipeline->desc.primitive_topology,
                               draw->index_count, &triangle_count);
    if (result != RIN_GPU_OK)
        return result;
    if (image->allocation_desc.mip_levels != 0u) {
        result = sw_color_target_valid(render_pass->color);
        if (result != RIN_GPU_OK)
            return result;
    }
    if ((pipeline->desc.resource_count == 0u && bind_group != NULL) ||
        (pipeline->desc.resource_count != 0u &&
         (!bind_group || bind_group->pipeline != pipeline ||
          bind_group->binding_count != pipeline->desc.resource_count))) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    result = sw_vertex_bindings_from_v2(pipeline, draw->vertex_buffers,
                                        draw->vertex_binding_count,
                                        &vertex_bindings);
    if (result != RIN_GPU_OK) return result;
    result = sw_pipeline_shader_io(pipeline, &vertex_outputs, &fragment_inputs,
                                   &fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    result = sw_render_pass_fragment_outputs_supported(render_pass,
                                                       fragment_outputs);
    if (result != RIN_GPU_OK) return result;
    for (uint32_t instance = 0u; instance < draw->instance_count;
         ++instance) {
        for (uint32_t index = 0u; index < draw->index_count; ++index) {
            uint32_t vertex_index;
            float inputs[SW_MAX_IO];

            result = sw_read_index(index_buffer, draw->index_format,
                                   draw->index_offset, draw->first_index, index,
                                   &vertex_index);
            if (result != RIN_GPU_OK) return result;
            result = sw_rebase_vertex_index(vertex_index, base_vertex,
                                             draw->vertex_count, &vertex_index);
            if (result != RIN_GPU_OK) return result;
            result = sw_load_vertex_inputs(pipeline, &vertex_bindings,
                                           vertex_index, instance,
                                           draw->first_instance, inputs);
            if (result != RIN_GPU_OK) return result;
        }
    }
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        uint64_t fragment_invocations = 0u;

        for (uint32_t instance = 0u; instance < draw->instance_count;
             ++instance) {
            for (uint32_t index = 0u; index < triangle_count; ++index) {
                result = sw_draw_indexed_primitive(
                    pipeline, render_pass, &vertex_bindings, index_buffer,
                    draw->index_format, draw->index_offset, draw->first_index,
                    draw->index_count, base_vertex, draw->vertex_count, index, instance,
                    draw->first_instance, bind_group, vertex_outputs,
                    fragment_inputs,
                    fragment_outputs, pass != 0u, &fragment_invocations);
                if (result != RIN_GPU_OK) return result;
            }
        }
    }
    return RIN_GPU_OK;
}

/* Direct entry points remain useful for deterministic unit tests, which
 * include this implementation translation unit. Production command submission
 * always supplies the active pass, so these test-only wrappers are deliberately
 * not referenced by the freestanding object. */
static int __attribute__((unused))
sw_draw_vertices(const RinGpuBackendDrawVerticesV1* draw)
{
    return sw_draw_vertices_in_pass(draw, NULL);
}

/* The original DRAW command has no vertex-buffer field.  Core validation
 * deliberately admits it only for a constant-only pipeline, so execute it by
 * translating the backend-private form into the already atomic V1 vertex
 * executor.  Do not treat a missing streamed binding as a zero-filled buffer:
 * any pipeline which declares vertex inputs remains unsupported here. */
static int sw_draw_constant_vertices_in_pass(const RinGpuBackendDrawV1* draw,
                                             const SwRenderPass* active_pass)
{
    RinGpuBackendDrawVerticesV1 vertices;
    const SwPipeline* pipeline;

    if (!draw || draw->reserved0 != 0u || draw->reserved1 != 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    pipeline = (const SwPipeline*)(uintptr_t)draw->pipeline_cookie;
    if (!pipeline || pipeline->desc.vertex_input_count != 0u ||
        pipeline->desc.vertex_stride != 0u ||
        pipeline->desc.vertex_binding_count != 0u) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    memset(&vertices, 0, sizeof(vertices));
    vertices.pipeline_cookie = draw->pipeline_cookie;
    vertices.color_target_cookie = draw->color_target_cookie;
    vertices.bind_group_cookie = draw->bind_group_cookie;
    vertices.mip_level = draw->mip_level;
    vertices.array_layer = draw->array_layer;
    vertices.vertex_count = draw->vertex_count;
    vertices.instance_count = draw->instance_count;
    vertices.first_vertex = draw->first_vertex;
    vertices.first_instance = draw->first_instance;
    return sw_draw_vertices_in_pass(&vertices, active_pass);
}

static int __attribute__((unused))
sw_draw_vertices_v2(const RinGpuBackendDrawVerticesV2* draw)
{
    return sw_draw_vertices_v2_in_pass(draw, NULL);
}

static int __attribute__((unused))
sw_draw_indexed(const RinGpuBackendDrawIndexedV1* draw)
{
    return sw_draw_indexed_in_pass(draw, NULL);
}

static int __attribute__((unused))
sw_draw_indexed_v2(const RinGpuBackendDrawIndexedV2* draw)
{
    return sw_draw_indexed_v2_in_pass(draw, 0, NULL);
}

typedef struct SwComputeShadow {
    SwBuffer* buffer;
    uint8_t* bytes;
} SwComputeShadow;

static SwComputeShadow* sw_compute_shadow_for(SwComputeShadow* shadows,
                                              uint32_t shadow_count,
                                              const SwBuffer* buffer)
{
    for (uint32_t index = 0u; index < shadow_count; ++index) {
        if (shadows[index].buffer == buffer)
            return &shadows[index];
    }
    return NULL;
}

/* Dispatches in a deterministic z/y/x workgroup and z/y/x local order. This
 * is not a claim about parallel scheduling; it is the software backend's
 * defined execution order. Writable physical buffers are copied before any
 * invocation starts, and only copied back after every invocation succeeds.
 * Thus a dynamic OOB index, domain error, or allocation failure cannot leak a
 * prefix of storage writes into the caller-visible buffer. */
static int sw_dispatch_compute(RinGpuSoftwareBackend* backend,
                               const RinGpuBackendDispatchV1* dispatch,
                               const uint8_t* push_constants,
                               uint32_t push_constant_size,
                               uint64_t* invocation_count_out)
{
    const SwComputePipeline* pipeline;
    const SwComputeBindGroup* group;
    const SwShader* shader;
    const RinShaderHeaderV1* header;
    SwComputeShadow shadows[RIN_SHADER_MAX_RESOURCES];
    SwComputeExecution execution;
    uint64_t workgroup_invocations;
    uint64_t dispatch_invocations;
    uint64_t shadow_bytes = 0u;
    uint32_t shadow_count = 0u;
    uint8_t* shared_memory = NULL;
    int result = RIN_GPU_OK;

    if (invocation_count_out != NULL) *invocation_count_out = 0u;
    if (!dispatch || dispatch->pipeline_cookie == 0u ||
        dispatch->bind_group_cookie == 0u || dispatch->group_count_x == 0u ||
        dispatch->group_count_y == 0u || dispatch->group_count_z == 0u ||
        dispatch->reserved != 0u) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    pipeline = (const SwComputePipeline*)(uintptr_t)dispatch->pipeline_cookie;
    group = (const SwComputeBindGroup*)(uintptr_t)dispatch->bind_group_cookie;
    if (!pipeline || !group || group->pipeline != pipeline ||
        !pipeline->shader || !pipeline->shader->ir ||
        pipeline->shader->ir_size < sizeof(*header)) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    shader = pipeline->shader;
    header = (const RinShaderHeaderV1*)shader->ir;
    if (header->stage != RIN_SHADER_STAGE_COMPUTE ||
        group->binding_count != header->resource_count ||
        header->workgroup_x == 0u || header->workgroup_y == 0u ||
        header->workgroup_z == 0u ||
        !sw_multiply_u64(header->workgroup_x, header->workgroup_y,
                         &workgroup_invocations) ||
        !sw_multiply_u64(workgroup_invocations, header->workgroup_z,
                         &workgroup_invocations) ||
        !sw_multiply_u64(dispatch->group_count_x, dispatch->group_count_y,
                         &dispatch_invocations) ||
        !sw_multiply_u64(dispatch_invocations, dispatch->group_count_z,
                         &dispatch_invocations) ||
        !sw_multiply_u64(dispatch_invocations, workgroup_invocations,
                         &dispatch_invocations) ||
        dispatch_invocations > SW_MAX_COMPUTE_INVOCATIONS) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if ((header->flags & RIN_SHADER_FLAG_WORKGROUP_SHARED) != 0u &&
        (header->reserved0 == 0u ||
         header->reserved0 > RIN_SHADER_MAX_WORKGROUP_SHARED_BYTES ||
         workgroup_invocations != 1u)) {
        return RIN_GPU_ERROR_UNSUPPORTED;
    }
    if (invocation_count_out != NULL)
        *invocation_count_out = dispatch_invocations;
    memset(shadows, 0, sizeof(shadows));
    memset(&execution, 0, sizeof(execution));
    execution.bind_group = group;
    execution.push_constants = push_constants;
    execution.push_constant_size = push_constant_size;
    if ((header->flags & RIN_SHADER_FLAG_WORKGROUP_SHARED) != 0u) {
        shared_memory = sw_alloc(backend, header->reserved0, 1);
        if (!shared_memory) {
            result = RIN_GPU_ERROR_NO_MEMORY;
            goto done;
        }
        execution.shared_memory = shared_memory;
        execution.shared_memory_size = header->reserved0;
    }

    for (uint32_t binding = 0u; binding < group->binding_count; ++binding) {
        SwBuffer* buffer = group->buffers[binding];
        SwComputeShadow* shadow;

        if (!buffer || !buffer->bytes ||
            group->offsets[binding] > buffer->size_bytes ||
            group->sizes[binding] >
                buffer->size_bytes - group->offsets[binding]) {
            result = RIN_GPU_ERROR_INVALID_ARGUMENT;
            goto done;
        }
        if ((group->access[binding] & RIN_GPU_RESOURCE_WRITE) == 0u)
            continue;
        shadow = sw_compute_shadow_for(shadows, shadow_count, buffer);
        if (shadow != NULL)
            continue;
        if (shadow_count == RIN_SHADER_MAX_RESOURCES ||
            buffer->size_bytes > SW_MAX_COMPUTE_SHADOW_BYTES - shadow_bytes ||
            buffer->size_bytes > SIZE_MAX) {
            result = RIN_GPU_ERROR_UNSUPPORTED;
            goto done;
        }
        shadows[shadow_count].bytes = sw_alloc(backend, buffer->size_bytes, 0);
        if (!shadows[shadow_count].bytes) {
            result = RIN_GPU_ERROR_NO_MEMORY;
            goto done;
        }
        memcpy(shadows[shadow_count].bytes, buffer->bytes,
               (size_t)buffer->size_bytes);
        shadows[shadow_count].buffer = buffer;
        shadow_bytes += buffer->size_bytes;
        ++shadow_count;
    }
    for (uint32_t binding = 0u; binding < group->binding_count; ++binding) {
        SwComputeShadow* shadow = sw_compute_shadow_for(
            shadows, shadow_count, group->buffers[binding]);

        execution.storage[binding] = shadow != NULL
            ? shadow->bytes : group->buffers[binding]->bytes;
    }
    for (uint32_t workgroup_z = 0u;
         workgroup_z < dispatch->group_count_z; ++workgroup_z) {
        for (uint32_t workgroup_y = 0u;
             workgroup_y < dispatch->group_count_y; ++workgroup_y) {
            for (uint32_t workgroup_x = 0u;
                 workgroup_x < dispatch->group_count_x; ++workgroup_x) {
                if (shared_memory != NULL) {
                    memset(shared_memory, 0, header->reserved0);
                    execution.shared_epoch = 0u;
                }
                for (uint32_t local_z = 0u; local_z < header->workgroup_z;
                     ++local_z) {
                    for (uint32_t local_y = 0u;
                         local_y < header->workgroup_y; ++local_y) {
                        for (uint32_t local_x = 0u;
                             local_x < header->workgroup_x; ++local_x) {
                            float f32_outputs[SW_MAX_IO];
                            int32_t i32_outputs[SW_MAX_IO];
                            uint8_t output_types[SW_MAX_IO];
                            SwShaderIo io;

                            memset(&io, 0, sizeof(io));
                            execution.global_x =
                                workgroup_x * header->workgroup_x + local_x;
                            execution.global_y =
                                workgroup_y * header->workgroup_y + local_y;
                            execution.global_z =
                                workgroup_z * header->workgroup_z + local_z;
                            execution.local_x = local_x;
                            execution.local_y = local_y;
                            execution.local_z = local_z;
                            execution.workgroup_x = workgroup_x;
                            execution.workgroup_y = workgroup_y;
                            execution.workgroup_z = workgroup_z;
                            io.f32_outputs = f32_outputs;
                            io.i32_outputs = i32_outputs;
                            io.output_types = output_types;
                            io.output_count = header->output_count;
                            io.compute_execution = &execution;
                            io.push_constants = execution.push_constants;
                            io.push_constant_size = execution.push_constant_size;
                            result = sw_run_shader_typed(shader, &io);
                            if (result != RIN_GPU_OK)
                                goto done;
                        }
                    }
                }
            }
        }
    }
    for (uint32_t index = 0u; index < shadow_count; ++index) {
        memcpy(shadows[index].buffer->bytes, shadows[index].bytes,
               (size_t)shadows[index].buffer->size_bytes);
    }

done:
    if (shared_memory != NULL)
        sw_free(backend, shared_memory, header->reserved0);
    for (uint32_t index = 0u; index < shadow_count; ++index) {
        sw_free(backend, shadows[index].bytes,
                shadows[index].buffer->size_bytes);
    }
    return result;
}

static int sw_indirect_packet_offset(const SwBuffer* buffer,
                                     uint64_t base, uint32_t index,
                                     uint32_t stride, uint32_t packet_size,
                                     uint64_t* offset_out)
{
    uint64_t delta;
    uint64_t offset;
    if (!buffer || !offset_out || stride < packet_size ||
        !sw_multiply_u64(index, stride, &delta) ||
        !sw_add_u64(base, delta, &offset) ||
        offset > buffer->size_bytes ||
        packet_size > buffer->size_bytes - offset) {
        return RIN_GPU_ERROR_BOUNDS;
    }
    *offset_out = offset;
    return RIN_GPU_OK;
}

static int sw_indirect_read_u32(const SwBuffer* buffer, uint64_t offset,
                                uint32_t* value_out)
{
    if (!buffer || !value_out || offset > buffer->size_bytes ||
        sizeof(uint32_t) > buffer->size_bytes - offset)
        return RIN_GPU_ERROR_BOUNDS;
    memcpy(value_out, buffer->bytes + offset, sizeof(*value_out));
    return RIN_GPU_OK;
}

static int sw_draw_indirect(RinGpuSoftwareBackend* backend,
                            const RinGpuBackendDrawIndirectV1* indirect,
                            const SwRenderPass* active_pass,
                            uint32_t* executed_out)
{
    SwBuffer* buffer;
    uint32_t executed = 0u;
    int result;
    if (!backend || !indirect || !executed_out ||
        indirect->draw_count == 0u ||
        indirect->draw_count > RIN_GPU_MAX_INDIRECT_COMMANDS) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    buffer = (SwBuffer*)(uintptr_t)indirect->indirect_buffer_cookie;
    if (!buffer || !buffer->bytes || indirect->vertex_binding_count >
        RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    /* Preflight the complete argument stream before publishing any draw. */
    for (uint32_t index = 0u; index < indirect->draw_count; ++index) {
        uint64_t offset;
        uint32_t vertex_count;
        uint32_t instance_count;
        uint32_t first_vertex;
        uint32_t first_instance;
        result = sw_indirect_packet_offset(buffer, indirect->indirect_offset,
                                           index, indirect->stride, 16u,
                                           &offset);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset, &vertex_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 4u, &instance_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 8u, &first_vertex);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 12u, &first_instance);
        if (result != RIN_GPU_OK) return result;
        if ((vertex_count != 0u && instance_count != 0u) &&
            (vertex_count > RIN_GPU_MAX_DRAW_VERTICES ||
             instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
             first_vertex > UINT32_MAX - vertex_count ||
             first_instance > UINT32_MAX - instance_count)) {
            return RIN_GPU_ERROR_BOUNDS;
        }
    }
    for (uint32_t index = 0u; index < indirect->draw_count; ++index) {
        RinGpuBackendDrawVerticesV2 draw;
        uint64_t offset;
        result = sw_indirect_packet_offset(buffer, indirect->indirect_offset,
                                           index, indirect->stride, 16u,
                                           &offset);
        if (result != RIN_GPU_OK) return result;
        memset(&draw, 0, sizeof(draw));
        draw.pipeline_cookie = indirect->pipeline_cookie;
        draw.color_target_cookie = indirect->color_target_cookie;
        draw.bind_group_cookie = indirect->bind_group_cookie;
        draw.mip_level = indirect->mip_level;
        draw.array_layer = indirect->array_layer;
        result = sw_indirect_read_u32(buffer, offset, &draw.vertex_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 4u,
                                      &draw.instance_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 8u, &draw.first_vertex);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 12u,
                                      &draw.first_instance);
        if (result != RIN_GPU_OK) return result;
        draw.vertex_binding_count = indirect->vertex_binding_count;
        memcpy(draw.vertex_buffers, indirect->vertex_buffers,
               sizeof(draw.vertex_buffers));
        if (draw.vertex_count == 0u || draw.instance_count == 0u)
            continue;
        result = sw_draw_vertices_v2_in_pass(
            &draw, active_pass != NULL ? active_pass : NULL);
        if (result != RIN_GPU_OK) return result;
        result = sw_query_record_draw(backend, draw.vertex_count,
                                      draw.instance_count);
        if (result != RIN_GPU_OK) return result;
        ++executed;
    }
    *executed_out = executed;
    return RIN_GPU_OK;
}

static int sw_draw_indexed_indirect(
    RinGpuSoftwareBackend* backend,
    const RinGpuBackendDrawIndexedIndirectV1* indirect,
    const SwRenderPass* active_pass, uint32_t* executed_out)
{
    SwBuffer* buffer;
    uint32_t executed = 0u;
    int result;
    if (!backend || !indirect || !executed_out || indirect->draw_count == 0u ||
        indirect->draw_count > RIN_GPU_MAX_INDIRECT_COMMANDS ||
        indirect->vertex_count == 0u ||
        indirect->vertex_count > RIN_GPU_MAX_DRAW_VERTICES) {
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    }
    buffer = (SwBuffer*)(uintptr_t)indirect->indirect_buffer_cookie;
    if (!buffer || !buffer->bytes || indirect->vertex_binding_count >
        RIN_GPU_MAX_VERTEX_BUFFER_BINDINGS)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < indirect->draw_count; ++index) {
        uint64_t offset;
        uint32_t index_count;
        uint32_t instance_count;
        uint32_t first_index;
        uint32_t first_instance;
        int32_t base_vertex;
        result = sw_indirect_packet_offset(buffer, indirect->indirect_offset,
                                           index, indirect->stride, 20u,
                                           &offset);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset, &index_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 4u, &instance_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 8u, &first_index);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 12u,
                                      (uint32_t*)&base_vertex);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 16u, &first_instance);
        if (result != RIN_GPU_OK) return result;
        if ((index_count != 0u && instance_count != 0u) &&
            (index_count > SW_MAX_DRAW_INDICES ||
             instance_count > RIN_GPU_MAX_DRAW_INSTANCES ||
             first_index > UINT32_MAX - index_count ||
             first_instance > UINT32_MAX - instance_count)) {
            return RIN_GPU_ERROR_BOUNDS;
        }
        if (index_count != 0u && instance_count != 0u) {
            uint32_t raw_index;
            result = sw_read_index(
                (const SwBuffer*)(uintptr_t)indirect->index_buffer_cookie,
                indirect->index_format, indirect->index_offset, first_index,
                0u, &raw_index);
            if (result != RIN_GPU_OK && result != RIN_GPU_ERROR_BOUNDS)
                return result;
            (void)raw_index;
        }
    }
    for (uint32_t index = 0u; index < indirect->draw_count; ++index) {
        RinGpuBackendDrawIndexedV2 draw;
        uint64_t offset;
        result = sw_indirect_packet_offset(buffer, indirect->indirect_offset,
                                           index, indirect->stride, 20u,
                                           &offset);
        if (result != RIN_GPU_OK) return result;
        memset(&draw, 0, sizeof(draw));
        draw.pipeline_cookie = indirect->pipeline_cookie;
        draw.color_target_cookie = indirect->color_target_cookie;
        draw.bind_group_cookie = indirect->bind_group_cookie;
        draw.index_buffer_cookie = indirect->index_buffer_cookie;
        draw.index_offset = indirect->index_offset;
        draw.index_format = indirect->index_format;
        draw.mip_level = indirect->mip_level;
        draw.array_layer = indirect->array_layer;
        result = sw_indirect_read_u32(buffer, offset, &draw.index_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 4u,
                                      &draw.instance_count);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 8u, &draw.first_index);
        if (result != RIN_GPU_OK) return result;
        result = sw_indirect_read_u32(buffer, offset + 16u,
                                      &draw.first_instance);
        if (result != RIN_GPU_OK) return result;
        draw.vertex_count = indirect->vertex_count;
        draw.vertex_binding_count = indirect->vertex_binding_count;
        memcpy(draw.vertex_buffers, indirect->vertex_buffers,
               sizeof(draw.vertex_buffers));
        if (draw.index_count == 0u || draw.instance_count == 0u)
            continue;
        {
            uint32_t base_vertex_bits;
            int32_t base_vertex;
            result = sw_indirect_read_u32(buffer, offset + 12u,
                                          &base_vertex_bits);
            if (result != RIN_GPU_OK) return result;
            memcpy(&base_vertex, &base_vertex_bits, sizeof(base_vertex));
            result = sw_draw_indexed_v2_in_pass(&draw, base_vertex,
                                            active_pass != NULL ? active_pass : NULL);
        }
        if (result != RIN_GPU_OK) return result;
        result = sw_query_record_draw(backend, draw.vertex_count,
                                      draw.instance_count);
        if (result != RIN_GPU_OK) return result;
        ++executed;
    }
    *executed_out = executed;
    return RIN_GPU_OK;
}

static int sw_dispatch_indirect(RinGpuSoftwareBackend* backend,
                                const RinGpuBackendDispatchIndirectV1* indirect,
                                const uint8_t* push_constants,
                                uint32_t push_constant_size,
                                uint64_t* invocations_out)
{
    SwBuffer* buffer;
    uint32_t groups[3];
    RinGpuBackendDispatchV1 dispatch;
    int result;
    if (!backend || !indirect || !invocations_out)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    buffer = (SwBuffer*)(uintptr_t)indirect->indirect_buffer_cookie;
    if (!buffer || !buffer->bytes)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < 3u; ++index) {
        result = sw_indirect_read_u32(buffer, indirect->indirect_offset +
                                      (uint64_t)index * sizeof(uint32_t),
                                      &groups[index]);
        if (result != RIN_GPU_OK) return result;
        if (groups[index] == 0u || groups[index] > RIN_GPU_MAX_DISPATCH_GROUPS)
            return RIN_GPU_ERROR_BOUNDS;
    }
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.pipeline_cookie = indirect->pipeline_cookie;
    dispatch.bind_group_cookie = indirect->bind_group_cookie;
    dispatch.group_count_x = groups[0];
    dispatch.group_count_y = groups[1];
    dispatch.group_count_z = groups[2];
    return sw_dispatch_compute(backend, &dispatch, push_constants,
                               push_constant_size, invocations_out);
}

static int sw_submit(void* opaque, const RinGpuBackendCommandV1* commands,
                     uint32_t command_count)
{
    RinGpuSoftwareBackend* backend = opaque;
    uint32_t index;
    SwRenderPass active_pass;
    uint8_t push_constants[RIN_SHADER_PUSH_CONSTANT_BYTES];
    uint32_t push_constant_size = 0u;
    RinGpuSoftwareBackendStatsV1 delta;
    uint64_t invocation_count = 0u;
    int result;
    if (!commands && command_count != 0u)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    memset(&active_pass, 0, sizeof(active_pass));
    memset(push_constants, 0, sizeof(push_constants));
    memset(&delta, 0, sizeof(delta));
    for (index = 0u; index < command_count; ++index) {
        const RinGpuBackendCommandV1* command = &commands[index];
        if (backend != NULL) {
            if (backend->timestamp_ticks == UINT64_MAX)
                return RIN_GPU_ERROR_LIMIT;
            ++backend->timestamp_ticks;
        }
        active_pass.push_constants = push_constant_size != 0u
            ? push_constants : NULL;
        active_pass.push_constant_size = push_constant_size;
        active_pass.backend = backend;
        switch (command->type) {
        case RIN_GPU_BACKEND_COMMAND_BEGIN_QUERY:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_query_begin(backend, &command->value.query);
            if (result != RIN_GPU_OK) return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_END_QUERY:
            result = sw_query_end(backend, &command->value.query);
            if (result != RIN_GPU_OK) return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_RESET_QUERY:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_query_reset(backend, &command->value.query);
            if (result != RIN_GPU_OK) return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_COPY_BUFFER:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_copy_buffer(&command->value.buffer_copy);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.copy_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_CLEAR_BUFFER:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_clear_buffer(&command->value.buffer_clear);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.copy_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_COPY_IMAGE:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_copy_image(&command->value.image_copy);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.copy_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_BLIT_IMAGE:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_blit_image(&command->value.image_blit);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.copy_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_RESOLVE_IMAGE:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_resolve_image(&command->value.image_resolve);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.copy_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_CLEAR_IMAGE:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_clear_image(&command->value.image_clear);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.copy_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_TRANSITION_IMAGE:
            ++delta.transition_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_TRANSFER_IMAGE_OWNERSHIP:
            if (active_pass.color != NULL ||
                command->value.image_ownership_transfer.image_cookie == 0u ||
                command->value.image_ownership_transfer.transfer.flags != 0u ||
                command->value.image_ownership_transfer.transfer.reserved != 0u ||
                (command->value.image_ownership_transfer.transfer.source_family_index ==
                     command->value.image_ownership_transfer.transfer.destination_family_index &&
                 command->value.image_ownership_transfer.transfer.source_engine_index ==
                     command->value.image_ownership_transfer.transfer.destination_engine_index)) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            /* Ownership is committed transactionally by RinGPU core after
             * this backend accepts the command.  Keeping this marker in the
             * stream lets native backends lower it to their queue engine,
             * while the software profile has a concrete owner state rather
             * than an ignored transfer. */
            ++delta.transition_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_SET_PUSH_CONSTANTS:
            if (command->value.push_constants.flags != 0u ||
                command->value.push_constants.reserved != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            memcpy(push_constants, command->value.push_constants.data,
                   sizeof(push_constants));
            push_constant_size = sizeof(push_constants);
            break;
        case RIN_GPU_BACKEND_COMMAND_DISPATCH:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_dispatch_compute((RinGpuSoftwareBackend*)opaque,
                                         &command->value.dispatch,
                                         push_constant_size != 0u
                                             ? push_constants : NULL,
                                         push_constant_size,
                                         &invocation_count);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_add_pipeline(
                backend, RIN_GPU_PIPELINE_STAT_COMPUTE_SHADER_INVOCATIONS,
                invocation_count);
            if (result != RIN_GPU_OK) return result;
            result = sw_query_add_pipeline(
                backend, RIN_GPU_PIPELINE_STAT_DISPATCH_CALLS, 1u);
            if (result != RIN_GPU_OK) return result;
            ++delta.dispatch_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_DISPATCH_INDIRECT:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_dispatch_indirect(
                (RinGpuSoftwareBackend*)opaque,
                &command->value.dispatch_indirect,
                push_constant_size != 0u ? push_constants : NULL,
                push_constant_size, &invocation_count);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_add_pipeline(
                backend, RIN_GPU_PIPELINE_STAT_COMPUTE_SHADER_INVOCATIONS,
                invocation_count);
            if (result != RIN_GPU_OK) return result;
            result = sw_query_add_pipeline(
                backend, RIN_GPU_PIPELINE_STAT_DISPATCH_CALLS, 1u);
            if (result != RIN_GPU_OK) return result;
            ++delta.dispatch_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER:
            if (active_pass.color != NULL ||
                command->value.compute_barrier.source_access !=
                    RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                command->value.compute_barrier.destination_access !=
                    RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                command->value.compute_barrier.flags != 0u ||
                command->value.compute_barrier.reserved != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            /* Execution is synchronous and each dispatch commits atomically,
             * so the validated public barrier has no additional CPU work. */
            ++delta.barrier_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_COMPUTE_BARRIER_V2:
            if (active_pass.color != NULL ||
                command->value.compute_barrier_v2.source_stage == 0u ||
                command->value.compute_barrier_v2.destination_stage == 0u ||
                (command->value.compute_barrier_v2.source_stage &
                 ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
                (command->value.compute_barrier_v2.destination_stage &
                 ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
                command->value.compute_barrier_v2.source_access == 0u ||
                command->value.compute_barrier_v2.destination_access == 0u ||
                (command->value.compute_barrier_v2.source_access &
                 ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                (command->value.compute_barrier_v2.destination_access &
                 ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                command->value.compute_barrier_v2.flags != 0u ||
                command->value.compute_barrier_v2.reserved != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            ++delta.barrier_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER:
            if (active_pass.color == NULL)
                return RIN_GPU_ERROR_STATE;
            if (command->value.graphics_barrier.source_access !=
                    RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                command->value.graphics_barrier.destination_access !=
                    RIN_GPU_RESOURCE_KNOWN_ACCESS ||
                command->value.graphics_barrier.flags != 0u ||
                command->value.graphics_barrier.reserved != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            /* Raster work is completed synchronously before this command is
             * reached. Validation preserves the public ordering boundary
             * without fabricating an asynchronous cache operation. */
            ++delta.barrier_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_GRAPHICS_BARRIER_V2:
            if (active_pass.color == NULL ||
                command->value.graphics_barrier_v2.source_stage == 0u ||
                command->value.graphics_barrier_v2.destination_stage == 0u ||
                (command->value.graphics_barrier_v2.source_stage &
                 ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
                (command->value.graphics_barrier_v2.destination_stage &
                 ~RIN_GPU_PIPELINE_STAGE_KNOWN) != 0u ||
                command->value.graphics_barrier_v2.source_access == 0u ||
                command->value.graphics_barrier_v2.destination_access == 0u ||
                (command->value.graphics_barrier_v2.source_access &
                 ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                (command->value.graphics_barrier_v2.destination_access &
                 ~RIN_GPU_RESOURCE_KNOWN_ACCESS) != 0u ||
                command->value.graphics_barrier_v2.flags != 0u ||
                command->value.graphics_barrier_v2.reserved != 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            ++delta.barrier_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_begin_render_pass_active(
                &command->value.render_pass_begin, &active_pass);
            if (result != RIN_GPU_OK)
                return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_MRT:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_begin_render_pass_mrt(
                &command->value.render_pass_mrt_begin, &active_pass);
            if (result != RIN_GPU_OK)
                return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_begin_render_pass_depth(
                &command->value.render_pass_depth_begin, &active_pass);
            if (result != RIN_GPU_OK)
                return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_BEGIN_RENDER_PASS_DEPTH_STENCIL:
            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            result = sw_begin_render_pass_depth_stencil(
                &command->value.render_pass_depth_stencil_begin, &active_pass);
            if (result != RIN_GPU_OK)
                return result;
            break;
        case RIN_GPU_BACKEND_COMMAND_END_RENDER_PASS:
            if (active_pass.color == NULL)
                return RIN_GPU_ERROR_STATE;
            /* Active attachments are pass-local views, so there is no shared
             * backing-image selection to restore here. */
            memset(&active_pass, 0, sizeof(active_pass));
            break;
        case RIN_GPU_BACKEND_COMMAND_PRESENT: {
            SwImage* image;
            RinGpuSoftwarePresentedImageV1 presented;
            uint32_t bytes_per_pixel;

            if (active_pass.color != NULL)
                return RIN_GPU_ERROR_STATE;
            /* Preserve V1's no-op PRESENT behavior for direct executor tests
             * and embeddings that did not opt into publication. */
            if (!backend || backend->present_callback == NULL)
                break;
            /* Publishing is a submission boundary. Reject a trailing command
             * before exposing the completed image to an embedder. */
            if (index + 1u != command_count)
                return RIN_GPU_ERROR_STATE;
            image = (SwImage*)(uintptr_t)command->value.present.image_cookie;
            if (image != NULL) {
                result = sw_image_select_mip(image, 0u);
                if (result != RIN_GPU_OK)
                    return result;
            }
            bytes_per_pixel = image ? sw_image_bytes_per_pixel(image->desc.format)
                                    : 0u;
            if (!image || !image->bytes ||
                image->desc.dimension != RIN_GPU_IMAGE_DIMENSION_2D ||
                image->desc.depth != 1u || image->desc.array_layers != 1u ||
                image->desc.mip_levels != 1u || image->desc.sample_count != 1u ||
                bytes_per_pixel == 0u) {
                return RIN_GPU_ERROR_INVALID_ARGUMENT;
            }
            memset(&presented, 0, sizeof(presented));
            presented.struct_size = sizeof(presented);
            presented.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
            presented.pixels = image->bytes;
            presented.size_bytes = image->size_bytes;
            if (!sw_image_row_pitch(image, bytes_per_pixel,
                                    &presented.row_pitch_bytes))
                return RIN_GPU_ERROR_BOUNDS;
            presented.format = image->desc.format;
            presented.width = image->desc.width;
            presented.height = image->desc.height;
            presented.display_id = command->value.present.display_id;
            result = backend->present_callback(backend->present_context,
                                               &presented);
            if (result != RIN_GPU_OK)
                return result;
            ++delta.present_commands;
            if (backend->flags != 0u) {
                uint64_t hash = sw_hash_presented_image(
                    backend, image, presented.row_pitch_bytes);
                if (hash == 0u)
                    return RIN_GPU_ERROR_BOUNDS;
                backend->stats.output_hash = hash;
                ++backend->stats.output_hash_count;
            }
            break;
        }
        case RIN_GPU_BACKEND_COMMAND_SET_RASTER_STATE: {
            SwRasterState raster;

            if (active_pass.color == NULL)
                return RIN_GPU_ERROR_STATE;
            raster = active_pass.raster;
            result = sw_raster_state_from_command(
                &command->value.raster_state, active_pass.color, &raster);
            if (result != RIN_GPU_OK)
                return result;
            active_pass.raster = raster;
            break;
        }
        case RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES:
            if (active_pass.color != NULL &&
                command->value.draw_vertices.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_vertices_in_pass(&command->value.draw_vertices,
                                              active_pass.color != NULL
                                                  ? &active_pass : NULL);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_record_draw(
                backend, command->value.draw_vertices.vertex_count,
                command->value.draw_vertices.instance_count);
            if (result != RIN_GPU_OK) return result;
            ++delta.draw_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_VERTICES_V2:
            if (active_pass.color != NULL &&
                command->value.draw_vertices_v2.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_vertices_v2_in_pass(
                &command->value.draw_vertices_v2,
                active_pass.color != NULL ? &active_pass : NULL);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_record_draw(
                backend, command->value.draw_vertices_v2.vertex_count,
                command->value.draw_vertices_v2.instance_count);
            if (result != RIN_GPU_OK) return result;
            ++delta.draw_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDIRECT: {
            uint32_t executed = 0u;
            if (active_pass.color != NULL &&
                command->value.draw_indirect.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_indirect(
                backend, &command->value.draw_indirect,
                active_pass.color != NULL ? &active_pass : NULL, &executed);
            if (result != RIN_GPU_OK) return result;
            delta.draw_commands += executed;
            break;
        }
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_INDIRECT: {
            uint32_t executed = 0u;
            if (active_pass.color != NULL &&
                command->value.draw_indexed_indirect.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_indexed_indirect(
                backend, &command->value.draw_indexed_indirect,
                active_pass.color != NULL ? &active_pass : NULL, &executed);
            if (result != RIN_GPU_OK) return result;
            delta.draw_commands += executed;
            break;
        }
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED:
            if (active_pass.color != NULL &&
                command->value.draw_indexed.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_indexed_in_pass(&command->value.draw_indexed,
                                             active_pass.color != NULL
                                                 ? &active_pass : NULL);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_record_draw(
                backend, command->value.draw_indexed.vertex_count,
                command->value.draw_indexed.instance_count);
            if (result != RIN_GPU_OK) return result;
            ++delta.draw_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_V2:
            if (active_pass.color != NULL &&
                command->value.draw_indexed_v2.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_indexed_v2_in_pass(
                &command->value.draw_indexed_v2,
                0,
                active_pass.color != NULL ? &active_pass : NULL);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_record_draw(
                backend, command->value.draw_indexed_v2.vertex_count,
                command->value.draw_indexed_v2.instance_count);
            if (result != RIN_GPU_OK) return result;
            ++delta.draw_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW:
            if (active_pass.color != NULL &&
                command->value.draw.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_constant_vertices_in_pass(
                &command->value.draw,
                active_pass.color != NULL ? &active_pass : NULL);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_record_draw(
                backend, command->value.draw.vertex_count,
                command->value.draw.instance_count);
            if (result != RIN_GPU_OK) return result;
            ++delta.draw_commands;
            break;
        case RIN_GPU_BACKEND_COMMAND_DRAW_INDEXED_BASE_VERTEX:
            if (active_pass.color != NULL &&
                command->value.draw_indexed_base_vertex.color_target_cookie !=
                    active_pass.color_cookie) {
                return RIN_GPU_ERROR_STATE;
            }
            result = sw_draw_indexed_base_vertex_in_pass(
                &command->value.draw_indexed_base_vertex,
                active_pass.color != NULL ? &active_pass : NULL);
            if (result != RIN_GPU_OK)
                return result;
            result = sw_query_record_draw(
                backend, command->value.draw_indexed_base_vertex.vertex_count,
                command->value.draw_indexed_base_vertex.instance_count);
            if (result != RIN_GPU_OK) return result;
            ++delta.draw_commands;
            break;
        default:
            return RIN_GPU_ERROR_UNSUPPORTED;
        }
    }
    if (active_pass.color != NULL)
        return RIN_GPU_ERROR_STATE;
    if (backend &&
        (backend->flags & RIN_GPU_SOFTWARE_BACKEND_FLAG_COLLECT_STATS) != 0u) {
        backend->stats.submitted_commands += command_count;
        backend->stats.copy_commands += delta.copy_commands;
        backend->stats.draw_commands += delta.draw_commands;
        backend->stats.dispatch_commands += delta.dispatch_commands;
        backend->stats.transition_commands += delta.transition_commands;
        backend->stats.barrier_commands += delta.barrier_commands;
        backend->stats.present_commands += delta.present_commands;
    }
    return RIN_GPU_OK;
}

static const RinGpuBackendOpsV1 g_sw_ops = {
    .abi_version = RIN_GPU_ABI_VERSION,
    .struct_size = sizeof(RinGpuBackendOpsV1),
    .create_buffer = sw_create_buffer,
    .destroy_buffer = sw_destroy_buffer,
    .upload_buffer = sw_upload_buffer,
    .readback_buffer = sw_readback_buffer,
    .create_image = sw_create_image,
    .destroy_image = sw_destroy_image,
    .upload_image = sw_upload_image,
    .create_sampler = sw_create_sampler,
    .destroy_sampler = sw_destroy_sampler,
    .create_shader_module = sw_create_shader,
    .destroy_shader_module = sw_destroy_shader,
    .create_compute_pipeline = sw_create_compute_pipeline,
    .destroy_compute_pipeline = sw_destroy_compute_pipeline,
    .create_graphics_pipeline = sw_create_graphics_pipeline,
    .destroy_graphics_pipeline = sw_destroy_graphics_pipeline,
    .create_compute_bind_group = sw_create_compute_bind_group,
    .destroy_compute_bind_group = sw_destroy_compute_bind_group,
    .create_graphics_bind_group = sw_create_graphics_bind_group,
    .destroy_graphics_bind_group = sw_destroy_graphics_bind_group,
    .submit_commands = sw_submit,
    .get_query_result = sw_query_result,
    .get_timestamp_period = sw_timestamp_period,
    .destroy_query = sw_query_destroy,
    .wait_for_completion = sw_wait_for_completion,
    .readback_image = sw_readback_image,
};

int ringpu_software_backend_create(
    const RinGpuSoftwareBackendDescV1* desc,
    RinGpuSoftwareBackend** backend_out)
{
    RinGpuSoftwareBackend* backend;
    RinGpuSoftwareBackendDescV2 desc_v2;
    RinGpuSoftwareBackendDescV3 desc_v3;

    if (!desc || !backend_out || desc->max_total_bytes == 0u ||
        desc->reserved0 != 0u ||
        (desc->version != RIN_GPU_SOFTWARE_BACKEND_VERSION &&
         desc->version != RIN_GPU_SOFTWARE_BACKEND_VERSION_2 &&
         desc->version != RIN_GPU_SOFTWARE_BACKEND_VERSION_3 &&
         desc->version != RIN_GPU_SOFTWARE_BACKEND_VERSION_4) ||
        (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION &&
         desc->struct_size != sizeof(*desc)) ||
        (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_2 &&
         desc->struct_size != sizeof(desc_v2)) ||
        (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_3 &&
         desc->struct_size != sizeof(desc_v3)) ||
        (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_4 &&
         desc->struct_size != sizeof(RinGpuSoftwareBackendDescV4)) ||
         (desc->version != RIN_GPU_SOFTWARE_BACKEND_VERSION_4 &&
          desc->flags != 0u))
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    if (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_4 &&
        (desc->flags & RIN_GPU_SOFTWARE_BACKEND_FLAG_DETERMINISTIC) != 0u &&
        (sizeof(float) != 4u || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
         FLT_ROUNDS != 1))
        return RIN_GPU_ERROR_UNSUPPORTED;
    *backend_out = NULL;
    backend = calloc(1u, sizeof(*backend));
    if (!backend)
        return RIN_GPU_ERROR_NO_MEMORY;
    backend->max_total_bytes = desc->max_total_bytes;
    /* The backend object is caller-owned resource metadata just like every
     * typed object it creates. Count it before publishing the handle so an
     * untrusted embedding cannot bypass the budget with a tiny resource cap. */
    if (desc->max_total_bytes < sizeof(*backend)) {
        free(backend);
        return RIN_GPU_ERROR_NO_MEMORY;
    }
    backend->allocated_bytes = sizeof(*backend);
    backend->flags = desc->flags;
    if (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_2 ||
        desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_3 ||
        desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_4) {
        memcpy(&desc_v2, desc, sizeof(desc_v2));
        if (!desc_v2.present_callback) {
            free(backend);
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        backend->present_callback = desc_v2.present_callback;
        backend->present_context = desc_v2.present_context;
    }
    if (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_3 ||
        desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_4) {
        memcpy(&desc_v3, desc, sizeof(desc_v3));
        if (!desc_v3.acquire_image) {
            free(backend);
            return RIN_GPU_ERROR_INVALID_ARGUMENT;
        }
        backend->acquire_image = desc_v3.acquire_image;
        backend->image_context = desc_v3.image_context;
    }
    if (desc->version == RIN_GPU_SOFTWARE_BACKEND_VERSION_4) {
        RinGpuSoftwareBackendDescV4 desc_v4;

        memcpy(&desc_v4, desc, sizeof(desc_v4));
        if ((desc_v4.base.base.base.flags &
             ~RIN_GPU_SOFTWARE_BACKEND_FLAGS_KNOWN) != 0u ||
            desc_v4.reserved[0] != 0u || desc_v4.reserved[1] != 0u)
            goto fail_v4;
        backend->deterministic_seed = desc_v4.deterministic_seed;
    }
    backend->stats.struct_size = sizeof(backend->stats);
    backend->stats.version = RIN_GPU_SOFTWARE_BACKEND_VERSION;
    backend->stats.flags = backend->flags;
    backend->stats.deterministic_seed = backend->deterministic_seed;
    *backend_out = backend;
    return RIN_GPU_OK;

fail_v4:
    free(backend);
    *backend_out = NULL;
    return RIN_GPU_ERROR_INVALID_ARGUMENT;
}

void ringpu_software_backend_destroy(RinGpuSoftwareBackend* backend)
{
    free(backend);
}

int ringpu_software_backend_query_stats(
    const RinGpuSoftwareBackend* backend,
    RinGpuSoftwareBackendStatsV1* stats_out)
{
    if (!backend || !stats_out || stats_out->struct_size != sizeof(*stats_out) ||
        stats_out->version != RIN_GPU_SOFTWARE_BACKEND_VERSION)
        return RIN_GPU_ERROR_INVALID_ARGUMENT;
    *stats_out = backend->stats;
    return RIN_GPU_OK;
}

const RinGpuBackendOpsV1* ringpu_software_backend_ops(void)
{
    return &g_sw_ops;
}
