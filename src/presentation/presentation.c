/* SPDX-License-Identifier: MIT */
#include "../../include/ringpu/presentation.h"

#include <stddef.h>
#include <string.h>

#define PRESENTATION_MAGIC UINT64_C(0x52494e5052455331)
#define PRESENTATION_SLOT_FREE 0u
#define PRESENTATION_SLOT_LIVE 1u

typedef struct PresentationOutputSlot {
    RinGpuPresentationOutputV1 descriptor;
    uint64_t current_image;
    uint64_t current_frame;
    uint64_t pending_image;
    uint64_t pending_frame;
    uint64_t pending_fence;
    uint64_t scanout_transaction_id;
    uint64_t scanout_image;
    uint64_t scanout_previous_image;
    uint64_t scanout_frame;
    uint32_t state;
    uint32_t pending_mode;
    uint32_t slot_state;
    uint32_t reserved;
} PresentationOutputSlot;

typedef struct PresentationImageSlot {
    RinGpuPresentationImageV1 descriptor;
    uint64_t frame_id;
    uint64_t fence_value;
    uint32_t state;
    uint32_t slot_state;
    uint32_t reserved[2];
} PresentationImageSlot;

typedef struct PresentationState {
    uint64_t magic;
    uint64_t device_generation;
    uint64_t next_fence_value;
    uint64_t next_transaction_id;
    uint64_t submitted_count;
    uint64_t completed_count;
    uint64_t dropped_count;
    uint64_t stale_completion_count;
    uint32_t state;
    uint32_t output_count;
    uint32_t image_count;
    uint32_t full_redraw_required;
    RinGpuPresentationBackendV1 backend;
    PresentationOutputSlot outputs[RIN_GPU_PRESENTATION_MAX_OUTPUTS];
    PresentationImageSlot images[RIN_GPU_PRESENTATION_MAX_IMAGES];
} PresentationState;

static PresentationState* presentation_state(
    RinGpuPresentationRuntime* runtime) {
    return runtime ? (PresentationState*)runtime->opaque : NULL;
}

static int versioned(uint32_t struct_size, uint32_t version,
                     size_t required_size) {
    return version == RIN_GPU_PRESENTATION_VERSION &&
           struct_size >= required_size;
}

static int runtime_ready(PresentationState* state) {
    if (!state || state->magic != PRESENTATION_MAGIC)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (state->state == RIN_GPU_PRESENTATION_STATE_LOST)
        return RIN_GPU_PRESENTATION_DEVICE_LOST;
    return RIN_GPU_PRESENTATION_OK;
}

static int runtime_active(PresentationState* state) {
    int result = runtime_ready(state);
    if (result != RIN_GPU_PRESENTATION_OK) return result;
    return state->state == RIN_GPU_PRESENTATION_STATE_ACTIVE
               ? RIN_GPU_PRESENTATION_OK
               : RIN_GPU_PRESENTATION_STATE;
}

static PresentationOutputSlot* find_output(PresentationState* state,
                                            uint32_t display_id) {
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_OUTPUTS;
         ++index) {
        PresentationOutputSlot* output = &state->outputs[index];
        if (output->slot_state == PRESENTATION_SLOT_LIVE &&
            output->descriptor.display_id == display_id)
            return output;
    }
    return NULL;
}

static PresentationImageSlot* find_image(PresentationState* state,
                                         uint64_t image_token) {
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_IMAGES;
         ++index) {
        PresentationImageSlot* image = &state->images[index];
        if (image->slot_state == PRESENTATION_SLOT_LIVE &&
            image->descriptor.image_token == image_token)
            return image;
    }
    return NULL;
}

static int output_matches(const PresentationState* state,
                          const PresentationOutputSlot* output,
                          const PresentationImageSlot* image) {
    const RinGpuPresentationOutputV1* out = &output->descriptor;
    const RinGpuPresentationImageV1* desc = &image->descriptor;
    return out->device_generation == state->device_generation &&
           desc->device_generation == state->device_generation &&
           out->output_generation == desc->output_generation &&
           out->width == desc->width && out->height == desc->height &&
           out->format == desc->format &&
           (desc->usage & RIN_GPU_PRESENTATION_IMAGE_PRESENT) != 0u;
}

static int mode_supported(const PresentationState* state,
                          const PresentationOutputSlot* output, uint32_t mode) {
    uint32_t flag;
    if (mode == RIN_GPU_PRESENTATION_MODE_FIFO)
        flag = RIN_GPU_PRESENTATION_OUTPUT_FIFO;
    else if (mode == RIN_GPU_PRESENTATION_MODE_MAILBOX)
        flag = RIN_GPU_PRESENTATION_OUTPUT_MAILBOX;
    else if (mode == RIN_GPU_PRESENTATION_MODE_IMMEDIATE)
        flag = RIN_GPU_PRESENTATION_OUTPUT_IMMEDIATE;
    else
        return 0;
    if ((output->descriptor.flags & flag) == 0u) return 0;
    if (mode == RIN_GPU_PRESENTATION_MODE_MAILBOX &&
        !state->backend.cancel)
        return 0;
    return 1;
}

