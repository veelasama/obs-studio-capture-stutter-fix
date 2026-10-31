// Vulkan adapter: each v2 slot owns a D3D11 shared texture imported as a VkImage.
// A VkFence proves the copy complete before the common D3D11 shared fence is signaled.
#pragma once
#include <d3d11_4.h>
#include "ready_queue_c_protocol.h"

struct rq_vk_slot {
    ID3D11Texture2D *texture11;
    HANDLE handle;
    VkImage image;
    VkDeviceMemory memory;
    bool layout_initialized;
};

struct rq_vk_producer {
    struct vk_data *data;
    struct vk_swap_data *swap;
    ID3D11Device5 *device11;
    ID3D11DeviceContext4 *context11;
    ID3D11Fence *fence11;
    HANDLE mapping, shared_fence;
    RQCShared *shared;
    struct rq_vk_slot slots[RQC_COUNT];
    uint64_t sequence, handoffs;
};

static void rq_vk_core_close(struct rq_vk_producer *rq)
{
    if (rq->shared) {
        InterlockedExchange(&rq->shared->alive, 0);
        hlog("[ready-queue-vulkan] producer v2 stop submitted=%llu full=%lld handoffs=%llu",
             rq->sequence, rq->shared->full, rq->handoffs);
        UnmapViewOfFile(rq->shared);
        rq->shared = NULL;
    }
    if (rq->mapping) { CloseHandle(rq->mapping); rq->mapping = NULL; }
    if (rq->shared_fence) { CloseHandle(rq->shared_fence); rq->shared_fence = NULL; }
    if (rq->fence11) { ID3D11Fence_Release(rq->fence11); rq->fence11 = NULL; }
    if (rq->context11) { ID3D11DeviceContext4_Release(rq->context11); rq->context11 = NULL; }
    if (rq->device11) { ID3D11Device5_Release(rq->device11); rq->device11 = NULL; }
}

static bool rq_vk_core_init(struct rq_vk_producer *rq)
{
    HRESULT hr = ID3D11Device_QueryInterface(rq->data->d3d11_device, &IID_ID3D11Device5,
                                             (void **)&rq->device11);
    if (FAILED(hr)) return false;
    hr = ID3D11DeviceContext_QueryInterface(rq->data->d3d11_context, &IID_ID3D11DeviceContext4,
                                            (void **)&rq->context11);
    if (FAILED(hr)) return false;
    hr = ID3D11Device5_CreateFence(rq->device11, 0, D3D11_FENCE_FLAG_SHARED,
                                  &IID_ID3D11Fence, (void **)&rq->fence11);
    if (FAILED(hr)) return false;
    hr = ID3D11Fence_CreateSharedHandle(rq->fence11, NULL, GENERIC_ALL, NULL, &rq->shared_fence);
    if (FAILED(hr)) return false;
    wchar_t name[96];
    rqc_name(name, (uint32_t)(uintptr_t)rq->swap->handle);
    rq->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(RQCShared), name);
    if (!rq->mapping || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    rq->shared = MapViewOfFile(rq->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RQCShared));
    if (!rq->shared) return false;
    memset(rq->shared, 0, sizeof(*rq->shared));
    rq->shared->magic = RQC_MAGIC;
    rq->shared->version = RQC_VERSION;
    rq->shared->bytes = sizeof(*rq->shared);
    rq->shared->pid = GetCurrentProcessId();
    rq->shared->fence_handle = (uint64_t)(uintptr_t)rq->shared_fence;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    rq->shared->frequency = frequency.QuadPart;
    rq->shared->width = rq->swap->image_extent.width;
    rq->shared->height = rq->swap->image_extent.height;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(rq->slots[0].texture11, &desc);
    rq->shared->format = desc.Format;
    for (unsigned i = 0; i < RQC_COUNT; ++i)
        rq->shared->slots[i].handle = (uint32_t)(uintptr_t)rq->slots[i].handle;
    InterlockedExchange(&rq->shared->alive, 1);
    return true;
}

static void rq_vk_free(struct vk_data *data, struct vk_swap_data *swap)
{
    struct rq_vk_producer *rq = swap->rq;
    if (!rq) return;
    swap->rq = NULL;
    rq_vk_core_close(rq);
    for (unsigned i = 0; i < RQC_COUNT; ++i) {
        if (rq->slots[i].image)
            data->funcs.DestroyImage(data->device, rq->slots[i].image, data->ac);
        if (rq->slots[i].memory)
            data->funcs.FreeMemory(data->device, rq->slots[i].memory, NULL);
        if (rq->slots[i].texture11)
            ID3D11Texture2D_Release(rq->slots[i].texture11);
    }
    free(rq);
}

