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
 */

/*
 * Sunstorm Project bppmidi backend.
 *
 * Writes a raw MIDI byte stream to /dev/bppmidi0 — the SS-4 parallel-port
 * MIDI-OUT driver bit-bangs the bytes at 31.25 kbaud to a real MT-32 (or
 * any MIDI device) hanging off a passive DB-25 -> 5-pin DIN adapter.
 *
 * Modeled on stmidi.cpp (the Atari ST raw-MIDI backend): pure write()
 * to a character device, no OSS sequencer ioctl handshake.  We are NOT
 * a synth — we are a MIDI port talking to external hardware.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"

#if defined(USE_BPPMIDI)

#include "common/error.h"
#include "common/textconsole.h"
#include "common/util.h"
#include "audio/musicplugin.h"
#include "audio/mpu401.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

class MidiDriver_BppMidi : public MidiDriver_MPU401 {
public:
	MidiDriver_BppMidi() : _isOpen(false), _device(-1) { }
	int open() override;
	bool isOpen() const override { return _isOpen; }
	void close() override;
	void send(uint32 b) override;
	void sysEx(const byte *msg, uint16 length) override;

	static const char *getDeviceName();

private:
	bool _isOpen;
	int _device;
};

const char *MidiDriver_BppMidi::getDeviceName() {
	const char *env = getenv("SCUMMVM_BPPMIDI");
	return env ? env : "/dev/bppmidi0";
}

int MidiDriver_BppMidi::open() {
	if (_isOpen)
		return MERR_ALREADY_OPEN;

	const char *deviceName = getDeviceName();
	_device = ::open(deviceName, O_WRONLY);
	if (_device < 0) {
		warning("MidiDriver_BppMidi: cannot open %s (%s)",
		        deviceName, strerror(errno));
		return MERR_DEVICE_NOT_AVAILABLE;
	}

	_isOpen = true;
	return 0;
}

void MidiDriver_BppMidi::close() {
	MidiDriver_MPU401::close();
	if (_isOpen && _device >= 0) {
		::close(_device);
	}
	_device = -1;
	_isOpen = false;
}

// Helper: write all bytes; loop on short writes / EINTR.  Drops nothing.
static void bpp_write_all(int fd, const byte *buf, int len) {
	while (len > 0) {
		int n = ::write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			warning("MidiDriver_BppMidi: write failed (%s)", strerror(errno));
			return;
		}
		if (n == 0) {
			warning("MidiDriver_BppMidi: write returned 0");
			return;
		}
		buf += n;
		len -= n;
	}
}

void MidiDriver_BppMidi::send(uint32 b) {
	midiDriverCommonSend(b);

	byte buf[3];
	int len = 0;
	byte status = (byte)(b & 0xFF);
	byte data1  = (byte)((b >> 8) & 0xFF);
	byte data2  = (byte)((b >> 16) & 0xFF);

	switch (status & 0xF0) {
	case 0x80: // Note Off
	case 0x90: // Note On
	case 0xA0: // Polyphonic Key Pressure
	case 0xB0: // Control Change
	case 0xE0: // Pitch Bend
		buf[0] = status; buf[1] = data1; buf[2] = data2; len = 3;
		break;
	case 0xC0: // Program Change
	case 0xD0: // Channel Pressure
		buf[0] = status; buf[1] = data1; len = 2;
		break;
	default:
		warning("MidiDriver_BppMidi::send: unknown status %02x (raw %08x)",
		        status, (unsigned int)b);
		return;
	}

	if (_device < 0)
		return;
	bpp_write_all(_device, buf, len);
}

void MidiDriver_BppMidi::sysEx(const byte *msg, uint16 length) {
	midiDriverCommonSysEx(msg, length);

	if (_device < 0)
		return;

	// MT-32 sysex must reach the device as a single uninterrupted byte
	// stream so the receiver computes the checksum over exactly the bytes
	// the sender intended.  Stage F0 + body + F7 into one buffer and
	// issue a single write() — three separate writes risked interleaving
	// with concurrent send() bytes from the SCI timer thread.
	//
	// SCI's longest single sysex is bounded by kMaxSysExSize (~270 bytes
	// in current builds); we cap at 4 KB for headroom, with a fallback
	// to heap allocation for anything larger.
	enum { kStackBuf = 4096 };
	byte stack[kStackBuf];
	byte *frame = stack;
	bool heap = false;
	uint32 total = (uint32)length + 2;

	if (total > kStackBuf) {
		frame = (byte *)malloc(total);
		if (!frame) {
			warning("MidiDriver_BppMidi::sysEx: oversized payload (%u)", total);
			return;
		}
		heap = true;
	}

	frame[0] = 0xF0;
	if (length > 0)
		memcpy(frame + 1, msg, length);
	frame[1 + length] = 0xF7;

	bpp_write_all(_device, frame, (int)total);

	if (heap)
		free(frame);
}

// Plugin interface

class BppMidiMusicPlugin : public MusicPluginObject {
public:
	const char *getName() const override { return "Sunstorm bppmidi (SS-4 parallel MT-32)"; }
	const char *getId() const override { return "bppmidi"; }

	MusicDevices getDevices() const override;
	Common::Error createInstance(MidiDriver **mididriver,
	                             MidiDriver::DeviceHandle = 0) const override;
};

MusicDevices BppMidiMusicPlugin::getDevices() const {
	MusicDevices devices;
	// Advertise both MT-32 and General MIDI roles so SCI / SCUMM can
	// route either music type through the same external port.  The user
	// has a real MT-32 on the other end; native_mt32=true selects the
	// MT_MT32 entry.
	devices.push_back(MusicDevice(this, "MT-32", MT_MT32));
	devices.push_back(MusicDevice(this, "General MIDI", MT_GM));
	return devices;
}

Common::Error BppMidiMusicPlugin::createInstance(MidiDriver **mididriver,
                                                 MidiDriver::DeviceHandle) const {
	*mididriver = new MidiDriver_BppMidi();
	return Common::kNoError;
}

REGISTER_PLUGIN_STATIC(BPPMIDI, PLUGIN_TYPE_MUSIC, BppMidiMusicPlugin);

#endif
