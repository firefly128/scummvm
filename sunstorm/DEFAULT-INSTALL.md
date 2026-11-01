# SPARCstation 4 default installation, 2026-10-31

The installed ScummVM 2026.4.0 runtime is `/opt/sst/libexec/scummvm`.
`/opt/sst/bin/scummvm` is an ordinary 0755 console launcher;
`scummvm-run` is an alias. Hardware launches authenticate through Solaris's
existing `su root` password prompt. The former setuid game executable and
its obsolete installed backup were removed at the user's request.

The runtime has a private directory containing the tested SDL 2.32.10,
framebuffer/input, compatibility, MIDI-related, image and audio dependencies.
`ldd` resolves application-specific libraries there; only Solaris libraries
come from system directories. This is a clean matched installation, not a
claim that every upstream library was upgraded to its newest release.

The canonical configuration is the user's `.scummvmrc`; the old XDG config
path links to it. Existing saves and independent test saves were consolidated
in `.local/share/scummvm/saves`. Conflicting files were kept in imported
subdirectories rather than overwritten. The menu includes the SQ2 VGA remake,
working HQ settings, and the latest Indy text, centering and Euphony balance
options. Game exit returns to the launcher. Audio remains on rear port 6,
gain 152.

The fullscreen menu is 1152x900. Both the native manager and SDL window sizing
use the guardian-validated SS4 desktop rather than a temporary startup mode.
The diagnostic adapter must also be observational: v5 queries the existing
mode on framebuffer initialization and tracks real application mode changes.
Earlier v4 forced 640x480 and could crop the launcher or corrupt scanout
metadata. The local system-harness adapter source was corrected accordingly.

Hardware checks: full-menu TCX capture, EOB launch into centered 640x400 content
on a 640x480 display, return to the full menu, keyboard/console restoration,
removal of the display lease on exit, and `scummvm --version` as the normal
user. The menu is left open for use. Default binary POSIX checksum:
`280691172 27811140`.

Source and deployment records for the fixed console launcher are retained in
the Sunstorm workspace under `research/scummvm-2026.4-indy/default-install`.
No game assets, generated audio, private saves or credentials are committed.
