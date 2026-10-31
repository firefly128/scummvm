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

#include "ags/lib/allegro/color.h"
#include "ags/ags.h"
#include "common/array.h"
#include "ags/lib/allegro/indexed_blit.h"
#include "ags/lib/allegro/surface.h"
#include "ags/globals.h"
#include "ags/shared/ac/sst_asset_cache.h"

#include "ags/lib/allegro/indexed_blend.h"

namespace AGS3 {

template<int DestBytesPerPixel, int SrcBytesPerPixel, bool Scale>
void BITMAP::drawInnerGeneric(DrawInnerArgs &args) {
	// Source-alpha sprites lose their alpha channel when cached as INDEX8.
	// A zero blend constant in source-alpha mode means use pixel alpha,
	// not a fully transparent global opacity. Keep cached mask semantics.
	const int indexedAlpha = (_G(_blender_mode) == kSourceAlphaBlender) ? 255 : args.srcAlpha;
	// Cache RGB palette blends by source/destination index for this opacity.
	// Rebuild lazily when opacity or palette changes: avoid a 256-colour
	// nearest-match search for every pixel of every fullscreen overlay.
	static RGB blendPalette[256];
	static int blendAlpha = -2;
	static uint16 blendTable[65536];
	static uint16 blendStamp = 0;
	const byte *nearestColours = nullptr;
	if (DestBytesPerPixel == 1 && indexedAlpha > 0 && indexedAlpha < 255 && !args.useTint) {
		byte palette6[768];
		for (int i = 0; i < 256; ++i) {
			palette6[i*3] = _G(current_palette)[i].r;
			palette6[i*3+1] = _G(current_palette)[i].g;
			palette6[i*3+2] = _G(current_palette)[i].b;
		}
		nearestColours = AGS::Shared::SSTAssetCache::GetColourLookup(palette6);
		if (blendAlpha != indexedAlpha || memcmp(blendPalette, _G(current_palette), sizeof(blendPalette))) {
			memcpy(blendPalette, _G(current_palette), sizeof(blendPalette));
			blendAlpha = indexedAlpha;
			// A generation tag invalidates cached colours without clearing 128 KB
			// every time adjacent sprites use different opacity values.
			blendStamp += 0x100;
			if (blendStamp == 0) {
				memset(blendTable, 0, sizeof(blendTable));
				blendStamp = 0x100;
			}
		}
	}
	const int xDir = args.horizFlip ? -1 : 1;
	byte rSrc, gSrc, bSrc, aSrc;
	byte rDest = 0, gDest = 0, bDest = 0, aDest = 0;

	// Instead of skipping pixels outside our boundary here, we just clip
	// our area instead.
	int xCtrStart = 0, xCtrBppStart = 0, xCtrWidth = args.dstRect.width();
	if (args.xStart + xCtrWidth > args.destArea.w) { // Clip the right
		xCtrWidth = args.destArea.w - args.xStart;
	}
	if (args.xStart < 0) { // Clip the left
		xCtrStart = -args.xStart;
		xCtrBppStart = xCtrStart * SrcBytesPerPixel;
		args.xStart = 0;
	}
	int destY = args.yStart, yCtr = 0, srcYCtr = 0, scaleYCtr = 0, yCtrHeight = args.dstRect.height();
	if (args.yStart < 0) { // Clip the top
		yCtr = -args.yStart;
		destY = 0;
		if (Scale) {
			scaleYCtr = yCtr * args.scaleY;
			srcYCtr = scaleYCtr / BITMAP::SCALE_THRESHOLD;
		}
	}
	if (args.yStart + yCtrHeight > args.destArea.h) { // Clip the bottom
		yCtrHeight = args.destArea.h - args.yStart;
	}

	byte *destP = (byte *)args.destArea.getBasePtr(0, destY);
	const byte *srcP = (const byte *)args.src.getBasePtr(
	                       args.horizFlip ? args.srcArea.right - 1 : args.srcArea.left,
	                       args.vertFlip ? args.srcArea.bottom - 1 - yCtr :
	                       args.srcArea.top + yCtr);
	// Cached INDEX8 backgrounds and opaque sprites need no colour decoding.
	// Clipping above also supplies the correct source offset for negative X/Y.
	if (DestBytesPerPixel == 1 && SrcBytesPerPixel == 1 && !Scale &&
	    !args.horizFlip && (indexedAlpha == -1 || indexedAlpha >= 255 || args.useTint)) {
		copyIndexedRows(destP + args.xStart, args.destArea.pitch,
		                srcP + xCtrBppStart, args.vertFlip ? -args.src.pitch : args.src.pitch,
		                MAX(0, xCtrWidth - xCtrStart), MAX(0, yCtrHeight - yCtr),
		                args.skipTrans, static_cast<byte>(args.transColor));
		return;
	}
	if (DestBytesPerPixel == 1 && SrcBytesPerPixel == 1 && !Scale && !args.useTint &&
	    indexedAlpha != -1 && indexedAlpha <= 0) return;
	if (DestBytesPerPixel == 1 && SrcBytesPerPixel == 1 && !Scale && !args.useTint &&
	    indexedAlpha > 0 && indexedAlpha < 255) {
		const RGB *palette = _G(current_palette);
		const auto compute = [palette, indexedAlpha, nearestColours](byte source, byte dest) -> uint16 {
			const RGB &sc = palette[source], &dc = palette[dest];
			const int r = (sc.r * indexedAlpha + dc.r * (255-indexedAlpha) + 127) / 255;
			const int g = (sc.g * indexedAlpha + dc.g * (255-indexedAlpha) + 127) / 255;
			const int b = (sc.b * indexedAlpha + dc.b * (255-indexedAlpha) + 127) / 255;
			return nearestColours ? nearestColours[(r << 12) | (g << 6) | b] : bestfit_color(palette, r, g, b);
		};
        if (args.skipTrans && ::AGS::g_vm && ::AGS::g_vm->useMaskWordScan()) {
            const int width=MAX(0,xCtrWidth-xCtrStart),height=MAX(0,yCtrHeight-yCtr);
            byte *out=destP+args.xStart;
            const byte *in=srcP+xDir*xCtrBppStart;
            const int pitch=args.vertFlip ? -args.src.pitch : args.src.pitch;
            bool verify=::AGS::g_vm->verifyMaskWordScan() && width==320 && height==200 &&
                xDir==1 && pitch>=width && args.destArea.pitch>=width;
            if(verify) {
                const uintptr sb=reinterpret_cast<uintptr>(in),db=reinterpret_cast<uintptr>(out);
                verify=sb+pitch*(height-1)+width<=db || db+args.destArea.pitch*(height-1)+width<=sb;
            }
            Common::Array<byte> reference;
            if(verify) {
                reference.resize(width*height);
                for(int y=0;y<height;++y)memcpy(reference.data()+y*width,out+y*args.destArea.pitch,width);
                blendIndexedRows<true>(reference.data(),width,in,pitch,xDir,width,height,
                    static_cast<byte>(args.transColor),blendTable,blendStamp,compute);
            }
            blendIndexedRows<true,true>(out,args.destArea.pitch,in,pitch,xDir,width,height,
                static_cast<byte>(args.transColor),blendTable,blendStamp,compute);
            if(verify) {
                for(int y=0;y<height;++y)
                    if(memcmp(reference.data()+y*width,out+y*args.destArea.pitch,width))
                        error("SST mask-word scan differs from scalar blend");
                static unsigned checked=0;
                if(++checked%25==0)warning("SST mask-word pixels matched frames=%u alpha=%d",checked,indexedAlpha);
            }
        } else if (args.skipTrans)
            blendIndexedRows<true>(destP + args.xStart, args.destArea.pitch,
                srcP + xDir*xCtrBppStart, args.vertFlip ? -args.src.pitch : args.src.pitch,
                xDir, MAX(0, xCtrWidth-xCtrStart), MAX(0, yCtrHeight-yCtr),
                static_cast<byte>(args.transColor), blendTable, blendStamp, compute);
        else
			blendIndexedRows<false>(destP + args.xStart, args.destArea.pitch,
				srcP + xDir*xCtrBppStart, args.vertFlip ? -args.src.pitch : args.src.pitch,
				xDir, MAX(0, xCtrWidth-xCtrStart), MAX(0, yCtrHeight-yCtr),
				static_cast<byte>(args.transColor), blendTable, blendStamp, compute);
		return;
	}
	for (; yCtr < yCtrHeight; ++destY, ++yCtr, scaleYCtr += args.scaleY) {
		if (Scale) {
			int newSrcYCtr = scaleYCtr / BITMAP::SCALE_THRESHOLD;
			if (srcYCtr != newSrcYCtr) {
				int diffSrcYCtr = newSrcYCtr - srcYCtr;
				srcP += args.src.pitch * diffSrcYCtr;
				srcYCtr = newSrcYCtr;
			}
		}
		// Loop through the pixels of the row
		for (int destX = args.xStart, xCtr = xCtrStart, xCtrBpp = xCtrBppStart, scaleXCtr = xCtr * args.scaleX; xCtr < xCtrWidth; ++destX, ++xCtr, xCtrBpp += SrcBytesPerPixel, scaleXCtr += args.scaleX) {
			const byte *srcVal = srcP + xDir * xCtrBpp;
			if (Scale) {
				srcVal = srcP + (scaleXCtr / BITMAP::SCALE_THRESHOLD) * SrcBytesPerPixel;
			}
			uint32 srcCol = getColor(srcVal, SrcBytesPerPixel);

			// Check if this is a transparent color we should skip
			if (args.skipTrans && ((srcCol & args.alphaMask) == args.transColor))
				continue;

			byte *destVal = (byte *)&destP[destX * DestBytesPerPixel];

			// When blitting to the same format we can just copy the color
			if (DestBytesPerPixel == 1) {
				// Indexed pixels must blend in RGB space, never as palette indices.
				// Keep opaque copies exact, including index zero on unmasked blits.
				if (indexedAlpha == -1 || indexedAlpha >= 255 || args.useTint) {
					*destVal = srcCol;
				} else if (indexedAlpha > 0) {
					uint16 &cached = blendTable[(srcCol << 8) | *destVal];
					if ((cached & 0xff00) != blendStamp) {
						const RGB &sc = _G(current_palette)[srcCol];
						const RGB &dc = _G(current_palette)[*destVal];
						const int alpha = indexedAlpha;
						const int r = (sc.r * alpha + dc.r * (255-alpha) + 127) / 255;
						const int g = (sc.g * alpha + dc.g * (255-alpha) + 127) / 255;
						const int b = (sc.b * alpha + dc.b * (255-alpha) + 127) / 255;
						cached = blendStamp | (nearestColours ? nearestColours[(r << 12) | (g << 6) | b] :
						         bestfit_color(_G(current_palette), r, g, b));
					}
					*destVal = cached;
				}
				continue;
			} else if ((DestBytesPerPixel == SrcBytesPerPixel) && args.srcAlpha == -1) {
				if (DestBytesPerPixel == 4)
					*(uint32 *)destVal = srcCol;
				else
					*(uint16 *)destVal = srcCol;
				continue;
			}

			// We need the rgb values to do blending and/or convert between formats
			if (SrcBytesPerPixel == 1) {
				const RGB &rgb = args.palette[srcCol];
				aSrc = 0xff;
				rSrc = rgb.r;
				gSrc = rgb.g;
				bSrc = rgb.b;
			} else {
				if (SrcBytesPerPixel == 4) {
					aSrc = srcCol >> 24;
					rSrc = (srcCol >> 16) & 0xff;
					gSrc = (srcCol >> 8) & 0xff;
					bSrc = srcCol & 0xff;
				} else { // SrcBytesPerPixel == 2
					aSrc = 0xff;
					rSrc = (srcCol >> 11) & 0x1f;
					rSrc = (rSrc << 3) | (rSrc >> 2);
					gSrc = (srcCol >> 5) & 0x3f;
					gSrc = (gSrc << 2) | (gSrc >> 4);
					bSrc = srcCol & 0x1f;
					bSrc = (bSrc << 3) | (bSrc >> 2);
				}
				//src.format.colorToARGB(srcCol, aSrc, rSrc, gSrc, bSrc);
			}

			if (args.srcAlpha == -1) {
				// This means we don't use blending.
				aDest = aSrc;
				rDest = rSrc;
				gDest = gSrc;
				bDest = bSrc;
			} else {
				if (args.useTint) {
					rDest = rSrc;
					gDest = gSrc;
					bDest = bSrc;
					aDest = aSrc;
					rSrc = args.tintRed;
					gSrc = args.tintGreen;
					bSrc = args.tintBlue;
					aSrc = args.srcAlpha;
				} else {
					uint32 destCol = getColor(destVal, DestBytesPerPixel);
					if (DestBytesPerPixel == 1) {
						const RGB &rgb = args.palette[destCol];
						aDest = 0xff;
						rDest = rgb.r;
						gDest = rgb.g;
						bDest = rgb.b;
					} else {
						if (DestBytesPerPixel == 4) {
							aDest = destCol >> 24;
							rDest = (destCol >> 16) & 0xff;
							gDest = (destCol >> 8) & 0xff;
							bDest = destCol & 0xff;
						} else { // DestBytesPerPixel == 2
							aDest = 0xff;
							rDest = (destCol >> 11) & 0x1f;
							rDest = (rDest << 3) | (rDest >> 2);
							gDest = (destCol >> 5) & 0x3f;
							gDest = (gDest << 2) | (gDest >> 4);
							bDest = destCol & 0x1f;
							bDest = (bDest << 3) | (bDest >> 2);
						}
						//src.format.colorToARGB(srcCol, aSrc, rSrc, gSrc, bSrc);
					}
				}
				blendPixel(aSrc, rSrc, gSrc, bSrc, aDest, rDest, gDest, bDest, args.srcAlpha, args.useTint, destVal);
			}

			uint32 pixel;// = format.ARGBToColor(aDest, rDest, gDest, bDest);
			if (DestBytesPerPixel == 4) {
				pixel = (aDest << 24) | (rDest << 16) | (gDest << 8) | (bDest);
				*(uint32 *)destVal = pixel;
			}
			else {
				pixel = ((rDest >> 3) << 11) | ((gDest >> 2) << 5) | (bDest >> 3);
				*(uint16 *)destVal = pixel;
			}
		}

		destP += args.destArea.pitch;
		if (!Scale) srcP += args.vertFlip ? -args.src.pitch : args.src.pitch;
	}
}

template<bool Scale>
void BITMAP::drawGeneric(DrawInnerArgs &args) {
	if (args.sameFormat) {
		switch (format.bytesPerPixel) {
		case 1: drawInnerGeneric<1, 1, Scale>(args); break;
		case 2: drawInnerGeneric<2, 2, Scale>(args); break;
		case 4: drawInnerGeneric<4, 4, Scale>(args); break;
		}
	} else if (format.bytesPerPixel == 4 && args.src.format.bytesPerPixel == 2) { 
		drawInnerGeneric<4, 2, Scale>(args);
	} else if (format.bytesPerPixel == 2 && args.src.format.bytesPerPixel == 4) {
		drawInnerGeneric<2, 4, Scale>(args);
	}
}

template void BITMAP::drawGeneric<false>(DrawInnerArgs &);
template void BITMAP::drawGeneric<true>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<4, 4, false>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<4, 4, true>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<4, 2, false>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<4, 2, true>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<2, 4, false>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<2, 4, true>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<4, 1, false>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<4, 1, true>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<2, 1, false>(DrawInnerArgs &);
template void BITMAP::drawInnerGeneric<2, 1, true>(DrawInnerArgs &);

} // end of namespace AGS3
