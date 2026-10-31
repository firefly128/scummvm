#ifndef SST_SPARCFB_BLIT_PLAN_H
#define SST_SPARCFB_BLIT_PLAN_H
#include "sparcfb-triplets.h"

namespace SparcFbBlit {
// Scale only dirty horizontal runs. A zero source stride in the driver
// repeats the staged row vertically, avoiding extra RAM copies.
template<class Copy>
inline void directRow(const unsigned char *dirty, int firstTile, int tilesX,
                      int tileWidth, int nativeWidth, int scale,
                      const unsigned char *source, unsigned char *scratch, bool packed,
                      const Copy &copy) {
 int tile=firstTile;
 while (tile<tilesX) {
  while (tile<tilesX && !dirty[tile]) ++tile;
  if (tile==tilesX) break;
  const int first=tile;
  while (tile<tilesX && dirty[tile]) ++tile;
  const int left=first*tileWidth;
  const int right=(tile*tileWidth<nativeWidth) ? tile*tileWidth : nativeWidth;
  if (right<=left) continue;
  if (scale==1) { copy(left,source+left,right-left); continue; }
  if (scale==3 && packed) {
   SparcFbScale::triple(source+left,scratch+left*3,right-left);
   copy(left*3,scratch+left*3,(right-left)*3);
   continue;
  }
  for (int x=left; x<right; ++x) {
   const unsigned char p=source[x];
   if (scale==3) { scratch[x*3]=p; scratch[x*3+1]=p; scratch[x*3+2]=p; }
   else if (scale==2) { scratch[x*2]=p; scratch[x*2+1]=p; }
   else for (int sx=0;sx<scale;++sx) scratch[x*scale+sx]=p;
  }
  copy(left*scale,scratch+left*scale,(right-left)*scale);
 }
}

// Coalesce only adjacent dirty tiles. Short strips retain the audio yield
// cadence. The caller uses single-row strips for unaligned driver strides.
template<class Copy, class Yield>
inline void tileRow(const unsigned char *dirty, int tilesX, int tileWidth,
                    int nativeWidth, int scale, int firstRow, int endRow,
                    int stripRows, const Copy &copy, const Yield &yield) {
 for (int row=firstRow; row<endRow; row+=stripRows) {
  const int height=(endRow-row<stripRows) ? endRow-row : stripRows;
  int tile=0;
  while (tile<tilesX) {
   while (tile<tilesX && !dirty[tile]) ++tile;
   if (tile==tilesX) break;
   const int first=tile;
   while (tile<tilesX && dirty[tile]) ++tile;
   const int left=first*tileWidth;
   const int right=(tile*tileWidth<nativeWidth) ? tile*tileWidth : nativeWidth;
   if (right>left) copy(left*scale,row,(right-left)*scale,height);
  }
  if (stripRows>1 || (row&7)==0) yield();
 }
}
}
#endif
