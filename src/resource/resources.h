// SPDX-License-Identifier: MIT
#ifndef RIN_GPU_RESOURCE_RESOURCES_H
#define RIN_GPU_RESOURCE_RESOURCES_H

#include "../core/core.h"

int ringpu_create_buffer(RinGpuCore* core, const RinGpuBufferDescV1* desc,
                         RinGpuHandle* buffer);
int ringpu_upload_buffer(RinGpuCore* core, RinGpuHandle buffer,
                         uint64_t destination_offset, const void* source,
                         uint64_t size_bytes);
int ringpu_create_image(RinGpuCore* core, const RinGpuImageDescV1* desc,
                        RinGpuHandle* image);
int ringpu_create_sampler(RinGpuCore* core, const RinGpuSamplerDescV1* desc,
                          RinGpuHandle* sampler);
int ringpu_get_sampler_info(const RinGpuCore* core, RinGpuHandle sampler,
                            RinGpuSamplerDescV1* info);
int ringpu_get_image_info(const RinGpuCore* core, RinGpuHandle image,
                          RinGpuImageInfoV1* info);
int ringpu_get_image_state(const RinGpuCore* core, RinGpuHandle image,
                           uint32_t mip_level, uint32_t array_layer,
                           uint32_t* state);
int ringpu_upload_image(RinGpuCore* core, RinGpuHandle image,
                        const RinGpuImageUploadV1* upload,
                        const void* source, uint64_t source_size);
int ringpu_readback_image(RinGpuCore* core, RinGpuHandle image,
                          const RinGpuImageReadbackV1* readback,
                          void* destination, uint64_t destination_size);

#endif /* RIN_GPU_RESOURCE_RESOURCES_H */
