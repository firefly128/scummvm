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

#include "ags/shared/ac/common.h"
#include "ags/engine/ac/draw.h"
#include "ags/shared/ac/game_setup_struct.h"
#include "ags/engine/ac/sprite.h"
#include "ags/engine/ac/system.h"
#include "ags/engine/platform/base/ags_platform_driver.h"
#include "ags/plugins/ags_plugin_evts.h"
#include "ags/plugins/plugin_engine.h"
#include "ags/shared/gfx/bitmap.h"
#include "ags/shared/ac/sst_asset_cache.h"
#include "ags/shared/ac/sst_palette_gen.h"
#include "ags/engine/gfx/graphics_driver.h"

namespace AGS3 {

using namespace AGS::Shared;
using namespace AGS::Engine;

Size get_new_size_for_sprite(const Size &size, const uint32_t sprite_flags) {
	if (!_GP(game).AllowRelativeRes() || ((sprite_flags & SPF_VAR_RESOLUTION) == 0))
		return size;
	Size newsz = size;
	ctx_data_to_game_size(newsz.Width, newsz.Height, ((sprite_flags & SPF_HIRES) != 0));
	return newsz;
}

// from is a 32-bit RGBA image, to is a 15/16/24-bit destination image
Bitmap *remove_alpha_channel(Bitmap *from) {
	const int game_cd = _GP(game).GetColorDepth();
	Bitmap *to = BitmapHelper::CreateBitmap(from->GetWidth(), from->GetHeight(), game_cd);
	const int maskcol = to->GetMaskColor();
	int y, x;
	unsigned int c, b, g, r;

	if (game_cd == 24) { // 32-to-24
		for (y = 0; y < from->GetHeight(); y++) {
			unsigned int *psrc = (unsigned int *)from->GetScanLine(y);
			unsigned char *pdest = (unsigned char *)to->GetScanLine(y);

			for (x = 0; x < from->GetWidth(); x++) {
				c = psrc[x];
				// less than 50% opaque, remove the pixel
				if (((c >> 24) & 0x00ff) < 128)
					c = maskcol;

				// copy the RGB values across
				memcpy(&pdest[x * 3], &c, 3);
			}
		}
	} else if (game_cd > 8) { // 32 to 15 or 16
		for (y = 0; y < from->GetHeight(); y++) {
			unsigned int *psrc = (unsigned int *)from->GetScanLine(y);
			unsigned short *pdest = (unsigned short *)to->GetScanLine(y);

			for (x = 0; x < from->GetWidth(); x++) {
				c = psrc[x];
				// less than 50% opaque, remove the pixel
				if (((c >> 24) & 0x00ff) < 128)
					pdest[x] = maskcol;
				else {
					// otherwise, copy it across
					r = (c >> 16) & 0x00ff;
					g = (c >> 8) & 0x00ff;
					b = c & 0x00ff;
					pdest[x] = makecol_depth(game_cd, r, g, b);
				}
			}
		}
	} else { // 32 to 8-bit game
		// TODO: consider similar to above approach if this becomes a wanted feature
		to->Blit(from);
	}
	return to;
}

Bitmap *initialize_sprite(sprkey_t index, Bitmap *image, uint32_t &sprite_flags) {
	int oldeip = get_our_eip();
	set_our_eip(4300);

	if (sprite_flags & SPF_HADALPHACHANNEL) {
		// we stripped the alpha channel out last time, put
		// it back so that we can remove it properly again
		// CHECKME: find out what does this mean, and explain properly
		sprite_flags |= SPF_ALPHACHANNEL;
	}

	// stretch sprites to correct resolution
	Size newsz = get_new_size_for_sprite(image->GetSize(), sprite_flags);

	_G(eip_guinum) = index;
	_G(eip_guiobj) = newsz.Width;

	Bitmap *use_bmp = image;
	if (newsz != image->GetSize()) {
		use_bmp = new Bitmap(newsz.Width, newsz.Height, image->GetColorDepth());
		use_bmp->StretchBlt(image, RectWH(0, 0, use_bmp->GetWidth(), use_bmp->GetHeight()));
		delete image;
	}

	/* SST: when SSTPaletteGen is active (precompute sample pass), feed
	 * the raw high-bpp pixels into the palette histogram BEFORE
	 * PrepareSpriteForUse downgrades the bitmap to game depth.  No-op
	 * once sampling completes and palette generation has run. */
	if (AGS::Shared::SSTPaletteGen::IsActive() &&
	    use_bmp && use_bmp->GetColorDepth() > 8) {
		AGS::Shared::SSTPaletteGen::SamplePixels(use_bmp);
		/* This exclusive pass immediately discards the sprite; quantizing
		 * its full image against a temporary palette is wasted work. */
		set_our_eip(oldeip);
		return use_bmp;
	}

	use_bmp = PrepareSpriteForUse(use_bmp, (sprite_flags & SPF_ALPHACHANNEL) != 0);
	if (_GP(game).GetColorDepth() < 32) {
		sprite_flags &= ~SPF_ALPHACHANNEL;
		// save the fact that it had one for the next time this is re-loaded from disk
		// CHECKME: find out what does this mean, and explain properly
		sprite_flags |= SPF_HADALPHACHANNEL;
	}

	set_our_eip(oldeip);

	/* SST: after the full conversion pipeline (PrepareSpriteForUse +
	 * potential 32->8 Blit because we forced game.color_depth = 1),
	 * use_bmp is the final 8bpp bitmap that gets stored in the sprite
	 * cache.  Persist it to disk so the next launch can skip everything
	 * above.  We pass the *current* sprite_flags (post-conversion: the
	 * SPF_HADALPHACHANNEL bit reflects the original asset and is what
	 * we'll want back on cache hit). */
	if (use_bmp && use_bmp->GetColorDepth() == 8) {
		AGS::Shared::SSTAssetCache::Store(index, use_bmp, sprite_flags);
	}

	return use_bmp;
}

void post_init_sprite(sprkey_t index) {
	pl_run_plugin_hooks(AGSE_SPRITELOAD, index);
}

} // namespace AGS3
