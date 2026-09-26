/******************************************************************************
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <utility/DisplayRefreshMeasure.hpp>
#include <utility/GameDisplay.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <string>

static std::wstring ConfiguredDisplay(config_t *config)
{
	const char *device = config_get_string(config, "Video", "FPSMatchDisplayDevice");
	if (!device || !*device)
		return {};

	const int length = MultiByteToWideChar(CP_UTF8, 0, device, -1, nullptr, 0);
	std::wstring wide(length > 0 ? (size_t)length - 1 : 0, L'\0');
	if (length > 1)
		MultiByteToWideChar(CP_UTF8, 0, device, -1, wide.data(), length);
	return wide;
}

void OBSBasic::UpdateDisplayRefreshMatcher()
{
	const bool enabled = config_get_bool(activeConfiguration, "Video", "FPSMatchDisplayAuto");
	if (!enabled && !displayRefreshMatcher)
		return;
	if (!displayRefreshMatcher)
		displayRefreshMatcher = std::make_unique<DisplayRefreshMatcher>();
	/* An empty device ("the display the game is on") measures every display. */
	displayRefreshMatcher->Configure(enabled, ConfiguredDisplay(activeConfiguration));
}

bool OBSBasic::LatestDisplayRefresh(const std::wstring &device, double &hz, double &uncertaintyPpm,
				    uint64_t &ageMs) const
{
	return displayRefreshMatcher && displayRefreshMatcher->Latest(device, hz, uncertaintyPpm, ageMs);
}

void OBSBasic::ApplyDisplayMatchedFPS()
{
	if (!config_get_bool(activeConfiguration, "Video", "FPSMatchDisplayAuto"))
		return;

	uint32_t num = 0, den = 0;
	GetConfigFPS(num, den);

	std::wstring device = ConfiguredDisplay(activeConfiguration);
	const char *how = "selected";
	if (device.empty()) {
		const GameDisplay game = FindGameDisplay();
		device = game.device;
		how = game.source == GameDisplay::Source::GameWindow   ? "game window"
		      : game.source == GameDisplay::Source::Foreground ? "foreground window"
								      : "primary display";
	}

	double hz = 0.0, ppm = 0.0;
	uint64_t ageMs = 0;
	if (!LatestDisplayRefresh(device, hz, ppm, ageMs)) {
		blog(LOG_INFO, "[display-fps] no measurement of %ls (%s) yet; recording at %u/%u", device.c_str(), how,
		     num, den);
		return;
	}

	double locked = 0.0;
	if (!den || !DisplayLockedFrameRate(hz, (double)num / den, locked)) {
		blog(LOG_WARNING, "[display-fps] %u/%u FPS is not a simple ratio of %ls at %.6f Hz; unchanged", num, den,
		     device.c_str(), hz);
		return;
	}

	/* Measurements differ by a few hundredths of a ppm; do not reset video
	 * for a change that would move a correction by days. */
	const uint32_t type = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSType");
	if (type == 2 && std::fabs(locked / ((double)num / den) - 1.0) < 0.2e-6) {
		blog(LOG_INFO, "[display-fps] %ls (%s) %.7f Hz (%.1f s old); output already %u/%u", device.c_str(), how,
		     hz, ageMs / 1000.0, num, den);
		return;
	}

	uint32_t newNum = 0, newDen = 0;
	BestFrameRateFraction(locked, 1000000, newNum, newDen);

	/* Video cannot be reset while a stream, replay buffer or virtual camera
	 * uses it; the recording then keeps the current rate. */
	if (outputHandler && outputHandler->Active()) {
		blog(LOG_WARNING, "[display-fps] another output is active; recording at %u/%u instead of %u/%u", num,
		     den, newNum, newDen);
		return;
	}

	config_set_uint(activeConfiguration, "Video", "FPSType", 2);
	config_set_uint(activeConfiguration, "Video", "FPSNum", newNum);
	config_set_uint(activeConfiguration, "Video", "FPSDen", newDen);

	const int ret = ResetVideo();
	if (ret == OBS_VIDEO_SUCCESS) {
		blog(LOG_INFO, "[display-fps] %ls (%s) %.7f Hz (+/- %.3f ppm, %.1f s old): output %u/%u -> %u/%u",
		     device.c_str(), how, hz, ppm, ageMs / 1000.0, num, den, newNum, newDen);
		return;
	}

	blog(LOG_WARNING, "[display-fps] video reset failed (%d); restoring %u/%u", ret, num, den);
	config_set_uint(activeConfiguration, "Video", "FPSType", type);
	config_set_uint(activeConfiguration, "Video", "FPSNum", num);
	config_set_uint(activeConfiguration, "Video", "FPSDen", den);
	ResetVideo();
}
