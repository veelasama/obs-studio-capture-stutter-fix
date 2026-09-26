#include "GameDisplay.hpp"

#include <obs.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <cwchar>

namespace {

std::wstring Utf8ToWide(const std::string &text)
{
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	std::wstring wide(length > 0 ? (size_t)length - 1 : 0, L'\0');
	if (length > 1)
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
	return wide;
}

/* Game Capture stores "title:class:executable" with ':' written as "#3A". */
std::wstring DecodeWindowPart(std::string part)
{
	for (size_t at = part.find("#3A"); at != std::string::npos; at = part.find("#3A", at + 1))
		part.replace(at, 3, ":");
	return Utf8ToWide(part);
}

struct CaptureSpec {
	bool found = false;
	bool active = false;
	std::wstring windowClass;
	std::wstring executable;
};

bool ReadCaptureSpec(void *param, obs_source_t *source)
{
	auto *spec = static_cast<CaptureSpec *>(param);
	if (strcmp(obs_source_get_unversioned_id(source), "game_capture") != 0)
		return true;

	const bool active = obs_source_active(source);
	if (spec->found && (spec->active || !active))
		return true;

	obs_data_t *settings = obs_source_get_settings(source);
	const char *mode = obs_data_get_string(settings, "capture_mode");
	const std::string window = obs_data_get_string(settings, "window");
	obs_data_release(settings);
	if (!mode || strcmp(mode, "window") != 0 || window.empty())
		return true;

	const size_t first = window.find(':');
	const size_t second = first == std::string::npos ? std::string::npos : window.find(':', first + 1);
	if (second == std::string::npos)
		return true;

	spec->found = true;
	spec->active = active;
	spec->windowClass = DecodeWindowPart(window.substr(first + 1, second - first - 1));
	spec->executable = DecodeWindowPart(window.substr(second + 1));
	return !active; // an active source is the best answer
}

std::wstring ExecutableOf(HWND window)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return {};
	wchar_t path[MAX_PATH] = {};
	DWORD size = MAX_PATH;
	const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
	CloseHandle(process);
	if (!ok)
		return {};
	const wchar_t *name = wcsrchr(path, L'\\');
	return name ? name + 1 : path;
}

struct WindowSearch {
	const CaptureSpec *spec;
	HWND found = nullptr;
};

BOOL CALLBACK FindCaptureWindow(HWND window, LPARAM param)
{
	auto *search = reinterpret_cast<WindowSearch *>(param);
	/* A minimized game still counts: fullscreen games minimize on Alt+Tab. */
	if (!IsWindowVisible(window))
		return TRUE;

	wchar_t windowClass[256] = {};
	GetClassNameW(window, windowClass, 256);
	if (!search->spec->windowClass.empty() && search->spec->windowClass != windowClass)
		return TRUE;
	if (_wcsicmp(ExecutableOf(window).c_str(), search->spec->executable.c_str()) != 0)
		return TRUE;

	search->found = window;
	return FALSE;
}

std::wstring DisplayOf(HWND window)
{
	HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (IsIconic(window)) {
		/* Minimized windows sit off-screen; use where they will return. */
		WINDOWPLACEMENT placement = {};
		placement.length = sizeof(placement);
		if (GetWindowPlacement(window, &placement))
			monitor = MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
	}

	MONITORINFOEXW info = {};
	info.cbSize = sizeof(info);
	return monitor && GetMonitorInfoW(monitor, &info) ? info.szDevice : L"";
}

} // namespace

GameDisplay FindGameDisplay()
{
	GameDisplay result;

	CaptureSpec spec;
	obs_enum_sources(ReadCaptureSpec, &spec);
	if (spec.found) {
		WindowSearch search{&spec};
		EnumWindows(FindCaptureWindow, reinterpret_cast<LPARAM>(&search));
		if (search.found) {
			result.device = DisplayOf(search.found);
			result.executable = spec.executable;
			result.source = GameDisplay::Source::GameWindow;
			if (!result.device.empty())
				return result;
		}
	}

	HWND foreground = GetForegroundWindow();
	DWORD pid = 0;
	if (foreground)
		GetWindowThreadProcessId(foreground, &pid);
	if (foreground && pid != GetCurrentProcessId() && !IsIconic(foreground)) {
		result.device = DisplayOf(foreground);
		result.executable = ExecutableOf(foreground);
		result.source = GameDisplay::Source::Foreground;
		if (!result.device.empty())
			return result;
	}

	MONITORINFOEXW info = {};
	info.cbSize = sizeof(info);
	const POINT origin = {0, 0};
	if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info))
		result.device = info.szDevice;
	result.executable.clear();
	result.source = GameDisplay::Source::Primary;
	return result;
}
