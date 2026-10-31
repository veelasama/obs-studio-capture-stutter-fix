// Declarations missing from the installed SDK 10.0.19041.0.
// Verified against Microsoft's win32metadata headers, saved in
// audit/evidence/sdk-reference/d3dkmdt.h and d3dkmthk.h on 2026-09-13.
// These preserve upstream's optional read-only WDDM 2.9 status query.
#pragma once
#include <d3dkmthk.h>
#ifndef DXGK_FEATURE_SUPPORT_ALWAYS_OFF
#define DXGK_FEATURE_SUPPORT_ALWAYS_OFF ((UINT)0)
#define DXGK_FEATURE_SUPPORT_EXPERIMENTAL ((UINT)1)
#define DXGK_FEATURE_SUPPORT_STABLE ((UINT)2)
#define DXGK_FEATURE_SUPPORT_ALWAYS_ON ((UINT)3)
typedef struct _D3DKMT_WDDM_2_9_CAPS {
    union {
        struct { UINT HwSchSupportState:2; UINT HwSchEnabled:1;
                 UINT SelfRefreshMemorySupported:1; UINT Reserved:28; };
        UINT Value;
    };
} D3DKMT_WDDM_2_9_CAPS;
static_assert(sizeof(D3DKMT_WDDM_2_9_CAPS)==4, "WDDM 2.9 ABI");
#define KMTQAITYPE_WDDM_2_9_CAPS ((KMTQUERYADAPTERINFOTYPE)75)
#endif
