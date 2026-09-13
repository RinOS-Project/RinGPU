/* SPDX-License-Identifier: MIT */
#include <ringpu/cross_process_capability.h>

#include <rin/gpu_capability.h>
#include <rin/ipc.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    RIN_GPU_CAPABILITY_IPC_BUFFER_SIZE =
        (int)(sizeof(RinGpuCapabilityIpcHeaderV1) +
              sizeof(RinGpuCrossProcessCapabilityDescV1))
};

static int capability_result_is_known(int32_t status)
{
    switch (status) {
    case RIN_GPU_CROSS_PROCESS_OK:
    case RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT:
    case RIN_GPU_CROSS_PROCESS_PROTOCOL:
    case RIN_GPU_CROSS_PROCESS_LIMIT:
    case RIN_GPU_CROSS_PROCESS_STALE:
    case RIN_GPU_CROSS_PROCESS_DENIED:
    case RIN_GPU_CROSS_PROCESS_IPC:
        return 1;
    default:
        return 0;
    }
}

static int map_sdk_result(RinResult result)
{
    switch (result) {
    case RIN_SUCCESS:
        return RIN_GPU_CROSS_PROCESS_OK;
    case RIN_ERROR_INVALID_ARGUMENT:
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    case RIN_ERROR_ABI_MISMATCH:
    case RIN_ERROR_INTEGRITY:
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    case RIN_ERROR_NOT_FOUND:
    case RIN_ERROR_STALE_HANDLE:
        return RIN_GPU_CROSS_PROCESS_STALE;
    case RIN_ERROR_ACCESS_DENIED:
        return RIN_GPU_CROSS_PROCESS_DENIED;
    default:
        return RIN_GPU_CROSS_PROCESS_IPC;
    }
}

static int client_valid(const RinGpuCrossProcessCapabilityIpcClientV1* client)
{
    return client != NULL && client->channel != RIN_HANDLE_INVALID &&
           client->next_request_id != 0u && client->reserved[0] == 0u &&
           client->reserved[1] == 0u;
}

