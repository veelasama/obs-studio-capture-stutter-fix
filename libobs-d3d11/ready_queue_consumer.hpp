// Experimental v2 one-way fresh legacy handoff; no CPU waits or per-frame probes.
// Include in d3d11-subsystem.cpp after the OBS D3D11 types are defined.
#include <d3d11_4.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "ready_queue_protocol.hpp"
#include "ready_queue_lowlatency.hpp"

#ifdef RQ_LIFECYCLE_TEST
static uint64_t rq_test_threshold(const wchar_t *name)
{
    wchar_t text[32] = {};
    const DWORD length = GetEnvironmentVariableW(name, text, 32);
    if (!length || length >= 32)
        return 0;
    uint64_t value = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9')
            return 0;
        const uint64_t digit = uint64_t(text[i] - L'0');
        if (value > (UINT64_MAX - digit) / 10)
            return 0;
        value = value * 10 + digit;
    }
    return value;
}
static bool rq_test_retire_fired = false;
static bool rq_test_rebuild_fired = false;
#endif

struct RQConsumer {
    gs_device_t *owner;
    gs_texture_2d *output = nullptr; // Returned to, and destroyed by, libobs.
    std::unique_ptr<gs_texture_2d> opening_output; // Owns private output until API handoff succeeds.
    std::unique_ptr<gs_texture_2d> legacy; // Stays registered with core gs_obj lifecycle.
    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11Fence> producer_done, consumer_done;
    ComPtr<ID3D11Fence> handoff_done; // Always opened on the current OBS device.
    ComPtr<ID3D11Texture2D> textures[RQ_COUNT];
    HANDLE mapping = nullptr;
    RQShared *shared = nullptr;
    uint64_t releases[RQ_COUNT] = {}, serial = 0, selected = 0, repeats = 0, drops = 0;
    int64_t started = 0;
    bool attached = false, failed = false, primed = false, latest = false, lowlatency = false;
    RQLowLatency ll;
    bool mapping_found = false;
    bool fallback_error_reported = false, fallback_clear_attempted = false;
    bool clear_after_rebuild = false, handoff_ready = false;
    bool handoff_open_attempted = false, handoff_unavailable = false;
    bool handoff_pending_reported = false, handoff_ack_reported = false;

    explicit RQConsumer(gs_device_t *d) : owner(d) {}

    void retire(const char *reason)
    {
        if (failed)
            return;
        failed = true;
        if (attached && shared) {
            // Ordering matters: a producer must first switch away from queue destinations.
            InterlockedExchange(&shared->consumer, -1);
            InterlockedExchange(&shared->handoff_request, 1);
        }
        if (attached)
            blog(LOG_WARNING, "[ready-queue] retired: %s; v2 fresh legacy handoff requested", reason);
        // Do not mark unconfirmed slots FREE. The retired mapping cannot reattach.
        for (auto &texture : textures)
            texture.Clear();
        consumer_done.Clear();
        producer_done.Clear();
        context.Clear();
        device.Clear();
    }

    ~RQConsumer()
    {
        retire("consumer destruction");
        if (shared)
            UnmapViewOfFile(shared);
        if (mapping)
            CloseHandle(mapping);
        blog(LOG_INFO, "[ready-queue] consumer stop selections=%llu repeats=%llu age_drops=%llu latency_sheds=%llu",
             serial, repeats, drops, ll.sheds);
        // legacy is destroyed here; output is still owned by the API caller.
    }

