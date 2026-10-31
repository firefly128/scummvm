# Towns audio work in progress (2026-10-31)

Paused at the user's request while they develop a throughput test harness.
The remaining audible stutter is **not resolved**. These commits preserve
working changes and diagnostics for that next stage.

## Changes and validation

- Fixed-track Towns v1 Euphony exporter and optional resident PCM playback.
  The Indy FM-Towns resources 29, 40, 54 and 66 render deterministically;
  repeated native exports were byte-identical. Cache keys include resource
  content, output rate and track level. Missing variants fall back to synthesis.
  Loop boundaries, logical end, SFX volume/mute and original save fields are
  retained. No generated game audio is included in the repository.
- The resident bank uses roughly 15 MB. The original clothes-change fanfare
  caused a 1.62-second synthesis callback; cached playback removed that stall.
- Prepared PCM mixing builds voice lists once per block, avoiding repeated
  virtual checks for inactive voices. Original behavior handles terminal and
  degenerate loops and stopped-but-active channels. Sanitized randomized
  comparisons passed in value-table and bank modes: 40,000 blocks per mode,
  including 33,854 optimized blocks with identical samples and channel state.
  Typical real Berlin mixing windows fell from 40–44 ms to about 18–22 ms.
- SDL queue refill now queries actual queued bytes after mixing; the previous
  estimate ignored bytes consumed during mixing. Optional queue diagnostics
  report empty counts, pump gaps and mixing times. The queue depth environment
  setting can be adjusted while running by a diagnostic helper.
  A deterministic SDL-drain simulation passed under ASan and UBSan.
- Full Solaris SPARC cross-builds passed for the cache, PCM and queue changes.
  The legacy native headless Euphony build also passed before the final queue
  diagnostics; it does not use the SDL queue backend.

## Unresolved result

Berlin plays cached Euphony resource 66 alongside PCM resource 12. The user
still heard breakup after the PCM speedup. Hardware AUDIO_GETINFO error=0
was misleading: SDL's software queue was emptying. Increasing its target from
50 ms to 150 ms reduced frequent breakup to occasional breakup. A quiet
50-second run at 300 ms showed no empty reports, but this adds audio latency.
A 150 ms queue with diagnostic logs on tmpfs still emptied during a 228 ms
main-thread gap, so disk logging alone does not explain the problem.

The latest comparison uses direct SDL callback playback with the resident
cache, optimized PCM and tmpfs logs, to avoid main-thread queue feeding.
Its timed monitor was interrupted when the user paused work; do not count it
as a completed 135-second pass. No further runtime tests were performed for
this push. The default user launcher has not been switched to an unverified
final configuration.

## Runtime preferences and scope

Original saves and executable are preserved. Tests use independent save
folders. A copied motorcycle checkpoint and a separate debugger-created
Berlin checkpoint shorten reproduction. Peaceful checkpoint replies are
3, 3, 1, 1; offer the pass in Berlin.

The user's latest audio preference supersedes older notes: keep audio ON at
approximately 60%, through rear headphone and line-out outputs connected to
their mixer; internal speaker OFF. Do not toggle gain for tests. Do not issue
remote controls while the user is playing.

No binaries, game assets, generated audio, credentials or private saves are
included. The separate earlier AGS changes preserve the palette around video,
allow an opt-in AVI replacement for Theora, and avoid scanning known Ogg video
through unsupported fallback decoders. SQ2 gameplay was tested successfully;
its converted intro still has a known playback-performance limitation.
