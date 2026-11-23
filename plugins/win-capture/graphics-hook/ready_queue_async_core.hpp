// Common v2 queue publication for producers whose GPU work completes outside D3D11.
// The API adapter proves completion first, then calls publish().
#pragma once
#include <d3d11_4.h>
#include "ready_queue_protocol.hpp"

struct RQAsyncCore {
    ID3D11Device5 *device = nullptr;
    ID3D11DeviceContext4 *context = nullptr;
    ID3D11Fence *fence = nullptr;
    HANDLE mapping = nullptr, shared_fence = nullptr;
    RQShared *shared = nullptr;
    uint64_t sequence = 0, handoffs_submitted = 0;

    ~RQAsyncCore() { close(); }

    void close()
    {
        if (shared) {
            InterlockedExchange(&shared->alive, 0);
            hlog("[ready-queue-async] producer v2 stop submitted=%llu full=%lld handoffs=%llu",
                 sequence, shared->full, handoffs_submitted);
            UnmapViewOfFile(shared);
            shared = nullptr;
        }
        if (mapping) { CloseHandle(mapping); mapping = nullptr; }
        if (shared_fence) { CloseHandle(shared_fence); shared_fence = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (context) { context->Release(); context = nullptr; }
        if (device) { device->Release(); device = nullptr; }
    }

    bool init(ID3D11Device *base_device, ID3D11DeviceContext *base_context, HANDLE legacy_handle,
              uint32_t width, uint32_t height, DXGI_FORMAT format, const HANDLE handles[RQ_COUNT])
    {
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            format != DXGI_FORMAT_B8G8R8X8_UNORM)
            return false;
        if (FAILED(base_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device)) ||
            FAILED(base_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context)))
            return false;
        if (FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&fence)) ||
            FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_fence)))
            return false;
        wchar_t name[96];
        rq_name(name, (uint32_t)(uintptr_t)legacy_handle);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RQShared), name);
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
            return false;
        shared = (RQShared *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RQShared));
        if (!shared)
            return false;
        memset(shared, 0, sizeof(*shared));
        shared->magic = RQ_MAGIC;
        shared->version = RQ_VERSION;
        shared->bytes = sizeof(*shared);
        shared->pid = GetCurrentProcessId();
        shared->fence_handle = (uint64_t)(uintptr_t)shared_fence;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        shared->frequency = frequency.QuadPart;
        shared->width = width;
        shared->height = height;
        shared->format = format;
        for (unsigned i = 0; i < RQ_COUNT; ++i)
            shared->slots[i].handle = (uint32_t)(uintptr_t)handles[i];
        InterlockedExchange(&shared->alive, 1);
        return true;
    }

    int claim(bool &legacy_handoff)
    {
        legacy_handoff = false;
        if (!shared)
            return -1;
        if (rq_load(&shared->consumer) != 1 || rq_load(&shared->alive) != 1) {
            legacy_handoff = rq_load(&shared->handoff_request) == 1 &&
                             rq_load(&shared->handoff_status) == RQ_HANDOFF_PENDING;
            return -1;
        }
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (InterlockedCompareExchange(&shared->slots[i].state, RQ_WRITING, RQ_FREE) == RQ_FREE) {
                auto &slot = shared->slots[i];
                slot.sequence = ++sequence;
                slot.submitted = rq_clock();
                return (int)i;
            }
        }
        InterlockedIncrement64(&shared->full);
        return -2;
    }

    bool publish(int index)
    {
        if (!shared || index < 0 || index >= RQ_COUNT)
            return false;
        auto &slot = shared->slots[index];
        const HRESULT hr = context->Signal(fence, slot.sequence);
        if (FAILED(hr)) {
            InterlockedExchange(&shared->alive, 0);
            InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            hlog_hr("[ready-queue-async] queue fence failed", hr);
            return false;
        }
        ((ID3D11DeviceContext *)context)->Flush();
        InterlockedExchange(&slot.state, RQ_PUBLISHED);
        return true;
    }

    bool publish_handoff()
    {
        if (!shared || rq_load(&shared->alive) != 1) {
            if (shared) InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            return false;
        }
        const uint64_t value = ++sequence;
        const HRESULT hr = context->Signal(fence, value);
        if (FAILED(hr)) {
            InterlockedExchange(&shared->alive, 0);
            InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            return false;
        }
        ((ID3D11DeviceContext *)context)->Flush();
        InterlockedExchange64(&shared->handoff_fence_value, (LONG64)value);
        InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_SUBMITTED);
        ++handoffs_submitted;
        return true;
    }
};
