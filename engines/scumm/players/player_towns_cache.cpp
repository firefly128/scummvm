/* Offline PCM cache for the fixed-track Towns v1 Euphony player. */
#include "common/config-manager.h"
#include "common/endian.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/md5.h"
#include "common/memstream.h"
#include "common/textconsole.h"
#include "audio/audiostream.h"
#include "audio/decoders/wave.h"
#include "audio/mixer_intern.h"
#include "scumm/players/player_towns.h"
#include "scumm/resource.h"
namespace Scumm {
// Rendering at gain 64 leaves headroom; apply the live music volume before
// returning samples to the plain mixer channel. FM music and the reserved PCM
// effects have independent controls, matching the live player channel mask.
class EuphonyPCMStream : public Audio::AudioStream {
 Audio::AudioStream *_source; Audio::Mixer *_mixer; int _volumeScale;
public:
 EuphonyPCMStream(Audio::AudioStream *source,Audio::Mixer *mixer,int volumeScale):_source(source),_mixer(mixer),_volumeScale(volumeScale){}
 ~EuphonyPCMStream(){delete _source;}
 int readBuffer(int16 *buffer,const int count) override {
  int n=_source->readBuffer(buffer,count);
  int volume=_mixer->isSoundTypeMuted(Audio::Mixer::kMusicSoundType)?0:_mixer->getVolumeForSoundType(Audio::Mixer::kMusicSoundType);
  volume = volume * _volumeScale / 256;
  for(int i=0;i<n;++i)buffer[i]=CLIP<int32>((int32)buffer[i]*volume/64,-32768,32767);
  return n;
 }
 bool isStereo()const override{return _source->isStereo();}
 int getRate()const override{return _source->getRate();}
 bool endOfData()const override{return _source->endOfData();}
 bool endOfStream()const override{return _source->endOfStream();}
};
static int trackLevel(const uint8 *d,int l,int r) {return ((l+r)?l+r:d[8]+d[9])>>2;}
static void waveHeader(Common::DumpFile &f,uint32 frames) {
 byte h[44]={};memcpy(h,"RIFF",4);WRITE_LE_UINT32(h+4,frames*4+36);memcpy(h+8,"WAVEfmt ",8);
 WRITE_LE_UINT32(h+16,16);WRITE_LE_UINT16(h+20,1);WRITE_LE_UINT16(h+22,2);WRITE_LE_UINT32(h+24,22050);
 WRITE_LE_UINT32(h+28,88200);WRITE_LE_UINT16(h+32,4);WRITE_LE_UINT16(h+34,16);memcpy(h+36,"data",4);
 WRITE_LE_UINT32(h+40,frames*4);f.seek(0);f.write(h,44);
}
void Player_Towns_v1::loadEuphonyCache() {
 if(!ConfMan.hasKey("sst_euphony_cache_path"))return;
 Common::FSNode root(ConfMan.getPath("sst_euphony_cache_path"));Common::FSList files;
 if(!root.getChildren(files,Common::FSNode::kListFilesOnly))error("Cannot read Euphony cache directory");
 uint32 bytes=0;
 for(uint i=0;i<files.size();++i) {
  Common::String name=files[i].getName();
  if(!name.hasPrefix("eup-v2-")||(!name.hasSuffix(".wav")&&!name.hasSuffix(".meta")))continue;
  Common::File file;if(!file.open(files[i]))error("Cannot preload Euphony file %s",name.c_str());
  int64 size=file.size();
  if(size<=0||size>128*1024*1024||bytes+(uint64)size>128*1024*1024)error("Euphony cache exceeds memory limit");
  Common::Array<byte> &data=_euphonyFiles[name];data.resize((uint32)size);
  if(file.read(data.data(),data.size())!=data.size())error("Cannot finish Euphony preload %s",name.c_str());
  bytes+=data.size();
 }
 debug(1,"[sst-euphony] resident files=%u bytes=%u",_euphonyFiles.size(),bytes);
}
Common::String Player_Towns_v1::euphonyCacheKey(int sound,int level) {
 const byte *p=_vm->getResourceAddress(rtSound,sound);uint32 size=_vm->getResourceSize(rtSound,sound);
 Common::MemoryReadStream stream(p,size);Common::String digest=Common::computeStreamMD5AsString(stream);
 return Common::String::format("eup-v2-%s-22050-%d",digest.c_str(),level);
}
bool Player_Towns_v1::euphonyPlaying() const {
 if(!_cachedEuphony)return _player->isPlaying();
 return _cachedHeld || (_cacheMixer->isSoundHandleActive(_euphonyCacheHandle) &&
  (_eupLooping || _cacheMixer->getSoundElapsedTime(_euphonyCacheHandle)<_cacheLogicalEndMs));
}
bool Player_Towns_v1::playCachedEuphony(int sound,const uint8 *data) {
 if(!ConfMan.hasKey("sst_euphony_cache_path"))return false;
 int left=_soundOverride?_soundOverride[sound].vLeft:0,right=_soundOverride?_soundOverride[sound].vRight:0;
 int level=trackLevel(data,left,right);
 Common::String key=euphonyCacheKey(sound,level);
 if(!_euphonyFiles.contains(key+".meta")||!_euphonyFiles.contains(key+".wav")) {
  warning("[sst-euphony] cache miss sound=%d level=%d key=%s",sound,level,key.c_str());return false;
 }
 const Common::Array<byte> &metaData=_euphonyFiles[key+".meta"];
 Common::MemoryReadStream meta(metaData.data(),metaData.size());
 byte magic[8];meta.read(magic,8);uint32 frames=meta.readUint32LE(),ls=meta.readUint32LE(),le=meta.readUint32LE();
 uint32 logical=meta.readUint32LE(),held=meta.readUint32LE(),gain=meta.readUint32LE();
 if(memcmp(magic,"SSTEUP2",8)||meta.err()||!frames||frames>22050*600||logical>frames||le>frames||(le&&ls>=le)||held>1||gain!=64)
  error("Invalid Euphony cache metadata for sound %d",sound);
 const Common::Array<byte> &waveData=_euphonyFiles[key+".wav"];
 if(waveData.size()!=44+(uint64)frames*4)error("Incomplete Euphony PCM cache for sound %d",sound);
 Common::MemoryReadStream *file=new Common::MemoryReadStream(waveData.data(),waveData.size());
 Audio::SeekableAudioStream *wave=Audio::makeWAVStream(file,DisposeAfterUse::YES);
 if(!wave||wave->getRate()!=22050||!wave->isStereo()) {delete wave;error("Invalid Euphony PCM format for sound %d",sound);}
 Audio::AudioStream *stream=wave;
 if(le)stream=new Audio::SubLoopingAudioStream(wave,0,Audio::Timestamp(0,ls,22050),Audio::Timestamp(0,le,22050));
 _cacheMixer->playStream(Audio::Mixer::kPlainSoundType,&_euphonyCacheHandle,new EuphonyPCMStream(stream,_cacheMixer,_euphonyVolumeScale));
 _cachedEuphony=true;_cachedHeld=held!=0;_cacheLogicalEndMs=(uint64)logical*1000/22050;
 _eupCurrentSound=sound;_eupLooping=le!=0;_eupVolLeft=left;_eupVolRight=right;
 debug(1,"[sst-euphony] PCM sound=%d level=%d frames=%u loop=%u..%u held=%u",sound,level,frames,ls,le,held);return true;
}
void Player_Towns_v1::renderEuphonyCache() {
#ifdef SPARCFB_NATIVE_BACKEND
 error("Generate the Euphony cache on the build machine, not the Sun");
#else
 _renderingEuphony=true;Common::FSNode dir(ConfMan.getPath("sst_euphony_render_all"));
 if((!dir.exists()&&!dir.createDirectory())||!dir.isDirectory())error("Cannot create Euphony export directory");
 if(_cacheMixer->getOutputRate()!=22050)error("Euphony exporter requires 22050Hz output");
 _cacheMixer->setVolumeForSoundType(Audio::Mixer::kMusicSoundType,255);
 _cacheMixer->setVolumeForSoundType(Audio::Mixer::kSFXSoundType,255);
 _player->driver()->setSoundEffectVolume(64);_player->driver()->setMusicVolume(64);
 Common::DumpFile index;if(!index.open(dir.getPath().appendComponent("tracks.tsv")))error("Cannot write Euphony index");
 const char *heading="sound\tlevel\tframes\tloop_start\tloop_end\tlogical_end\theld\tkey\n";index.write(heading,strlen(heading));int total=0;
 for(int sound=1;sound<MIN(_numSoundMax,(int)_vm->_res->_types[rtSound].size());++sound) {
  byte *resource=_vm->getResourceAddress(rtSound,sound);if(!resource)continue;
  uint32 size=_vm->getResourceSize(rtSound,sound),skip=_vm->_game.version==3?6:8;if(size<skip+16)continue;
  const byte *data=resource+skip;if(data[7]!=1&&!(_vm->_game.id==GID_INDY3&&sound==40))continue;
  uint32 header=16+(uint32)data[14]*48+150;
  if(header>size-skip||READ_LE_UINT32(data+header-8)>size-skip-header)error("Malformed Euphony resource %d",sound);
  _player->stop();_player->driver()->reset();_player->driver()->reserveSoundEffectChannels(8);_intf->setSoundEffectChanMask(-1);
  if(_soundOverride)memset(&_soundOverride[sound],0,sizeof(SoundOvrParameters));
  int level=trackLevel(data,0,0);Common::String key=euphonyCacheKey(sound,level);Common::DumpFile wave;
  if(!wave.open(dir.getPath().appendComponent(key+".wav")))error("Cannot write Euphony PCM");
  waveHeader(wave,0);wave.seek(44);playEuphonyTrack(sound,data);
  uint32 frames=0,ls=0,le=0,logical=0;bool held=false,done=false;
  while(frames<22050*600) {
   int16 samples[128];byte output[256];static_cast<Audio::MixerImpl *>(_cacheMixer)->mixCallback((byte *)samples,sizeof(samples));
   for(uint i=0;i<128;++i)WRITE_LE_UINT16(output+i*2,(uint16)samples[i]);wave.write(output,sizeof(output));frames+=64;
   if(_player->completedLoops()==1&&!ls)ls=frames;
   if(_player->completedLoops()>=2){le=frames;logical=frames;done=true;break;}
   if(!_player->isPlaying()||_player->isPaused()) {
    if(!logical){logical=frames;held=_player->isPaused();}
    if(frames>=logical+11025){done=true;break;}
   }
  }
  _player->stop();if(!done)error("Euphony resource %d did not terminate or loop within 10 minutes",sound);
  waveHeader(wave,frames);if(wave.err())error("Euphony PCM write failed");wave.close();
  Common::DumpFile meta;if(!meta.open(dir.getPath().appendComponent(key+".meta")))error("Cannot write Euphony metadata");
  meta.write("SSTEUP2",8);meta.writeUint32LE(frames);meta.writeUint32LE(ls);meta.writeUint32LE(le);meta.writeUint32LE(logical);meta.writeUint32LE(held?1:0);meta.writeUint32LE(64);meta.close();
  Common::String line=Common::String::format("%d\t%d\t%u\t%u\t%u\t%u\t%d\t%s\n",sound,level,frames,ls,le,logical,held,key.c_str());
  index.write(line.c_str(),line.size());index.flush();warning("[sst-euphony-export] %s",line.c_str());++total;
 }
 warning("[sst-euphony-export] COMPLETE tracks=%d",total);_renderingEuphony=false;
#endif
}
} // namespace Scumm
