#define FORBIDDEN_SYMBOL_EXCEPTION_getenv
#include <stdlib.h>
#include "audio/prepared-pcm-cache.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/memstream.h"
#include "common/md5.h"
#include "common/endian.h"
#include "common/textconsole.h"
namespace Audio {
PreparedPCMCache::PreparedPCMCache():_valueMode(false),_idleHits(0),_idleVerified(0),_builds(0),_haveMiss(false),_trace(false),_verify(false),_bytes(0),_hits(0),_misses(0),_verified(0),_frames(0){
 for(int i=0;i<8;i++)_transforms[i].valid=false;
 _valueMode=getenv("SST_PCM_TRANSFORM_CACHE")!=nullptr;_verify=getenv("SST_PREPARED_PCM_VERIFY")!=nullptr;
 if(_valueMode){warning("PCM_TRANSFORM ready bytes=%u verify=%d",(uint)sizeof(_transforms),_verify);return;}
 const char *p=getenv("SST_PREPARED_PCM_DIR");if(!p||!*p)return;
 _directory=p;_trace=getenv("SST_PREPARED_PCM_TRACE")!=nullptr;_verify=getenv("SST_PREPARED_PCM_VERIFY")!=nullptr;
 loadIndex();warning("PREPARED_PCM ready entries=%u bytes=%u trace=%d verify=%d",_entries.size(),_bytes,_trace,_verify);
}
void PreparedPCMCache::loadIndex(){
 Common::File index;if(!index.open(Common::FSNode(Common::Path(_directory+"/index.txt"))))return;
 while(!index.eos()){
  Common::String name=index.readLine();name.trim();if(name.size()!=32)continue;
  bool valid=true;for(uint i=0;i<name.size();i++)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))valid=false;
  if(!valid)continue;
  Common::File f;if(!f.open(Common::FSNode(Common::Path(_directory+"/"+name+".bank"))))continue;
  byte magic[8],payloadDigest[16];Entry e;
  if(f.read(magic,8)!=8||memcmp(magic,"SSTPCMB1",8)||f.read(e.key,44)!=44||f.read(payloadDigest,16)!=16)continue;
  uint32 length=READ_BE_UINT32(e.key+16);
  if(!length||length>65536||READ_BE_UINT32(e.key+40)!=2||f.size()!=68+length*8||_bytes+length*8>4*1024*1024)continue;
  e.wave.resize(length*2);if(f.read(&e.wave[0],length*8)!=length*8)continue;
  byte digest[16];Common::MemoryReadStream source((const byte*)&e.wave[0],length*8);Common::computeStreamMD5(source,digest);
  if(memcmp(digest,payloadDigest,16))continue;
  for(uint32 i=0;i<length*2;i++)e.wave[i]=(int32)READ_BE_UINT32((byte*)&e.wave[i]);
  _bytes+=length*8;_entries.push_back(e);
 }
}
const int32 *PreparedPCMCache::lookup(const byte digest[16],const int8 *data,uint32 length,uint32 volume,uint32 left,uint32 right,uint32 gain,uint32 deviceVolume){
 byte key[44];memcpy(key,digest,16);uint32 fields[]={length,volume,left,right,gain,deviceVolume,2};
 for(int i=0;i<7;i++)WRITE_BE_UINT32(key+16+i*4,fields[i]);
 for(uint i=0;i<_entries.size();i++)if(!memcmp(key,_entries[i].key,44))return &_entries[i].wave[0];
 Common::String requestName;if(_trace){Common::MemoryReadStream requestStream(key,44);requestName=Common::computeStreamMD5AsString(requestStream);}
 bool seen=false;for(uint i=0;i<_requested.size();i++)if(_requested[i]==requestName)seen=true;
 if(_trace&&!seen&&_requested.size()<128){
  _requested.push_back(requestName);
  _haveMiss=true;memcpy(_lastMiss,key,44);Common::String name=requestName;
  Common::DumpFile out;if(out.open(Common::Path(_directory+"/"+name+".req"))){out.write("SSTPCMR1",8);out.write(key,44);out.write(data,length);warning("PREPARED_PCM request=%s length=%u volume=%u pan=%u,%u gain=%u",name.c_str(),length,volume,left,right,gain);}
 }
 return nullptr;
}
void PreparedPCMCache::account(bool hit,uint32 frames,bool verified){
 if(!enabled())return;
 if(hit){_hits++;_frames+=frames;if(verified)_verified++;}else _misses++;
 if(((_hits+_misses)%1000)==0)warning("PREPARED_PCM hits=%u misses=%u verified=%u frames=%llu",_hits,_misses,_verified,(unsigned long long)_frames);
}
const int32 *PreparedPCMCache::lookupValues(int voice,uint32 volume,uint32 left,uint32 right,uint32 gain){
 Transform &t=_transforms[voice];uint32 params[]={volume,left,right,gain};
 if(!t.valid||memcmp(t.params,params,sizeof(params))){
  for(int i=0;i<256;i++){int sample=(int8)i;int32 l=((sample*(int)volume)*(int)left)>>3;int32 rr=((sample*(int)volume)*(int)right)>>3;t.wave[i*2]=(l*(int)gain)/256;t.wave[i*2+1]=(rr*(int)gain)/256;}
  memcpy(t.params,params,sizeof(params));t.valid=true;_builds++;
 }
 return t.wave;
}
void PreparedPCMCache::accountIdle(uint32,bool verified){_idleHits++;if(verified)_idleVerified++;if((_idleHits%4000)==0)warning("PCM_TRANSFORM idle=%u verified_idle=%u builds=%u",_idleHits,_idleVerified,_builds);}

}