    static bool rgba8(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
               format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

    static bool bgra8(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    static bool bgrx8(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_B8G8R8X8_TYPELESS || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
               format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    }

    static bool supported_color32(DXGI_FORMAT format)
    {
        return rgba8(format) || bgra8(format) || bgrx8(format) ||
               format == DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    static bool copy_compatible(const D3D11_TEXTURE2D_DESC &a, const D3D11_TEXTURE2D_DESC &b)
    {
        return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
               a.ArraySize == b.ArraySize && a.SampleDesc.Count == b.SampleDesc.Count &&
               a.SampleDesc.Quality == b.SampleDesc.Quality &&
               (a.Format == b.Format || (rgba8(a.Format) && rgba8(b.Format)) ||
                (bgra8(a.Format) && bgra8(b.Format)) || (bgrx8(a.Format) && bgrx8(b.Format)));
    }

    bool current_resource(ID3D11Texture2D *texture) const
    {
        if (!texture || !owner->device || !owner->context)
            return false;
        ComPtr<ID3D11Device> resource_device;
        texture->GetDevice(&resource_device);
        return resource_device.Get() == owner->device.Get() && SUCCEEDED(owner->device->GetDeviceRemovedReason());
    }

    void fallback_error(const char *reason)
    {
        handoff_unavailable = true;
        if (!fallback_error_reported) {
            blog(LOG_ERROR, "[ready-queue] handoff v2 unavailable: %s; no fresh legacy copy claimed", reason);
            fallback_error_reported = true;
        }
    }

    bool clear_rebuilt_output()
    {
        // Normal retirement preserves the last private frame. Initial zero data is
        // retained by gs_texture_2d, so core Rebuild also starts with defined black.
        // Reuse that backup here; never allocate a new clear buffer during recovery.
        if (fallback_clear_attempted)
            return false;
        if (!output || !current_resource(output->texture)) {
            fallback_error("rebuilt private output missing or on a removed device");
            return false;
        }
        fallback_clear_attempted = true;
        D3D11_TEXTURE2D_DESC desc = {};
        output->texture->GetDesc(&desc);
        if (!supported_color32(desc.Format) || !desc.Width || !desc.Height || desc.MipLevels != 1 ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Usage != D3D11_USAGE_DEFAULT) {
            fallback_error("rebuilt private output cannot be safely cleared");
            return false;
        }
        if (output->data.size() != 1 || output->data[0].size() != size_t(desc.Width) * desc.Height * 4) {
            fallback_error("rebuilt private output zero backup missing");
            return false;
        }
        owner->context->UpdateSubresource(output->texture, 0, nullptr, output->data[0].data(), desc.Width * 4, 0);
        clear_after_rebuild = false;
        return true;
    }

    void handoff_pending()
    {
        if (!handoff_pending_reported) {
            blog(LOG_INFO, "[ready-queue] handoff v2 pending; retaining initialized private output");
            handoff_pending_reported = true;
        }
    }

    bool open_handoff_fence()
    {
        if (handoff_done)
            return true;
        if (handoff_open_attempted)
            return false;
        handoff_open_attempted = true;
        ComPtr<ID3D11Device5> current_device;
        if (FAILED(owner->device->QueryInterface(__uuidof(ID3D11Device5), (void **)&current_device))) {
            fallback_error("current device cannot open a shared fence");
            return false;
        }
        HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, shared->pid), fence_handle = nullptr;
        if (!process) {
            fallback_error("producer process unavailable for fence duplication");
            return false;
        }
        const BOOL duplicated = DuplicateHandle(process, (HANDLE)(uintptr_t)shared->fence_handle,
                                                GetCurrentProcess(), &fence_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CloseHandle(process);
        if (!duplicated) {
            fallback_error("producer fence handle duplication failed");
            return false;
        }
        const HRESULT opened = current_device->OpenSharedFence(fence_handle, __uuidof(ID3D11Fence),
                                                               (void **)&handoff_done);
        CloseHandle(fence_handle);
        if (FAILED(opened)) {
            fallback_error("producer fence cannot be opened on current device");
            return false;
        }
        return true;
    }

    void fallback_tick()
    {
        if (clear_after_rebuild && !clear_rebuilt_output())
            return;
        if (handoff_unavailable)
            return;
        if (!legacy || !legacy->isShared || !output ||
            !current_resource(legacy->texture) || !current_resource(output->texture)) {
            fallback_error("missing, substituted or lost-device resource");
            return;
        }
        D3D11_TEXTURE2D_DESC source_desc = {}, output_desc = {};
        legacy->texture->GetDesc(&source_desc);
        output->texture->GetDesc(&output_desc);
        if (!copy_compatible(source_desc, output_desc)) {
            fallback_error("texture dimensions, format family or sample layout differ");
            return;
        }
        if (!shared || rq_load(&shared->alive) != 1 || rq_load(&shared->handoff_request) != 1) {
            fallback_error("producer stopped or handoff request absent");
            return;
        }
        const LONG status = rq_load(&shared->handoff_status);
        if (status == RQ_HANDOFF_FAILED) {
            fallback_error("producer reported handoff failure");
            return;
        }
        if (status == RQ_HANDOFF_PENDING) {
            handoff_pending();
            return;
        }
        if (status != RQ_HANDOFF_SUBMITTED) {
            fallback_error("invalid producer handoff status");
            return;
        }
        if (!handoff_ready) {
            const uint64_t value = (uint64_t)InterlockedCompareExchange64(&shared->handoff_fence_value, 0, 0);
            if (!value || value == UINT64_MAX) {
                fallback_error("invalid producer handoff fence value");
                return;
            }
            if (!open_handoff_fence())
                return;
            const uint64_t completed = handoff_done->GetCompletedValue();
            if (completed == UINT64_MAX) {
                fallback_error("handoff fence reported device removal");
                return;
            }
            if (completed < value) {
                handoff_pending();
                return;
            }
            // This proves one legacy write after retirement completed. It is not a
            // per-frame synchronization contract for subsequent stock legacy capture.
            handoff_ready = true;
            if (!handoff_ack_reported) {
                blog(LOG_INFO, "[ready-queue] handoff v2 ready fence=%llu; stock legacy capture resumes", value);
                handoff_ack_reported = true;
            }
        }
        // Recheck terminal publication before enqueueing the first/next legacy read.
        if (rq_load(&shared->alive) != 1 || rq_load(&shared->handoff_status) != RQ_HANDOFF_SUBMITTED) {
            fallback_error("producer became unavailable after handoff");
            return;
        }
        owner->context->CopyResource(output->texture, legacy->texture);
    }

    bool open(uint32_t handle, gs_texture_2d *legacy_input)
    {
        wchar_t name[96];
        rq_name(name, handle);
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!mapping) {
            // A missing v2 endpoint may use stock capture. Other mapping errors fail closed.
            mapping_found = GetLastError() != ERROR_FILE_NOT_FOUND;
            return false;
        }
        mapping_found = true;
        shared = (RQShared *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RQShared));
        if (!shared)
            return false;
        if (shared->magic != RQ_MAGIC || shared->version != RQ_VERSION || shared->bytes != sizeof(*shared) ||
            shared->frequency <= 0 || shared->width != legacy_input->width ||
            shared->height != legacy_input->height || !supported_color32((DXGI_FORMAT)shared->format))
            return false;
        if (rq_load(&shared->consumer) != 0) {
            blog(LOG_WARNING, "[ready-queue] duplicate v2 open refused; existing consumer is unchanged");
            return false;
        }
        if (rq_load(&shared->alive) != 1 || rq_load(&shared->handoff_request) != 0 ||
            rq_load(&shared->handoff_status) != RQ_HANDOFF_PENDING)
            return false;
        if (FAILED(owner->device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device)) ||
            FAILED(owner->context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context)))
            return false;
        HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, shared->pid), fence_handle = nullptr;
        if (!process)
            return false;
        const BOOL duplicated = DuplicateHandle(process, (HANDLE)(uintptr_t)shared->fence_handle,
                                                GetCurrentProcess(), &fence_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CloseHandle(process);
        if (!duplicated)
            return false;
        const HRESULT opened = device->OpenSharedFence(fence_handle, __uuidof(ID3D11Fence), (void **)&producer_done);
        CloseHandle(fence_handle);
        if (FAILED(opened) || FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_NONE,
                                                       __uuidof(ID3D11Fence), (void **)&consumer_done)))
            return false;
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (FAILED(owner->device->OpenSharedResource((HANDLE)(uintptr_t)shared->slots[i].handle,
                                                        __uuidof(ID3D11Texture2D), (void **)&textures[i])))
                return false;
            D3D11_TEXTURE2D_DESC desc = {};
            textures[i]->GetDesc(&desc);
            if (desc.Width != legacy_input->width || desc.Height != legacy_input->height ||
                desc.Format != legacy_input->td.Format || desc.SampleDesc.Count != 1)
                return false;
        }
        std::vector<uint32_t> zeros(size_t(legacy_input->width) * legacy_input->height);
        const uint8_t *initial_data[] = {reinterpret_cast<const uint8_t *>(zeros.data())};
        // Flags=0 creates DEFAULT, not IMMUTABLE: CopyResource may continue writing.
        // OBS retains one zero CPU backup (width*height*4 bytes) for future Rebuild.
        auto fresh = std::make_unique<gs_texture_2d>(owner, legacy_input->width, legacy_input->height,
                                                    legacy_input->format, 1, initial_data, 0, GS_TEXTURE_2D, false);
        if (rq_load(&shared->alive) != 1)
            return false;
        const LONG previous = InterlockedCompareExchange(&shared->consumer, 1, 0);
        if (previous != 0)
            return false; // Another opt-in consumer won; never expose raw legacy.
        attached = true;
        opening_output = std::move(fresh);
        output = opening_output.get();
        started = rq_clock();
        wchar_t policy[16] = {};
        GetEnvironmentVariableW(L"OBS_READY_QUEUE_POLICY", policy, 16);
        // Default: lowlatency.  "bounded" keeps the N policy for comparison.
        latest = wcscmp(policy, L"latest") == 0;
        lowlatency = !latest && wcscmp(policy, L"bounded") != 0;
        ll.init(shared->frequency);
        blog(LOG_INFO, "[ready-queue] handoff consumer v2 attached; legacy retained; policy=%s",
             latest ? "latest" : lowlatency ? "lowlatency" : "bounded");
        return true;
    }

    void tick()
    {
        if (failed || !shared || rq_load(&shared->alive) != 1 || rq_load(&shared->consumer) != 1) {
            retire(failed ? "previously retired" : "producer stopped or shared consumer retired");
            fallback_tick();
            return;
        }
#ifdef RQ_LIFECYCLE_TEST
        static const uint64_t retire_after = rq_test_threshold(L"OBS_RQ_TEST_RETIRE_AFTER");
        if (!rq_test_retire_fired && retire_after && serial >= retire_after) {
            rq_test_retire_fired = true;
            retire("test-only selection threshold");
            fallback_tick();
            return;
        }
#endif
        const uint64_t p = producer_done->GetCompletedValue(), c = consumer_done->GetCompletedValue();
        if (p == UINT64_MAX || c == UINT64_MAX) {
            retire("queue fence reported device removal");
            fallback_tick();
            return;
        }
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (releases[i] && releases[i] <= c) {
                releases[i] = 0;
                InterlockedExchange(&shared->slots[i].state, RQ_FREE);
            }
        }
        const int64_t now = rq_clock();
        unsigned ready[RQ_COUNT], count = 0;
        for (unsigned i = 0; i < RQ_COUNT; ++i)
            if (rq_load(&shared->slots[i].state) == RQ_PUBLISHED && shared->slots[i].sequence <= p)
                ready[count++] = i;
        std::sort(ready, ready + count, [&](unsigned a, unsigned b) {
            return shared->slots[a].sequence < shared->slots[b].sequence;
        });
        unsigned k = 0;
        if (lowlatency) {
            uint64_t seq[RQ_COUNT];
            int64_t submitted[RQ_COUNT];
            ll.begin_tick(now);
            for (unsigned i = 0; i < RQ_COUNT; ++i)
                if (rq_load(&shared->slots[i].state) == RQ_PUBLISHED && shared->slots[i].sequence > p)
                    ll.observe_pending(now, shared->slots[i].submitted);
            for (unsigned i = 0; i < count; ++i) {
                seq[i] = shared->slots[ready[i]].sequence;
                submitted[i] = shared->slots[ready[i]].submitted;
            }
            const int chosen = ll.select(seq, submitted, count, RQ_COUNT, now);
            if (chosen < 0) {
                ++repeats;
                return;
            }
            while (k < (unsigned)chosen) {
                InterlockedExchange(&shared->slots[ready[k++]].state, RQ_FREE);
                ++drops;
            }
            consume(ready[k]);
            return;
        }
        bool fast_ready_stream = false;
        if (count >= 3) {
            const int64_t span = shared->slots[ready[count - 1]].submitted -
                                 shared->slots[ready[0]].submitted;
            fast_ready_stream = span > 0 &&
                                span * 1000.0 / shared->frequency /
                                        (count - 1) <
                                    14.0;
        }
        if (!primed) {
            if (!latest && count < 3 && (now - started) * 1000.0 / shared->frequency < 100)
                return;
            if (!count)
                return;
            primed = true;
        }
        if (latest)
            while (k + 1 < count) {
                InterlockedExchange(&shared->slots[ready[k++]].state, RQ_FREE);
                ++drops;
            }
        // Near 60 FPS, wait for the stock hook limiter to absorb a close Present
        // pair and enable cleanup only near saturation.  For a clearly faster
        // ready stream, retain M's original cleanup so 90/120/240 -> 60
        // decimation does not change cadence.
        if (!latest &&
            (count - k >= RQ_COUNT - 2 || fast_ready_stream))
            while (k + 1 < count &&
                   (now - shared->slots[ready[k]].submitted) * 1000.0 /
                           shared->frequency >
                       66.666667) {
                InterlockedExchange(&shared->slots[ready[k++]].state, RQ_FREE);
                ++drops;
            }
        if (k < count)
            consume(ready[k]);
        else
            ++repeats;
    }

    void consume(unsigned i)
    {
        auto &slot = shared->slots[i];
        selected = slot.sequence;
        InterlockedExchange(&slot.state, RQ_READING);
        owner->context->CopyResource(output->texture, textures[i]);
        const HRESULT hr = context->Signal(consumer_done, ++serial);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[ready-queue] consumer signal failed %08lx", hr);
            retire("consumer signal failure");
            fallback_tick();
            return;
        }
        releases[i] = serial;
        owner->context->Flush();
    }
};

