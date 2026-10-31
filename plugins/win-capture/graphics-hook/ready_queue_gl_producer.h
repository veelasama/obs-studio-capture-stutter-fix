// OpenGL adapter using WGL_NV_DX_interop unlock as the GL->D3D visibility point.
#pragma once
#include <d3d11_4.h>
#include "ready_queue_c_protocol.h"

struct rq_gl_core {
    ID3D11Device5 *device;
    ID3D11DeviceContext4 *context;
    ID3D11Fence *fence;
    HANDLE mapping, shared_fence;
    RQCShared *shared;
    uint64_t sequence, handoffs;
};

struct rq_gl_producer {
    struct rq_gl_core core;
    ID3D11Texture2D *textures11[RQC_COUNT];
    GLuint textures_gl[RQC_COUNT];
    HANDLE objects[RQC_COUNT];
    int chosen;
    bool handoff_after_copy;
};

static struct rq_gl_producer *rq_gl;

static void rq_gl_core_close(struct rq_gl_core *core)
{
    if (core->shared) {
        InterlockedExchange(&core->shared->alive, 0);
        hlog("[ready-queue-gl] producer v2 stop submitted=%llu full=%lld handoffs=%llu",
             core->sequence, core->shared->full, core->handoffs);
        UnmapViewOfFile(core->shared);
    }
    if (core->mapping) CloseHandle(core->mapping);
    if (core->shared_fence) CloseHandle(core->shared_fence);
    if (core->fence) ID3D11Fence_Release(core->fence);
    if (core->context) ID3D11DeviceContext4_Release(core->context);
    if (core->device) ID3D11Device5_Release(core->device);
    memset(core, 0, sizeof(*core));
}

static bool rq_gl_core_init(struct rq_gl_core *core, const HANDLE handles[RQC_COUNT])
{
    HRESULT hr = ID3D11Device_QueryInterface(data.d3d11_device, &IID_ID3D11Device5, (void **)&core->device);
    if (FAILED(hr)) return false;
    hr = ID3D11DeviceContext_QueryInterface(data.d3d11_context, &IID_ID3D11DeviceContext4,
                                            (void **)&core->context);
    if (FAILED(hr)) return false;
    hr = ID3D11Device5_CreateFence(core->device, 0, D3D11_FENCE_FLAG_SHARED,
                                  &IID_ID3D11Fence, (void **)&core->fence);
    if (FAILED(hr)) return false;
    hr = ID3D11Fence_CreateSharedHandle(core->fence, NULL, GENERIC_ALL, NULL, &core->shared_fence);
    if (FAILED(hr)) return false;
    wchar_t name[96];
    rqc_name(name, (uint32_t)(uintptr_t)data.handle);
    core->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(RQCShared), name);
    if (!core->mapping || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    core->shared = MapViewOfFile(core->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RQCShared));
    if (!core->shared) return false;
    memset(core->shared, 0, sizeof(*core->shared));
    core->shared->magic = RQC_MAGIC;
    core->shared->version = RQC_VERSION;
    core->shared->bytes = sizeof(*core->shared);
    core->shared->pid = GetCurrentProcessId();
    core->shared->fence_handle = (uint64_t)(uintptr_t)core->shared_fence;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    core->shared->frequency = frequency.QuadPart;
    core->shared->width = data.cx;
    core->shared->height = data.cy;
    core->shared->format = data.format;
    for (unsigned i = 0; i < RQC_COUNT; ++i)
        core->shared->slots[i].handle = (uint32_t)(uintptr_t)handles[i];
    InterlockedExchange(&core->shared->alive, 1);
    return true;
}

static int rq_gl_claim(struct rq_gl_core *core, bool *handoff)
{
    *handoff = false;
    if (rqc_load(&core->shared->consumer) != 1 || rqc_load(&core->shared->alive) != 1) {
        *handoff = rqc_load(&core->shared->handoff_request) == 1 &&
                   rqc_load(&core->shared->handoff_status) == RQC_HANDOFF_PENDING;
        return -1;
    }
    for (unsigned i = 0; i < RQC_COUNT; ++i) {
        if (InterlockedCompareExchange(&core->shared->slots[i].state, RQC_WRITING, RQC_FREE) == RQC_FREE) {
            RQCSlot *slot = &core->shared->slots[i];
            slot->sequence = ++core->sequence;
            slot->submitted = rqc_clock();
            return (int)i;
        }
    }
    InterlockedIncrement64(&core->shared->full);
    return -2;
}