static bool rq_vk_init(struct vk_data *data, struct vk_swap_data *swap)
{
    rq_vk_free(data, swap);
    struct rq_vk_producer *rq = calloc(1, sizeof(*rq));
    if (!rq) return false;
    rq->data = data;
    rq->swap = swap;
    swap->rq = rq;
    for (unsigned i = 0; i < RQC_COUNT; ++i) {
        struct vk_swap_data temp = *swap;
        temp.d3d11_tex = NULL;
        temp.handle = INVALID_HANDLE_VALUE;
        temp.export_image = VK_NULL_HANDLE;
        temp.export_mem = VK_NULL_HANDLE;
        temp.layout_initialized = false;
        temp.rq = NULL;
        if (!vk_shtex_init_d3d11_tex(data, &temp) || !vk_shtex_init_vulkan_tex(data, &temp)) {
            if (temp.export_image) data->funcs.DestroyImage(data->device, temp.export_image, data->ac);
            if (temp.export_mem) data->funcs.FreeMemory(data->device, temp.export_mem, NULL);
            if (temp.d3d11_tex) ID3D11Texture2D_Release(temp.d3d11_tex);
            rq_vk_free(data, swap);
            return false;
        }
        rq->slots[i].texture11 = temp.d3d11_tex;
        rq->slots[i].handle = temp.handle;
        rq->slots[i].image = temp.export_image;
        rq->slots[i].memory = temp.export_mem;
    }
    if (!rq_vk_core_init(rq)) {
        rq_vk_free(data, swap);
        return false;
    }
    hlog("[ready-queue-vulkan] producer v2 available; 8 imported images, VkFence completion");
    return true;
}

static int rq_vk_claim(struct rq_vk_producer *rq, bool *handoff)
{
    *handoff = false;
    if (rqc_load(&rq->shared->consumer) != 1 || rqc_load(&rq->shared->alive) != 1) {
        *handoff = rqc_load(&rq->shared->handoff_request) == 1 &&
                   rqc_load(&rq->shared->handoff_status) == RQC_HANDOFF_PENDING;
        return -1;
    }
    for (unsigned i = 0; i < RQC_COUNT; ++i) {
        if (InterlockedCompareExchange(&rq->shared->slots[i].state, RQC_WRITING, RQC_FREE) == RQC_FREE) {
            RQCSlot *slot = &rq->shared->slots[i];
            slot->sequence = ++rq->sequence;
            slot->submitted = rqc_clock();
            return (int)i;
        }
    }
    InterlockedIncrement64(&rq->shared->full);
    return -2;
}

static bool rq_vk_signal(struct rq_vk_producer *rq, uint64_t value)
{
    HRESULT hr = ID3D11DeviceContext4_Signal(rq->context11, rq->fence11, value);
    if (FAILED(hr)) {
        InterlockedExchange(&rq->shared->alive, 0);
        InterlockedExchange(&rq->shared->handoff_status, RQC_HANDOFF_FAILED);
        return false;
    }
    ID3D11DeviceContext_Flush((ID3D11DeviceContext *)rq->context11);
    return true;
}

static void rq_vk_frame_complete(struct rq_vk_producer *rq, int slot, bool handoff)
{
    if (!rq || !rq->shared) return;
    if (slot >= 0) {
        RQCSlot *shared_slot = &rq->shared->slots[slot];
        if (rq_vk_signal(rq, shared_slot->sequence))
            InterlockedExchange(&shared_slot->state, RQC_PUBLISHED);
    } else if (handoff) {
        uint64_t value = ++rq->sequence;
        if (rq_vk_signal(rq, value)) {
            InterlockedExchange64(&rq->shared->handoff_fence_value, (LONG64)value);
            InterlockedExchange(&rq->shared->handoff_status, RQC_HANDOFF_SUBMITTED);
            ++rq->handoffs;
        }
    }
}

static void rq_vk_abort(struct rq_vk_producer *rq, int slot)
{
    if (rq && rq->shared && slot >= 0)
        InterlockedExchange(&rq->shared->slots[slot].state, RQC_FREE);
}
