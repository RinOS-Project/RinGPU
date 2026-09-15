/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <ringpu/rin_shader.h>

typedef struct PathSource {
    const uint8_t* bytes;
    uint64_t size;
    uint32_t calls;
} PathSource;

static RinResourceCatalogStatus read_path(
    void* context, const char* path, uint32_t path_size, uint8_t* output,
    uint64_t output_capacity, uint64_t* output_size) {
    PathSource* source = (PathSource*)context;
    assert(source != NULL && path != NULL && output_size != NULL);
    assert(path_size == 18u && memcmp(path, "/shaders/basic.rsh", path_size) == 0);
    ++source->calls;
    if (source->size > output_capacity) return RIN_RESOURCE_CATALOG_BUFFER_TOO_SMALL;
    memcpy(output, source->bytes, (size_t)source->size);
    *output_size = source->size;
    return RIN_RESOURCE_CATALOG_OK;
}

static void make_shader(uint8_t* bytes, uint32_t size) {
    RinShaderHeaderV1* header = (RinShaderHeaderV1*)bytes;
    RinShaderInstructionV1* instruction;
    assert(size == sizeof(RinShaderHeaderV1) + sizeof(RinShaderInstructionV1));
    memset(bytes, 0, size);
    header->magic = RIN_SHADER_MAGIC;
    header->version = RIN_SHADER_IR_VERSION;
    header->header_size = sizeof(RinShaderHeaderV1);
    header->total_size = size;
    header->stage = RIN_SHADER_STAGE_VERTEX;
    header->instruction_count = 1u;
    header->register_count = 1u;
    instruction = (RinShaderInstructionV1*)(bytes + sizeof(*header));
    instruction->opcode = RIN_SHADER_OP_RETURN;
    instruction->destination = RIN_SHADER_UNUSED;
    instruction->source0 = RIN_SHADER_UNUSED;
    instruction->source1 = RIN_SHADER_UNUSED;
    instruction->resource = RIN_SHADER_UNUSED;
}

int main(void) {
    uint8_t shader[sizeof(RinShaderHeaderV1) + sizeof(RinShaderInstructionV1)];
    uint8_t storage[sizeof(shader)];
    uint8_t small[sizeof(shader) - 1u];
    RinResourceCatalogEntryV1 entries[2];
    RinResourceCatalogV1 catalog;
    RinShaderInfoV1 info;
    PathSource source;
    uint64_t storage_size = UINT64_MAX;

    make_shader(shader, sizeof(shader));
    memset(entries, 0, sizeof(entries));
    entries[0].struct_size = sizeof(entries[0]);
    entries[0].version = RIN_RESOURCE_CATALOG_VERSION_1;
    entries[0].type = RIN_RESOURCE_CATALOG_TYPE_SHADER;
    entries[0].resource_id = 11u;
    entries[0].flags = RIN_RESOURCE_CATALOG_SOURCE_BLOB |
                       RIN_RESOURCE_CATALOG_FLAG_IMMUTABLE;
    entries[0].data = shader;
    entries[0].data_size = sizeof(shader);
    entries[1] = entries[0];
    entries[1].resource_id = 12u;
    entries[1].flags = RIN_RESOURCE_CATALOG_SOURCE_PATH |
                       RIN_RESOURCE_CATALOG_FLAG_IMMUTABLE;
    entries[1].path = "/shaders/basic.rsh";
    entries[1].path_size = 18u;
    entries[1].data = NULL;
    entries[1].data_size = 0u;
    memset(&catalog, 0, sizeof(catalog));
    catalog.struct_size = sizeof(catalog);
    catalog.version = RIN_RESOURCE_CATALOG_VERSION_1;
    catalog.entries = entries;
    catalog.entry_count = 2u;
    catalog.generation = 1u;

    assert(ringpu_shader_validate_resource(
               &catalog, 11u, NULL, NULL, storage, sizeof(storage),
               &storage_size, &info) == RIN_SHADER_OK);
    assert(storage_size == sizeof(shader));
    assert(info.struct_size == sizeof(info) &&
           info.stage == RIN_SHADER_STAGE_VERTEX);

    source.bytes = shader;
    source.size = sizeof(shader);
    source.calls = 0u;
    storage_size = UINT64_MAX;
    assert(ringpu_shader_validate_resource(
               &catalog, 12u, read_path, &source, storage, sizeof(storage),
               &storage_size, &info) == RIN_SHADER_OK);
    assert(storage_size == sizeof(shader) && source.calls == 1u);

    storage_size = UINT64_MAX;
    memset(&info, 0xa5, sizeof(info));
    assert(ringpu_shader_validate_resource(
               &catalog, 11u, NULL, NULL, small, sizeof(small), &storage_size,
               &info) == RIN_SHADER_ERROR_BOUNDS);
    assert(storage_size == 0u && info.struct_size == 0u);

    shader[0] ^= 0xffu;
    storage_size = UINT64_MAX;
    assert(ringpu_shader_validate_resource(
               &catalog, 11u, NULL, NULL, storage, sizeof(storage),
               &storage_size, &info) != RIN_SHADER_OK);
    assert(storage_size == 0u && info.struct_size == 0u);
    return 0;
}
