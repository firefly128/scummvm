# Towns audio work in progress (2026-10-31)

Development resumed through the diagnostic harness. The remaining audible
stutter is **not resolved**. Tests use private game cases and preserved saves.

## Changes and validation

- Fixed-track Towns v1 Euphony exporter and optional resident PCM playback.
  The Indy FM-Towns resources 29, 40, 54 and 66 render deterministically;
  repeated native exports were byte-identical. Cache keys include resource
  content, output rate and track level. Missing variants fall back to synthesis.
  Loop boundaries, logical end, volume/mute and original save fields are
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
their mixer; internal speaker OFF. Do not toggle gain for tests. The user has authorized taking control and restarting for these tests.

No binaries, game assets, generated audio, credentials or private saves are
included. The separate earlier AGS changes preserve the palette around video,
allow an opt-in AVI replacement for Theora, and avoid scanning known Ogg video
through unsupported fallback decoders. SQ2 gameplay was tested successfully;
its converted intro still has a known playback-performance limitation.

## Harness candidate, 2026-10-31

The next candidate preserves background indices while masking and mapping the
separate 16-color Towns text layer to the active 256-color palette. It also
classifies v1 FM/Euphony music as music and reserved PCM effect channels as SFX;
cached Euphony now follows music volume/mute. The private test configuration uses
music 128 and SFX 255, rear outputs port 6, hardware gain 152. The Berlin
item-offer text is readable in the hardware capture; broader dialogue color
coverage remains to be checked.

The PCM mixer additionally batches held samples across the actual Towns
19300 Hz rational clock. Sanitized randomized comparisons passed 80,000 blocks
per cache mode, including 67,916 optimized blocks per mode. Six SPARC benchmark
cases retained identical checksums: one low-rate voice saved 59% CPU, two saved
47%, and four-voice fallback was effectively unchanged. These are component
measurements, not whole-game speed claims.

The full candidate cross-build and verified harness deployment passed. A fresh
35-second motorcycle capture had no software queue empty observations and no
new hardware underrun flag. A quiet 45-second Berlin signing replay still set
the hardware underrun flag and eventually observed two empty queues. Device
writes paused for 264 ms at the underrun while mixer calls were much shorter
(median 14.1 ms, maximum 70.3 ms; measured mutex waits <=12 us). ScummVM's
software queue was still nonempty at that hardware underrun. Audio-device
feeding and process scheduling therefore require further investigation.
The normal user launcher is still unchanged. No RT scheduling is enabled.

A comparison with the existing SDL_AUDIO_PRIOCNTL=ts option confirmed that
Solaris accepted ts_upri=60 for the audio LWP, but the capture still set the
hardware underrun flag. That startup-overlapping run is not counted as a clean
controlled signing comparison. Normal inherited scheduling was restored.

## CD / Euphony balance

The user reported Euphony much louder than CD even after routing both through
music volume. A read-only SPARC WAV diagnostic sampled 32 windows per CD track:
track 01 RMS 2967, track 02 RMS 2358, track 08 RMS 2496, track 16 RMS 3668
(on the signed 16-bit scale). Cached Berlin resource 66 has RMS 3051 at its
stored render gain of 64. Restoring music volume 128 multiplies that by two,
while a full-channel CD at music 128 is multiplied by approximately one half.
Thus the shared slider does not itself balance the two sources. RMS is a level
comparison, not a perceptual loudness or original hardware calibration claim.

Added optional per-game `sst_towns_euphony_volume`, an attenuation factor from
0 to 256 (default 256, unchanged gain). It applies before clipping to both
cached Euphony and live FM fallback, independently of CD and PCM effects.
The Indy test case uses 64, a quarter amplitude / approximately -12 dB. This
puts Berlin resource 66 around RMS 1525 at music 128, near the sampled CD
range of approximately 1179–1834 at that setting, assuming full CD channel
volume. Source files and v2 cache format are unchanged. Game-controlled track
levels and differences between compositions remain intact. The exporter now
also explicitly initializes both music and SFX mixer categories to full level.

Production-code sanitizer tests cover unity, quarter and zero attenuation,
negative full scale, music mute, SFX independence and clipping. The Solaris
cross-build passed; hardware deployment and gain validation are recorded in
the local diagnostic case rather than including copyrighted audio here.
