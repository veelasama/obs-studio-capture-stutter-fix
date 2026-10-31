// Experimental asynchronous DXGI Desktop Duplication producer for OBS.
// This file is included only by d3d11-duplicator.cpp after d3d11-subsystem.hpp.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "ready_queue_lowlatency.hpp"

static constexpr unsigned RQ_DXGI_SLOT_COUNT = 8;
static constexpr unsigned RQ_DXGI_PRIME_COUNT = 3;

enum class RQDXGISlotState : unsigned {
	Free,
	Writing,
	Published,
	Copying,
};

static std::atomic<bool> rq_dxgi_runtime_disabled{false};

struct RQDXGISlot {
	ComPtr<ID3D11Texture2D> producer_texture;
	ComPtr<ID3D11Texture2D> consumer_texture;
	HANDLE shared_handle = nullptr;
	std::atomic<RQDXGISlotState> state{RQDXGISlotState::Free};
	std::atomic<uint64_t> sequence{0};
	std::atomic<uint64_t> release{0};
	std::atomic<int64_t> submitted{0};
};

static bool rq_dxgi_requested()
{
	wchar_t value[16] = {};
	const DWORD length = GetEnvironmentVariableW(L"OBS_DXGI_READY_QUEUE", value, _countof(value));
	return length < _countof(value) && (!length || value[0] != L'0') &&
	       !rq_dxgi_runtime_disabled.load(std::memory_order_acquire);
}

struct RQDXGIAsyncQueue {
	gs_duplicator *owner;
	ComPtr<ID3D11Device5> producer_device;
	ComPtr<ID3D11DeviceContext4> producer_context;
	ComPtr<IDXGIOutputDuplication> duplication;
	ComPtr<ID3D11Fence> producer_done;
	ComPtr<ID3D11Fence> consumer_done_producer;
	ComPtr<ID3D11Device5> consumer_device;
	ComPtr<ID3D11DeviceContext4> consumer_context;
	ComPtr<ID3D11Fence> producer_done_consumer;
	ComPtr<ID3D11Fence> consumer_done;
	std::array<RQDXGISlot, RQ_DXGI_SLOT_COUNT> slots;
	HANDLE producer_fence_handle = nullptr;
	HANDLE consumer_fence_handle = nullptr;
	std::thread worker;
	std::mutex setup_mutex;
	std::atomic<bool> stopping{false};
	std::atomic<bool> configured{false};
	std::atomic<bool> attached{false};
	std::atomic<bool> failed{false};
	std::atomic<HRESULT> failure_hr{S_OK};
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT width = 0;
	UINT height = 0;
	LARGE_INTEGER frequency{};
	std::atomic<int64_t> configured_at{0};
	uint64_t producer_serial = 0;
	uint64_t consumer_serial = 0;
	uint64_t selections = 0;
	uint64_t repeats = 0;
	uint64_t producer_drops = 0;
	uint64_t consumer_drops = 0;
	uint64_t acquire_timeouts = 0;
	uint64_t pointer_only_skips = 0;
	bool primed = false;
	bool lowlatency = true; // OBS_READY_QUEUE_POLICY=bounded restores the depth-bounded FIFO.
	RQLowLatency ll;
	bool selected_this_frame = false;
	bool logged_attach = false;

	explicit RQDXGIAsyncQueue(gs_duplicator *d) : owner(d)
	{
		QueryPerformanceFrequency(&frequency);
		wchar_t policy[16] = {};
		GetEnvironmentVariableW(L"OBS_READY_QUEUE_POLICY", policy, 16);
		lowlatency = wcscmp(policy, L"bounded") != 0 && wcscmp(policy, L"latest") != 0;
		ll.init(frequency.QuadPart);
	}

	~RQDXGIAsyncQueue()
	{
		stop();
		blog(LOG_INFO,
		     "[dxgi-ready-queue] stop selections=%llu repeats=%llu producer_drops=%llu consumer_drops=%llu timeouts=%llu pointer_only=%llu latency_sheds=%llu",
		     selections, repeats, producer_drops, consumer_drops, acquire_timeouts, pointer_only_skips, ll.sheds);
	}

	static int64_t clock_now()
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	void set_failure(HRESULT hr, const char *where)
	{
		bool expected = false;
		if (failed.compare_exchange_strong(expected, true)) {
			rq_dxgi_runtime_disabled.store(true, std::memory_order_release);
			failure_hr.store(hr);
			blog(LOG_WARNING,
			     "[dxgi-ready-queue] %s failed (%08lX); async queue disabled for this process; stock DXGI will restart",
			     where,
			     (unsigned long)hr);
		}
	}

