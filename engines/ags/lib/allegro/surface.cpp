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

#define FORBIDDEN_SYMBOL_EXCEPTION_getenv
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_fopen
#define FORBIDDEN_SYMBOL_EXCEPTION_fclose
#define FORBIDDEN_SYMBOL_EXCEPTION_fwrite
#define FORBIDDEN_SYMBOL_EXCEPTION_snprintf
#include <stdio.h>
#include "ags/lib/allegro/gfx.h"
#include "ags/lib/allegro/color.h"
#include "ags/shared/ac/sst_asset_cache.h"
#include "ags/lib/allegro/flood.h"
#include "ags/ags.h"
#include "ags/globals.h"
#include "common/textconsole.h"
#include "graphics/screen.h"

namespace AGS3 {

BITMAP::BITMAP(Graphics::ManagedSurface *owner) : _owner(owner),
	w(owner->w), h(owner->h), pitch(owner->pitch), format(owner->format),
	clip(true), ct(0), cl(0), cr(owner->w), cb(owner->h) {
	line.resize(h);
	for (int y = 0; y < h; ++y)
		line[y] = (byte *)_owner->getBasePtr(0, y);
}

int BITMAP::getpixel(int x, int y) const {
	if (x < 0 || y < 0 || x >= w || y >= h)
		return -1;

	const byte *pixel = (const byte *)getBasePtr(x, y);
	if (format.bytesPerPixel == 1)
		return *pixel;
	else if (format.bytesPerPixel == 2)
		return *(const uint16 *)pixel;
	else
		return *(const uint32 *)pixel;
}

void BITMAP::makeOpaque() {
	if (format.aBits() == 0)
		return;
	assert(format.bytesPerPixel == 4);
	uint32 alphaMask = format.ARGBToColor(0xff, 0, 0, 0);

	unsigned char *pixels = getPixels();
	for (int y = 0 ; y < h ; ++y, pixels += pitch) {
		uint32 *data = (uint32 *)pixels;
		for (int x = 0 ; x < w ; ++x, ++data)
			(*data) |= alphaMask;
	}
}

void BITMAP::circlefill(int x, int y, int radius, int color) {
	int cx = 0;
	int cy = radius;
	int df = 1 - radius;
	int d_e = 3;
	int d_se = -2 * radius + 5;

	do {
		_owner->hLine(x - cy, y - cx, x + cy, color);

		if (cx)
			_owner->hLine(x - cy, y + cx, x + cy, color);

		if (df < 0) {
			df += d_e;
			d_e += 2;
			d_se += 2;
		} else {
			if (cx != cy) {
				_owner->hLine(x - cx, y - cy, x + cx, color);

				if (cy)
					_owner->hLine(x - cx, y + cy, x + cx, color);
			}

			df += d_se;
			d_e += 2;
			d_se += 4;
			cy--;
		}

		cx++;

	} while (cx <= cy);
}

void BITMAP::floodfill(int x, int y, int color) {
	AGS3::floodfill(this, x, y, color);
}

#define VGA_COLOR_TRANS(x) ((x) * 255 / 63)

BITMAP::DrawInnerArgs::DrawInnerArgs(BITMAP *_dstBitmap, const BITMAP *srcBitmap,
	const Common::Rect &srcRect, const Common::Rect &_dstRect, bool _skipTrans,
	int _srcAlpha, bool _horizFlip, bool _vertFlip, int _tintRed,
	int _tintGreen, int _tintBlue, bool doScale) : skipTrans(_skipTrans),
		srcAlpha(_srcAlpha), horizFlip(_horizFlip), vertFlip(_vertFlip),
		tintRed(_tintRed), tintGreen(_tintGreen), tintBlue(_tintBlue),
		src(**srcBitmap), shouldDraw(false), dstBitmap(*_dstBitmap),
		useTint(_tintRed >= 0 && _tintGreen >= 0 && _tintBlue >= 0),
		blenderMode(_G(_blender_mode)), dstRect(_dstRect) {
	// Allegro disables draw when the clipping rect has negative width/height.
	// Common::Rect instead asserts, which we don't want.
	if (dstBitmap.cr <= dstBitmap.cl || dstBitmap.cb <= dstBitmap.ct)
		return;

	// Figure out the dest area that will be updated
	srcArea = srcRect;
	srcArea.clip(Common::Rect(0, 0, srcBitmap->w, srcBitmap->h));
	if (srcArea.isEmpty())
		return;

	if (!doScale) {
		// Ensure the src rect is constrained to the source bitmap
		dstRect.setWidth(srcArea.width());
		dstRect.setHeight(srcArea.height());
	}
	Common::Rect destRect = dstRect.findIntersectingRect(
	                            Common::Rect(dstBitmap.cl, dstBitmap.ct, dstBitmap.cr, dstBitmap.cb));
	if (destRect.isEmpty())
		// Area is entirely outside the clipping area, so nothing to draw
		return;

	// Get source and dest surface. Note that for the destination we create
	// a temporary sub-surface based on the allowed clipping area
	Graphics::ManagedSurface &dest = *dstBitmap._owner;
	destArea = dest.getSubArea(destRect);

	// Define scaling and other stuff used by the drawing loops
	scaleX = SCALE_THRESHOLD * srcRect.width() / dstRect.width();
	scaleY = SCALE_THRESHOLD * srcRect.height() / dstRect.height();
	sameFormat = (src.format == dstBitmap.format);

	if (src.format.bytesPerPixel == 1 && dstBitmap.format.bytesPerPixel != 1) {
		for (int i = 0; i < PAL_SIZE; ++i) {
			palette[i].r = VGA_COLOR_TRANS(_G(current_palette)[i].r);
			palette[i].g = VGA_COLOR_TRANS(_G(current_palette)[i].g);
			palette[i].b = VGA_COLOR_TRANS(_G(current_palette)[i].b);
		}
	}

	transColor = 0, alphaMask = 0xff;
	if (skipTrans && src.format.bytesPerPixel != 1) {
		transColor = src.format.ARGBToColor(0, 255, 0, 255);
		alphaMask = src.format.ARGBToColor(255, 0, 0, 0);
		alphaMask = ~alphaMask;
	}

	xStart = (dstRect.left < destRect.left) ? dstRect.left - destRect.left : 0;
	yStart = (dstRect.top < destRect.top) ? dstRect.top - destRect.top : 0;
	shouldDraw = true;
}

/* SST: convert a >1bpp source rect to a fresh 8bpp BITMAP via
 * per-pixel bestfit_color against _G(current_palette).  Returns a
 * new BITMAP that the caller must destroy_bitmap().  Returns NULL
 * on empty rect or alloc failure. */
static BITMAP *sst_quantize_to_8bit(const BITMAP *src, const Common::Rect &srcRect) {
	const int sw = srcRect.width();
	const int sh = srcRect.height();
	if (sw <= 0 || sh <= 0) return nullptr;
	BITMAP *temp = create_bitmap_ex(8, sw, sh);
	if (!temp) return nullptr;
	const ::Graphics::ManagedSurface &srcSurf = src->getSurface();
	const byte srcBpp = srcSurf.format.bytesPerPixel;
	const int sx0 = srcRect.left;
	const int sy0 = srcRect.top;
	uint8 a, r, g, b;
	/* SST: alpha cutoff for treating a pixel as transparent.
	 * Default 128 (50%) was the original cutoff — works for AGS
	 * title-screen tinted overlays behind menu text.  HQ's night
	 * village layers a full-screen alpha-blended dark tint on top
	 * of the room bg; that overlay sits at ~alpha 160-200 and
	 * renders opaquely at threshold=128, hiding the bg + objects.
	 * Tunable via SPARCFB_ALPHA_THRESHOLD env (0..255).  Set higher
	 * to make more partial-alpha pixels transparent (kills night
	 * overlays); lower to keep them opaque (preserves title-screen
	 * tinted backgrounds). */
	static int s_alpha_thresh = -1;
	if (s_alpha_thresh < 0) {
		const char *e = getenv("SPARCFB_ALPHA_THRESHOLD");
		s_alpha_thresh = e ? atoi(e) : 128;
		if (s_alpha_thresh < 0) s_alpha_thresh = 0;
		if (s_alpha_thresh > 256) s_alpha_thresh = 256;
	}
	byte palette6[768];
	for (int i = 0; i < 256; ++i) {
		palette6[i*3] = _G(current_palette)[i].r;
		palette6[i*3+1] = _G(current_palette)[i].g;
		palette6[i*3+2] = _G(current_palette)[i].b;
	}
	const byte *nearest = AGS::Shared::SSTAssetCache::GetColourLookup(palette6);
	for (int yi = 0; yi < sh; ++yi) {
		const byte *srow = (const byte *)srcSurf.getBasePtr(sx0, sy0 + yi);
		byte *drow = (byte *)temp->getSurface().getBasePtr(0, yi);
		for (int xi = 0; xi < sw; ++xi) {
			uint32 sv = 0;
			switch (srcBpp) {
			case 1: sv = srow[xi]; break;
			case 2: sv = ((const uint16 *)srow)[xi]; break;
			case 4: sv = ((const uint32 *)srow)[xi]; break;
			}
			srcSurf.format.colorToARGB(sv, a, r, g, b);
			if ((int)a < s_alpha_thresh || (r == 255 && g == 0 && b == 255)) {
				drow[xi] = 0;
			} else {
				drow[xi] = nearest ? nearest[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)]
					: (byte)bestfit_color(_G(current_palette), r >> 2, g >> 2, b >> 2);
			}
		}
	}
	return temp;
}

