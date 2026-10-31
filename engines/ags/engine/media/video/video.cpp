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

#include "common/config-manager.h"
#include "graphics/palette.h"
#include "graphics/paletteman.h"
#include "video/avi_decoder.h"
#include "video/flic_decoder.h"
#include "video/mpegps_decoder.h"
#include "video/theora_decoder.h"
#include "ags/shared/core/platform.h"
#include "ags/shared/core/types.h"
#include "ags/engine/debugging/debug_log.h"
#include "ags/shared/debugging/out.h"
#include "ags/engine/ac/asset_helper.h"
#include "ags/shared/ac/common.h"
#include "ags/engine/ac/draw.h"
#include "ags/shared/ac/game_version.h"
#include "ags/shared/ac/game_setup_struct.h"
#include "ags/engine/ac/game_state.h"
#include "ags/engine/ac/global_display.h"
#include "ags/engine/ac/mouse.h"
#include "ags/engine/ac/sys_events.h"
#include "ags/engine/ac/runtime_defines.h"
#include "ags/engine/ac/system.h"
#include "ags/shared/core/asset_manager.h"
#include "ags/shared/gfx/bitmap.h"
#include "ags/engine/gfx/ddb.h"
#include "ags/engine/gfx/graphics_driver.h"
#include "ags/engine/main/game_run.h"
#include "ags/shared/util/stream.h"
#include "ags/engine/media/audio/audio_system.h"
#include "ags/engine/media/video/video.h"
#include "ags/ags.h"
#include "ags/events.h"

