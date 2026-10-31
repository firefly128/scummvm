#ifndef SST_SPARCFB_DIRTY_COPY_H
#define SST_SPARCFB_DIRTY_COPY_H
#include "common/scummsys.h"
namespace SparcFbDirty {
inline bool equalRow(const byte *source, const byte *dest, int width) {
#if defined(SPARCFB_NATIVE_BACKEND) && (defined(__GNUC__) || defined(__clang__))
 if (width == 16 && ((reinterpret_cast<uintptr_t>(source) | reinterpret_cast<uintptr_t>(dest)) & 3) == 0) {
  const byte *s = static_cast<const byte *>(__builtin_assume_aligned(source, 4));
  const byte *d = static_cast<const byte *>(__builtin_assume_aligned(dest, 4));
  uint32_t difference = 0;
  for (int x = 0; x < 16; x += 4) {
   uint32_t left, right;
   memcpy(&left, s + x, 4); memcpy(&right, d + x, 4);
   difference |= left ^ right;
  }
  return difference == 0;
 }
#endif
 return memcmp(source, dest, width) == 0;
}
// Compare only the submitted rectangle. Existing dirty flags are retained.
// Returns whether any data needs copying; the caller can then do one linear
// copy per row instead of thousands of small tile copies on animated screens.
inline bool compare(const byte *source, int sourcePitch, const byte *dest,
                    int destPitch, int x, int y, int width, int height,
                    byte *tiles, int tilesX, int tileWidth, int tileHeight) {
 if (width <= 0 || height <= 0) return false;
 bool changed = false;
 const int right = x + width, bottom = y + height;
 for (int ty = y / tileHeight; ty <= (bottom - 1) / tileHeight; ++ty) {
  for (int tx = x / tileWidth; tx <= (right - 1) / tileWidth; ++tx) {
   byte &dirty = tiles[ty * tilesX + tx];
   if (dirty) { changed = true; continue; }
   const int left = x > tx * tileWidth ? x : tx * tileWidth;
   const int endX = right < (tx + 1) * tileWidth ? right : (tx + 1) * tileWidth;
   const int top = y > ty * tileHeight ? y : ty * tileHeight;
   const int endY = bottom < (ty + 1) * tileHeight ? bottom : (ty + 1) * tileHeight;
   for (int row = top; row < endY; ++row) {
    if (!equalRow(source + (row-y) * sourcePitch + left-x,
               dest + row * destPitch + left, endX-left)) {
     dirty = 1; changed = true; break;
    }
   }
  }
 }
 return changed;
}
}
#endif