void BITMAP::draw(const BITMAP *srcBitmap, const Common::Rect &srcRect,
                  int dstX, int dstY, bool horizFlip, bool vertFlip,
                  bool skipTrans, int srcAlpha, int tintRed, int tintGreen,
                  int tintBlue) {
	/* SST: optional per-call draw trace.  SPARCFB_DEBUG_DRAW=N logs
	 * the first N BITMAP::draw calls so we can correlate what AGS
	 * submits in a scene vs. what actually paints.  Disabled by
	 * default (env unset or 0).  warning() goes to scummvm stderr. */
	{
		static int s_draw_log_count = 0;
		static int s_draw_log_limit = -1;
		if (s_draw_log_limit < 0) {
			const char *e = getenv("SPARCFB_DEBUG_DRAW");
			s_draw_log_limit = e ? atoi(e) : 0;
		}
		if (s_draw_log_count < s_draw_log_limit) {
			s_draw_log_count++;
			warning("[sst-draw %d] src=%dx%d/%dbpp rect=(%d,%d)-(%d,%d) -> dst=(%d,%d) ours=%dx%d/%dbpp sT=%d sA=%d",
			        s_draw_log_count, srcBitmap->w, srcBitmap->h, srcBitmap->format.bytesPerPixel,
			        srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
			        dstX, dstY, w, h, format.bytesPerPixel, skipTrans?1:0, srcAlpha);
		}
	}

	/* SST: bg-sprite dump for diagnostic.  SPARCFB_DUMP_BG=N writes
	 * up to N UNIQUE 320x200 8bpp sprites drawn at dst=(0,0) — those
	 * are the room background blits — to /tmp/sst-bg-NNN.bin (raw
	 * 320x200 pal8 bytes, no header).  Decode on host against the
	 * active _palette.bin.  Dedupes via 8-row sparse checksum so
	 * identical bgs across frames don't waste slots. */
	{
		static int s_dump_bg_count = 0;
		static int s_dump_bg_limit = -1;
		static uint32 s_dump_bg_last_sig = 0;
		if (s_dump_bg_limit < 0) {
			const char *e = getenv("SPARCFB_DUMP_BG");
			s_dump_bg_limit = e ? atoi(e) : 0;
		}
		/* Match any 8bpp sprite at dst=(0,0) with screen-like or
		 * room-buffer dimensions.  HQ uses 640x200 internal bg
		 * buffers; menu screens use 320x200 directly.  Dump both. */
		const int sw = srcBitmap->w, sh = srcBitmap->h;
		const bool sst_dump_size_ok =
		    sh == 200 && (sw == 320 || sw == 640) &&
		    srcBitmap->format.bytesPerPixel == 1 &&
		    dstX == 0 && dstY == 0;
		if (s_dump_bg_count < s_dump_bg_limit && sst_dump_size_ok) {
			const ::Graphics::ManagedSurface &dsurf = srcBitmap->getSurface();
			/* Sparse 8-row signature for quick dedupe — sample over
			 * full src width (sw), works for 320 and 640. */
			uint32 sig = 0xdeadbeef;
			for (int row = 0; row < 200; row += 25) {
				const byte *rrow = (const byte *)dsurf.getBasePtr(0, row);
				for (int x = 0; x < sw; x += 8) {
					sig = sig * 31 + rrow[x];
				}
			}
			if (sig != s_dump_bg_last_sig) {
				s_dump_bg_last_sig = sig;
				char dpath[64];
				snprintf(dpath, sizeof(dpath), "/tmp/sst-bg-%03d-%dx%d.bin", s_dump_bg_count, sw, sh);
				FILE *dfp = fopen(dpath, "wb");
				if (dfp) {
					for (int yi = 0; yi < 200; yi++) {
						fwrite(dsurf.getBasePtr(0, yi), 1, sw, dfp);
					}
					fclose(dfp);
					warning("[sst-dump-bg %d] wrote %s (sig=%08x)", s_dump_bg_count, dpath, sig);
				}
				s_dump_bg_count++;
			}
		}
	}


	// A restricted number of 8bit games (e.g. Snow Problem) contain (leftover?) 32bit resources.
	// We can ignore these to prevent conversion on load (and triggering the assertion)
	if (format.bytesPerPixel == 1 && srcBitmap->format.bytesPerPixel != 1) {
		/* SST: force-8bit games hit this for engine-internal bitmaps
		 * that bypass PrepareSpriteForUse.  Quantize, recurse. */
		BITMAP *sst_q = sst_quantize_to_8bit(srcBitmap, srcRect);
		if (!sst_q) return;
		this->draw(sst_q, Common::Rect(0, 0, sst_q->w, sst_q->h),
		           dstX, dstY, horizFlip, vertFlip,
		           skipTrans, srcAlpha, tintRed, tintGreen, tintBlue);
		destroy_bitmap(sst_q);
		return;
	}

	assert(format.bytesPerPixel == 2 || format.bytesPerPixel == 4 ||
	       (format.bytesPerPixel == 1 && srcBitmap->format.bytesPerPixel == 1));

	Graphics::ManagedSurface flipped;
	if (horizFlip || vertFlip) {
		// Horizontal flipping produces errors in the optimized paths, while vertical
		// may result in crashes. For now, we pre-flip to a temporary surface
		Graphics::ManagedSurface cropped(const_cast<BITMAP *>(srcBitmap)->getSurface(), srcRect);
		flipped.copyFrom(cropped);

		if (horizFlip) {
			flipped.surfacePtr()->flipHorizontal(flipped.getBounds());
		}

		if (vertFlip) {
			flipped.surfacePtr()->flipVertical(flipped.getBounds());
		}
	}
	BITMAP temp(&flipped);

	auto args = DrawInnerArgs(this, (horizFlip || vertFlip) ? &temp : srcBitmap, (horizFlip || vertFlip) ? flipped.getBounds() : srcRect, Common::Rect(dstX, dstY, dstX + 1, dstY + 1), skipTrans, srcAlpha, false, false, tintRed, tintGreen, tintBlue, false);
	if (!args.shouldDraw) return;
	if (!args.sameFormat && args.src.format.bytesPerPixel == 1) {
		if (format.bytesPerPixel == 4)
			drawInnerGeneric<4, 1, false>(args);
		else
			drawInnerGeneric<2, 1, false>(args);
		return;
	}
#ifdef SCUMMVM_NEON
	if (_G(simd_flags) & AGS3::Globals::SIMD_NEON) {
		drawNEON<false>(args);
		return;
	}
#endif
#ifdef SCUMMVM_AVX2
	if (_G(simd_flags) & AGS3::Globals::SIMD_AVX2) {
		drawAVX2<false>(args);
		return;
	}
#endif
#ifdef SCUMMVM_SSE2
	if (_G(simd_flags) & AGS3::Globals::SIMD_SSE2) {
		drawSSE2<false>(args);
		return;
	}
#endif
	drawGeneric<false>(args);
}

