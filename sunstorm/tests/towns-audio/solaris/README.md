# Solaris diagnostic sources

Private test helpers, separate from ScummVM; no game assets or credentials.
SPARC cross-builds passed and these sources were used in the recorded tests.

- `audio-port.c`: reads routing; `audio-port rear` selects available headphone
  and line-out ports, excluding the internal speaker. Gain/mute are preserved.
  User preference: rear outputs on, approximately 60% (gain 153, reported 152).
- `audio-watch.c`: `audio-watch SECONDS [clear]` samples AUDIO_GETINFO at 10 Hz.
  `clear` resets the hardware error latch. A clear latch does not establish
  that SDL's software queue stayed filled. Counter gaps include observer
  scheduling delay. Write measurement logs to tmpfs.
- `control-keys.c`: optional SDL event shim for a diagnostic game. It consumes
  commands atomically renamed to `/var/tmp/tcx-play-control.command`.
  Commands include `debugger`, `text ...`, `enter`, `click X Y`, `quit`, and
  `queue-ms 50..1000`. The latter adjusts the running queue-depth environment
  setting, effective only in queued-audio mode. Coordinates differ between
  game and overlay. The existing TCX guardian handles display restoration.

Compile audio helpers with Solaris/SPARC C headers. The control shim also
needs SDL2 headers and dynamic-loader support and is built as a shared
library. These files do not start tests or alter the game by being present.
