/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PUBLIC_SPIRV_FRONTEND_H
#define RINGPU_PUBLIC_SPIRV_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#include "rin_shader.h"

#define RIN_SPIRV_MAGIC UINT32_C(0x07230203)
#define RIN_SPIRV_DIAGNOSTIC_MAX 160u
#define RIN_SPIRV_MAX_IO 32u
#define RIN_SPIRV_MAX_RESOURCES 64u
#define RIN_SPIRV_MAX_SPECIALIZATIONS 64u
#define RIN_SPIRV_NAME_MAX 64u

/* Exact-format authorization for extended storage-image formats. These bits
 * are narrower than Vulkan's shaderStorageImageExtendedFormats feature: each
 * enables only a format implemented by the selected RinGPU backend. The
 * caller derives them from its API/device contract; V1/V2 never enable them
 * implicitly. */
#define RIN_SPIRV_TRANSLATE_FEATURE_R8_STORAGE_IMAGE UINT32_C(0x00000001)
#define RIN_SPIRV_TRANSLATE_FEATURE_RGBA16F_STORAGE_IMAGE UINT32_C(0x00000002)
#define RIN_SPIRV_TRANSLATE_FEATURE_KNOWN \
    (RIN_SPIRV_TRANSLATE_FEATURE_R8_STORAGE_IMAGE | \
     RIN_SPIRV_TRANSLATE_FEATURE_RGBA16F_STORAGE_IMAGE)

typedef enum RinSpirvResult {
    RIN_SPIRV_OK = 0,
    RIN_SPIRV_ERROR_INVALID_ARGUMENT = -1,
    RIN_SPIRV_ERROR_MALFORMED = -2,
    RIN_SPIRV_ERROR_UNSUPPORTED = -3,
    RIN_SPIRV_ERROR_BOUNDS = -4,
    RIN_SPIRV_ERROR_NO_MEMORY = -5,
    RIN_SPIRV_ERROR_RSH1 = -6
} RinSpirvResult;

typedef enum RinSpirvIoStorage {
    RIN_SPIRV_IO_INPUT = 1,
    RIN_SPIRV_IO_OUTPUT = 2
} RinSpirvIoStorage;

typedef struct RinSpirvIoV1 {
    uint32_t id;
    uint32_t storage;
    uint32_t location;
    uint32_t width;
    uint32_t base_type;
    uint32_t builtin;
    char name[RIN_SPIRV_NAME_MAX];
} RinSpirvIoV1;

typedef struct RinSpirvDescriptorV1 {
    uint32_t id;
    uint32_t set;
    uint32_t binding;
    uint32_t resource_index;
    uint32_t resource_kind;
    uint32_t storage_class;
    uint32_t type_id;
    char name[RIN_SPIRV_NAME_MAX];
} RinSpirvDescriptorV1;

typedef struct RinSpirvPushConstantV1 {
    uint32_t id;
    uint32_t type_id;
    uint32_t size_bytes;
    char name[RIN_SPIRV_NAME_MAX];
} RinSpirvPushConstantV1;

typedef struct RinSpirvSpecializationV1 {
    uint32_t id;
    uint32_t spec_id;
    uint32_t base_type;
    uint32_t default_bits;
    uint32_t override_bits;
    uint32_t has_override;
    char name[RIN_SPIRV_NAME_MAX];
} RinSpirvSpecializationV1;

typedef struct RinSpirvTranslationInfoV1 {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t stage;
    uint32_t entry_id;
    char entry_name[RIN_SPIRV_NAME_MAX];
    uint32_t input_count;
    uint32_t output_count;
    uint32_t descriptor_count;
    uint32_t push_constant_count;
    uint32_t specialization_count;
    RinSpirvIoV1 inputs[RIN_SPIRV_MAX_IO];
    RinSpirvIoV1 outputs[RIN_SPIRV_MAX_IO];
    RinSpirvDescriptorV1 descriptors[RIN_SPIRV_MAX_RESOURCES];
    RinSpirvPushConstantV1 push_constants[RIN_SPIRV_MAX_RESOURCES];
    RinSpirvSpecializationV1 specializations[RIN_SPIRV_MAX_SPECIALIZATIONS];
    RinShaderInfoV1 shader;
    char diagnostic[RIN_SPIRV_DIAGNOSTIC_MAX];
} RinSpirvTranslationInfoV1;

typedef enum RinSpirvInterpolationV2 {
    RIN_SPIRV_INTERPOLATION_UNSPECIFIED_V2 = 0,
    RIN_SPIRV_INTERPOLATION_PERSPECTIVE_V2 = 1,
    RIN_SPIRV_INTERPOLATION_NO_PERSPECTIVE_V2 = 2,
    RIN_SPIRV_INTERPOLATION_FLAT_V2 = 3
} RinSpirvInterpolationV2;

/* V2 preserves per-interface interpolation decorations without changing the
 * V1 metadata ABI. Values are indexed in parallel with base.inputs/outputs.
 * UNSPECIFIED means the SPIR-V object had no Flat/NoPerspective decoration;
 * the consuming API applies its specified default. */
typedef struct RinSpirvTranslationInfoV2 {
    uint32_t struct_size;
    uint32_t api_version;
    RinSpirvTranslationInfoV1 base;
    uint32_t input_interpolation[RIN_SPIRV_MAX_IO];
    uint32_t output_interpolation[RIN_SPIRV_MAX_IO];
} RinSpirvTranslationInfoV2;

typedef struct RinSpirvSpecializationValueV1 {
    uint32_t spec_id;
    uint32_t bits;
} RinSpirvSpecializationValueV1;

/* Translate the deliberately bounded scalar SPIR-V profile to RSH1.  The
 * output is only published after both structural SPIR-V checks and the
 * regular RinShader validator succeed. */
int ringpu_spirv_translate(const uint32_t* words, size_t word_count,
                           uint32_t expected_stage,
                           const RinSpirvSpecializationValueV1* overrides,
                           uint32_t override_count,
                           void* rin_shader_out, size_t rin_shader_capacity,
                           RinSpirvTranslationInfoV1* info);

/* V2 accepts and preserves Flat/NoPerspective on scalar/vector IO. V1
 * remains unchanged and rejects decorations it cannot represent. */
int ringpu_spirv_translate_v2(const uint32_t* words, size_t word_count,
                              uint32_t expected_stage,
                              const RinSpirvSpecializationValueV1* overrides,
                              uint32_t override_count,
                              void* rin_shader_out,
                              size_t rin_shader_capacity,
                              RinSpirvTranslationInfoV2* info);

/* V3 adds exact-format storage-image feature authorization while preserving
 * V2 interpolation metadata. A module declaring StorageImageExtendedFormats
 * is accepted only for explicitly authorized formats; every other extended
 * image format remains unsupported. */
int ringpu_spirv_translate_v3(
    const uint32_t* words, size_t word_count, uint32_t expected_stage,
    const RinSpirvSpecializationValueV1* overrides,
    uint32_t override_count, uint32_t feature_flags,
    void* rin_shader_out, size_t rin_shader_capacity,
    RinSpirvTranslationInfoV2* info);

#endif /* RINGPU_PUBLIC_SPIRV_FRONTEND_H */
