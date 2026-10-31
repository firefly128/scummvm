/* SST AGS asset disk cache.
 *
 * Persists quantized 8-bit copies of AGS sprites to <game-dir>/_quantized/
 * keyed by sprite index.  On a fresh install the first launch quantizes
 * every >8bpp sprite and writes it out; subsequent launches mmap the
 * cached bytes back instead of re-running the 32->8 Allegro bestfit
 * pass.  The cube palette is installed game-wide so AGS's own
 * 32->8 Blit path produces the same bytes we want to cache.
 *
 * Cache file layout (little-endian, naturally aligned):
 *   u32 magic   = 'S','S','A','Q' (0x51415353)
 *   u32 version = 1
 *   u32 width
 *   u32 height
 *   u32 sprite_flags  (SPF_* from AGS, post-conversion)
 *   u32 pixel_bytes   (width * height for 8bpp)
 *   u8  pixels[pixel_bytes]
 *
 * One file per sprite index: <game-dir>/_quantized/NNNNNN.pal8
 * (zero-padded six-digit decimal, ASCII).
 */

#ifndef AGS_SHARED_AC_SST_ASSET_CACHE_H
#define AGS_SHARED_AC_SST_ASSET_CACHE_H

#include "ags/shared/core/types.h"

namespace AGS3 {
namespace AGS {
namespace Shared {

class Bitmap;

namespace SSTAssetCache {

/* Initializes the on-disk cache directory in the user cache.  Idempotent;
 * safe to call once at engine startup.  Returns true if the directory is
 * usable (exists / was created); false disables the cache (engine falls
 * through to runtime conversion). */
bool Init();
/* Original high-colour games need a fixed game-tuned palette on TCX. */
void SetPaletteConversionRequired(bool required);
bool RequiresPaletteConversion();

/* True if Init() succeeded and we have a writable cache dir. */
bool IsEnabled();

/* Try to load sprite "index" from disk.  Returns a freshly-allocated
 * Bitmap on hit (caller owns it); nullptr on miss or read error.
 * sprite_flags is filled from the cache file on hit. */
Bitmap *TryLoad(int32 index, uint32 *sprite_flags_out);

/* Write sprite "index" to disk.  No-op if already present.  Errors are
 * logged but non-fatal (engine continues with the in-memory bitmap). */
void Store(int32 index, const Bitmap *image, uint32 sprite_flags);

/* True if sprite "index" has a cache file on disk.  Used by the
 * precompute dialog to count outstanding work. */
bool HasEntry(int32 index);
/* Discard derived pixels when rebuilding their palette. */
void InvalidateEntry(int32 index);

/* When set, Store() becomes a no-op.  Used by the precompute palette
 * generation pass to avoid writing .pal8 files quantized against the
 * wrong (default cube) palette before the real game-tuned palette has
 * been generated. */
void SetStoreSuppressed(bool on);

/* Save/load a 256-colour 768-byte RGB palette as the graphics cache palette file.
 * Returns true on success.  LoadPalette returns false if the file is
 * absent or malformed (caller should fall back to cube). */
bool SavePalette(const byte *rgb768);
bool LoadPalette(byte *rgb768);
bool HasPaletteFile();
/* Build the exact blend lookup natively, with progress, when absent. */
bool EnsureColourLookup();

// Exact nearest-colour map generated offline for this 6-bit RGB palette.
// Returns null on missing, corrupt or mismatched data (normal search fallback).
const byte *GetColourLookup(const byte *palette6);

} // namespace SSTAssetCache
} // namespace Shared
} // namespace AGS
} // namespace AGS3

#endif