static std::vector<std::unique_ptr<RQConsumer>> rq_consumers;

static gs_texture_2d *rq_consumer_open(gs_device_t *device, uint32_t handle, gs_texture_2d *legacy)
{
    wchar_t value[8] = {};
    const DWORD length = GetEnvironmentVariableW(L"OBS_READY_QUEUE", value, 8);
    if (length >= 8 || (length && value[0] == L'0'))
        return legacy;
    std::unique_ptr<gs_texture_2d> legacy_guard(legacy);
    try {
        rq_consumers.reserve(rq_consumers.size() + 1);
        auto consumer = std::make_unique<RQConsumer>(device);
        if (!consumer->open(handle, legacy)) {
            if (!consumer->mapping_found)
                return legacy_guard.release();
            blog(LOG_WARNING, "[ready-queue] v2 attachment refused; shared texture open fails explicitly");
            return nullptr;
        }
        // Both textures remain owned across every throwing operation. reserve and
        // noexcept unique_ptr moves make publication into this vector non-throwing.
        consumer->legacy = std::move(legacy_guard);
        auto *out = consumer->output;
        rq_consumers.push_back(std::move(consumer));
        rq_consumers.back()->opening_output.release(); // API caller now owns output.
        return out;
    } catch (...) {
        blog(LOG_WARNING, "[ready-queue] v2 attachment exception; shared texture open fails explicitly");
        return nullptr;
    }
}

