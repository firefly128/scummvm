#!/bin/bash
set -eu
cp /sun-mad/mad.h /opt/sysroot/opt/sst/include/
cp /sun-mad/libmad.so.0.2.1 /opt/sysroot/opt/sst/lib/
ln -sf libmad.so.0.2.1 /opt/sysroot/opt/sst/lib/libmad.so
ln -sf libmad.so.0.2.1 /opt/sysroot/opt/sst/lib/libmad.so.0
cd /migration/source
export CXX=/opt/cross/bin/sparc-sun-solaris2.7-g++
export CXXFLAGS='--sysroot=/opt/sysroot -mcpu=v8 -mno-fsmuld -O2 --param l1-cache-size=8 --param l1-cache-line-size=16 --param max-inline-insns-auto=40 --param max-inline-insns-single=200 --param max-unroll-times=4 --param max-unrolled-insns=100 -fschedule-insns -fschedule-insns2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fno-delete-null-pointer-checks -D__EXTENSIONS__ -Wno-implicit-int -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -std=gnu17 -I/opt/sysroot/opt/sst/include -D_POSIX_PTHREAD_SEMANTICS -I/opt/sysroot/opt/sst/include/freetype2 -fpermissive -U__STRICT_ANSI__ -fPIC -DNDEBUG -DSST_PROFILE_WFTLOOP -DSST_DELTACAP -DSST_GETMILLIS_SCALE -DSST_PROFILE_SURFACE -DSST_PROFILE_BYPASS -DSPARCFB_NATIVE_BACKEND -DUSE_BPPMIDI -funroll-loops -fprefetch-loop-arrays -fmerge-all-constants --param max-unroll-times=8'
export LDFLAGS='--sysroot=/opt/sysroot -L/opt/sysroot/opt/sst/lib -L/opt/sysroot/usr/lib -R/opt/sst/lib -Wl,-rpath-link,/opt/cross/sparc-sun-solaris2.7/lib -Wl,-rpath-link,/opt/sysroot/usr/lib -Wl,-rpath-link,/opt/sysroot/opt/sst/lib -Wl,-z,relro -Wl,-z,text -lsparc_fb -lrt -lsolcompat -ldl -lresolv -lnsl -lsocket -lgcc'
export PKG_CONFIG_LIBDIR=/opt/sysroot/opt/sst/lib/pkgconfig:/opt/sysroot/opt/sst/share/pkgconfig:/opt/sysroot/usr/lib/pkgconfig
export PATH=/opt/cross/bin:$PATH
./configure --host=sparc-sun-solaris2.7 --prefix=/opt/sst --with-sdl-prefix=/opt/sysroot/opt/sst --disable-all-engines --enable-engine=scumm,sci,sci32,agi,dgds,mm,mm1,xeen,ultima,akalabeth,ultima1,ultima4,ultima6,ultima8,kyra,lol,eob,ags --opengl-mode=none --disable-opengl-game --enable-release --disable-tts --disable-cloud --disable-seq-midi --disable-imgui --disable-detection-full --disable-tinygl --enable-freetype2 --enable-ogg --enable-vorbis --enable-mad --disable-tremor --with-freetype2-prefix=/opt/sysroot/opt/sst --with-png-prefix=/opt/sysroot/opt/sst --with-jpeg-prefix=/opt/sysroot/opt/sst --with-zlib-prefix=/opt/sysroot/opt/sst --with-mad-prefix=/opt/sysroot/opt/sst --with-ogg-prefix=/opt/sysroot/opt/sst --with-vorbis-prefix=/opt/sysroot/opt/sst > /migration/configure.log 2>&1
make -j8 > /migration/build.log 2>&1
cp scummvm /migration/scummvm-2026.4-sunstorm
/opt/cross/bin/sparc-sun-solaris2.7-strip /migration/scummvm-2026.4-sunstorm
