/* SPDX-License-Identifier: MIT */
#ifndef RINGPU_PLATFORM_BACKEND_BRIDGE_H
#define RINGPU_PLATFORM_BACKEND_BRIDGE_H

#include "../core/core.h"
#include <ringpu/platform_bridge.h>

typedef struct RinGpuPlatformBackendAdapterV1 {
    const RinGpuPlatformBackendBridgeV1* bridge;
    void* bridge_context;
    RinGpuBackendOpsV1 backend_ops;
} RinGpuPlatformBackendAdapterV1;

int ringpu_platform_backend_adapter_init(
    RinGpuPlatformBackendAdapterV1* adapter,
    const RinGpuPlatformBackendBridgeV1* bridge, void* bridge_context,
    uint32_t requested_family);

#endif /* RINGPU_PLATFORM_BACKEND_BRIDGE_H */