static void make_image_available(PresentationImageSlot* image) {
    image->state = RIN_GPU_PRESENTATION_IMAGE_AVAILABLE;
    image->frame_id = 0u;
    image->fence_value = 0u;
}

static void invalidate_output(PresentationState* state,
                              PresentationOutputSlot* output) {
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_IMAGES;
         ++index) {
        PresentationImageSlot* image = &state->images[index];
        if (image->slot_state != PRESENTATION_SLOT_LIVE ||
            image->descriptor.display_id != output->descriptor.display_id)
            continue;
        image->state = RIN_GPU_PRESENTATION_IMAGE_INVALID;
        image->frame_id = 0u;
        image->fence_value = 0u;
    }
    output->current_image = 0u;
    output->current_frame = 0u;
    output->pending_image = 0u;
    output->pending_frame = 0u;
    output->pending_fence = 0u;
    output->pending_mode = 0u;
    output->scanout_transaction_id = 0u;
    output->scanout_image = 0u;
    output->scanout_previous_image = 0u;
    output->scanout_frame = 0u;
}

int rin_gpu_presentation_runtime_init(
    RinGpuPresentationRuntime* runtime, uint64_t device_generation,
    const RinGpuPresentationBackendV1* backend) {
    PresentationState* state;
    if (!runtime || !backend || device_generation == 0u ||
        !versioned(backend->struct_size, backend->version, sizeof(*backend)) ||
        !backend->submit || backend->reserved[0] != 0u ||
        backend->reserved[1] != 0u || backend->reserved[2] != 0u ||
        backend->reserved[3] != 0u) {
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    }
    state = presentation_state(runtime);
    memset(state, 0, sizeof(*state));
    state->magic = PRESENTATION_MAGIC;
    state->device_generation = device_generation;
    state->next_fence_value = 1u;
    state->state = RIN_GPU_PRESENTATION_STATE_ACTIVE;
    state->backend = *backend;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_register_output(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationOutputV1* output) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* slot;
    if (runtime_active(state) != RIN_GPU_PRESENTATION_OK || !output ||
        !versioned(output->struct_size, output->version, sizeof(*output)) ||
        (output->flags & ~RIN_GPU_PRESENTATION_OUTPUT_KNOWN_FLAGS) != 0u ||
        (output->flags & RIN_GPU_PRESENTATION_OUTPUT_FIFO) == 0u ||
        output->display_id == UINT32_MAX || output->width == 0u ||
        output->height == 0u || output->refresh_millihertz == 0u ||
        output->output_generation == 0u ||
        output->device_generation != state->device_generation ||
        output->reserved[0] != 0u || output->reserved[1] != 0u) {
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    }
    slot = find_output(state, output->display_id);
    if (!slot) {
        for (uint32_t index = 0u;
             index < RIN_GPU_PRESENTATION_MAX_OUTPUTS; ++index) {
            if (state->outputs[index].slot_state == PRESENTATION_SLOT_FREE) {
                slot = &state->outputs[index];
                ++state->output_count;
                break;
            }
        }
        if (!slot) return RIN_GPU_PRESENTATION_LIMIT;
    } else {
        if (output->output_generation <= slot->descriptor.output_generation)
            return RIN_GPU_PRESENTATION_STALE;
        invalidate_output(state, slot);
    }
    slot->descriptor = *output;
    slot->descriptor.struct_size = sizeof(slot->descriptor);
    slot->slot_state = PRESENTATION_SLOT_LIVE;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_update_output(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationOutputV1* output) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* slot;
    if (runtime_active(state) != RIN_GPU_PRESENTATION_OK || !output ||
        !versioned(output->struct_size, output->version, sizeof(*output)) ||
        (output->flags & ~RIN_GPU_PRESENTATION_OUTPUT_KNOWN_FLAGS) != 0u ||
        (output->flags & RIN_GPU_PRESENTATION_OUTPUT_FIFO) == 0u ||
        output->display_id == UINT32_MAX || output->width == 0u ||
        output->height == 0u || output->refresh_millihertz == 0u ||
        output->output_generation == 0u ||
        output->device_generation != state->device_generation ||
        output->reserved[0] != 0u || output->reserved[1] != 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    slot = find_output(state, output->display_id);
    if (!slot || slot->descriptor.output_generation !=
                     output->output_generation)
        return RIN_GPU_PRESENTATION_STALE;
    if (slot->pending_image != 0u) return RIN_GPU_PRESENTATION_BUSY;
    invalidate_output(state, slot);
    slot->descriptor = *output;
    slot->descriptor.struct_size = sizeof(slot->descriptor);
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_register_image(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationImageV1* image) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* slot;
    if (runtime_active(state) != RIN_GPU_PRESENTATION_OK || !image ||
        !versioned(image->struct_size, image->version, sizeof(*image)) ||
        image->image_token == 0u ||
        (image->usage & ~RIN_GPU_PRESENTATION_IMAGE_KNOWN_USAGE) != 0u ||
        (image->usage & RIN_GPU_PRESENTATION_IMAGE_PRESENT) == 0u ||
        image->width == 0u || image->height == 0u ||
        image->output_generation == 0u ||
        image->device_generation != state->device_generation ||
        image->reserved[0] != 0u || image->reserved[1] != 0u) {
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    }
    if (find_image(state, image->image_token))
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, image->display_id);
    if (!output || output->descriptor.device_generation !=
                       image->device_generation ||
        output->descriptor.output_generation != image->output_generation ||
        output->descriptor.width != image->width ||
        output->descriptor.height != image->height ||
        output->descriptor.format != image->format)
        return RIN_GPU_PRESENTATION_STALE;
    for (uint32_t index = 0u;
         index < RIN_GPU_PRESENTATION_MAX_IMAGES; ++index) {
        if (state->images[index].slot_state == PRESENTATION_SLOT_FREE) {
            slot = &state->images[index];
            memset(slot, 0, sizeof(*slot));
            slot->descriptor = *image;
            slot->descriptor.struct_size = sizeof(slot->descriptor);
            slot->state = RIN_GPU_PRESENTATION_IMAGE_AVAILABLE;
            slot->slot_state = PRESENTATION_SLOT_LIVE;
            ++state->image_count;
            return RIN_GPU_PRESENTATION_OK;
        }
    }
    return RIN_GPU_PRESENTATION_LIMIT;
}

