/* SPDX-License-Identifier: MIT */
#include <ringpu/cross_process_capability.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint8_t response_bytes[128];
static uint64_t response_size;
static RinResult receive_result;

RinResult rin_service_connect_v1(RinStringV1 name, RinChannel* channel)
{
    static const char expected_name[] = RIN_GPU_CAPABILITY_SERVICE_NAME;
    assert(name.size == sizeof(expected_name) - 1u);
    assert(memcmp((const void*)(uintptr_t)name.address, expected_name,
                  (size_t)name.size) == 0);
    assert(channel != NULL);
    *channel = UINT64_C(7);
    return RIN_SUCCESS;
}

RinResult rin_channel_send_v1(
    RinChannel channel, const RinIpcMessageV1* message)
{
    RinGpuCapabilityIpcHeaderV1 request;
    RinGpuCapabilityIpcHeaderV1 response;
    RinGpuCrossProcessCapabilityTokenV1 token;
    uint8_t* request_bytes;

    assert(channel == UINT64_C(7));
    assert(message != NULL);
    assert(message->bytes.size >= sizeof(request));
    request_bytes = (uint8_t*)(uintptr_t)message->bytes.address;
    memcpy(&request, request_bytes, sizeof(request));
    memset(response_bytes, 0, sizeof(response_bytes));
    response = request;
    response.status = RIN_GPU_CROSS_PROCESS_OK;
    response.payload_size = 0u;
    response_size = sizeof(response);
    memcpy(response_bytes, &response, sizeof(response));
    if (request.opcode == RIN_GPU_CAPABILITY_IPC_ISSUE) {
        memset(&token, 0, sizeof(token));
        token.struct_size = sizeof(token);
        token.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION;
        token.token = UINT64_C(9);
        token.owner_process_id = UINT64_C(11);
        token.device_generation = UINT64_C(13);
        token.rights = RIN_GPU_CROSS_PROCESS_RIGHT_READ;
        memcpy(response_bytes + sizeof(response), &token, sizeof(token));
        response.payload_size = sizeof(token);
        response_size += sizeof(token);
        memcpy(response_bytes, &response, sizeof(response));
    }
    return RIN_SUCCESS;
}

RinResult rin_channel_receive_v1(
    RinChannel channel, RinIpcMessageV1* message)
{
    assert(channel == UINT64_C(7));
    if (receive_result != RIN_SUCCESS) return receive_result;
    assert(message != NULL);
    assert(message->bytes.size >= response_size);
    memcpy((void*)(uintptr_t)message->bytes.address, response_bytes,
           (size_t)response_size);
    message->bytes.size = response_size;
    return RIN_SUCCESS;
}

int main(void)
{
    RinGpuCrossProcessCapabilityIpcClientV1 client;
    RinGpuCrossProcessCapabilityDescV1 desc;
    RinGpuCrossProcessCapabilityTokenV1 token;

    memset(&client, 0, sizeof(client));
    assert(rin_gpu_cross_process_capability_ipc_connect(&client) ==
           RIN_GPU_CROSS_PROCESS_OK);
    assert(rin_gpu_cross_process_capability_ipc_init(&client, 0u) ==
           RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT);
    assert(rin_gpu_cross_process_capability_ipc_init(&client, UINT64_C(7)) ==
           RIN_GPU_CROSS_PROCESS_OK);
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION;
    desc.owner_process_id = UINT64_C(11);
    desc.device_generation = UINT64_C(13);
    desc.resource_id = UINT64_C(17);
    desc.rights = RIN_GPU_CROSS_PROCESS_RIGHT_READ;
    memset(&token, 0, sizeof(token));
    receive_result = RIN_ERROR_IO;
    assert(rin_gpu_cross_process_capability_ipc_issue(&client, &desc, &token) ==
           RIN_GPU_CROSS_PROCESS_IPC);
    assert(token.token == 0u);
    receive_result = RIN_SUCCESS;
    assert(rin_gpu_cross_process_capability_ipc_issue(&client, &desc, &token) ==
           RIN_GPU_CROSS_PROCESS_OK);
    assert(token.token == UINT64_C(9));
    assert(token.rights == RIN_GPU_CROSS_PROCESS_RIGHT_READ);
    return 0;
}
