// Local experimental x64 ABI, GPL-2.0-or-later. Both endpoints require v2.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum { RQ_FREE = 0, RQ_WRITING = 1, RQ_PUBLISHED = 2, RQ_READING = 3, RQ_COUNT = 8 };
enum { RQ_MAGIC = 0x52515132, RQ_VERSION = 2 };
enum { RQ_HANDOFF_PENDING = 0, RQ_HANDOFF_SUBMITTED = 1, RQ_HANDOFF_FAILED = -1 };

struct alignas(64) RQSlot {
    volatile LONG state;
    uint32_t handle;
    uint64_t sequence;
    int64_t submitted;
    uint32_t marker_id;
};

struct alignas(64) RQShared {
    uint32_t magic, version, bytes, pid;
    uint64_t fence_handle;
    int64_t frequency;
    uint32_t width, height, format;
    volatile LONG alive, consumer;
    volatile LONG64 full;
    volatile LONG handoff_request; // Consumer: first consumer=-1, then request=1.
    volatile LONG handoff_status; // Producer: pending/submitted/failed; never reattach.
    alignas(8) volatile LONG64 handoff_fence_value;
    RQSlot slots[RQ_COUNT];
};

static_assert(sizeof(RQSlot) == 64, "v2 x64 slot ABI");
static_assert(offsetof(RQShared, full) == 56, "v2 full offset");
static_assert(offsetof(RQShared, handoff_request) == 64, "v2 handoff request offset");
static_assert(offsetof(RQShared, handoff_status) == 68, "v2 handoff status offset");
static_assert(offsetof(RQShared, handoff_fence_value) == 72, "v2 aligned handoff fence offset");
static_assert(offsetof(RQShared, slots) == 128 && sizeof(RQShared) == 640, "v2 shared ABI");
static inline LONG rq_load(volatile LONG *p) { return InterlockedCompareExchange(p, 0, 0); }
static inline int64_t rq_clock() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static inline void rq_name(wchar_t (&name)[96], uint32_t handle)
{
    swprintf_s(name, L"Local\\OBSReadyQueue_v2_%08x", handle);
}
