// C form of the v2 queue ABI for the OpenGL/Vulkan hook sources.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum { RQC_FREE = 0, RQC_WRITING = 1, RQC_PUBLISHED = 2, RQC_READING = 3, RQC_COUNT = 8 };
enum { RQC_MAGIC = 0x52515132, RQC_VERSION = 2 };
enum { RQC_HANDOFF_PENDING = 0, RQC_HANDOFF_SUBMITTED = 1, RQC_HANDOFF_FAILED = -1 };

typedef __declspec(align(64)) struct RQCSlot {
    volatile LONG state;
    uint32_t handle;
    uint64_t sequence;
    int64_t submitted;
    uint32_t marker_id;
} RQCSlot;

typedef __declspec(align(64)) struct RQCShared {
    uint32_t magic, version, bytes, pid;
    uint64_t fence_handle;
    int64_t frequency;
    uint32_t width, height, format;
    volatile LONG alive, consumer;
    volatile LONG64 full;
    volatile LONG handoff_request;
    volatile LONG handoff_status;
    __declspec(align(8)) volatile LONG64 handoff_fence_value;
    RQCSlot slots[RQC_COUNT];
} RQCShared;

C_ASSERT(sizeof(RQCSlot) == 64);
C_ASSERT(offsetof(RQCShared, slots) == 128);
C_ASSERT(sizeof(RQCShared) == 640);

static inline LONG rqc_load(volatile LONG *p) { return InterlockedCompareExchange(p, 0, 0); }
static inline int64_t rqc_clock(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static inline void rqc_name(wchar_t name[96], uint32_t handle)
{
    swprintf_s(name, 96, L"Local\\OBSReadyQueue_v2_%08x", handle);
}
