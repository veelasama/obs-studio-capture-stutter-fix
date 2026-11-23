// Included after create_d3d11_tex. No CPU waits for the consumer or GPU.
#include <d3d11_4.h>
#include "ready_queue_protocol.hpp" // Builder installs the v2 header under this name.

#ifdef RQ_HANDOFF_TEST
static volatile LONG rq_handoff_test_delay_used = 0;
static DWORD rq_handoff_delay_ms()
{
    wchar_t text[32] = {};
    const DWORD length = GetEnvironmentVariableW(L"OBS_RQ_TEST_HANDOFF_DELAY_MS", text, 32);
    if (!length || length >= 32)
        return 0;
    DWORD value = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9')
            return 0;
        // Saturate while parsing; arbitrarily large valid integers cannot overflow.
        value = value >= 1500 ? 1500 : value * 10 + DWORD(text[i] - L'0');
    }
    return value > 1500 ? 1500 : value;
}
#endif

struct RQProducer {
    ID3D11Device5 *device = nullptr;
    ID3D11DeviceContext4 *context = nullptr;
    ID3D11Fence *fence = nullptr;
    ID3D11Texture2D *textures[RQ_COUNT] = {};
    HANDLE mapping = nullptr, shared_fence = nullptr;
    RQShared *shared = nullptr;
    uint64_t sequence = 0, handoffs_submitted = 0;
    int chosen = -1;
    bool handoff_after_legacy_copy = false;
#ifdef RQ_HANDOFF_TEST
    bool delay_waiting = false;
    int64_t delay_until = 0;
#endif

    ~RQProducer()
    {
        if (shared) {
            InterlockedExchange(&shared->alive, 0);
#ifdef RQ_HANDOFF_TEST
            if (delay_waiting)
                hlog("[ready-queue] v2 test handoff delay aborted by producer destruction");
#endif
            hlog("[ready-queue] producer v2 stop submitted=%llu full=%lld handoffs_submitted=%llu",
                 sequence, shared->full, handoffs_submitted);
            UnmapViewOfFile(shared);
        }
        if (mapping) CloseHandle(mapping);
        if (shared_fence) CloseHandle(shared_fence);
        for (auto *texture : textures) if (texture) texture->Release();
        if (fence) fence->Release();
        if (context) context->Release();
        if (device) device->Release();
    }

    bool init()
    {
        if (data.format != DXGI_FORMAT_R8G8B8A8_UNORM)
            return false;
        if (FAILED(data.device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device)) ||
            FAILED(data.context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context)))
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
            if (!create_d3d11_tex(data.cx, data.cy, &textures[i], &handle))
                return false;
            shared->slots[i].handle = (uint32_t)(uintptr_t)handle;
        }
        InterlockedExchange(&shared->alive, 1);
        hlog("[ready-queue] producer v2 available; 8 slots, shared fence, one-way legacy handoff");
        return true;
    }

    ID3D11Texture2D *begin()
    {
        chosen = -1;
        handoff_after_legacy_copy = false;
        if (rq_load(&shared->consumer) != 1 || rq_load(&shared->alive) != 1) {
            handoff_after_legacy_copy = rq_load(&shared->handoff_request) == 1 &&
                                       rq_load(&shared->handoff_status) == RQ_HANDOFF_PENDING;
#ifdef RQ_HANDOFF_TEST
            if (handoff_after_legacy_copy && rq_load(&shared->alive) == 1) {
                const DWORD delay = rq_handoff_delay_ms();
                if (!delay_waiting && delay &&
                    InterlockedCompareExchange(&rq_handoff_test_delay_used, 1, 0) == 0) {
                    delay_waiting = true;
                    delay_until = rq_clock() + shared->frequency * delay / 1000;
                    hlog("[ready-queue] v2 test handoff delay start %lu ms; capture copies deferred, game Present not blocked", delay);
                }
                if (delay_waiting) {
                    if (rq_clock() < delay_until) {
                        handoff_after_legacy_copy = false;
                        return nullptr; // Caller must skip the actual copy and end().
                    }
                    delay_waiting = false;
                    hlog("[ready-queue] v2 test handoff delay end; next legacy copy may publish completion");
                }
            }
#endif
            return data.texture;
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
        return nullptr;
    }

    void end(uint32_t marker)
    {
        if (chosen >= 0) {
            auto &slot = shared->slots[chosen];
            slot.marker_id = marker;
            const HRESULT hr = context->Signal(fence, slot.sequence);
            if (FAILED(hr)) {
                InterlockedExchange(&shared->alive, 0);
                InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
                hlog_hr("[ready-queue] producer v2 queue fence failed", hr);
                return;
            }
            data.context->Flush();
            InterlockedExchange(&slot.state, RQ_PUBLISHED);
            return; // A request arriving during this queue copy is handled by next begin().
        }
        if (!handoff_after_legacy_copy)
            return; // Ordinary legacy capture has no added Signal or Flush.
        handoff_after_legacy_copy = false;
        if (rq_load(&shared->alive) != 1) {
            InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            hlog("[ready-queue] producer v2 handoff failed: producer already unavailable");
            return;
        }
        // Caller has already copied this frame into data.texture (the legacy texture).
        // This value is greater than all previously assigned queue fence values.
        const uint64_t handoff_value = ++sequence;
        const HRESULT hr = context->Signal(fence, handoff_value);
        if (FAILED(hr)) {
            InterlockedExchange(&shared->alive, 0);
            InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_FAILED);
            hlog_hr("[ready-queue] producer v2 legacy handoff fence failed", hr);
            return;
        }
        data.context->Flush();
        InterlockedExchange64(&shared->handoff_fence_value, (LONG64)handoff_value);
        InterlockedExchange(&shared->handoff_status, RQ_HANDOFF_SUBMITTED);
        ++handoffs_submitted;
        hlog("[ready-queue] producer v2 legacy handoff submitted fence=%llu count=%llu", handoff_value, handoffs_submitted);
    }
};

static RQProducer *rq_producer = nullptr;
static void rq_producer_free() { delete rq_producer; rq_producer = nullptr; }
static void rq_producer_init()
{
    rq_producer_free();
    try {
        rq_producer = new RQProducer;
        if (!rq_producer->init())
            rq_producer_free();
    } catch (...) {
        rq_producer_free();
    }
}
