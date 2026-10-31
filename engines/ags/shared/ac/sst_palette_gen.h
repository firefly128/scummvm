/* SST auto palette generator — sample raw sprite pixels, build a 32K-bucket
 * 5-bit-per-channel RGB histogram, then pick the top 239 most-frequent
 * colours and append a 16-entry grayscale ramp.  Result is a 256-colour
 * palette tuned to the specific game's actual art (skin/earth tones for
 * adventure games, vivid for action games, etc.).  Vastly better than a
 * uniform RGB cube for any specific game.
 *
 * One-shot at first launch via the precompute pass; saved to
 * <game-dir>/_palette.bin and loaded on every subsequent launch. */

#ifndef AGS_SHARED_SST_PALETTE_GEN_H
#define AGS_SHARED_SST_PALETTE_GEN_H

#include "common/scummsys.h"
#include "ags/lib/allegro/color.h"   /* for the AGS3::color type used below */

namespace AGS3 {
namespace AGS {
namespace Shared {
class Bitmap;

namespace SSTPaletteGen {

/* Reset the histogram (call before starting a sample pass). */
void Reset();

/* Activate sampling — when on, the initialize_sprite hook in
 * sprite.cpp feeds each raw 32-bit-or-better sprite into SamplePixels.
 * Defaults off. */
void SetActive(bool on);
bool IsActive();

/* Sample up to ~100 representative pixels from src into the histogram.
 * src must be >1bpp (32-bit or 16-bit); 8-bit bitmaps are skipped
 * (they're already quantized). */
void SamplePixels(const Bitmap *src);

/* Build the 256-colour palette from the histogram and write it as 768
 * bytes (256 entries × R,G,B) into out_rgb.  Slot 0 is always pure
 * black (mask colour); slots 240..255 are a uniform grayscale ramp; the
 * remaining 239 slots are the top-frequency colours from sampling. */
void Generate(byte *out_rgb);

/* Diagnostics. */
int UniqueColoursSeen();
uint32 TotalPixelsSampled();

/* Walk every room file in the game asset bundle, load each one just long
 * enough to read its background bitmap(s), and feed those raw pixels
 * into SamplePixels.  Returns the number of room files visited.  Must
 * be called only when sampling is active (SetActive(true)). */
int SampleAllRoomBackgrounds();
/* Incremental first-run preparation: call at most once per GUI tick. */
int RoomBackgroundSlotCount();
bool SampleRoomBackground(int room_id);

/* Translate an 8-bit room background bitmap from its ROOM-LOCAL palette
 * indices to indices against the active global palette
 * (`_G(current_palette)`, our auto-tuned palette).
 *
 * Why: AGS room files ship background art quantized against a room-
 * specific 256-colour palette stored in the .crm.  When AGS runs
 * natively it would call set_palette(roomPalette) on room enter and
 * the bg's indices would resolve correctly against the just-installed
 * palette.  Our SPARCFB_CUBE_PALETTE mode holds the global palette
 * fixed — without this translation step the bg's bytes get rendered
 * via the global palette and every byte points to some unrelated
 * colour (typically collapsing the whole bg into one near-uniform
 * dim tone). */
void TranslateRoomBgPalette(Bitmap *bg, const ::AGS3::color *src_palette);

} // namespace SSTPaletteGen
} // namespace Shared
} // namespace AGS
} // namespace AGS3

#endif
