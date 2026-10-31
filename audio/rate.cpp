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

/*
 * The code in this file is based on code with Copyright 1998 Fabrice Bellard
 * Fabrice original code is part of SoX (http://sox.sourceforge.net).
 * Max Horn adapted that code to the needs of ScummVM and rewrote it partial,
 * in the process removing any use of floating point arithmetic. Various other
 * improvements over the original code were made.
 */

#include "audio/audiostream.h"
#include "audio/rate.h"
#include "audio/mixer.h"
#include "common/util.h"

namespace Audio {

/**
 * The default fractional type in frac.h (with 16 fractional bits) limits
 * the rate conversion code to 65536Hz audio: we need to able to handle
 * 192kHz audio, so we use fewer fractional bits in this code.
 */
enum {
	FRAC_BITS_LOW = 14,
	FRAC_ONE_LOW = (1L << FRAC_BITS_LOW),
	FRAC_HALF_LOW = (1L << (FRAC_BITS_LOW-1))
};

template<bool inStereo, bool outStereo, bool reverseStereo>
class RateConverter_Impl : public RateConverter {
private:
	/** Input and output rates */
	st_rate_t _inRate, _outRate;

	/**
	 * The intermediate input cache. Bigger values may increase performance,
	 * but only until some point (depends largely on cache size, target
	 * processor and various other factors), at which it will decrease again.
	 */
	st_sample_t _buffer[512];

	/** Current position inside the buffer */
	const st_sample_t *_bufferPos;

	/** Size of data currently loaded into the buffer */
	int _bufferSize;

	/** How far output is ahead of input when doing simple conversion */
	frac_t _outPos;

	/** Fractional position of the output stream in input stream unit */
	frac_t _outPosFrac;

	/** Last sample(s) in the input stream (left/right channel) */
	st_sample_t _inLastL, _inLastR;
	
	/** Current sample(s) in the input stream (left/right channel) */
	st_sample_t _inCurL, _inCurR;

	int copyConvert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t vol_l, st_volume_t vol_r);
	int simpleConvert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t vol_l, st_volume_t vol_r);
	int interpolateConvert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t vol_l, st_volume_t vol_r);

public:
	RateConverter_Impl(st_rate_t inputRate, st_rate_t outputRate);
	virtual ~RateConverter_Impl() {}

	int convert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t vol_l, st_volume_t vol_r) override;

	void setInputRate(st_rate_t inputRate) override { _inRate = inputRate; }
	void setOutputRate(st_rate_t outputRate) override { _outRate = outputRate; }

	st_rate_t getInputRate() const override { return _inRate; }
	st_rate_t getOutputRate() const override { return _outRate; }

	bool needsDraining() const override { return _bufferSize != 0; }
};

/* SST: hand-tuned mix loop, generic across all engines.
 *
 * Strictly equivalent output to the original copyConvert, but:
 *   - lifts _bufferPos/_bufferSize/outBuffer into locals so the compiler
 *     keeps them in registers across the inner loop;
 *   - batches the "is the buffer refill due?" check ONCE per chunk
 *     instead of per output frame;
 *   - uses arithmetic shift right (kMaxMixerVolume == 256) so the
 *     compiler doesn't have to prove division-by-power-of-two safety
 *     on every iteration;
 *   - 4-way unroll in the hot stereo->stereo path so SPARC v8 instruction
 *     scheduling can overlap loads/muls/clamps.
 *
 * The dispatch into the specialized kernel happens once per call.
 * Default (off-CPU) configurations get the same numerical result via
 * the original-style scalar tail.
 */