namespace AGS3 {

using AGS::Shared::AssetManager;

class SSTVideoPlaybackState {
public:
	SSTVideoPlaybackState(bool indexed, const char *name, uint32 duration) :
		_indexed(indexed), _name(name), _start(g_system->getMillis()), _duration(duration), frames(0) {
		if (_indexed)
			g_system->getPaletteManager()->grabPalette(_palette, 0, 256);
	}
	~SSTVideoPlaybackState() {
		if (_indexed)
			g_system->getPaletteManager()->setPalette(_palette, 0, 256);
		warning("[sst-video-cache] %s frames=%u elapsed-ms=%u duration-ms=%u palette-restored=%d",
			_name, frames, g_system->getMillis() - _start, _duration, _indexed);
	}
	uint32 frames;
private:
	bool _indexed;
	const char *_name;
	uint32 _start, _duration;
	byte _palette[768];
};

static bool play_video(Video::VideoDecoder *decoder, const char *name, int flags, VideoSkipType skip, bool showError) {
	std::unique_ptr<Stream> video_stream(_GP(AssetMgr)->OpenAsset(name));
	if (!video_stream) {
		if (showError)
			Display("Unable to load video '%s'", name);
		return false;
	}

	AGS::Shared::ScummVMReadStream *stream = new AGS::Shared::ScummVMReadStream(video_stream.get(), DisposeAfterUse::NO);

	if (!decoder->loadStream(stream)) {
		warning("Unable to decode video '%s'", name);
		return false;
	}

	update_polled_stuff();  // TODO: probably unneeded

	Graphics::Screen scr;
	bool stretchVideo = (flags & kVideo_Stretch) != 0;
	bool enableVideo = (flags & kVideo_EnableVideo) != 0;
	bool enableAudio = (flags & kVideo_EnableAudio) != 0;
	bool clut8Screen = scr.format.isCLUT8();
	SSTVideoPlaybackState playback(clut8Screen, name, decoder->getDuration().msecs());

	if (!enableAudio)
		decoder->setVolume(0);

	update_polled_stuff();

	decoder->start();
	while (!SHOULD_QUIT && !decoder->endOfVideo()) {
		if (decoder->needsUpdate()) {
			// Get the next video frame and draw onto the screen
			const Graphics::Surface *frame = decoder->decodeNextFrame();
			if (frame)
				++playback.frames;
			if (clut8Screen && decoder->hasDirtyPalette())
				scr.setPalette(decoder->getPalette(), 0, 256);

			if (frame && enableVideo) {
				Rect dstRect = PlaceInRect(RectWH(0, 0, scr.w, scr.h), RectWH(0, 0, frame->w, frame->h),
					(stretchVideo ? kPlaceStretchProportional : kPlaceCenter));

				if (stretchVideo && frame->w == dstRect.GetWidth() && frame->h == dstRect.GetHeight())
					// Don't need to stretch video after all
					stretchVideo = false;

				Graphics::Palette p(decoder->getPalette(), 256);
				if (stretchVideo) {
					scr.fillRect(Common::Rect(dstRect.Left, dstRect.Top, dstRect.Right + 1, dstRect.Bottom + 1), 0);
					scr.transBlitFrom(*frame, Common::Rect(0, 0, frame->w, frame->h),
									  Common::Rect(dstRect.Left, dstRect.Top, dstRect.Right + 1, dstRect.Bottom + 1),
									  &p);
				} else {
					scr.blitFrom(*frame, Common::Point(dstRect.Left, dstRect.Top), &p);
				}
			}

			scr.update();
		}

		g_system->delayMillis(10);
		::AGS::g_events->pollEvents();

		if (skip != VideoSkipNone) {
			// Check for whether user aborted video
			KeyInput ki;
			eAGSMouseButton mbut;
			int mwheelz;
			// Handle all the buffered key events
			bool do_break = false;
			while (ags_keyevent_ready()) {
				if (run_service_key_controls(ki) && !IsAGSServiceKey(ki.Key)) {
					if ((ki.Key == eAGSKeyCodeEscape) && (skip == VideoSkipEscape))
						do_break = true;
					if (skip >= VideoSkipAnyKey)
						do_break = true;  // skip on any key
				}
			}
			if (do_break)
				return true; // skip on key press
			if (run_service_mb_controls(mbut, mwheelz) && mbut >= kMouseNone && skip == VideoSkipKeyOrMouse)
				return true; // skip on mouse click
		}
	}

	// Clear the screen after playback
	if (_G(gfxDriver)->UsesMemoryBackBuffer())
		_G(gfxDriver)->GetMemoryBackBuffer()->Clear();
	render_to_screen();

	invalidate_screen();

	return true;
}

bool play_avi_video(const char *name, int flags, VideoSkipType skip, bool showError) {
	Video::AVIDecoder decoder;
	return play_video(&decoder, name, flags, skip, showError);
}

bool play_mpeg_video(const char *name, int flags, VideoSkipType skip, bool showError) {
	Video::MPEGPSDecoder decoder;
	return play_video(&decoder, name, flags, skip, showError);
}

bool play_theora_video(const char *name, int flags, VideoSkipType skip, bool showError) {
	// Optional offline conversion for machines with indexed framebuffers.
	// Keep original assets intact and use a same-basename AVI only when the
	// user enables this policy and the replacement is actually available.
	if (ConfMan.hasKey("sst_prefer_avi_video") && ConfMan.getBool("sst_prefer_avi_video")) {
		Common::String replacement(name);
		if (replacement.size() >= 4 && replacement.hasSuffix(".ogv")) {
			replacement = replacement.substr(0, replacement.size() - 4) + ".avi";
			if (_GP(AssetMgr)->DoesAssetExist(replacement.c_str())) {
				warning("[sst-video-cache] playing %s for %s", replacement.c_str(), name);
				if (play_avi_video(replacement.c_str(), flags, skip, false))
					return true;
			}
		}
	}
#if !defined (USE_THEORADEC)
	if (showError)
		Display("This games uses Theora videos but ScummVM has been compiled without Theora support");
	return false;
#else
	Video::TheoraDecoder decoder;
	return play_video(&decoder, name, flags, skip, showError);
#endif
}

bool play_flc_video(int numb, int flags, VideoSkipType skip) {
	Video::FlicDecoder decoder;
	String flicName = String::FromFormat("flic%d.flc", numb);

	return play_video(&decoder, flicName, flags, skip, false);
}

void video_pause() {
	// TODO
}

void video_resume() {
	// TODO
}

} // namespace AGS3