void BITMAP::stretchDraw(const BITMAP *srcBitmap, const Common::Rect &srcRect,
                         const Common::Rect &dstRect, bool skipTrans, int srcAlpha) {
	{
		static int s_stretch_log_count = 0;
		static int s_stretch_log_limit = -1;
		if (s_stretch_log_limit < 0) {
			const char *e = getenv("SPARCFB_DEBUG_DRAW");
			s_stretch_log_limit = e ? atoi(e) : 0;
		}
		if (s_stretch_log_count < s_stretch_log_limit) {
			s_stretch_log_count++;
			warning("[sst-stretch %d] src=%dx%d/%dbpp rect=(%d,%d)-(%d,%d) -> dstRect=(%d,%d)-(%d,%d) ours=%dx%d/%dbpp sT=%d sA=%d",
			        s_stretch_log_count, srcBitmap->w, srcBitmap->h, srcBitmap->format.bytesPerPixel,
			        srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
			        dstRect.left, dstRect.top, dstRect.right, dstRect.bottom,
			        w, h, format.bytesPerPixel, skipTrans?1:0, srcAlpha);
		}
	}
	if (format.bytesPerPixel == 1 && srcBitmap->format.bytesPerPixel != 1) {
		/* SST: same quantize+recurse as BITMAP::draw, here so the
		 * stretched character portrait / scaled sprites also render. */
		BITMAP *sst_q = sst_quantize_to_8bit(srcBitmap, srcRect);
		if (!sst_q) return;
		this->stretchDraw(sst_q, Common::Rect(0, 0, sst_q->w, sst_q->h),
		                  dstRect, skipTrans, srcAlpha);
		destroy_bitmap(sst_q);
		return;
	}
	assert(format.bytesPerPixel == 2 || format.bytesPerPixel == 4 ||
	       (format.bytesPerPixel == 1 && srcBitmap->format.bytesPerPixel == 1));
	auto args = DrawInnerArgs(this, srcBitmap, srcRect, dstRect, skipTrans, srcAlpha, false, false, -1, -1, -1, true);
	if (!args.shouldDraw) return;
	if (!args.sameFormat && args.src.format.bytesPerPixel == 1) {
		if (format.bytesPerPixel == 4)
			drawInnerGeneric<4, 1, true>(args);
		else
			drawInnerGeneric<2, 1, true>(args);
		return;
	}

	// Stretching at the same time as blitting produces errors when
	// using the optimized paths; for now, we pre-stretch to a temporary surface
	Graphics::ManagedSurface cropped(const_cast<BITMAP *>(srcBitmap)->getSurface(), srcRect);
	// We need to use Surface::scale, since ManagedSurface _always_ respects the source alpha, and thus skips transparent pixels
	Graphics::ManagedSurface *stretched = cropped.scale(dstRect.width(), dstRect.height());
	BITMAP temp(stretched);
	auto optimizedArgs = DrawInnerArgs(this, &temp, stretched->getBounds(), dstRect, skipTrans, srcAlpha, false, false, -1, -1, -1, false);

#ifdef SCUMMVM_NEON
	if (_G(simd_flags) & AGS3::Globals::SIMD_NEON) {
		drawNEON<false>(optimizedArgs);
	} else
#endif
#ifdef SCUMMVM_AVX2
	if (_G(simd_flags) & AGS3::Globals::SIMD_AVX2) {
		drawAVX2<false>(optimizedArgs);
	} else
#endif
#ifdef SCUMMVM_SSE2
	if (_G(simd_flags) & AGS3::Globals::SIMD_SSE2) {
		drawSSE2<false>(optimizedArgs);
	} else
#endif
	{
		drawGeneric<true>(optimizedArgs);
	}

	delete stretched;
}
void BITMAP::blendPixel(uint8 aSrc, uint8 rSrc, uint8 gSrc, uint8 bSrc, uint8 &aDest, uint8 &rDest, uint8 &gDest, uint8 &bDest, uint32 alpha, bool useTint, byte *destVal) const {
	switch (_G(_blender_mode)) {
	case kSourceAlphaBlender:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendSourceAlpha(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kArgbToArgbBlender:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendArgbToArgb(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kArgbToRgbBlender:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendArgbToRgb(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kRgbToArgbBlender:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendRgbToArgb(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kRgbToRgbBlender:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendRgbToRgb(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kAlphaPreservedBlenderMode:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendPreserveAlpha(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kOpaqueBlenderMode:
		blendOpaque(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kAdditiveBlenderMode:
		if (!useTint) format.colorToARGB(getColor(destVal, format.bytesPerPixel), aDest, rDest, gDest, bDest);
		blendAdditiveAlpha(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha);
		break;
	case kTintBlenderMode:
		blendTintSprite(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha, false);
		break;
	case kTintLightBlenderMode:
		blendTintSprite(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, alpha, true);
		break;
	}
}

void BITMAP::blendTintSprite(uint8 aSrc, uint8 rSrc, uint8 gSrc, uint8 bSrc, uint8 &aDest, uint8 &rDest, uint8 &gDest, uint8 &bDest, uint32 alpha, bool light) const {
	// Used from draw_lit_sprite after set_blender_mode(kTintBlenderMode or kTintLightBlenderMode)
	// Original blender function: _myblender_color32 and _myblender_color32_light
	float xh, xs, xv;
	float yh, ys, yv;
	int r, g, b;
	rgb_to_hsv(rSrc, gSrc, bSrc, &xh, &xs, &xv);
	rgb_to_hsv(rDest, gDest, bDest, &yh, &ys, &yv);
	if (light) {
		// adjust luminance
		// (I think the writer meant value, since they are using hsV)
		yv -= (1.0 - ((float)alpha / 250.0));
		if (yv < 0.0)
			yv = 0.0;
	}
	hsv_to_rgb(xh, xs, yv, &r, &g, &b);
	rDest = static_cast<uint8>(r & 0xff);
	gDest = static_cast<uint8>(g & 0xff);
	bDest = static_cast<uint8>(b & 0xff);
	// Preserve value in aDest
}

/*-------------------------------------------------------------------*/

/**
 * Dervied screen surface
 */
class Screen : public Graphics::Screen, public BITMAP {
public:
	Screen() : Graphics::Screen(), BITMAP(this) {}
	Screen(int width, int height) : Graphics::Screen(width, height), BITMAP(this) {}
	Screen(int width, int height, const Graphics::PixelFormat &pixelFormat) :
		Graphics::Screen(width, height, pixelFormat), BITMAP(this) {}
	~Screen() override {}
};

/*-------------------------------------------------------------------*/

BITMAP *create_bitmap(int width, int height) {
	return create_bitmap_ex(get_color_depth(), width, height);
}

BITMAP *create_bitmap_ex(int color_depth, int width, int height) {
	Graphics::PixelFormat format;

	switch (color_depth) {
	case 8:
		format = Graphics::PixelFormat::createFormatCLUT8();
		break;
	case 16:
		format = Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0);
		break;
	case 32:
		format = Graphics::PixelFormat(4, 8, 8, 8, 8, 16, 8, 0, 24);
		break;
	default:
		error("Invalid color depth");
	}
	width = MAX(width, 0);
	height = MAX(height, 0);
	BITMAP *bitmap = new Surface(width, height, format);
	return bitmap;
}

BITMAP *create_sub_bitmap(BITMAP *parent, int x, int y, int width, int height) {
	Graphics::ManagedSurface &surf = **parent;
	return new Surface(surf, Common::Rect(x, y, x + width, y + height));
}

BITMAP *create_video_bitmap(int width, int height) {
	return new Screen(width, height);
}

BITMAP *create_system_bitmap(int width, int height) {
	return create_bitmap(width, height);
}

void destroy_bitmap(BITMAP *bitmap) {
	delete bitmap;
}

} // namespace AGS3
