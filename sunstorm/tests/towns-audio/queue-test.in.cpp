#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#define SDL_VERSION_ATLEAST(a,b,c) 0
#define MAX(a,b) std::max(a,b)
using Uint32=uint32_t;using uint64=uint64_t;
static Uint32 tick=0,queuedBytes=0,mixCost=20,calls=0;
static Uint32 SDL_GetTicks(){return tick;}
static Uint32 SDL_GetQueuedAudioSize(int){return queuedBytes;}
static int SDL_GetAudioDeviceStatus(int){return 1;}
static int SDL_QueueAudio(int,void*,int n){queuedBytes+=n;return 0;}
static const char *SDL_GetError(){return "stub";}
static void warning(const char*,...){abort();}
struct SdlMixerManager{
 bool _queueMode=true,_isAudioOpen=true;int _queueDevId=1;
 char storage[4096];char *_queueBuf=storage;int _queueBufSize=4096;
 struct {int freq=22050,channels=2;} _obtained;
 void callbackHandler(char*,int){calls++;tick+=mixCost;Uint32 consumed=88200*mixCost/1000;queuedBytes=queuedBytes>consumed?queuedBytes-consumed:0;}
 void pump();
};
// PRODUCTION_PUMP

int main(){
 SdlMixerManager m;setenv("SCUMM_AUDIO_QUEUE_DEBUG","0",1);
 setenv("SCUMM_AUDIO_QUEUE_MS","50",1);queuedBytes=1000;m.pump();assert(queuedBytes>=4410&&calls==2);
 setenv("SCUMM_AUDIO_QUEUE_MS","150",1);queuedBytes=3000;calls=0;m.pump();assert(queuedBytes>=13230&&calls==5);
 setenv("SCUMM_AUDIO_QUEUE_MS","100",1);queuedBytes=1000;calls=0;m.pump();assert(queuedBytes>=8820&&calls<=8);
 mixCost=60;queuedBytes=0;calls=0;m.pump();assert(calls==8);
 m._queueMode=false;calls=0;m.pump();assert(calls==0);
 puts("QUEUE_REFILL_PASS: consumption during mixing, live target changes, bounded slow producer, disabled queue");
}
