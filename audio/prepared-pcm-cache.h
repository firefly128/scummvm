#ifndef AUDIO_PREPARED_PCM_CACHE_H
#define AUDIO_PREPARED_PCM_CACHE_H
#include "common/scummsys.h"
#include "common/array.h"
#include "common/str.h"
namespace Audio {
// Engine-independent prepared signed-PCM contributions; game clock/state stay with the caller.
class PreparedPCMCache {
 struct Entry { byte key[44]; Common::Array<int32> wave; };
 struct Transform { bool valid; uint32 params[4]; int32 wave[512]; };
 Transform _transforms[8];
 bool _valueMode;uint32 _idleHits,_idleVerified,_builds;
 Common::Array<Entry> _entries;
 Common::Array<Common::String> _requested;
 Common::String _directory;
 byte _lastMiss[44]; bool _haveMiss,_trace,_verify;
 uint32 _bytes,_hits,_misses,_verified;uint64 _frames;
 void loadIndex();
public:
 PreparedPCMCache();
 bool trace()const{return _trace;}
 bool valueMode()const{return _valueMode;}
 bool enabled()const{return _valueMode||!_directory.empty();}
 const int32 *lookupValues(int voice,uint32 volume,uint32 left,uint32 right,uint32 gain);
 bool verifyIdle()const{return _verify&&_idleVerified<512;}
 void accountIdle(uint32 frames,bool verified);
 const int32 *lookup(const byte digest[16],const int8 *data,uint32 length,uint32 volume,uint32 left,uint32 right,uint32 gain,uint32 deviceVolume);
 bool verify()const{return _verify&&_verified<2048;}
 void account(bool hit,uint32 frames,bool verified);
};
}
#endif
