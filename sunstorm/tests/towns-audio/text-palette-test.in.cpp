#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
using uint8=uint8_t;using uint16=uint16_t;using uint32=uint32_t;
struct Pal {uint8 colors[768]={};void grabPalette(uint8*out,int start,int num){memcpy(out,colors+start*3,num*3);}};
struct System {Pal p;Pal*getPaletteManager(){return &p;}};
struct TownsScreen {
 struct TownsScreenLayer {uint8*pixels=nullptr,*palette=nullptr;int pitch=0,width=0,height=0,bpp=1,numCol=256;uint16 hScroll=0;bool onBottom=true,ready=true;uint16*bltTmpPal=nullptr;uint8 indexedTextPal[16]={};} _layers[2];
 struct {int bytesPerPixel=1;} _pixelFormat;
 System *_system;bool _indexedTextPaletteValid=false;uint8 _indexedSystemPalette[768]={},_indexedTextPalette[48]={};
 bool updateIndexedTextPalette();
 template<typename D,typename S,int W,int H,bool C>void transferRect(uint8*,int,TownsScreenLayer*,int,int,int,int);
};
// PRODUCTION_TEXT_COMPOSITOR
int main(){System sys;TownsScreen t;t._system=&sys;uint8 textPal[48]={};textPal[45]=textPal[46]=textPal[47]=224;auto &l=t._layers[1];l.palette=textPal;l.numCol=16;sys.p.colors[600]=sys.p.colors[601]=sys.p.colors[602]=224;uint8 before[768];memcpy(before,sys.p.colors,768);assert(t.updateIndexedTextPalette()&&l.indexedTextPal[15]==200);assert(!memcmp(before,sys.p.colors,768));assert(!t.updateIndexedTextPalette());
uint8 bg[]={17,160,255},dst[12]={};auto &b=t._layers[0];b.pixels=bg;b.pitch=b.width=3;b.height=1;t.transferRect<uint8,uint8,1,1,true>(dst,3,&b,0,0,2,0);assert(dst[0]==17&&dst[1]==160&&dst[2]==255);
uint8 txt[]={0,31,16};l.pixels=txt;l.pitch=l.width=3;l.height=1;l.onBottom=false;t.transferRect<uint8,uint8,1,1,true>(dst,3,&l,0,0,2,0);assert(dst[0]==17&&dst[1]==200&&dst[2]==0);
memset(dst,0,sizeof(dst));t.transferRect<uint8,uint8,2,2,false>(dst,6,&b,0,0,2,0);uint8 expected[]={17,17,160,160,255,255,17,17,160,160,255,255};assert(!memcmp(dst,expected,12));b.hScroll=2;t.transferRect<uint8,uint8,1,1,true>(dst,3,&b,0,0,2,0);assert(dst[0]==255&&dst[1]==17&&dst[2]==160);
sys.p.colors[600]=sys.p.colors[601]=sys.p.colors[602]=0;sys.p.colors[603]=sys.p.colors[604]=sys.p.colors[605]=224;assert(t.updateIndexedTextPalette()&&l.indexedTextPal[15]==201);puts("TEXT_PALETTE_PASS: color flags, separate palette, transparent/opaque black, full background indices, scaling, scroll, palette changes");}