	static void close_handle(HANDLE &handle)
	{
		if (handle) {
			CloseHandle(handle);
			handle = nullptr;
		}
	}

	void close_shared_handles()
	{
		// Texture handles come from IDXGIResource::GetSharedHandle and are not NT handles.
		for (auto &slot : slots)
			slot.shared_handle = nullptr;
		close_handle(producer_fence_handle);
		close_handle(consumer_fence_handle);
	}

	bool initialize(int monitor_idx)
	{
		ComPtr<IDXGIOutput> output;
		HRESULT hr = owner->device->adapter->EnumOutputs((UINT)monitor_idx, output.Assign());
		if (FAILED(hr))
			return false;

		DXGI_OUTPUT_DESC output_desc = {};
		if (SUCCEEDED(output->GetDesc(&output_desc))) {
			const gs_monitor_color_info info = owner->device->GetMonitorColorInfo(output_desc.Monitor);
			owner->hdr = info.hdr;
			owner->sdr_white_nits = (float)info.sdr_white_nits;
		}

		ComPtr<ID3D11Device> base_device;
		ComPtr<ID3D11DeviceContext> base_context;
		const D3D_FEATURE_LEVEL levels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
		};
		hr = D3D11CreateDevice(owner->device->adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
				       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, _countof(levels), D3D11_SDK_VERSION,
				       base_device.Assign(), nullptr, base_context.Assign());
		if (FAILED(hr))
			return false;
		if (FAILED(base_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&producer_device)) ||
		    FAILED(base_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&producer_context)))
			return false;

		ComPtr<IDXGIOutput5> output5;
		if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), (void **)&output5))) {
			const DXGI_FORMAT formats[] = {
				DXGI_FORMAT_R16G16B16A16_FLOAT,
				DXGI_FORMAT_B8G8R8A8_UNORM,
			};
			hr = output5->DuplicateOutput1(producer_device, 0, _countof(formats), formats,
						      duplication.Assign());
		} else {
			ComPtr<IDXGIOutput1> output1;
			hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
			if (SUCCEEDED(hr))
				hr = output1->DuplicateOutput(producer_device, duplication.Assign());
		}
		if (FAILED(hr))
			return false;

		worker = std::thread([this]() { run(); });
		return true;
	}

	bool configure(const D3D11_TEXTURE2D_DESC &source_desc)
	{
		std::lock_guard<std::mutex> guard(setup_mutex);
		if (configured.load())
			return width == source_desc.Width && height == source_desc.Height && format == source_desc.Format;
		if (!source_desc.Width || !source_desc.Height || source_desc.MipLevels != 1 ||
		    source_desc.ArraySize != 1 || source_desc.SampleDesc.Count != 1) {
			blog(LOG_WARNING,
			     "[dxgi-ready-queue] unsupported source desc width=%u height=%u mips=%u array=%u samples=%u format=%u",
			     source_desc.Width, source_desc.Height, source_desc.MipLevels, source_desc.ArraySize,
			     source_desc.SampleDesc.Count, (unsigned)source_desc.Format);
			return false;
		}

		D3D11_TEXTURE2D_DESC desc = source_desc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		for (auto &slot : slots) {
			HRESULT hr = producer_device->CreateTexture2D(&desc, nullptr, &slot.producer_texture);
			if (FAILED(hr)) {
				blog(LOG_WARNING, "[dxgi-ready-queue] CreateTexture2D failed (%08lX), format=%u size=%ux%u",
				     (unsigned long)hr, (unsigned)desc.Format, desc.Width, desc.Height);
				return false;
			}
			ComPtr<IDXGIResource> resource;
			hr = slot.producer_texture->QueryInterface(__uuidof(IDXGIResource), (void **)&resource);
			if (FAILED(hr)) {
				blog(LOG_WARNING, "[dxgi-ready-queue] IDXGIResource query failed (%08lX)",
				     (unsigned long)hr);
				return false;
			}
			hr = resource->GetSharedHandle(&slot.shared_handle);
			if (FAILED(hr)) {
				blog(LOG_WARNING, "[dxgi-ready-queue] texture GetSharedHandle failed (%08lX)",
				     (unsigned long)hr);
				return false;
			}
		}

		HRESULT hr = producer_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
						  (void **)&producer_done);
		if (FAILED(hr)) {
			blog(LOG_WARNING, "[dxgi-ready-queue] producer CreateFence failed (%08lX)", (unsigned long)hr);
			return false;
		}
		hr = producer_done->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &producer_fence_handle);
		if (FAILED(hr)) {
			blog(LOG_WARNING, "[dxgi-ready-queue] producer fence handle failed (%08lX)", (unsigned long)hr);
			return false;
		}
		hr = producer_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
						 (void **)&consumer_done_producer);
		if (FAILED(hr)) {
			blog(LOG_WARNING, "[dxgi-ready-queue] consumer CreateFence failed (%08lX)", (unsigned long)hr);
			return false;
		}
		hr = consumer_done_producer->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
								 &consumer_fence_handle);
		if (FAILED(hr)) {
			blog(LOG_WARNING, "[dxgi-ready-queue] consumer fence handle failed (%08lX)", (unsigned long)hr);
			return false;
		}

		width = source_desc.Width;
		height = source_desc.Height;
		format = source_desc.Format;
		configured_at.store(clock_now());
		configured.store(true, std::memory_order_release);
		return true;
	}

	void reclaim_slots()
	{
		const uint64_t completed = consumer_done_producer ? consumer_done_producer->GetCompletedValue() : 0;
		if (completed == UINT64_MAX) {
			set_failure(DXGI_ERROR_DEVICE_REMOVED, "consumer fence");
			return;
		}
		for (auto &slot : slots) {
			if (slot.state.load(std::memory_order_acquire) == RQDXGISlotState::Copying) {
				const uint64_t release = slot.release.load(std::memory_order_acquire);
				if (release && release <= completed) {
					slot.release.store(0, std::memory_order_relaxed);
					slot.state.store(RQDXGISlotState::Free, std::memory_order_release);
				}
			}
		}
	}

	RQDXGISlot *claim_slot()
	{
		reclaim_slots();
		for (auto &slot : slots) {
			RQDXGISlotState expected = RQDXGISlotState::Free;
			if (slot.state.compare_exchange_strong(expected, RQDXGISlotState::Writing))
				return &slot;
		}

		RQDXGISlot *oldest = nullptr;
		uint64_t oldest_sequence = UINT64_MAX;
		for (auto &slot : slots) {
			if (slot.state.load(std::memory_order_acquire) == RQDXGISlotState::Published) {
				const uint64_t sequence = slot.sequence.load(std::memory_order_relaxed);
				if (sequence < oldest_sequence) {
					oldest_sequence = sequence;
					oldest = &slot;
				}
			}
		}
		if (oldest) {
			RQDXGISlotState expected = RQDXGISlotState::Published;
			if (oldest->state.compare_exchange_strong(expected, RQDXGISlotState::Writing)) {
				++producer_drops;
				return oldest;
			}
		}
		return nullptr;
	}

	void run()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		while (!stopping.load(std::memory_order_acquire)) {
			DXGI_OUTDUPL_FRAME_INFO info = {};
			ComPtr<IDXGIResource> resource;
			HRESULT hr = duplication->AcquireNextFrame(50, &info, resource.Assign());
			if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
				++acquire_timeouts;
				continue;
			}
			if (FAILED(hr)) {
				if (!stopping.load())
					set_failure(hr, "AcquireNextFrame");
				break;
			}
			if (info.LastPresentTime.QuadPart == 0) {
				++pointer_only_skips;
				duplication->ReleaseFrame();
				continue;
			}

			ComPtr<ID3D11Texture2D> source;
			hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&source);
			if (SUCCEEDED(hr)) {
				D3D11_TEXTURE2D_DESC desc = {};
				source->GetDesc(&desc);
				if (!configure(desc)) {
					set_failure(E_FAIL, "queue texture configuration");
				} else if (RQDXGISlot *slot = claim_slot()) {
					producer_context->CopyResource(slot->producer_texture, source);
					const uint64_t sequence = ++producer_serial;
					hr = producer_context->Signal(producer_done, sequence);
					if (SUCCEEDED(hr)) {
						producer_context->Flush();
						slot->sequence.store(sequence, std::memory_order_relaxed);
						slot->submitted.store(clock_now(), std::memory_order_relaxed);
						slot->state.store(RQDXGISlotState::Published, std::memory_order_release);
					} else {
						slot->state.store(RQDXGISlotState::Free, std::memory_order_release);
						set_failure(hr, "producer fence signal");
					}
				} else {
					++producer_drops;
				}
			} else {
				set_failure(hr, "desktop texture query");
			}
			duplication->ReleaseFrame();
			if (failed.load())
				break;
		}
	}

	bool attach_consumer()
	{
		if (attached.load(std::memory_order_acquire))
			return true;
		if (!configured.load(std::memory_order_acquire))
			return false;
		std::lock_guard<std::mutex> guard(setup_mutex);
		if (attached.load())
			return true;
		if (FAILED(owner->device->device->QueryInterface(__uuidof(ID3D11Device5),
								 (void **)&consumer_device)) ||
		    FAILED(owner->device->context->QueryInterface(__uuidof(ID3D11DeviceContext4),
								  (void **)&consumer_context))) {
			set_failure(E_NOINTERFACE, "OBS D3D11 fence interface");
			return false;
		}
		for (auto &slot : slots) {
			if (!slot.shared_handle ||
			    FAILED(consumer_device->OpenSharedResource(slot.shared_handle, __uuidof(ID3D11Texture2D),
								    (void **)&slot.consumer_texture))) {
				set_failure(E_FAIL, "shared desktop texture open");
				return false;
			}
		}
		if (FAILED(consumer_device->OpenSharedFence(producer_fence_handle, __uuidof(ID3D11Fence),
								    (void **)&producer_done_consumer)) ||
		    FAILED(consumer_device->OpenSharedFence(consumer_fence_handle, __uuidof(ID3D11Fence),
								    (void **)&consumer_done))) {
			set_failure(E_FAIL, "shared desktop fence open");
			return false;
		}
		attached.store(true, std::memory_order_release);
		if (!logged_attach) {
			blog(LOG_INFO,
			     "[dxgi-ready-queue] asynchronous Desktop Duplication attached; 8 shared textures; policy=%s",
			     lowlatency ? "lowlatency" : "bounded");
			logged_attach = true;
		}
		return true;
	}

	void begin_frame()
	{
		selected_this_frame = false;
	}

	bool ensure_output()
	{
		const gs_color_format converted = ConvertDXGITextureFormat(format);
		const gs_color_format general = gs_generalize_format(converted);
		if (general == GS_UNKNOWN)
			return false;
		if (owner->texture && (owner->texture->width != width || owner->texture->height != height ||
				       owner->texture->format != general)) {
			delete owner->texture;
			owner->texture = nullptr;
		}
		if (!owner->texture)
			owner->texture =
				(gs_texture_2d *)gs_texture_create(width, height, general, 1, nullptr, 0);
		if (!owner->texture)
			return false;
		owner->color_space = owner->hdr ? GS_CS_709_SCRGB
						 : (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_CS_SRGB_16F
												      : GS_CS_SRGB);
		return true;
	}

	bool update()
	{
		if (failed.load(std::memory_order_acquire))
			return false;
		if (selected_this_frame)
			return true;
		selected_this_frame = true;
		if (!attach_consumer())
			return !failed.load();

		const uint64_t completed = producer_done_consumer->GetCompletedValue();
		if (completed == UINT64_MAX) {
			set_failure(DXGI_ERROR_DEVICE_REMOVED, "producer fence");
			return false;
		}
		std::array<unsigned, RQ_DXGI_SLOT_COUNT> ready{};
		unsigned count = 0;
		for (unsigned i = 0; i < slots.size(); ++i) {
			if (slots[i].state.load(std::memory_order_acquire) == RQDXGISlotState::Published &&
			    slots[i].sequence.load(std::memory_order_relaxed) <= completed)
				ready[count++] = i;
		}
		std::sort(ready.begin(), ready.begin() + count, [this](unsigned a, unsigned b) {
			return slots[a].sequence.load(std::memory_order_relaxed) <
			       slots[b].sequence.load(std::memory_order_relaxed);
		});

		if (lowlatency) {
			const int64_t now = clock_now();
			ll.begin_tick(now);
			for (auto &slot : slots)
				if (slot.state.load(std::memory_order_acquire) == RQDXGISlotState::Published &&
				    slot.sequence.load(std::memory_order_relaxed) > completed)
					ll.observe_pending(now, slot.submitted.load(std::memory_order_relaxed));
			uint64_t seq[RQ_DXGI_SLOT_COUNT];
			int64_t submitted[RQ_DXGI_SLOT_COUNT];
			for (unsigned i = 0; i < count; ++i) {
				seq[i] = slots[ready[i]].sequence.load(std::memory_order_relaxed);
				submitted[i] = slots[ready[i]].submitted.load(std::memory_order_relaxed);
			}
			const int chosen = ll.select(seq, submitted, count, RQ_DXGI_SLOT_COUNT, now);
			if (chosen < 0) {
				++repeats;
				return true;
			}
			for (int i = 0; i < chosen; ++i) {
				RQDXGISlotState expected = RQDXGISlotState::Published;
				if (slots[ready[i]].state.compare_exchange_strong(expected, RQDXGISlotState::Free))
					++consumer_drops;
			}
			ready[0] = ready[chosen];
			count = 1;
			primed = true;
		}

		if (!primed) {
			const int64_t elapsed = clock_now() - configured_at.load();
			if (count < RQ_DXGI_PRIME_COUNT &&
			    elapsed * 1000.0 / (double)frequency.QuadPart < 100.0) {
				++repeats;
				return true;
			}
			if (!count) {
				++repeats;
				return true;
			}
			primed = true;
		}

		while (count >= RQ_DXGI_SLOT_COUNT - 1) {
			RQDXGISlotState expected = RQDXGISlotState::Published;
			if (slots[ready[0]].state.compare_exchange_strong(expected, RQDXGISlotState::Free)) {
				++consumer_drops;
			}
			for (unsigned i = 1; i < count; ++i)
				ready[i - 1] = ready[i];
			--count;
		}
		if (!count) {
			++repeats;
			return true;
		}

		RQDXGISlot *selected = nullptr;
		for (unsigned i = 0; i < count; ++i) {
			RQDXGISlotState expected = RQDXGISlotState::Published;
			if (slots[ready[i]].state.compare_exchange_strong(expected, RQDXGISlotState::Copying)) {
				selected = &slots[ready[i]];
				break;
			}
		}
		if (!selected) {
			++repeats;
			return true;
		}
		RQDXGISlot &slot = *selected;
		if (!ensure_output()) {
			slot.state.store(RQDXGISlotState::Published, std::memory_order_release);
			set_failure(E_FAIL, "OBS output texture creation");
			return false;
		}
		owner->device->context->CopyResource(owner->texture->texture, slot.consumer_texture);
		const uint64_t release = ++consumer_serial;
		slot.release.store(release, std::memory_order_release);
		const HRESULT hr = consumer_context->Signal(consumer_done, release);
		if (FAILED(hr)) {
			set_failure(hr, "consumer fence signal");
			return false;
		}
		owner->device->context->Flush();
		++selections;
		return true;
	}

	void stop()
	{
		stopping.store(true, std::memory_order_release);
		if (worker.joinable())
			worker.join();
		std::lock_guard<std::mutex> guard(setup_mutex);
		close_shared_handles();
		for (auto &slot : slots) {
			slot.consumer_texture.Clear();
			slot.producer_texture.Clear();
		}
		consumer_done.Clear();
		producer_done_consumer.Clear();
		consumer_context.Clear();
		consumer_device.Clear();
		consumer_done_producer.Clear();
		producer_done.Clear();
		duplication.Clear();
		producer_context.Clear();
		producer_device.Clear();
	}
};

static bool rq_dxgi_start(gs_duplicator *duplicator)
{
	if (!rq_dxgi_requested())
		return false;
	auto *queue = new RQDXGIAsyncQueue(duplicator);
	if (!queue->initialize(duplicator->idx)) {
		delete queue;
		blog(LOG_WARNING, "[dxgi-ready-queue] initialization unavailable; using stock Desktop Duplication");
		return false;
	}
	duplicator->dxgi_queue = queue;
	blog(LOG_INFO, "[dxgi-ready-queue] asynchronous Desktop Duplication producer started");
	return true;
}

static void rq_dxgi_begin_frame(gs_duplicator *duplicator)
{
	if (duplicator->dxgi_queue)
		duplicator->dxgi_queue->begin_frame();
}

static bool rq_dxgi_update(gs_duplicator *duplicator)
{
	return duplicator->dxgi_queue && duplicator->dxgi_queue->update();
}

static void rq_dxgi_release(gs_duplicator *duplicator)
{
	delete duplicator->dxgi_queue;
	duplicator->dxgi_queue = nullptr;
}