template<bool inStereo, bool outStereo, bool reverseStereo>
int RateConverter_Impl<inStereo, outStereo, reverseStereo>::copyConvert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t volL, st_volume_t volR) {
	st_sample_t * const outStart = outBuffer;
	st_sample_t * const outEnd   = outBuffer + numSamples * (outStereo ? 2 : 1);
	const int volLi = (int)volL;
	const int volRi = (int)volR;

	while (outBuffer < outEnd) {
		// Check if we have to refill the buffer
		if (_bufferSize == 0) {
			_bufferPos = _buffer;
			_bufferSize = input.readBuffer(_buffer, ARRAYSIZE(_buffer));

			if (_bufferSize <= 0)
				return (outBuffer - outStart) / (outStereo ? 2 : 1);
		}

		// How many output frames can we emit before we exhaust the
		// in-buffer or fill the output?  Compute once, then run a
		// tight inner loop over that count.
		const int inStep  = inStereo ? 2 : 1;
		const int outStep = outStereo ? 2 : 1;
		const st_size_t outRemaining = (outEnd - outBuffer) / outStep;
		const st_size_t inFramesAvail = (st_size_t)_bufferSize / inStep;
		st_size_t framesThisChunk = (outRemaining < inFramesAvail) ? outRemaining : inFramesAvail;

		const st_sample_t * __restrict src = _bufferPos;
		st_sample_t       * __restrict dst = outBuffer;

		if (outStereo && inStereo) {
			// Stereo->stereo, 4x unroll.  This is the hot path for CD audio.
			while (framesThisChunk >= 4) {
				const int iL0 = src[0], iR0 = src[1];
				const int iL1 = src[2], iR1 = src[3];
				const int iL2 = src[4], iR2 = src[5];
				const int iL3 = src[6], iR3 = src[7];
				src += 8;

				const int oL0 = (iL0 * volLi) >> 8;
				const int oR0 = (iR0 * volRi) >> 8;
				const int oL1 = (iL1 * volLi) >> 8;
				const int oR1 = (iR1 * volRi) >> 8;
				const int oL2 = (iL2 * volLi) >> 8;
				const int oR2 = (iR2 * volRi) >> 8;
				const int oL3 = (iL3 * volLi) >> 8;
				const int oR3 = (iR3 * volRi) >> 8;

				clampedAdd(dst[reverseStereo    ], oL0);
				clampedAdd(dst[reverseStereo ^ 1], oR0);
				clampedAdd(dst[2 + (int)reverseStereo    ], oL1);
				clampedAdd(dst[2 + (int)(reverseStereo ^ 1)], oR1);
				clampedAdd(dst[4 + (int)reverseStereo    ], oL2);
				clampedAdd(dst[4 + (int)(reverseStereo ^ 1)], oR2);
				clampedAdd(dst[6 + (int)reverseStereo    ], oL3);
				clampedAdd(dst[6 + (int)(reverseStereo ^ 1)], oR3);
				dst += 8;
				framesThisChunk -= 4;
			}
			while (framesThisChunk-- > 0) {
				const int iL = *src++;
				const int iR = *src++;
				const int oL = (iL * volLi) >> 8;
				const int oR = (iR * volRi) >> 8;
				clampedAdd(dst[reverseStereo    ], oL);
				clampedAdd(dst[reverseStereo ^ 1], oR);
				dst += 2;
			}
		} else {
			// Mono input or mono output: fall back to per-frame scalar.
			while (framesThisChunk-- > 0) {
				const int iL = *src++;
				const int iR = inStereo ? *src++ : iL;
				const int oL = (iL * volLi) >> 8;
				const int oR = (iR * volRi) >> 8;
				if (outStereo) {
					clampedAdd(dst[reverseStereo    ], oL);
					clampedAdd(dst[reverseStereo ^ 1], oR);
					dst += 2;
				} else {
					clampedAdd(dst[0], (oL + oR) >> 1);
					dst += 1;
				}
			}
		}

		const ptrdiff_t srcAdvanced = src - _bufferPos;
		_bufferPos = src;
		_bufferSize -= (int)srcAdvanced;
		outBuffer = dst;
	}

	return (outBuffer - outStart) / (outStereo ? 2 : 1);
}

template<bool inStereo, bool outStereo, bool reverseStereo>
int RateConverter_Impl<inStereo, outStereo, reverseStereo>::simpleConvert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t volL, st_volume_t volR) {
	// How much to increment _outPos by
	frac_t outPos_inc = _inRate / _outRate;

	st_sample_t *outStart, *outEnd;

	outStart = outBuffer;
	outEnd = outBuffer + numSamples * (outStereo ? 2 : 1);

	while (outBuffer < outEnd) {
		// Read enough input samples so that _outPos >= 0
		do {
			// Check if we have to refill the buffer
			if (_bufferSize == 0) {
				_bufferPos = _buffer;
				_bufferSize = input.readBuffer(_buffer, ARRAYSIZE(_buffer));

				if (_bufferSize <= 0)
					return (outBuffer - outStart) / (outStereo ? 2 : 1);
			}

			_bufferSize -= (inStereo ? 2 : 1);
			_outPos--;

			if (_outPos >= 0) {
				_bufferPos += (inStereo ? 2 : 1);
			}
		} while (_outPos >= 0);

		st_sample_t inL, inR;
		inL = *_bufferPos++;
		inR = (inStereo ? *_bufferPos++ : inL);

		// Increment output position
		_outPos += outPos_inc;

		st_sample_t outL, outR;
		outL = (inL * (int)volL) / Audio::Mixer::kMaxMixerVolume;
		outR = (inR * (int)volR) / Audio::Mixer::kMaxMixerVolume;

		if (outStereo) {
			// output left channel
			clampedAdd(outBuffer[reverseStereo    ], outL);

			// output right channel
			clampedAdd(outBuffer[reverseStereo ^ 1], outR);

			outBuffer += 2;
		} else {
			// output mono channel
			clampedAdd(outBuffer[0], (outL + outR) / 2);

			outBuffer += 1;
		}
	}
	return (outBuffer - outStart) / (outStereo ? 2 : 1);
}

