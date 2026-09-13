/* SPDX-License-Identifier: MIT */
#include <ringpu/rin_shader.h>

#include <stdint.h>
#include <string.h>

int main(void) {
    uint8_t malformed[sizeof(RinShaderHeaderV1)] = {0};
    RinShaderInfoV1 info;
    memset(&info, 0, sizeof(info));
    if (ringpu_shader_validate(malformed, sizeof(malformed), &info) == RIN_SHADER_OK)
        return 1;
    return 0;
}
