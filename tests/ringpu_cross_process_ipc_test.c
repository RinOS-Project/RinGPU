/* SPDX-License-Identifier: MIT */
#include <ringpu/cross_process_capability.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint8_t response_bytes[128];
static uint64_t response_size;
static RinResult receive_result;
static int32_t response_status;

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
    RinGpuCrossProcessCapabilityTokenV2 token_v2;
    RinGpuCrossProcessCapabilityLeaseV2 lease_v2;
    RinGpuCrossProcessCapabilityMapReadableLeaseRequestV1 map_readable;
    RinGpuCrossProcessCapabilityMappedReadableLeaseV1 mapped_readable;
    uint8_t* request_bytes;

    assert(channel == UINT64_C(7));
    assert(message != NULL);
    assert(message->bytes.size >= sizeof(request));
    request_bytes = (uint8_t*)(uintptr_t)message->bytes.address;
    memcpy(&request, request_bytes, sizeof(request));
    memset(response_bytes, 0, sizeof(response_bytes));
    response = request;
    response.status = response_status;
    response.payload_size = 0u;
    response_size = sizeof(response);
    memcpy(response_bytes, &response, sizeof(response));
    if (response_status != RIN_GPU_CROSS_PROCESS_OK) {
        response_status = RIN_GPU_CROSS_PROCESS_OK;
        memcpy(response_bytes, &response, sizeof(response));
    } else if (request.opcode == RIN_GPU_CAPABILITY_IPC_ISSUE) {
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
    } else if (request.opcode == RIN_GPU_CAPABILITY_IPC_ISSUE_V2) {
        RinGpuCrossProcessCapabilityGrantDescV2 desc;
        assert(request.payload_size == sizeof(desc));
        memcpy(&desc, request_bytes + sizeof(request), sizeof(desc));
        memset(&token_v2, 0, sizeof(token_v2));
        token_v2.struct_size = sizeof(token_v2);
        token_v2.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
        token_v2.token = UINT64_C(19);
        token_v2.device_generation = desc.device_generation;
        token_v2.rights = desc.rights;
        memcpy(response_bytes + sizeof(response), &token_v2,
               sizeof(token_v2));
        response.payload_size = sizeof(token_v2);
        response_size += sizeof(token_v2);
        memcpy(response_bytes, &response, sizeof(response));
    } else if (request.opcode == RIN_GPU_CAPABILITY_IPC_ACQUIRE_V2) {
        RinGpuCrossProcessCapabilityAcquireRequestV2 acquire;
        assert(request.payload_size == sizeof(acquire));
        memcpy(&acquire, request_bytes + sizeof(request), sizeof(acquire));
        memset(&lease_v2, 0, sizeof(lease_v2));
        lease_v2.struct_size = sizeof(lease_v2);
        lease_v2.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
        lease_v2.token = acquire.token;
        lease_v2.lease_id = UINT64_C(23);
        lease_v2.resource_id = UINT64_C(29);
        lease_v2.device_generation = acquire.device_generation;
        lease_v2.rights = RIN_GPU_CROSS_PROCESS_RIGHT_READ |
                          RIN_GPU_CROSS_PROCESS_RIGHT_PRESENT;
        memcpy(response_bytes + sizeof(response), &lease_v2,
               sizeof(lease_v2));
        response.payload_size = sizeof(lease_v2);
        response_size += sizeof(lease_v2);
        memcpy(response_bytes, &response, sizeof(response));
    } else if (request.opcode ==
               RIN_GPU_CAPABILITY_IPC_MAP_READABLE_LEASE_V1) {
        assert(request.payload_size == sizeof(map_readable));
        memcpy(&map_readable, request_bytes + sizeof(request),
               sizeof(map_readable));
        memset(&mapped_readable, 0, sizeof(mapped_readable));
        mapped_readable.struct_size = sizeof(mapped_readable);
        mapped_readable.version =
            RIN_GPU_CROSS_PROCESS_MAPPED_READABLE_V1_VERSION;
        mapped_readable.token = map_readable.token;
        mapped_readable.lease_id = map_readable.lease_id;
        mapped_readable.resource_id = UINT64_C(29);
        mapped_readable.device_generation = map_readable.device_generation;
        mapped_readable.address_in_recipient = UINT64_C(0x40001000);
        mapped_readable.allocation_size = UINT64_C(4096);
        mapped_readable.rights = RIN_GPU_CROSS_PROCESS_RIGHT_READ;
        mapped_readable.access =
            RIN_GPU_CROSS_PROCESS_MAPPED_ACCESS_CPU_READ;
        memcpy(response_bytes + sizeof(response), &mapped_readable,
               sizeof(mapped_readable));
        response.payload_size = sizeof(mapped_readable);
        response_size += sizeof(mapped_readable);
        memcpy(response_bytes, &response, sizeof(response));
    } else if (request.opcode == RIN_GPU_CAPABILITY_IPC_RELEASE_LEASE_V2 ||
               request.opcode == RIN_GPU_CAPABILITY_IPC_REVOKE_V2) {
        assert(request.payload_size ==
               (request.opcode == RIN_GPU_CAPABILITY_IPC_REVOKE_V2
                    ? sizeof(RinGpuCrossProcessCapabilityTokenV2)
                    : sizeof(RinGpuCrossProcessCapabilityReleaseLeaseV2)));
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
    RinGpuCrossProcessCapabilityGrantDescV2 desc_v2;
    RinGpuCrossProcessCapabilityTokenV2 token_v2;
    RinGpuCrossProcessCapabilityAcquireRequestV2 acquire_v2;
    RinGpuCrossProcessCapabilityLeaseV2 lease_v2;
    RinGpuCrossProcessCapabilityMapReadableLeaseRequestV1 map_readable;
    RinGpuCrossProcessCapabilityMappedReadableLeaseV1 mapped_readable;
    RinGpuCrossProcessCapabilityReleaseLeaseV2 release_v2;

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

    memset(&desc_v2, 0, sizeof(desc_v2));
    desc_v2.struct_size = sizeof(desc_v2);
    desc_v2.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
    desc_v2.rights = RIN_GPU_CROSS_PROCESS_RIGHT_READ |
                     RIN_GPU_CROSS_PROCESS_RIGHT_PRESENT;
    desc_v2.device_generation = UINT64_C(31);
    desc_v2.resource_id = UINT64_C(37);
    desc_v2.recipient_process_id = UINT64_C(41);
    desc_v2.recipient_process_instance_cookie = UINT64_C(43);
    memset(&token_v2, 0, sizeof(token_v2));
    assert(rin_gpu_cross_process_capability_ipc_issue_v2(
               &client, &desc_v2, &token_v2) == RIN_GPU_CROSS_PROCESS_OK);
    assert(token_v2.token == UINT64_C(19));
    assert(token_v2.device_generation == desc_v2.device_generation);
    assert(token_v2.rights == desc_v2.rights);

    memset(&acquire_v2, 0, sizeof(acquire_v2));
    acquire_v2.struct_size = sizeof(acquire_v2);
    acquire_v2.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
    acquire_v2.token = token_v2.token;
    acquire_v2.device_generation = token_v2.device_generation;
    acquire_v2.required_rights = RIN_GPU_CROSS_PROCESS_RIGHT_PRESENT;
    memset(&lease_v2, 0, sizeof(lease_v2));
    assert(rin_gpu_cross_process_capability_ipc_acquire_v2(
               &client, &acquire_v2, &lease_v2) == RIN_GPU_CROSS_PROCESS_OK);
    assert(lease_v2.token == token_v2.token);
    assert(lease_v2.lease_id == UINT64_C(23));
    assert(lease_v2.resource_id == UINT64_C(29));

    memset(&map_readable, 0, sizeof(map_readable));
    map_readable.struct_size = sizeof(map_readable);
    map_readable.version = RIN_GPU_CROSS_PROCESS_MAPPED_READABLE_V1_VERSION;
    map_readable.token = lease_v2.token;
    map_readable.lease_id = lease_v2.lease_id;
    map_readable.device_generation = lease_v2.device_generation;
    memset(&mapped_readable, 0, sizeof(mapped_readable));
    assert(rin_gpu_cross_process_capability_ipc_map_readable_lease_v1(
               &client, &map_readable, &mapped_readable) ==
           RIN_GPU_CROSS_PROCESS_OK);
    assert(mapped_readable.address_in_recipient == UINT64_C(0x40001000));
    assert(mapped_readable.allocation_size == UINT64_C(4096));
    response_status = RIN_GPU_CROSS_PROCESS_UNSUPPORTED;
    assert(rin_gpu_cross_process_capability_ipc_map_readable_lease_v1(
               &client, &map_readable, &mapped_readable) ==
           RIN_GPU_CROSS_PROCESS_UNSUPPORTED);
    assert(mapped_readable.address_in_recipient == 0u);

    memset(&release_v2, 0, sizeof(release_v2));
    release_v2.struct_size = sizeof(release_v2);
    release_v2.version = RIN_GPU_CROSS_PROCESS_CAPABILITY_V2_VERSION;
    release_v2.token = lease_v2.token;
    release_v2.lease_id = lease_v2.lease_id;
    release_v2.device_generation = lease_v2.device_generation;
    assert(rin_gpu_cross_process_capability_ipc_release_lease_v2(
               &client, &release_v2) == RIN_GPU_CROSS_PROCESS_OK);
    response_status = RIN_GPU_CROSS_PROCESS_BUSY;
    assert(rin_gpu_cross_process_capability_ipc_revoke_v2(
               &client, &token_v2) == RIN_GPU_CROSS_PROCESS_BUSY);
    assert(rin_gpu_cross_process_capability_ipc_revoke_v2(
               &client, &token_v2) == RIN_GPU_CROSS_PROCESS_OK);
    return 0;
}