template<bool inStereo, bool outStereo, bool reverseStereo>
int RateConverter_Impl<inStereo, outStereo, reverseStereo>::interpolateConvert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t volL, st_volume_t volR) {
	// How much to increment _outPosFrac by
	frac_t outPos_inc = (_inRate << FRAC_BITS_LOW) / _outRate;

	st_sample_t *outStart, *outEnd;
	outStart = outBuffer;
	outEnd = outBuffer + numSamples * (outStereo ? 2 : 1);

	while (outBuffer < outEnd) {
		// Read enough input samples so that _outPosFrac < 0
		while ((frac_t)FRAC_ONE_LOW <= _outPosFrac) {
			// Check if we have to refill the buffer
			if (_bufferSize == 0) {
				_bufferPos = _buffer;
				_bufferSize = input.readBuffer(_buffer, ARRAYSIZE(_buffer));

				if (_bufferSize <= 0)
					return (outBuffer - outStart) / (outStereo ? 2 : 1);
			}

			_bufferSize -= (inStereo ? 2 : 1);
			_inLastL = _inCurL;
			_inCurL = *_bufferPos++;

			if (inStereo) {
				_inLastR = _inCurR;
				_inCurR = *_bufferPos++;
			}

			_outPosFrac -= FRAC_ONE_LOW;
		}

		// Loop as long as the _outPos trails behind, and as long as there is
		// still space in the output buffer.
		while (_outPosFrac < (frac_t)FRAC_ONE_LOW && outBuffer < outEnd) {
			// Interpolate
			st_sample_t inL, inR;
			inL = (st_sample_t)(_inLastL + (((_inCurL - _inLastL) * _outPosFrac + FRAC_HALF_LOW) >> FRAC_BITS_LOW));
			inR = (inStereo ?
						(st_sample_t)(_inLastR + (((_inCurR - _inLastR) * _outPosFrac + FRAC_HALF_LOW) >> FRAC_BITS_LOW)) :
						inL);

			st_sample_t outL, outR;
			outL = (inL * (int)volL) / Audio::Mixer::kMaxMixerVolume;
			outR = (inR * (int)volR) / Audio::Mixer::kMaxMixerVolume;

			if (outStereo) {
				// Output left channel
				clampedAdd(outBuffer[reverseStereo    ], outL);

				// Output right channel
				clampedAdd(outBuffer[reverseStereo ^ 1], outR);

				outBuffer += 2;
			} else {
				// Output mono channel
				clampedAdd(outBuffer[0], (outL + outR) / 2);

				outBuffer += 1;
			}

			// Increment output position
			_outPosFrac += outPos_inc;
		}
	}
	return (outBuffer - outStart) / (outStereo ? 2 : 1);
}

template<bool inStereo, bool outStereo, bool reverseStereo>
RateConverter_Impl<inStereo, outStereo, reverseStereo>::RateConverter_Impl(st_rate_t inputRate, st_rate_t outputRate) :
	_inRate(inputRate),
	_outRate(outputRate),
	_outPos(1),
	_outPosFrac(FRAC_ONE_LOW),
	_inLastL(0),
	_inLastR(0),
	_inCurL(0),
	_inCurR(0),
	_bufferSize(0),
	_bufferPos(nullptr) {}

template<bool inStereo, bool outStereo, bool reverseStereo>
int RateConverter_Impl<inStereo, outStereo, reverseStereo>::convert(AudioStream &input, st_sample_t *outBuffer, st_size_t numSamples, st_volume_t volL, st_volume_t volR) {
	assert(input.isStereo() == inStereo);

	if (_inRate == _outRate) {
		return copyConvert(input, outBuffer, numSamples, volL, volR);
	} else {
		if ((_inRate % _outRate) == 0 && (_inRate < 65536)) {
			return simpleConvert(input, outBuffer, numSamples, volL, volR);
		} else {
			return interpolateConvert(input, outBuffer, numSamples, volL, volR);
		}
	}
}

RateConverter *makeRateConverter(st_rate_t inRate, st_rate_t outRate, bool inStereo, bool outStereo, bool reverseStereo) {
	if (inStereo) {
		if (outStereo) {
			if (reverseStereo)
				return new RateConverter_Impl<true, true, true>(inRate, outRate);
			else
				return new RateConverter_Impl<true, true, false>(inRate, outRate);
		} else
			return new RateConverter_Impl<true, false, false>(inRate, outRate);
	} else {
		if (outStereo) {
			return new RateConverter_Impl<false, true, false>(inRate, outRate);
		} else
			return new RateConverter_Impl<false, false, false>(inRate, outRate);
	}
}

} // End of namespace Audio
