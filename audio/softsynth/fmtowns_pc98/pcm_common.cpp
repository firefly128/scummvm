/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "audio/softsynth/fmtowns_pc98/pcm_common.h"
#include "audio/mixer.h"
#include "audio/prepared-pcm-cache.h"
#include "common/memstream.h"
#include "common/md5.h"
#include "common/textconsole.h"

PCMChannel_Base::PCMChannel_Base() : _vol(0), _data(nullptr), _dataEnd(0), _loopLen(0), _pos(0), _loopStart(0), _step(0), _panLeft(7), _panRight(7), _activeOutput(false)  {
 _sourceImmutable=false;_sourceLength=0;memset(_sourceDigest,0,16);
}

PCMChannel_Base::~PCMChannel_Base() {
	clear();
}

void PCMChannel_Base::clear() {
	_sourceImmutable=false;_sourceLength=0;memset(_sourceDigest,0,16);
	_vol = 0;
	_data = nullptr;
	_dataEnd = 0;
	_loopLen = 0;
	_pos = 0;
	_loopStart = 0;
	_step = 0;
	_panLeft = _panRight = 7;
	_activeOutput = false;
}

void PCMChannel_Base::updateOutput() {
	if (!isPlaying())
		return;

	_pos += _step;

	if (_pos >= _dataEnd) {
		if (_loopStart != _dataEnd) {
			_pos = _loopStart;
			_dataEnd = _loopStart + _loopLen;
		} else {
			_pos = 0;
			stopInternal();
		}
	}
}

int32 PCMChannel_Base::currentSampleLeft() {
	return (isActive() && _panLeft) ? (((_data[_pos >> 11] * _vol) * _panLeft) >> 3) : 0;
}

int32 PCMChannel_Base::currentSampleRight() {
	return (isActive() && _panRight) ? (((_data[_pos >> 11] * _vol) * _panRight) >> 3) : 0;
}

bool PCMChannel_Base::isActive() const {
	return _activeOutput;
}

void PCMChannel_Base::activate() {
	_activeOutput = true;
}

void PCMChannel_Base::deactivate() {
	_activeOutput = false;
}

void PCMChannel_Base::setData(const int8 *data, uint32 dataEnd, uint32 dataStart, bool immutable) {
	_sourceImmutable=immutable;_sourceLength=dataEnd>>11;
	memset(_sourceDigest,0,16);
	if(immutable&&data&&_sourceLength){Common::MemoryReadStream source((const byte*)data,_sourceLength);Common::computeStreamMD5(source,_sourceDigest);}
	_data = data;
	_dataEnd = dataEnd;
	_pos = dataStart;
}

void PCMChannel_Base::setVolume(uint8 vol) {
	_vol = vol;
}

void PCMChannel_Base::setPanPos(uint8 pan) {
	_panLeft = pan & 0x0f;
	_panRight = pan >> 4;
}

void PCMChannel_Base::setupLoop(uint32 loopStart, uint32 loopLen) {
	_loopStart = loopStart << 11;
	_loopLen = loopLen << 11;
}

void PCMChannel_Base::setRate(uint16 rate) {
	_step = rate;
}

PCMDevice_Base::PCMDevice_Base(int samplingRate, int deviceVolume, int numChannels) : _numChannels(numChannels), _deviceVolume(deviceVolume), _intRate((7670454 << 8) / samplingRate),
	_extRate(72 << 8), _timer(0), _musicVolume(Audio::Mixer::kMaxMixerVolume), _sfxVolume(Audio::Mixer::kMaxMixerVolume), _pcmSfxChanMask(0) {
	_prepared=new Audio::PreparedPCMCache();
	_channels = new PCMChannel_Base*[numChannels];
}

PCMDevice_Base::~PCMDevice_Base() {
	delete _prepared;
	delete[] _channels;
}

void PCMDevice_Base::assignChannel(uint8 id, PCMChannel_Base *const chan) {
	assert(id < _numChannels);
	_channels[id] = chan;
}

void PCMDevice_Base::setMusicVolume(uint16 vol) {
	_musicVolume = vol;
}

void PCMDevice_Base::setSfxVolume(uint16 vol) {
	_sfxVolume = vol;
}

void PCMDevice_Base::setSfxChanMask(int mask) {
	_pcmSfxChanMask = mask;
}

void PCMDevice_Base::readBufferOriginal(int32 *buffer, uint32 bufferSize) {
	for (uint32 i = 0; i < bufferSize; i++) {
		_timer += _extRate;
		while (_timer >= _intRate) {
			_timer -= _intRate;

			for (int ii = 0; ii < 8; ii++)
				_channels[ii]->updateOutput();
		}

		int32 finOutL = 0;
		int32 finOutR = 0;

		for (int ii = 0; ii < _numChannels; ii++) {
			if (_channels[ii]->isActive()) {
				int32 oL = _channels[ii]->currentSampleLeft();
				int32 oR = _channels[ii]->currentSampleRight();
				if ((1 << ii) & (~_pcmSfxChanMask)) {
					oL = (oL * _musicVolume) / Audio::Mixer::kMaxMixerVolume;
					oR = (oR * _musicVolume) / Audio::Mixer::kMaxMixerVolume;
				}
				if ((1 << ii) & _pcmSfxChanMask) {
					oL = (oL * _sfxVolume) / Audio::Mixer::kMaxMixerVolume;
					oR = (oR * _sfxVolume) / Audio::Mixer::kMaxMixerVolume;
				}
				finOutL += oL;
				finOutR += oR;

				if (!_channels[ii]->isPlaying())
					_channels[ii]->deactivate();
			}
		}

		buffer[i << 1] += ((finOutL * _deviceVolume) >> 7);
		buffer[(i << 1) + 1] += ((finOutR * _deviceVolume) >> 7);
	}
}

