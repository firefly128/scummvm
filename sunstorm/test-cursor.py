from pathlib import Path
import subprocess
w=Path(__file__).resolve().parent;s=(w.parent/'backends/graphics/surfacesdl/sparcfb-graphics-noX.cpp').read_text();a=s.index('void SparcFbGraphicsManager::compositeNativeCursor()');b=s.index('\n/* ===',a);fn=s[a:b]
h='''#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>
using byte=uint8_t;using uint32=uint32_t;using int64=int64_t;using frac_t=int32_t;
const int FRAC_ONE=65536;
class SparcFbGraphicsManager {public:
bool _native_cursor_visible=true;byte *_native_cursor_pixels,*_scaled_pixels;int _native_cursor_w=2,_native_cursor_h=2,_native_cursor_hotx=1,_native_cursor_hoty=1,_native_mouse_x=4,_native_mouse_y=4,_scale_factor=2,_lb_top_applied=0,_scaled_height=20,_scaled_width=20,_scaled_pitch=20;uint32 _native_cursor_keycolor=0;frac_t _native_cursor_scale_x=FRAC_ONE,_native_cursor_scale_y=FRAC_ONE;void compositeNativeCursor();};
'''
t='''
int main(){
byte cursor[]={1,2,0,3};byte pixels[400];SparcFbGraphicsManager m;m._native_cursor_pixels=cursor;m._scaled_pixels=pixels;
// Explicit expected pixel blocks: 2x game cursor and transparent lower left.
memset(pixels,9,400);m.compositeNativeCursor();assert(pixels[6*20+6]==1&&pixels[7*20+7]==1&&pixels[6*20+8]==2&&pixels[8*20+6]==9&&pixels[9*20+9]==3&&pixels[5*20+6]==9);
// Scale zero means physical-size cursor, independently on each axis.
memset(pixels,9,400);m._native_cursor_scale_x=0;m._native_cursor_scale_y=0;m.compositeNativeCursor();assert(pixels[7*20+7]==1&&pixels[7*20+8]==2&&pixels[8*20+7]==9&&pixels[8*20+8]==3&&pixels[9*20+8]==9);
memset(pixels,9,400);m._native_cursor_scale_x=FRAC_ONE;m._native_cursor_scale_y=0;m.compositeNativeCursor();assert(pixels[7*20+6]==1&&pixels[7*20+9]==2&&pixels[8*20+6]==9&&pixels[8*20+9]==3);
// Letterbox adjustment and clipping at the physical top-left.
memset(pixels,9,400);m._native_cursor_scale_y=FRAC_ONE;m._native_mouse_x=0;m._native_mouse_y=2;m._lb_top_applied=2;m.compositeNativeCursor();assert(pixels[0]==3&&pixels[21]==3&&pixels[2]==9);
printf("CURSOR_PASS unity, physical-size, anisotropic, transparency, clipping, letterbox\\n");
}
'''
(w/'cursor-test.cpp').write_text(h+fn+t)
subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-g',str(w/'cursor-test.cpp'),'-o',str(w/'cursor-test')],check=True);subprocess.run([str(w/'cursor-test')],check=True)
