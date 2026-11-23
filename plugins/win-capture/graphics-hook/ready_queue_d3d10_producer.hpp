// D3D10 adapter: EVENT queries prove copy completion before v2 publication.
#pragma once
#include <d3d11.h>
#include "ready_queue_async_core.hpp"

struct RQD3D10Producer {
    RQAsyncCore core;
    ID3D11Device *device11 = nullptr;
    ID3D11DeviceContext *context11 = nullptr;
    ID3D10Texture2D *textures[RQ_COUNT] = {};
    ID3D10Query *queries[RQ_COUNT] = {};
    ID3D10Query *handoff_query = nullptr;
    int pending[RQ_COUNT] = {};
    unsigned pending_head = 0, pending_count = 0;
    int chosen = -1;
    bool handoff_after_copy = false, handoff_pending = false;

    ~RQD3D10Producer()
    {
        core.close();
        if (handoff_query) handoff_query->Release();
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (queries[i]) queries[i]->Release();
            if (textures[i]) textures[i]->Release();
        }
        if (context11) context11->Release();
        if (device11) device11->Release();
    }

    bool create_d3d11_helper()
    {
        IDXGIDevice *dxgi_device = nullptr;
        IDXGIAdapter *adapter = nullptr;
        if (FAILED(data.device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device)))
            return false;
        HRESULT hr = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(hr)) return false;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL used;
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, _countof(levels),
                               D3D11_SDK_VERSION, &device11, &used, &context11);
        adapter->Release();
        return SUCCEEDED(hr);
    }

    bool init()
    {
        if (!create_d3d11_helper()) return false;
        HANDLE handles[RQ_COUNT] = {};
        D3D10_QUERY_DESC query_desc = {D3D10_QUERY_EVENT, 0};
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (!create_d3d10_tex(data.cx, data.cy, &textures[i], &handles[i]) ||
                FAILED(data.device->CreateQuery(&query_desc, &queries[i])))
                return false;
        }
        if (FAILED(data.device->CreateQuery(&query_desc, &handoff_query))) return false;
        if (!core.init(device11, context11, data.handle, data.cx, data.cy, data.format, handles)) return false;
        hlog("[ready-queue-d3d10] producer v2 available; 8 slots, EVENT completion");
        return true;
    }

    void poll()
    {
        while (pending_count) {
            const int index = pending[pending_head];
            if (queries[index]->GetData(nullptr, 0, D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
            core.publish(index);
            pending_head = (pending_head + 1) % RQ_COUNT;
            --pending_count;
        }
        if (handoff_pending && handoff_query->GetData(nullptr, 0, D3D10_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            handoff_pending = false;
            core.publish_handoff();
        }
    }

    ID3D10Texture2D *begin()
    {
        poll();
        chosen = core.claim(handoff_after_copy);
        if (chosen >= 0) return textures[chosen];
        if (chosen == -1) return data.texture;
        return nullptr;
    }

    void end()
    {
        if (chosen >= 0) {
            queries[chosen]->End();
            pending[(pending_head + pending_count) % RQ_COUNT] = chosen;
            ++pending_count;
        } else if (chosen == -1 && handoff_after_copy && !handoff_pending) {
            handoff_query->End();
            handoff_pending = true;
        }
    }


    void abort()
    {
        if (chosen >= 0 && core.shared)
            InterlockedExchange(&core.shared->slots[chosen].state, RQ_FREE);
    }
};

static RQD3D10Producer *rq_d3d10_producer = nullptr;
static void rq_d3d10_producer_free() { delete rq_d3d10_producer; rq_d3d10_producer = nullptr; }
static void rq_d3d10_producer_init()
{
    rq_d3d10_producer_free();
    try {
        rq_d3d10_producer = new RQD3D10Producer;
        if (!rq_d3d10_producer->init()) rq_d3d10_producer_free();
    } catch (...) { rq_d3d10_producer_free(); }
}
