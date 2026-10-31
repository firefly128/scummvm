# Host audio checks

Run `python3 sunstorm/tests/towns-audio/run.py` from the repository root.
Requires a C++11 compiler with AddressSanitizer and UndefinedBehaviorSanitizer.
The runner compiles current production code in a temporary directory; only
platform/file-cache dependencies are stubbed. It needs no game assets and
does not contact the Sun.

PCM tests compare optimized samples and voice state with the original mixer
across 80,000 randomized blocks in each cache mode. Queue tests simulate audio
draining while mixing, live target changes, a slow producer and disabled queue
mode. Text compositor checks cover separate palettes, color flag bits,
transparency, scaling and scrolling. Euphony stream checks cover music volume,
independent attenuation, mute, SFX independence and clipping. These checks do not establish real-time scheduling or audible quality;
see ../../TOWNS-AUDIO-WIP.md for unresolved hardware findings.