static void rq_consumer_frame(gs_device_t *device)
{
    for (auto &consumer : rq_consumers)
        if (consumer->owner == device)
            consumer->tick();
}

static void rq_consumer_remove(gs_texture_t *texture)
{
    rq_consumers.erase(std::remove_if(rq_consumers.begin(), rq_consumers.end(), [&](auto &consumer) {
        return consumer->output == texture;
    }), rq_consumers.end());
}

static void rq_consumer_stop(gs_device_t *device)
{
    rq_consumers.erase(std::remove_if(rq_consumers.begin(), rq_consumers.end(), [&](auto &consumer) {
        return consumer->owner == device;
    }), rq_consumers.end());
}

void rq_consumer_device_lost(gs_device_t *device)
{
    for (auto &consumer : rq_consumers) {
        if (consumer->owner == device) {
            consumer->retire("core graphics-device rebuild");
            consumer->handoff_done.Clear();
            consumer->handoff_ready = false;
            consumer->handoff_open_attempted = false;
            consumer->handoff_unavailable = false;
            consumer->clear_after_rebuild = true;
            consumer->fallback_clear_attempted = false;
            // The same one-shot producer acknowledgement is rechecked on the new device.
            // No request/status reset, and no old-device fence is retained through Rebuild.
            // Do not erase legacy/output gs_obj; core must Release/Rebuild both.
        }
    }
}

static bool rq_consumer_test_rebuild_requested(gs_device_t *device)
{
#ifdef RQ_LIFECYCLE_TEST
    static const uint64_t rebuild_after = rq_test_threshold(L"OBS_RQ_TEST_REBUILD_AFTER");
    if (!rq_test_rebuild_fired && rebuild_after) {
        for (auto &consumer : rq_consumers) {
            if (consumer->owner == device && !consumer->failed && consumer->serial >= rebuild_after) {
                rq_test_rebuild_fired = true;
                blog(LOG_WARNING, "[ready-queue] test-only local device rebuild requested after %llu selections", consumer->serial);
                return true;
            }
        }
    }
#else
    (void)device;
#endif
    return false;
}
