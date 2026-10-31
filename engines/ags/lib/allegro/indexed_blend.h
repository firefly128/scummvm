#ifndef AGS_INDEXED_BLEND_H
#define AGS_INDEXED_BLEND_H
#include "common/scummsys.h"
// The native SPARC renderer benefits from compact loops with their own register window.
#if defined(SPARCFB_NATIVE_BACKEND) && defined(__GNUC__) && !defined(__clang__)
#define SST_INDEXED_LOOP_ATTR __attribute__((noinline, optimize("no-unroll-loops")))
#else
#define SST_INDEXED_LOOP_ATTR
#endif
namespace AGS3 {
template<bool Masked,class Compute>
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void blendIndexedPixel(byte *d,byte sc,byte mask,uint16 *table,uint16 stamp,const Compute &compute) {
 if(Masked && sc==mask)return;
 const byte dc=*d;
 uint16 &cached=table[(unsigned(sc)<<8)|dc];
 if((cached&0xff00)!=stamp)cached=stamp|compute(sc,dc);
 *d=static_cast<byte>(cached);
}

// Pass geometry and mask by value so pixel writes cannot alias drawing arguments.
template<bool Masked, bool SkipWords=false, class Compute>
SST_INDEXED_LOOP_ATTR inline void blendIndexedRows(byte *dest, int destPitch, const byte *source,
                             int sourcePitch, int sourceXStep, int width, int height,
                             byte mask, uint16 *table, uint16 stamp, const Compute &compute) {
 if(SkipWords && Masked && sourceXStep==1) {
  const uint32 maskWord=uint32(mask)*0x01010101U;
  for(int y=0;y<height;++y) {
   const byte *s=source;byte *d=dest;int x=0;
   while(x<width && (reinterpret_cast<uintptr>(s)&3U)) {
    blendIndexedPixel<Masked>(d,*s,mask,table,stamp,compute);++x;++s;++d;
   }
   for(;x+4<=width;x+=4,s+=4,d+=4) {
    uint32 word;
#if defined(__GNUC__)
    const void *aligned=__builtin_assume_aligned(s,4);
#else
    const void *aligned=s;
#endif
    memcpy(&word,aligned,4);
    if(word==maskWord)continue;
    blendIndexedPixel<Masked>(d,s[0],mask,table,stamp,compute);
    blendIndexedPixel<Masked>(d+1,s[1],mask,table,stamp,compute);
    blendIndexedPixel<Masked>(d+2,s[2],mask,table,stamp,compute);
    blendIndexedPixel<Masked>(d+3,s[3],mask,table,stamp,compute);
   }
   for(;x<width;++x,++s,++d)blendIndexedPixel<Masked>(d,*s,mask,table,stamp,compute);
   source+=sourcePitch;dest+=destPitch;
  }
  return;
 }

 for (int y = 0; y < height; ++y) {
  const byte *s = source;
  byte *d = dest;
  for (int x = 0; x < width; ++x, ++d, s += sourceXStep) {
   const byte sc = *s;
   if (Masked && sc == mask) continue;
   const byte dc = *d;
   uint16 &cached = table[(unsigned(sc) << 8) | dc];
   if ((cached & 0xff00) != stamp) cached = stamp | compute(sc, dc);
   *d = static_cast<byte>(cached);
  }
  dest += destPitch;
  source += sourcePitch;
 }
}
}
#undef SST_INDEXED_LOOP_ATTR
#endif
