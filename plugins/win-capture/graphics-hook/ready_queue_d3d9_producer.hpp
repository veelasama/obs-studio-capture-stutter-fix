// D3D9 adapter: EVENT queries prove StretchRect completion before v2 publication.
#pragma once
#include "ready_queue_async_core.hpp"

struct RQD3D9Producer {
    RQAsyncCore core;
    ID3D11Texture2D *textures11[RQ_COUNT] = {};
    IDirect3DSurface9 *surfaces9[RQ_COUNT] = {};
    IDirect3DQuery9 *queries[RQ_COUNT] = {};
    IDirect3DQuery9 *handoff_query = nullptr;
    int pending[RQ_COUNT] = {};
    unsigned pending_head = 0, pending_count = 0;
    int chosen = -1;
    bool handoff_after_copy = false, handoff_pending = false;

    ~RQD3D9Producer()
    {
        core.close();
        if (handoff_query) handoff_query->Release();
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (queries[i]) queries[i]->Release();
            if (surfaces9[i]) surfaces9[i]->Release();
            if (textures11[i]) textures11[i]->Release();
        }
    }

    bool create_surface(HANDLE handle, IDirect3DSurface9 **surface)
    {
        struct d3d9_offsets offsets = global_hook_info->offsets.d3d9;
        uint8_t *patch_addr = nullptr;
        BOOL *p_is_d3d9 = nullptr;
        uint8_t saved_data[MAX_PATCH_SIZE];
        size_t patch_size = 0;
        BOOL was_d3d9ex = false;
        DWORD protect_val = 0;
        if (offsets.d3d9_clsoff && offsets.is_d3d9ex_clsoff) {
            uint8_t *device_ptr = (uint8_t *)data.device;
            uint8_t *d3d9_ptr = *(uint8_t **)(device_ptr + offsets.d3d9_clsoff);
            p_is_d3d9 = (BOOL *)(d3d9_ptr + offsets.is_d3d9ex_clsoff);
        } else {
            patch_addr = get_d3d9_patch_addr(data.d3d9, data.patch);
        }
        if (p_is_d3d9) {
            was_d3d9ex = *p_is_d3d9;
            *p_is_d3d9 = true;
        } else if (patch_addr) {
            patch_size = patch[data.patch].size;
            VirtualProtect(patch_addr, patch_size, PAGE_EXECUTE_READWRITE, &protect_val);
            memcpy(saved_data, patch_addr, patch_size);
            memcpy(patch_addr, patch[data.patch].data, patch_size);
        }
        IDirect3DTexture9 *texture = nullptr;
        HRESULT hr = data.device->CreateTexture(data.cx, data.cy, 1, D3DUSAGE_RENDERTARGET,
                                                data.d3d9_format, D3DPOOL_DEFAULT, &texture, &handle);
        if (p_is_d3d9) {
            *p_is_d3d9 = was_d3d9ex;
        } else if (patch_addr && patch_size) {
            memcpy(patch_addr, saved_data, patch_size);
            VirtualProtect(patch_addr, patch_size, protect_val, &protect_val);
        }
        if (FAILED(hr))
            return false;
        hr = texture->GetSurfaceLevel(0, surface);
        texture->Release();
        return SUCCEEDED(hr);
    }

    bool init()
    {
        HANDLE handles[RQ_COUNT] = {};
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = data.cx; desc.Height = data.cy; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = data.dxgi_format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (unsigned i = 0; i < RQ_COUNT; ++i) {
            if (FAILED(data.d3d11_device->CreateTexture2D(&desc, nullptr, &textures11[i])))
                return false;
            IDXGIResource *resource = nullptr;
            if (FAILED(textures11[i]->QueryInterface(__uuidof(IDXGIResource), (void **)&resource)))
                return false;
            HRESULT hr = resource->GetSharedHandle(&handles[i]);
            resource->Release();
            if (FAILED(hr) || !create_surface(handles[i], &surfaces9[i]) ||
                FAILED(data.device->CreateQuery(D3DQUERYTYPE_EVENT, &queries[i])))
                return false;
        }
        if (FAILED(data.device->CreateQuery(D3DQUERYTYPE_EVENT, &handoff_query)))
            return false;
        if (!core.init(data.d3d11_device, data.d3d11_context, data.handle,
                       data.cx, data.cy, data.dxgi_format, handles))
            return false;
        hlog("[ready-queue-d3d9] producer v2 available; 8 slots, EVENT completion");
        return true;
    }

    void poll()
    {
        while (pending_count) {
            const int index = pending[pending_head];
            if (queries[index]->GetData(nullptr, 0, 0) != S_OK)
                break;
            core.publish(index);
            pending_head = (pending_head + 1) % RQ_COUNT;
            --pending_count;
        }
        if (handoff_pending && handoff_query->GetData(nullptr, 0, 0) == S_OK) {
            handoff_pending = false;
            core.publish_handoff();
        }
    }

    IDirect3DSurface9 *begin()
    {
        poll();
        chosen = core.claim(handoff_after_copy);
        if (chosen >= 0) return surfaces9[chosen];
        if (chosen == -1) return data.d3d9_copytex;
        return nullptr;
    }

    void end()
    {
        if (chosen >= 0) {
            queries[chosen]->Issue(D3DISSUE_END);
            pending[(pending_head + pending_count) % RQ_COUNT] = chosen;
            ++pending_count;
        } else if (chosen == -1 && handoff_after_copy && !handoff_pending) {
            handoff_query->Issue(D3DISSUE_END);
            handoff_pending = true;
        }
    }

    void abort()
    {
        if (chosen >= 0 && core.shared)
            InterlockedExchange(&core.shared->slots[chosen].state, RQ_FREE);
    }
};

static RQD3D9Producer *rq_d3d9_producer = nullptr;
static void rq_d3d9_producer_free() { delete rq_d3d9_producer; rq_d3d9_producer = nullptr; }
static void rq_d3d9_producer_init()
{
    rq_d3d9_producer_free();
    try {
        rq_d3d9_producer = new RQD3D9Producer;
        if (!rq_d3d9_producer->init()) rq_d3d9_producer_free();
    } catch (...) { rq_d3d9_producer_free(); }
}
