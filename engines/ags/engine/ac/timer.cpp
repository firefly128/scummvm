/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/std/thread.h"
#include "common/config-manager.h"
#include "ags/engine/ac/timer.h"
#include "ags/shared/core/platform.h"
#include "ags/engine/ac/sys_events.h"
#include "ags/engine/platform/base/ags_platform_driver.h"
#include "ags/ags.h"
#include "ags/globals.h"

namespace AGS3 {

namespace {
const auto MAXIMUM_FALL_BEHIND = 3; // number of full frames
}

std::chrono::microseconds GetFrameDuration() {
	if (_G(framerate_maxed)) {
		return std::chrono::microseconds(0);
	}
	return _G(tick_duration);
}

int setTimerFps(int new_fps) {
	assert(new_fps >= 0);
	if (new_fps <= 0)
		return _G(framerate);
	int old_fps = _G(framerate);
	_G(tick_duration) = std::chrono::microseconds(1000000LL / new_fps);
	_G(framerate) = new_fps;
	_G(framerate_maxed) = new_fps >= 1000;

	// Update next frame time
	_G(next_frame_timestamp) = _G(last_tick_time) + _G(tick_duration);
	return old_fps;
}

bool isTimerFpsMaxed() {
	return _G(framerate_maxed);
}

void WaitForNextFrame() {
	const auto now = AGS_Clock::now();
	const auto frameDuration = GetFrameDuration();

	// early exit if we're trying to maximise framerate
	if (frameDuration <= std::chrono::milliseconds::zero()) {
		_G(last_tick_time) = _G(next_frame_timestamp);
		_G(next_frame_timestamp) = now;
		// suspend while the game is being switched out
		while (_G(game_update_suspend) && !_G(want_exit) && !_G(abort_engine)) {
			sys_evt_process_pending();
			_G(platform)->YieldCPU();
		}
		return;
	}

	// jump ahead if we're lagging
	if (static_cast<int32>(now - _G(next_frame_timestamp)) >
	    static_cast<int32>(RealtimePacingEnabled() ? 1000 : MAXIMUM_FALL_BEHIND * static_cast<uint32>(frameDuration))) {
		_G(next_frame_timestamp) = now;
	}

	if (static_cast<int32>(_G(next_frame_timestamp) - now) > 0) {
		auto frame_time_remaining = _G(next_frame_timestamp) - now;
		std::this_thread::sleep_for(frame_time_remaining);
	}

	_G(last_tick_time) = _G(next_frame_timestamp);
	_G(next_frame_timestamp) += frameDuration;

	// suspend while the game is being switched out
	while (_G(game_update_suspend) && !_G(want_exit) && !_G(abort_engine)) {
		sys_evt_process_pending();
		_G(platform)->YieldCPU();
	}
}

void skipMissedTicks() {
	_G(last_tick_time) = AGS_Clock::now();
	_G(next_frame_timestamp) = AGS_Clock::now();
}


bool RealtimePacingEnabled() {
 return ConfMan.hasKey("sst_ags_realtime") && ConfMan.getBool("sst_ags_realtime");
}

static uint32 lastRenderFinished = 0;
static bool haveRenderedFrame = false;

void RecordGameFrameRendered() {
 lastRenderFinished = AGS_Clock::now();
 haveRenderedFrame = true;
}

bool ShouldRenderGameFrame() {
 static unsigned skipped = 0;
 if (!RealtimePacingEnabled() || _G(framerate_maxed)) {
  skipped = 0;
  haveRenderedFrame = false;
  return true;
 }
 const uint32 frame = static_cast<uint32>(GetFrameDuration());
 const uint32 now = AGS_Clock::now();
 const int32 late = static_cast<int32>(now - _G(next_frame_timestamp));
 // Run every simulation tick, including scripts, input and audio. Only the
 // expensive picture is omitted while catching up. Bound display starvation.
 // If simulation itself cannot sustain the target FPS, tick-based skipping
 // must not starve a cheap screen forever. Render within 100 ms of the
 // last completed picture, even while retaining simulation debt.
 if (late >= static_cast<int32>(frame) && skipped < 16 && haveRenderedFrame &&
     static_cast<uint32>(now - lastRenderFinished) < 100) {
  ++skipped;
  return false;
 }
 skipped = 0;
 return true;
}

TransitionClock::TransitionClock(uint32 interval)
 : _start(g_system->getMillis()),
   _interval(interval ? interval : static_cast<uint32>(GetFrameDuration())),
   _enabled(RealtimePacingEnabled()) {
 if (!_interval) _interval = 1;
}

TransitionClock::~TransitionClock() {
 // Blocking transitions do not accumulate simulation debt for the next room.
 if (_enabled) skipMissedTicks();
}

int TransitionClock::Step(int current, int count) const {
 if (!_enabled) return current;
 const uint32 elapsed = g_system->getMillis() - _start;
 const uint32 due = elapsed / _interval;
 return due >= static_cast<uint32>(count) ? count : MAX(current, static_cast<int>(due));
}

void TransitionClock::Wait(int completed) const {
 if (!_enabled) {
  WaitForNextFrame();
  return;
 }
 const uint32 elapsed = g_system->getMillis() - _start;
 const uint32 deadline = static_cast<uint32>(completed) * _interval;
 if (elapsed < deadline) g_system->delayMillis(deadline - elapsed);
}

} // namespace AGS3