void PCMDevice_Base::readPrepared(int32 *buffer,uint32 size,const int32 **waves){
 for(uint32 i=0;i<size;i++){
  _timer+=_extRate;
  while(_timer>=_intRate){_timer-=_intRate;for(int j=0;j<8;j++)_channels[j]->updateOutput();}
  int32 left=0,right=0;
  for(int j=0;j<_numChannels;j++)if(_channels[j]->_activeOutput){
   PCMChannel_Base *c=_channels[j];const uint32 pos=c->_pos>>11;
   if(!waves[j]||pos>=c->_sourceLength)error("Prepared PCM invalid voice");
   const uint32 sampleIndex=_prepared->valueMode()?(uint8)c->_data[pos]:pos;
   left+=waves[j][sampleIndex*2];right+=waves[j][sampleIndex*2+1];
   if(!c->isPlaying())c->deactivate();
  }
  buffer[i*2]+=(left*_deviceVolume)>>7;buffer[i*2+1]+=(right*_deviceVolume)>>7;
 }
}
void PCMDevice_Base::readBuffer(int32 *buffer,uint32 size){
 const int32 *waves[8]={nullptr};bool ready=_prepared->enabled()&&_numChannels==8,any=false;static int traceBlocks=0;
 const uint64 ticks=((uint64)_timer+(uint64)size*_extRate)/_intRate;
 if(ready)for(int i=0;i<8;i++){
  PCMChannel_Base *c=_channels[i];if(!c->isActive()&&!c->isPlaying())continue;any=true;
  if(_prepared->trace()&&traceBlocks<8)warning("PREPARED_TRACE voice=%d length=%u active=%d playing=%d vol=%u",i,c->_sourceLength,c->isActive(),c->isPlaying(),c->_vol);
  if(_prepared->valueMode()&&c->_data&&c->_sourceLength){
   uint32 gain=((1<<i)&_pcmSfxChanMask)?_sfxVolume:_musicVolume;waves[i]=_prepared->lookupValues(i,c->_vol,c->_panLeft,c->_panRight,gain);
  }else if(c->_sourceImmutable&&c->_data&&c->_sourceLength&&c->_sourceLength<=65536){
   uint32 gain=((1<<i)&_pcmSfxChanMask)?_sfxVolume:_musicVolume;
   waves[i]=_prepared->lookup(c->_sourceDigest,c->_data,c->_sourceLength,c->_vol,c->_panLeft,c->_panRight,gain,_deviceVolume);
  }
  if(!waves[i]||(c->_loopStart==c->_dataEnd&&(uint64)c->_pos+ticks*c->_step>=c->_dataEnd))ready=false;
 }
 if(any)traceBlocks++;
 if(ready&&!any&&_prepared->valueMode()){
  const uint32 expected=(uint32)(((uint64)_timer+(uint64)size*_extRate)%_intRate);bool verified=false;
  if(_prepared->verifyIdle()){int32 *reference=new int32[size*2];memcpy(reference,buffer,size*2*sizeof(int32));readBufferOriginal(reference,size);if(_timer!=expected||memcmp(reference,buffer,size*2*sizeof(int32)))error("PCM transform idle mismatch");delete[] reference;verified=true;}
  _timer=expected;_prepared->accountIdle(size,verified);return;
 }
 if(!ready||!any){readBufferOriginal(buffer,size);_prepared->account(false,size,false);return;}
 if(_prepared->verify()){
  int32 *reference=new int32[size*2];memcpy(reference,buffer,size*2*sizeof(int32));
  uint32 before[8][3],after[8][3];bool play[8];const uint32 timer=_timer;
  for(int i=0;i<8;i++){PCMChannel_Base*c=_channels[i];before[i][0]=c->_pos;before[i][1]=c->_dataEnd;before[i][2]=c->_activeOutput;play[i]=c->isPlaying();}
  readPrepared(buffer,size,waves);const uint32 outTimer=_timer;
  for(int i=0;i<8;i++){PCMChannel_Base*c=_channels[i];after[i][0]=c->_pos;after[i][1]=c->_dataEnd;after[i][2]=c->_activeOutput;if(play[i]!=c->isPlaying())error("Prepared PCM unanticipated stop");c->_pos=before[i][0];c->_dataEnd=before[i][1];c->_activeOutput=before[i][2];}
  _timer=timer;readBufferOriginal(reference,size);
  if(memcmp(reference,buffer,size*2*sizeof(int32))||_timer!=outTimer)error("Prepared PCM reference mismatch");
  for(int i=0;i<8;i++){PCMChannel_Base*c=_channels[i];if(c->_pos!=after[i][0]||c->_dataEnd!=after[i][1]||c->_activeOutput!=after[i][2]||c->isPlaying()!=play[i])error("Prepared PCM state mismatch");}
  delete[] reference;_prepared->account(true,size,true);
 }else{readPrepared(buffer,size,waves);_prepared->account(true,size,false);}
}