static bool rq_gl_signal(struct rq_gl_core *core, uint64_t value)
{
    HRESULT hr = ID3D11DeviceContext4_Signal(core->context, core->fence, value);
    if (FAILED(hr)) {
        InterlockedExchange(&core->shared->alive, 0);
        InterlockedExchange(&core->shared->handoff_status, RQC_HANDOFF_FAILED);
        return false;
    }
    ID3D11DeviceContext_Flush((ID3D11DeviceContext *)core->context);
    return true;
}

static void rq_gl_end(void)
{
    struct rq_gl_core *core = &rq_gl->core;
    if (rq_gl->chosen >= 0) {
        RQCSlot *slot = &core->shared->slots[rq_gl->chosen];
        if (rq_gl_signal(core, slot->sequence))
            InterlockedExchange(&slot->state, RQC_PUBLISHED);
    } else if (rq_gl->chosen == -1 && rq_gl->handoff_after_copy) {
        uint64_t value = ++core->sequence;
        if (rq_gl_signal(core, value)) {
            InterlockedExchange64(&core->shared->handoff_fence_value, (LONG64)value);
            InterlockedExchange(&core->shared->handoff_status, RQC_HANDOFF_SUBMITTED);
            ++core->handoffs;
        }
    }
}

static void rq_gl_free(void)
{
    if (!rq_gl) return;
    rq_gl_core_close(&rq_gl->core);
    for (unsigned i = 0; i < RQC_COUNT; ++i) {
        if (rq_gl->objects[i]) obsglDXUnregisterObjectNV(data.gl_device, rq_gl->objects[i]);
        if (rq_gl->textures_gl[i]) glDeleteTextures(1, &rq_gl->textures_gl[i]);
        if (rq_gl->textures11[i]) ID3D11Texture2D_Release(rq_gl->textures11[i]);
    }
    free(rq_gl);
    rq_gl = NULL;
}

static bool rq_gl_init(void)
{
    rq_gl_free();
    rq_gl = calloc(1, sizeof(*rq_gl));
    if (!rq_gl) return false;
    HANDLE handles[RQC_COUNT] = {0};
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = data.cx; desc.Height = data.cy; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = data.format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (unsigned i = 0; i < RQC_COUNT; ++i) {
        HRESULT hr = ID3D11Device_CreateTexture2D(data.d3d11_device, &desc, NULL, &rq_gl->textures11[i]);
        if (FAILED(hr)) goto fail;
        IDXGIResource *resource = NULL;
        hr = ID3D11Texture2D_QueryInterface(rq_gl->textures11[i], &GUID_IDXGIResource, (void **)&resource);
        if (FAILED(hr)) goto fail;
        hr = IDXGIResource_GetSharedHandle(resource, &handles[i]);
        IDXGIResource_Release(resource);
        if (FAILED(hr)) goto fail;
        glGenTextures(1, &rq_gl->textures_gl[i]);
        if (gl_error("rq_gl_init", "failed to generate slot texture")) goto fail;
        rq_gl->objects[i] = obsglDXRegisterObjectNV(data.gl_device, rq_gl->textures11[i],
                                                    rq_gl->textures_gl[i], GL_TEXTURE_2D,
                                                    WGL_ACCESS_WRITE_DISCARD_NV);
        if (!rq_gl->objects[i]) goto fail;
    }
    if (!rq_gl_core_init(&rq_gl->core, handles)) goto fail;
    hlog("[ready-queue-gl] producer v2 available; 8 slots, WGL interop synchronization");
    return true;
fail:
    rq_gl_free();
    return false;
}

static bool rq_gl_begin(GLuint *texture, HANDLE *object)
{
    if (!rq_gl) {
        *texture = data.texture; *object = data.gl_dxobj;
        return true;
    }
    rq_gl->chosen = rq_gl_claim(&rq_gl->core, &rq_gl->handoff_after_copy);
    if (rq_gl->chosen >= 0) {
        *texture = rq_gl->textures_gl[rq_gl->chosen];
        *object = rq_gl->objects[rq_gl->chosen];
        return true;
    }
    if (rq_gl->chosen == -1) {
        *texture = data.texture; *object = data.gl_dxobj;
        return true;
    }
    return false;
}