int rin_gpu_presentation_unregister_image(
    RinGpuPresentationRuntime* runtime, uint64_t image_token) {
    PresentationState* state = presentation_state(runtime);
    PresentationImageSlot* image;
    if (runtime_ready(state) != RIN_GPU_PRESENTATION_OK || image_token == 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    image = find_image(state, image_token);
    if (!image) return RIN_GPU_PRESENTATION_STALE;
    if (image->state != RIN_GPU_PRESENTATION_IMAGE_AVAILABLE &&
        image->state != RIN_GPU_PRESENTATION_IMAGE_INVALID)
        return RIN_GPU_PRESENTATION_BUSY;
    memset(image, 0, sizeof(*image));
    --state->image_count;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_begin_frame(
    RinGpuPresentationRuntime* runtime,
    RinGpuPresentationAcquireV1* acquire) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    int ready = runtime_active(state);
    if (ready != RIN_GPU_PRESENTATION_OK) return ready;
    if (!acquire ||
        !versioned(acquire->struct_size, acquire->version, sizeof(*acquire)) ||
        acquire->display_id == UINT32_MAX || acquire->image_token == 0u ||
        acquire->output_generation == 0u ||
        acquire->device_generation != state->device_generation ||
        acquire->frame_id != 0u || acquire->reserved[0] != 0u ||
        acquire->reserved[1] != 0u) {
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    }
    output = find_output(state, acquire->display_id);
    image = find_image(state, acquire->image_token);
    if (!output || !image || !output_matches(state, output, image) ||
        acquire->output_generation != output->descriptor.output_generation)
        return RIN_GPU_PRESENTATION_STALE;
    if (!mode_supported(state, output, acquire->mode))
        return RIN_GPU_PRESENTATION_UNSUPPORTED;
    if (image->state != RIN_GPU_PRESENTATION_IMAGE_AVAILABLE)
        return RIN_GPU_PRESENTATION_BUSY;
    if (acquire->mode == RIN_GPU_PRESENTATION_MODE_FIFO &&
        output->pending_image != 0u)
        return RIN_GPU_PRESENTATION_BUSY;
    if (acquire->mode == RIN_GPU_PRESENTATION_MODE_MAILBOX &&
        output->pending_image != 0u) {
        PresentationImageSlot* old = find_image(state, output->pending_image);
        RinGpuPresentationCompletionV1 pending;
        int cancel_result;
        memset(&pending, 0, sizeof(pending));
        pending.struct_size = sizeof(pending);
        pending.version = RIN_GPU_PRESENTATION_VERSION;
        pending.display_id = output->descriptor.display_id;
        pending.image_token = output->pending_image;
        pending.output_generation = output->descriptor.output_generation;
        pending.device_generation = state->device_generation;
        pending.frame_id = output->pending_frame;
        pending.fence_value = output->pending_fence;
        cancel_result = state->backend.cancel(state->backend.context,
                                              &pending);
        if (cancel_result != 0) return RIN_GPU_PRESENTATION_BACKEND;
        if (old) make_image_available(old);
        output->pending_image = 0u;
        output->pending_frame = 0u;
        output->pending_fence = 0u;
        output->pending_mode = 0u;
        ++state->dropped_count;
    }
    if (output->current_image == acquire->image_token)
        return RIN_GPU_PRESENTATION_BUSY;
    if (state->next_fence_value == UINT64_MAX)
        return RIN_GPU_PRESENTATION_LIMIT;
    image->state = RIN_GPU_PRESENTATION_IMAGE_ACQUIRED;
    image->frame_id = state->next_fence_value;
    image->fence_value = 0u;
    acquire->frame_id = image->frame_id;
    acquire->struct_size = sizeof(*acquire);
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_abandon_frame(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationAcquireV1* acquire) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    if (runtime_active(state) != RIN_GPU_PRESENTATION_OK || !acquire ||
        !versioned(acquire->struct_size, acquire->version, sizeof(*acquire)) ||
        acquire->display_id == UINT32_MAX || acquire->image_token == 0u ||
        acquire->output_generation == 0u || acquire->frame_id == 0u ||
        acquire->device_generation != state->device_generation ||
        acquire->reserved[0] != 0u || acquire->reserved[1] != 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, acquire->display_id);
    image = find_image(state, acquire->image_token);
    if (!output || !image || !output_matches(state, output, image) ||
        acquire->output_generation != output->descriptor.output_generation)
        return RIN_GPU_PRESENTATION_STALE;
    if (image->state != RIN_GPU_PRESENTATION_IMAGE_ACQUIRED ||
        image->frame_id != acquire->frame_id)
        return RIN_GPU_PRESENTATION_BUSY;
    make_image_available(image);
    return RIN_GPU_PRESENTATION_OK;
}

static int damage_valid(const RinGpuPresentationSubmitV1* submit,
                        const PresentationOutputSlot* output) {
    if ((submit->flags & ~RIN_GPU_PRESENTATION_SUBMIT_KNOWN_FLAGS) != 0u ||
        submit->damage_count > RIN_GPU_PRESENTATION_MAX_DAMAGE_RECTS)
        return 0;
    if ((submit->flags & RIN_GPU_PRESENTATION_SUBMIT_FULL_DAMAGE) != 0u &&
        submit->damage_count != 0u)
        return 0;
    for (uint32_t index = 0u; index < submit->damage_count; ++index) {
        const RinGpuPresentationDamageRectV1* rect = &submit->damage[index];
        if (rect->width == 0u || rect->height == 0u || rect->x < 0 ||
            rect->y < 0 || (uint32_t)rect->x >= output->descriptor.width ||
            (uint32_t)rect->y >= output->descriptor.height ||
            rect->width > output->descriptor.width - (uint32_t)rect->x ||
            rect->height > output->descriptor.height - (uint32_t)rect->y)
            return 0;
    }
    return (submit->flags & RIN_GPU_PRESENTATION_SUBMIT_FULL_DAMAGE) != 0u ||
           submit->damage_count != 0u;
}

int rin_gpu_presentation_submit_frame(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationSubmitV1* submit, uint64_t* fence_value_out) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    uint64_t fence;
    int result;
    int ready = runtime_active(state);
    if (fence_value_out) *fence_value_out = 0u;
    if (ready != RIN_GPU_PRESENTATION_OK) return ready;
    if (!submit ||
        !fence_value_out ||
        !versioned(submit->struct_size, submit->version, sizeof(*submit)) ||
        submit->display_id == UINT32_MAX || submit->image_token == 0u ||
        submit->output_generation == 0u ||
        submit->device_generation != state->device_generation ||
        submit->frame_id == 0u || submit->reserved[0] != 0u ||
        submit->reserved[1] != 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if ((submit->flags & RIN_GPU_PRESENTATION_SUBMIT_DIRECT_SCANOUT) != 0u)
        return RIN_GPU_PRESENTATION_UNSUPPORTED;
    output = find_output(state, submit->display_id);
    image = find_image(state, submit->image_token);
    if (!output || !image || !output_matches(state, output, image) ||
        submit->output_generation != output->descriptor.output_generation ||
        !mode_supported(state, output, submit->mode))
        return RIN_GPU_PRESENTATION_STALE;
    if (!damage_valid(submit, output))
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (image->state != RIN_GPU_PRESENTATION_IMAGE_ACQUIRED ||
        image->frame_id != submit->frame_id || output->pending_image != 0u)
        return RIN_GPU_PRESENTATION_BUSY;
    if (state->next_fence_value == UINT64_MAX)
        return RIN_GPU_PRESENTATION_LIMIT;
    fence = state->next_fence_value++;
    result = state->backend.submit(state->backend.context, submit, fence);
    if (result != 0) {
        make_image_available(image);
        return RIN_GPU_PRESENTATION_BACKEND;
    }
    image->state = RIN_GPU_PRESENTATION_IMAGE_SUBMITTED;
    image->fence_value = fence;
    output->pending_image = submit->image_token;
    output->pending_frame = submit->frame_id;
    output->pending_fence = fence;
    output->pending_mode = submit->mode;
    ++state->submitted_count;
    *fence_value_out = fence;
    return RIN_GPU_PRESENTATION_OK;
}

static int scanout_candidate_valid(
    const RinGpuPresentationScanoutCandidateV1* candidate) {
    return candidate &&
           versioned(candidate->struct_size, candidate->version,
                     sizeof(*candidate)) &&
           candidate->display_id != UINT32_MAX &&
           candidate->image_token != 0u &&
           (candidate->flags & ~RIN_GPU_PRESENTATION_SCANOUT_CANDIDATE_KNOWN_FLAGS) == 0u &&
           (candidate->flags & RIN_GPU_PRESENTATION_SCANOUT_CANDIDATE_FULLSCREEN) != 0u &&
           candidate->output_generation != 0u &&
           candidate->device_generation != 0u && candidate->width != 0u &&
           candidate->height != 0u && candidate->format != 0u &&
           candidate->reserved0 == 0u && candidate->reserved[0] == 0u;
}

static int scanout_transaction_matches(
    const PresentationOutputSlot* output,
    const RinGpuPresentationScanoutTransactionV1* transaction) {
    return output && transaction && output->scanout_transaction_id != 0u &&
           transaction->transaction_id == output->scanout_transaction_id &&
           transaction->image_token == output->scanout_image &&
           transaction->previous_image_token == output->scanout_previous_image &&
           transaction->frame_id == output->scanout_frame;
}

int rin_gpu_presentation_begin_direct_scanout(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationScanoutCandidateV1* candidate,
    RinGpuPresentationScanoutTransactionV1* transaction_out) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    int ready = runtime_active(state);

    if (ready != RIN_GPU_PRESENTATION_OK) return ready;
    if (!transaction_out || !scanout_candidate_valid(candidate) ||
        candidate->device_generation != state->device_generation)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, candidate->display_id);
    image = find_image(state, candidate->image_token);
    if (!output || !image || !output_matches(state, output, image) ||
        candidate->output_generation != output->descriptor.output_generation ||
        candidate->width != output->descriptor.width ||
        candidate->height != output->descriptor.height ||
        candidate->format != output->descriptor.format)
        return RIN_GPU_PRESENTATION_STALE;
    if (output->pending_image != 0u || output->scanout_transaction_id != 0u)
        return RIN_GPU_PRESENTATION_BUSY;
    if (image->state != RIN_GPU_PRESENTATION_IMAGE_AVAILABLE)
        return RIN_GPU_PRESENTATION_BUSY;
    if (!mode_supported(state, output, RIN_GPU_PRESENTATION_MODE_FIFO))
        return RIN_GPU_PRESENTATION_UNSUPPORTED;
    if (state->next_transaction_id == UINT64_MAX)
        return RIN_GPU_PRESENTATION_LIMIT;

    memset(transaction_out, 0, sizeof(*transaction_out));
    transaction_out->struct_size = sizeof(*transaction_out);
    transaction_out->version = RIN_GPU_PRESENTATION_VERSION;
    transaction_out->display_id = candidate->display_id;
    transaction_out->transaction_id = ++state->next_transaction_id;
    transaction_out->image_token = candidate->image_token;
    transaction_out->previous_image_token = output->current_image;
    transaction_out->output_generation = candidate->output_generation;
    transaction_out->device_generation = candidate->device_generation;
    transaction_out->frame_id = transaction_out->transaction_id;
    output->scanout_transaction_id = transaction_out->transaction_id;
    output->scanout_image = transaction_out->image_token;
    output->scanout_previous_image = transaction_out->previous_image_token;
    output->scanout_frame = transaction_out->frame_id;
    image->state = RIN_GPU_PRESENTATION_IMAGE_ACQUIRED;
    image->frame_id = transaction_out->frame_id;
    image->fence_value = 0u;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_commit_direct_scanout(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationScanoutTransactionV1* transaction,
    uint64_t* fence_value_out) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    RinGpuPresentationSubmitV1 submit;
    uint64_t fence;
    int result;
    int ready = runtime_active(state);

    if (fence_value_out) *fence_value_out = 0u;
    if (ready != RIN_GPU_PRESENTATION_OK) return ready;
    if (!fence_value_out || !transaction ||
        !versioned(transaction->struct_size, transaction->version,
                   sizeof(*transaction)) ||
        transaction->display_id == UINT32_MAX ||
        transaction->transaction_id == 0u || transaction->image_token == 0u ||
        transaction->output_generation == 0u ||
        transaction->device_generation != state->device_generation ||
        transaction->frame_id == 0u || transaction->reserved[0] != 0u ||
        transaction->reserved[1] != 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, transaction->display_id);
    image = find_image(state, transaction->image_token);
    if (!output || !image || !scanout_transaction_matches(output, transaction) ||
        transaction->output_generation != output->descriptor.output_generation ||
        image->state != RIN_GPU_PRESENTATION_IMAGE_ACQUIRED ||
        image->frame_id != transaction->frame_id)
        return RIN_GPU_PRESENTATION_STALE;
    if (!mode_supported(state, output, RIN_GPU_PRESENTATION_MODE_FIFO))
        return RIN_GPU_PRESENTATION_UNSUPPORTED;
    if (state->next_fence_value == UINT64_MAX)
        return RIN_GPU_PRESENTATION_LIMIT;

    memset(&submit, 0, sizeof(submit));
    submit.struct_size = sizeof(submit);
    submit.version = RIN_GPU_PRESENTATION_VERSION;
    submit.display_id = transaction->display_id;
    submit.mode = RIN_GPU_PRESENTATION_MODE_FIFO;
    submit.image_token = transaction->image_token;
    submit.output_generation = transaction->output_generation;
    submit.device_generation = transaction->device_generation;
    submit.frame_id = transaction->frame_id;
    submit.flags = RIN_GPU_PRESENTATION_SUBMIT_FULL_DAMAGE |
                   RIN_GPU_PRESENTATION_SUBMIT_DIRECT_SCANOUT;
    fence = state->next_fence_value++;
    result = state->backend.submit(state->backend.context, &submit, fence);
    if (result != 0) {
        make_image_available(image);
        output->scanout_transaction_id = 0u;
        output->scanout_image = 0u;
        output->scanout_previous_image = 0u;
        output->scanout_frame = 0u;
        return RIN_GPU_PRESENTATION_BACKEND;
    }
    image->state = RIN_GPU_PRESENTATION_IMAGE_SUBMITTED;
    image->fence_value = fence;
    output->pending_image = transaction->image_token;
    output->pending_frame = transaction->frame_id;
    output->pending_fence = fence;
    output->pending_mode = RIN_GPU_PRESENTATION_MODE_FIFO;
    output->scanout_transaction_id = 0u;
    output->scanout_image = 0u;
    output->scanout_previous_image = 0u;
    output->scanout_frame = 0u;
    ++state->submitted_count;
    *fence_value_out = fence;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_abort_direct_scanout(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationScanoutTransactionV1* transaction) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    int ready = runtime_active(state);

    if (ready != RIN_GPU_PRESENTATION_OK) return ready;
    if (!transaction ||
        !versioned(transaction->struct_size, transaction->version,
                   sizeof(*transaction)) || transaction->display_id == UINT32_MAX ||
        transaction->device_generation != state->device_generation)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, transaction->display_id);
    image = find_image(state, transaction->image_token);
    if (!output || !image || !scanout_transaction_matches(output, transaction))
        return RIN_GPU_PRESENTATION_STALE;
    if (image->state != RIN_GPU_PRESENTATION_IMAGE_ACQUIRED)
        return RIN_GPU_PRESENTATION_BUSY;
    make_image_available(image);
    output->scanout_transaction_id = 0u;
    output->scanout_image = 0u;
    output->scanout_previous_image = 0u;
    output->scanout_frame = 0u;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_complete(
    RinGpuPresentationRuntime* runtime,
    const RinGpuPresentationCompletionV1* completion) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    PresentationImageSlot* old;
    {
        int ready = runtime_ready(state);
        if (ready != RIN_GPU_PRESENTATION_OK) return ready;
    }
    if (!completion ||
        !versioned(completion->struct_size, completion->version,
                   sizeof(*completion)) ||
        completion->display_id == UINT32_MAX || completion->image_token == 0u ||
        completion->output_generation == 0u ||
        completion->device_generation != state->device_generation ||
        (completion->status != RIN_GPU_PRESENTATION_COMPLETION_SUCCESS &&
         completion->status != RIN_GPU_PRESENTATION_COMPLETION_FAILURE) ||
        completion->frame_id == 0u || completion->fence_value == 0u ||
        completion->reserved[0] != 0u || completion->reserved[1] != 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, completion->display_id);
    image = find_image(state, completion->image_token);
    if (!output || !image || completion->output_generation !=
                                    output->descriptor.output_generation ||
        completion->output_generation != image->descriptor.output_generation ||
        output->pending_image != completion->image_token ||
        output->pending_frame != completion->frame_id ||
        output->pending_fence != completion->fence_value ||
        image->state != RIN_GPU_PRESENTATION_IMAGE_SUBMITTED) {
        ++state->stale_completion_count;
        return RIN_GPU_PRESENTATION_STALE;
    }
    output->pending_image = 0u;
    output->pending_frame = 0u;
    output->pending_fence = 0u;
    output->pending_mode = 0u;
    if (completion->status == RIN_GPU_PRESENTATION_COMPLETION_FAILURE) {
        make_image_available(image);
        ++state->dropped_count;
        return RIN_GPU_PRESENTATION_TIMEOUT;
    }
    old = find_image(state, output->current_image);
    if (old) make_image_available(old);
    image->state = RIN_GPU_PRESENTATION_IMAGE_SCANNING;
    image->frame_id = completion->frame_id;
    image->fence_value = completion->fence_value;
    output->current_image = completion->image_token;
    output->current_frame = completion->frame_id;
    ++state->completed_count;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_drop_pending(
    RinGpuPresentationRuntime* runtime, uint32_t display_id,
    uint64_t output_generation, uint64_t device_generation) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    PresentationImageSlot* image;
    if (runtime_active(state) != RIN_GPU_PRESENTATION_OK ||
        device_generation != state->device_generation ||
        output_generation == 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, display_id);
    if (!output || output->descriptor.output_generation != output_generation)
        return RIN_GPU_PRESENTATION_STALE;
    if (output->pending_image == 0u) return RIN_GPU_PRESENTATION_OK;
    image = find_image(state, output->pending_image);
    if (!state->backend.cancel) return RIN_GPU_PRESENTATION_UNSUPPORTED;
    {
        RinGpuPresentationCompletionV1 pending;
        int cancel_result;
        memset(&pending, 0, sizeof(pending));
        pending.struct_size = sizeof(pending);
        pending.version = RIN_GPU_PRESENTATION_VERSION;
        pending.display_id = display_id;
        pending.image_token = output->pending_image;
        pending.output_generation = output_generation;
        pending.device_generation = device_generation;
        pending.frame_id = output->pending_frame;
        pending.fence_value = output->pending_fence;
        cancel_result = state->backend.cancel(state->backend.context,
                                              &pending);
        if (cancel_result != 0) return RIN_GPU_PRESENTATION_BACKEND;
    }
    if (image) make_image_available(image);
    output->pending_image = 0u;
    output->pending_frame = 0u;
    output->pending_fence = 0u;
    output->pending_mode = 0u;
    ++state->dropped_count;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_remove_output(
    RinGpuPresentationRuntime* runtime, uint32_t display_id,
    uint64_t next_output_generation) {
    PresentationState* state = presentation_state(runtime);
    PresentationOutputSlot* output;
    if (runtime_ready(state) != RIN_GPU_PRESENTATION_OK ||
        next_output_generation == 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    output = find_output(state, display_id);
    if (!output) return RIN_GPU_PRESENTATION_STALE;
    if (next_output_generation <= output->descriptor.output_generation)
        return RIN_GPU_PRESENTATION_STALE;
    if (output->pending_image != 0u) {
        if (!state->backend.cancel) return RIN_GPU_PRESENTATION_UNSUPPORTED;
        {
            RinGpuPresentationCompletionV1 pending;
            memset(&pending, 0, sizeof(pending));
            pending.struct_size = sizeof(pending);
            pending.version = RIN_GPU_PRESENTATION_VERSION;
            pending.display_id = display_id;
            pending.image_token = output->pending_image;
            pending.output_generation = output->descriptor.output_generation;
            pending.device_generation = state->device_generation;
            pending.frame_id = output->pending_frame;
            pending.fence_value = output->pending_fence;
            if (state->backend.cancel(state->backend.context, &pending) != 0)
                return RIN_GPU_PRESENTATION_BACKEND;
        }
    }
    invalidate_output(state, output);
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_IMAGES;
         ++index) {
        PresentationImageSlot* image = &state->images[index];
        if (image->slot_state == PRESENTATION_SLOT_LIVE &&
            image->descriptor.display_id == display_id)
            image->state = RIN_GPU_PRESENTATION_IMAGE_INVALID;
    }
    output->descriptor.output_generation = next_output_generation;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_suspend(RinGpuPresentationRuntime* runtime) {
    PresentationState* state = presentation_state(runtime);
    if (!state || state->magic != PRESENTATION_MAGIC)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (state->state == RIN_GPU_PRESENTATION_STATE_LOST)
        return RIN_GPU_PRESENTATION_DEVICE_LOST;
    if (state->state != RIN_GPU_PRESENTATION_STATE_ACTIVE)
        return RIN_GPU_PRESENTATION_STATE;
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_OUTPUTS;
         ++index) {
        PresentationOutputSlot* output = &state->outputs[index];
        if (output->slot_state != PRESENTATION_SLOT_LIVE) continue;
        if (output->pending_image != 0u) {
            int result = rin_gpu_presentation_drop_pending(
                runtime, output->descriptor.display_id,
                output->descriptor.output_generation,
                state->device_generation);
            if (result != RIN_GPU_PRESENTATION_OK) return result;
        }
    }
    state->state = RIN_GPU_PRESENTATION_STATE_SUSPENDED;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_resume(RinGpuPresentationRuntime* runtime) {
    PresentationState* state = presentation_state(runtime);
    if (!state || state->magic != PRESENTATION_MAGIC)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (state->state == RIN_GPU_PRESENTATION_STATE_LOST)
        return RIN_GPU_PRESENTATION_DEVICE_LOST;
    if (state->state != RIN_GPU_PRESENTATION_STATE_SUSPENDED)
        return RIN_GPU_PRESENTATION_STATE;
    state->state = RIN_GPU_PRESENTATION_STATE_ACTIVE;
    state->full_redraw_required = 1u;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_device_reset(
    RinGpuPresentationRuntime* runtime, uint64_t next_device_generation) {
    PresentationState* state = presentation_state(runtime);
    if (!state || state->magic != PRESENTATION_MAGIC ||
        next_device_generation == 0u)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (next_device_generation <= state->device_generation)
        return RIN_GPU_PRESENTATION_STALE;
    memset(state->outputs, 0, sizeof(state->outputs));
    memset(state->images, 0, sizeof(state->images));
    state->output_count = 0u;
    state->image_count = 0u;
    state->device_generation = next_device_generation;
    state->next_fence_value = 1u;
    state->state = RIN_GPU_PRESENTATION_STATE_ACTIVE;
    state->full_redraw_required = 1u;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_device_lost(RinGpuPresentationRuntime* runtime) {
    PresentationState* state = presentation_state(runtime);
    if (!state || state->magic != PRESENTATION_MAGIC)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (state->state == RIN_GPU_PRESENTATION_STATE_LOST)
        return RIN_GPU_PRESENTATION_DEVICE_LOST;
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_IMAGES;
         ++index) {
        if (state->images[index].slot_state == PRESENTATION_SLOT_LIVE)
            state->images[index].state = RIN_GPU_PRESENTATION_IMAGE_INVALID;
    }
    state->state = RIN_GPU_PRESENTATION_STATE_LOST;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_ack_full_redraw(RinGpuPresentationRuntime* runtime) {
    PresentationState* state = presentation_state(runtime);
    if (!state || state->magic != PRESENTATION_MAGIC)
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    if (state->state != RIN_GPU_PRESENTATION_STATE_ACTIVE)
        return state->state == RIN_GPU_PRESENTATION_STATE_LOST
                   ? RIN_GPU_PRESENTATION_DEVICE_LOST
                   : RIN_GPU_PRESENTATION_STATE;
    state->full_redraw_required = 0u;
    return RIN_GPU_PRESENTATION_OK;
}

int rin_gpu_presentation_get_status(
    RinGpuPresentationRuntime* runtime,
    RinGpuPresentationStatusV1* status_out) {
    PresentationState* state = presentation_state(runtime);
    if (!state || state->magic != PRESENTATION_MAGIC || !status_out ||
        !versioned(status_out->struct_size, status_out->version,
                   sizeof(*status_out)))
        return RIN_GPU_PRESENTATION_INVALID_ARGUMENT;
    memset(status_out, 0, sizeof(*status_out));
    status_out->struct_size = sizeof(*status_out);
    status_out->version = RIN_GPU_PRESENTATION_VERSION;
    status_out->state = state->state;
    status_out->output_count = state->output_count;
    status_out->image_count = state->image_count;
    status_out->full_redraw_required = state->full_redraw_required;
    status_out->device_generation = state->device_generation;
    status_out->next_fence_value = state->next_fence_value;
    status_out->submitted_count = state->submitted_count;
    status_out->completed_count = state->completed_count;
    status_out->dropped_count = state->dropped_count;
    status_out->stale_completion_count = state->stale_completion_count;
    for (uint32_t index = 0u; index < RIN_GPU_PRESENTATION_MAX_OUTPUTS;
         ++index) {
        const PresentationOutputSlot* output = &state->outputs[index];
        if (output->slot_state != PRESENTATION_SLOT_LIVE) continue;
        if (output->pending_image != 0u) ++status_out->pending_count;
        if (output->current_image != 0u) ++status_out->scanning_count;
    }
    return RIN_GPU_PRESENTATION_OK;
}
