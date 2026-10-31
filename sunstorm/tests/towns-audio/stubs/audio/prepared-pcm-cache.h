#pragma once
#include <vector>
#include <deque>
#include "common/scummsys.h"
namespace Audio{
extern uint64 verifiedCount,hitCount,missCount,idleCount;
class PreparedPCMCache{std::deque<std::vector<int32>> waves;public:bool valueMode()const{return getenv("PCM_TEST_BANK")==nullptr;}bool verifyIdle()const{return true;}void accountIdle(uint32,bool){idleCount++;}bool trace()const{return false;}bool enabled()const{return true;}bool verify()const{return true;}
const int32*lookupValues(int voice,uint32 v,uint32 l,uint32 r,uint32 g){static int32 vals[8][512];for(int i=0;i<256;i++){int sample=(int8)i;vals[voice][2*i]=((((sample*(int)v)*(int)l)>>3)*(int)g/256);vals[voice][2*i+1]=((((sample*(int)v)*(int)r)>>3)*(int)g/256);}return vals[voice];}
const int32*lookup(const byte*,const int8*d,uint32 n,uint32 v,uint32 l,uint32 r,uint32 g,uint32 device){waves.push_back(std::vector<int32>(n*2));std::vector<int32>&wave=waves.back();for(uint32 i=0;i<n;i++){wave[i*2]=((((d[i]*(int)v)*(int)l)>>3)*(int)g/256);wave[i*2+1]=((((d[i]*(int)v)*(int)r)>>3)*(int)g/256);}return wave.data();}
void account(bool h,uint32,bool v){if(h)hitCount++;else missCount++;if(v)verifiedCount++;}
};}
