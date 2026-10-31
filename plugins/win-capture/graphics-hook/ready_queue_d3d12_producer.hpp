// D3D11On12 producer for the v2 ready queue. Included after create_d3d12_tex.
// The caller owns Acquire/ReleaseWrappedResources and performs the final Flush.
#include <d3d11_4.h>
#include "ready_queue_protocol.hpp"
#ifdef RQ_D3D12_TRACE
#include "ready_queue_trace.hpp"
#endif

struct RQD3D12Producer {
    ID3D11Device5 *device = nullptr;
    ID3D11DeviceContext4 *context = nullptr;
    ID3D11Fence *fence = nullptr;
    ID3D11Texture2D *textures[RQ_COUNT] = {};
    HANDLE mapping = nullptr, shared_fence = nullptr;
    RQShared *shared = nullptr;
#ifdef RQ_D3D12_TRACE
    RQTraceMap trace;
    uint64_t present_sequence = 0, current_present = 0, current_interval = 0;
    int64_t current_present_qpc = 0;
#endif
    uint64_t sequence = 0, handoffs_submitted = 0;
    int chosen = -1;
    bool handoff_after_legacy_copy = false;

    ~RQD3D12Producer()
    {
        if (shared) {
            InterlockedExchange(&shared->alive, 0);
            hlog("[ready-queue-d3d12] producer v2 stop submitted=%llu full=%lld handoffs_submitted=%llu",
                 sequence, shared->full, handoffs_submitted);
            UnmapViewOfFile(shared);
        }
        if (mapping) CloseHandle(mapping);
#ifdef RQ_D3D12_TRACE
        trace.close();
#endif
        if (shared_fence) CloseHandle(shared_fence);
        for (auto *texture : textures) if (texture) texture->Release();
        if (fence) fence->Release();
        if (context) context->Release();
        if (device) device->Release();
    }

    bool create_slot(ID3D11Texture2D **texture, HANDLE *handle)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        data.copy_tex->GetDesc(&desc);
        HRESULT hr = data.device11->CreateTexture2D(&desc, nullptr, texture);
        if (FAILED(hr))
            return false;
        IDXGIResource *resource = nullptr;
        hr = (*texture)->QueryInterface(__uuidof(IDXGIResource), (void **)&resource);
        if (FAILED(hr))
            return false;
        hr = resource->GetSharedHandle(handle);
        resource->Release();
        return SUCCEEDED(hr);
    }

    bool init()
    {
        if (data.format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            data.format != DXGI_FORMAT_R10G10B10A2_UNORM)
            return false;
        if (FAILED(data.device11->QueryInterface(__uuidof(ID3D11Device5), (void **)&device)) ||
            FAILED(data.context11->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context)))
            return false;
        if (FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&fence)) ||
            FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_fence)))
            return false;
        wchar_t name[96];
        rq_name(name, (uint32_t)(uintptr_t)data.handle);
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
        shared->width = data.cx;
        shared->height = data.cy;
        shared->format = data.format;
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            HANDLE handle = nullptr;
            if (!create_slot(&textures[i], &handle))
                return false;
            shared->slots[i].handle = (uint32_t)(uintptr_t)handle;
        }
#ifdef RQ_D3D12_TRACE
        const bool trace_ready = trace.create((uint32_t)(uintptr_t)data.handle, frequency.QuadPart);
#endif
        InterlockedExchange(&shared->alive, 1);
        hlog("[ready-queue-d3d12] producer v2 available; format=%u, 8 slots, shared fence",
             (unsigned)data.format);
#ifdef RQ_D3D12_TRACE
        hlog("[ready-queue-trace] D3D12 producer ring %s; capacity=%u event_bytes=%u",
             trace_ready ? "ready" : "unavailable", RQ_TRACE_CAPACITY,
             (unsigned)sizeof(RQTraceEvent));
#endif
        return true;
    }

#ifdef RQ_D3D12_TRACE
    void on_present(bool capture, uint64_t interval)
    {
        current_present = ++present_sequence;
        current_present_qpc = rq_clock();
        current_interval = interval;
        if (!capture && shared && rq_load(&shared->consumer) == 1)
            trace.emit(RQ_TRACE_PRODUCER_SKIP, current_present_qpc, current_present,
                       sequence, 0, interval,
                       (uint64_t)InterlockedCompareExchange64(&shared->full, 0, 0));
    }
#endif

    ID3D11Texture2D *begin()
    {
        chosen = -1;
        handoff_after_legacy_copy = false;
        if (rq_load(&shared->consumer) != 1 || rq_load(&shared->alive) != 1) {
            handoff_after_legacy_copy = rq_load(&shared->handoff_request) == 1 &&
                                       rq_load(&shared->handoff_status) == RQ_HANDOFF_PENDING;
            return data.copy_tex;
        }
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (InterlockedCompareExchange(&shared->slots[i].state, RQ_WRITING, RQ_FREE) == RQ_FREE) {
                chosen = (int)i;
                auto &slot = shared->slots[i];
                slot.sequence = ++sequence;
                slot.submitted = rq_clock();
                return textures[i];
            }
        }
        InterlockedIncrement64(&shared->full);
#ifdef RQ_D3D12_TRACE
        trace.emit(RQ_TRACE_PRODUCER_FULL, rq_clock(), current_present,
                   sequence, current_present_qpc, current_interval,
                   (uint64_t)InterlockedCompareExchange64(&shared->full, 0, 0));
#endif
        return nullptr;
    }

    void end(uint32_t marker)
    {
        if (chosen >= 0) {
            auto &slot = shared->slots[chosen];
#ifdef RQ_D3D12_TRACE
            slot.marker_id = marker ? marker : (uint32_t)current_present;
#else
            slot.marker_id = marker;
#endif
            const HRESULT hr = context->Signal(fence, slot.sequence);
            if (FAILED(hr)) {
                InterlockedExchange(&shared->alive, 0);
                InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
                hlog_hr("[ready-queue-d3d12] queue fence failed", hr);
                return;
            }
            InterlockedExchange(&slot.state, RQ_PUBLISHED);
#ifdef RQ_D3D12_TRACE
            trace.emit(RQ_TRACE_PRODUCER_PUBLISH, rq_clock(), current_present,
                       slot.sequence, (uint64_t)slot.submitted, current_interval,
                       (uint64_t)InterlockedCompareExchange64(&shared->full, 0, 0),
                       (uint32_t)chosen);
#endif
            return;
        }
        if (!handoff_after_legacy_copy)
            return;
        handoff_after_legacy_copy = false;
        if (rq_load(&shared->alive) != 1) {
            InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            return;
        }
        const uint64_t handoff_value = ++sequence;
        const HRESULT hr = context->Signal(fence, handoff_value);
        if (FAILED(hr)) {
            InterlockedExchange(&shared->alive, 0);
            InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            return;
        }
        InterlockedExchange64(&shared->handoff_fence_value, (LONG64)handoff_value);
        InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_SUBMITTED);
        ++handoffs_submitted;
    }
};

static RQD3D12Producer *rq_d3d12_producer = nullptr;
static void rq_d3d12_producer_free() { delete rq_d3d12_producer; rq_d3d12_producer = nullptr; }
static void rq_d3d12_producer_init()
{
    rq_d3d12_producer_free();
    try {
        rq_d3d12_producer = new RQD3D12Producer;
        if (!rq_d3d12_producer->init())
            rq_d3d12_producer_free();
    } catch (...) {
        rq_d3d12_producer_free();
    }
}
