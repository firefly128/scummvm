#include <cstdint>
#include <cassert>
#include <cstdio>
using int16=int16_t;using int32=int32_t;
template<typename T>T CLIP(T v,T lo,T hi){return v<lo?lo:(v>hi?hi:v);}
namespace Audio {struct AudioStream {virtual ~AudioStream(){}virtual int readBuffer(int16*,int)=0;virtual bool isStereo()const=0;virtual int getRate()const=0;virtual bool endOfData()const=0;virtual bool endOfStream()const=0;};struct Mixer {enum Type{kMusicSoundType,kSFXSoundType};int music=64,sfx=256;bool musicMute=false,sfxMute=false;bool isSoundTypeMuted(Type t){return t==kMusicSoundType?musicMute:sfxMute;}int getVolumeForSoundType(Type t){return t==kMusicSoundType?music:sfx;}};}
struct Source:Audio::AudioStream {int16 value=4000;int readBuffer(int16*p,int n){for(int i=0;i<n;i++)p[i]=value;return n;}bool isStereo()const{return true;}int getRate()const{return 22050;}bool endOfData()const{return false;}bool endOfStream()const{return false;}};
// PRODUCTION_EUPHONY_STREAM
int main(){Audio::Mixer m;Source*s=new Source;EuphonyPCMStream stream(s,&m);int16 b[4];stream.readBuffer(b,4);assert(b[0]==4000);m.music=128;stream.readBuffer(b,4);assert(b[0]==8000);m.sfx=0;m.sfxMute=true;stream.readBuffer(b,4);assert(b[0]==8000);m.musicMute=true;stream.readBuffer(b,4);assert(b[0]==0);m.musicMute=false;m.music=256;s->value=16000;stream.readBuffer(b,4);assert(b[0]==32767);assert(stream.isStereo()&&stream.getRate()==22050);puts("MUSIC_VOLUME_PASS: music slider, independent effects control, music mute, clipping, stream format");}