static int descriptor_valid(const RinGpuCrossProcessCapabilityDescV1* desc)
{
    return desc != NULL && desc->struct_size >= sizeof(*desc) &&
           desc->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION &&
           desc->owner_process_id != 0u && desc->device_generation != 0u &&
           desc->resource_id != 0u && desc->rights != 0u &&
           (desc->rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           desc->reserved[0] == 0u && desc->reserved[1] == 0u;
}

static int token_valid(const RinGpuCrossProcessCapabilityTokenV1* token)
{
    return token != NULL && token->struct_size >= sizeof(*token) &&
           token->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION &&
           token->token != 0u && token->owner_process_id != 0u &&
           token->device_generation != 0u && token->rights != 0u &&
           (token->rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           token->reserved[0] == 0u && token->reserved[1] == 0u;
}

static int request_valid(const RinGpuCrossProcessCapabilityRequestV1* request)
{
    return request != NULL && request->struct_size >= sizeof(*request) &&
           request->version == RIN_GPU_CROSS_PROCESS_CAPABILITY_VERSION &&
           request->token != 0u && request->process_id != 0u &&
           request->device_generation != 0u &&
           (request->required_rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) == 0u &&
           request->reserved[0] == 0u && request->reserved[1] == 0u;
}

static uint64_t take_request_id(
    RinGpuCrossProcessCapabilityIpcClientV1* client)
{
    uint64_t request_id = client->next_request_id;
    ++client->next_request_id;
    if (client->next_request_id == 0u) client->next_request_id = 1u;
    return request_id;
}

static void init_message(
    RinIpcMessageV1* message, uint64_t request_id, uint8_t* bytes,
    size_t byte_size)
{
    memset(message, 0, sizeof(*message));
    message->struct_size = sizeof(*message);
    message->version = RIN_SDK_STRUCT_VERSION_1;
    message->message_id = request_id;
    message->bytes.address = (uint64_t)(uintptr_t)bytes;
    message->bytes.size = byte_size;
}

static int send_request(
    RinGpuCrossProcessCapabilityIpcClientV1* client, uint16_t opcode,
    const void* payload, size_t payload_size, uint8_t* buffer,
    size_t buffer_size, uint64_t* request_id_out)
{
    RinGpuCapabilityIpcHeaderV1 header;
    RinIpcMessageV1 message;
    uint64_t request_id;
    RinResult result;
    size_t byte_size = sizeof(header) + payload_size;

    if (!client_valid(client) || buffer == NULL || request_id_out == NULL ||
        payload_size > UINT32_MAX || byte_size > buffer_size ||
        (payload_size != 0u && payload == NULL))
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;

    request_id = take_request_id(client);
    if (request_id == 0u) return RIN_GPU_CROSS_PROCESS_IPC;
    memset(buffer, 0, byte_size);
    memset(&header, 0, sizeof(header));
    header.struct_size = sizeof(header);
    header.version = RIN_GPU_CAPABILITY_IPC_VERSION;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_size = (uint32_t)payload_size;
    memcpy(buffer, &header, sizeof(header));
    if (payload_size != 0u) memcpy(buffer + sizeof(header), payload, payload_size);

    init_message(&message, request_id, buffer, byte_size);
    result = rin_channel_send_v1(client->channel, &message);
    if (result != RIN_SUCCESS) return map_sdk_result(result);
    *request_id_out = request_id;
    return RIN_GPU_CROSS_PROCESS_OK;
}

static int receive_response(
    RinGpuCrossProcessCapabilityIpcClientV1* client, uint16_t opcode,
    uint64_t request_id, size_t expected_payload_size, uint8_t* buffer,
    size_t buffer_size, uint8_t** payload_out)
{
    RinGpuCapabilityIpcHeaderV1 header;
    RinIpcMessageV1 message;
    RinResult result;
    size_t byte_size;

    if (payload_out != NULL) *payload_out = NULL;
    if (!client_valid(client) || request_id == 0u || buffer == NULL ||
        expected_payload_size > buffer_size - sizeof(header) ||
        buffer_size < sizeof(header))
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;

    memset(buffer, 0, buffer_size);
    init_message(&message, request_id, buffer, buffer_size);
    result = rin_channel_receive_v1(client->channel, &message);
    if (result != RIN_SUCCESS) return map_sdk_result(result);
    if (message.struct_size < sizeof(message) ||
        message.version != RIN_SDK_STRUCT_VERSION_1 ||
        message.message_id != request_id || message.bytes.address !=
            (uint64_t)(uintptr_t)buffer || message.bytes.size < sizeof(header) ||
        message.bytes.size > buffer_size || message.handles.address != 0u ||
        message.handles.size != 0u || message.flags != 0u ||
        message.reserved[0] != 0u || message.reserved[1] != 0u ||
        message.reserved[2] != 0u)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;

    byte_size = (size_t)message.bytes.size;
    memcpy(&header, buffer, sizeof(header));
    if (header.struct_size < sizeof(header) ||
        header.version != RIN_GPU_CAPABILITY_IPC_VERSION ||
        header.opcode != opcode || header.request_id != request_id ||
        header.flags != 0u || header.reserved != 0u ||
        header.payload_size != byte_size - sizeof(header) ||
        !capability_result_is_known(header.status))
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    if (header.status != RIN_GPU_CROSS_PROCESS_OK) {
        if (header.payload_size != 0u)
            return RIN_GPU_CROSS_PROCESS_PROTOCOL;
        return header.status;
    }
    if (header.payload_size != expected_payload_size)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    if (payload_out != NULL) *payload_out = buffer + sizeof(header);
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_ipc_init(
    RinGpuCrossProcessCapabilityIpcClientV1* client, RinChannel channel)
{
    if (client == NULL || channel == RIN_HANDLE_INVALID)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    memset(client, 0, sizeof(*client));
    client->channel = channel;
    client->next_request_id = 1u;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_ipc_connect(
    RinGpuCrossProcessCapabilityIpcClientV1* client)
{
    static const char service_name[] = RIN_GPU_CAPABILITY_SERVICE_NAME;
    RinStringV1 name;
    RinChannel channel = RIN_HANDLE_INVALID;
    RinResult result;

    if (client == NULL) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    name.address = (uint64_t)(uintptr_t)service_name;
    name.size = sizeof(service_name) - 1u;
    result = rin_service_connect_v1(name, &channel);
    if (result != RIN_SUCCESS) return map_sdk_result(result);
    return rin_gpu_cross_process_capability_ipc_init(client, channel);
}

int rin_gpu_cross_process_capability_ipc_issue(
    RinGpuCrossProcessCapabilityIpcClientV1* client,
    const RinGpuCrossProcessCapabilityDescV1* desc,
    RinGpuCrossProcessCapabilityTokenV1* token_out)
{
    uint8_t buffer[RIN_GPU_CAPABILITY_IPC_BUFFER_SIZE];
    uint8_t* payload;
    RinGpuCrossProcessCapabilityTokenV1 token;
    uint64_t request_id;
    int result;

    if (token_out == NULL) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    memset(token_out, 0, sizeof(*token_out));
    if (!descriptor_valid(desc)) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    result = send_request(client, RIN_GPU_CAPABILITY_IPC_ISSUE, desc,
                          sizeof(*desc), buffer, sizeof(buffer), &request_id);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    result = receive_response(client, RIN_GPU_CAPABILITY_IPC_ISSUE, request_id,
                              sizeof(token), buffer, sizeof(buffer), &payload);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    memcpy(&token, payload, sizeof(token));
    if (!token_valid(&token)) return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    *token_out = token;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_ipc_validate(
    RinGpuCrossProcessCapabilityIpcClientV1* client,
    const RinGpuCrossProcessCapabilityRequestV1* request,
    uint64_t* resource_id_out, uint32_t* rights_out)
{
    uint8_t buffer[RIN_GPU_CAPABILITY_IPC_BUFFER_SIZE];
    uint8_t* payload;
    RinGpuCapabilityValidateResponseV1 response;
    uint64_t request_id;
    int result;

    if (resource_id_out == NULL || rights_out == NULL)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    *resource_id_out = 0u;
    *rights_out = 0u;
    if (!request_valid(request)) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    result = send_request(client, RIN_GPU_CAPABILITY_IPC_VALIDATE, request,
                          sizeof(*request), buffer, sizeof(buffer), &request_id);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    result = receive_response(client, RIN_GPU_CAPABILITY_IPC_VALIDATE, request_id,
                              sizeof(response), buffer, sizeof(buffer), &payload);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    memcpy(&response, payload, sizeof(response));
    if (response.resource_id == 0u || response.rights == 0u ||
        (response.rights & ~RIN_GPU_CROSS_PROCESS_RIGHT_MASK) != 0u ||
        response.reserved[0] != 0u || response.reserved[1] != 0u ||
        response.reserved[2] != 0u)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    *resource_id_out = response.resource_id;
    *rights_out = response.rights;
    return RIN_GPU_CROSS_PROCESS_OK;
}

int rin_gpu_cross_process_capability_ipc_release(
    RinGpuCrossProcessCapabilityIpcClientV1* client,
    const RinGpuCrossProcessCapabilityTokenV1* token)
{
    uint8_t buffer[RIN_GPU_CAPABILITY_IPC_BUFFER_SIZE];
    uint64_t request_id;
    int result;

    if (!token_valid(token)) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    result = send_request(client, RIN_GPU_CAPABILITY_IPC_RELEASE, token,
                          sizeof(*token), buffer, sizeof(buffer), &request_id);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    return receive_response(client, RIN_GPU_CAPABILITY_IPC_RELEASE, request_id,
                            0u, buffer, sizeof(buffer), NULL);
}

int rin_gpu_cross_process_capability_ipc_revoke_process(
    RinGpuCrossProcessCapabilityIpcClientV1* client, uint64_t process_id,
    uint32_t* revoked_count_out)
{
    uint8_t buffer[RIN_GPU_CAPABILITY_IPC_BUFFER_SIZE];
    uint8_t* payload;
    RinGpuCapabilityRevokeProcessRequestV1 request;
    RinGpuCapabilityRevokeProcessResponseV1 response;
    uint64_t request_id;
    int result;

    if (revoked_count_out == NULL)
        return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    *revoked_count_out = 0u;
    if (process_id == 0u) return RIN_GPU_CROSS_PROCESS_INVALID_ARGUMENT;
    memset(&request, 0, sizeof(request));
    request.process_id = process_id;
    result = send_request(client, RIN_GPU_CAPABILITY_IPC_REVOKE_PROCESS,
                          &request, sizeof(request), buffer, sizeof(buffer),
                          &request_id);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    result = receive_response(client, RIN_GPU_CAPABILITY_IPC_REVOKE_PROCESS,
                              request_id, sizeof(response), buffer,
                              sizeof(buffer), &payload);
    if (result != RIN_GPU_CROSS_PROCESS_OK) return result;
    memcpy(&response, payload, sizeof(response));
    if (response.revoked_count > RIN_GPU_CROSS_PROCESS_CAPABILITY_MAX ||
        response.reserved[0] != 0u || response.reserved[1] != 0u ||
        response.reserved[2] != 0u)
        return RIN_GPU_CROSS_PROCESS_PROTOCOL;
    *revoked_count_out = response.revoked_count;
    return RIN_GPU_CROSS_PROCESS_OK;
}
