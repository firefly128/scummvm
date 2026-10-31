// Native indexed copy path. Partial opacity continues through RGB blending.
#ifndef AGS_INDEXED_BLIT_H
#define AGS_INDEXED_BLIT_H
#include "common/scummsys.h"
// The native SPARC renderer benefits from compact loops with their own register window.
#if defined(SPARCFB_NATIVE_BACKEND) && defined(__GNUC__) && !defined(__clang__)
#define SST_INDEXED_LOOP_ATTR __attribute__((noinline, optimize("no-unroll-loops")))
#else
#define SST_INDEXED_LOOP_ATTR
#endif
namespace AGS3 {
SST_INDEXED_LOOP_ATTR inline void copyIndexedRows(byte *dest, int destPitch, const byte *source,
                           int sourcePitch, int width, int height,
                           bool masked, byte mask) {
 for (int y = 0; y < height; ++y) {
  const uintptr_t d = reinterpret_cast<uintptr_t>(dest);
  const uintptr_t s = reinterpret_cast<uintptr_t>(source);
  if (!masked && (d + width <= s || s + width <= d)) {
   memcpy(dest, source, width);
  } else {
#if defined(SPARCFB_NATIVE_BACKEND) && (defined(__GNUC__) || defined(__clang__))
   if (masked && ((d | s) & 3) == 0 && (d + width <= s || s + width <= d)) {
    // Aligned non-overlapping rows may copy four opaque indices together.
    const byte *alignedSource = static_cast<const byte *>(__builtin_assume_aligned(source, 4));
    byte *alignedDest = static_cast<byte *>(__builtin_assume_aligned(dest, 4));
    const uint32_t masks = uint32_t(mask) * 0x01010101u;
    int x = 0;
    for (; x + 4 <= width; x += 4) {
     uint32_t word;
     memcpy(&word, alignedSource + x, 4);
     const uint32_t changed = word ^ masks;
     if (!changed) continue;
     if (((changed - 0x01010101u) & ~changed & 0x80808080u) == 0) {
      memcpy(alignedDest + x, &word, 4);
     } else {
      for (int k = 0; k < 4; ++k)
       if (source[x + k] != mask) dest[x + k] = source[x + k];
     }
    }
    for (; x < width; ++x)
     if (source[x] != mask) dest[x] = source[x];
   } else
#endif
   // Preserve the generic loop's forward-copy semantics for overlapping rows.
   for (int x = 0; x < width; ++x)
    if (!masked || source[x] != mask) dest[x] = source[x];
  }
  dest += destPitch;
  source += sourcePitch;
 }
}
}
#undef SST_INDEXED_LOOP_ATTR
#endif
