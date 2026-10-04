# Changelog

All notable changes to SGTM Automix are documented here. Versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

- Instance link: every SGTM Automix instance on the computer finds the
  others through shared memory and shares gain with them, also across host
  processes. Live, the audio thread never waits; offline bounces of the
  whole mix match sample positions exactly when the host renders the tracks
  together.
- Channel list in the editor: every running channel with its name, input
  level, automix gain and weight. Channels can be named.
- Bypass per channel (also the host's bypass) and an all-channels automix
  on/off switch for A/B comparison.
- Groups A, B and C: gain is shared only within a group, so three automixes
  can run at once.
- Gain meters show the gain each channel lets through, 0 to −15 dB.
- Channels without signal (below −81 dBFS for a second; back in at −75 dBFS)
  take no share, so a faded-down channel doesn't turn the others down.
- Bypass (also the host's) now passes audio unchanged: the Output trim fades
  to 0 dB too.
- AAX prepared (untested, no Pro Tools build yet): multi-mono, AudioSuite
  and Pro Tools' dynamic plug-in processing are turned off.
- The level detector listens to the voice band only (150 Hz to 5 kHz
  band-pass on its copy; the audio is untouched), so rumble and hiss take no
  share and a channel with only rumble counts as no signal.
- Detector attack 15 ms (was 5 ms) for a softer fade-up on word starts.
- IN and OUT meters show left and right separately on stereo tracks, and the
  channel list shows a left/right input pair for stereo channels. Level
  meters run from 0 to −80 dBFS.
- Group selector coloured by group, matching the channel list.
- Resizable window: the channel list takes the extra height.
- Channel list sorted by group, then by name (numbers in numeric order).
- Help window ("?" in the title bar) with a control reference and the
  disclaimer.
- Fixes from the 2026-10-04 review:
  - A track the host stops running (Logic after a region ends) no longer
    holds the others down: the plug-in reports a 1 s tail so it keeps
    running until its level has decayed, and a channel that stops anyway
    fades out of the sharing over 200 ms instead of dropping out at once.
  - "Automix on (all channels)" no longer switches itself back on when a
    channel is added while the transport is stopped.
  - No gain overshoot (or polarity flip) after a jump in position in the
    middle of a gain change; every host now starts an offline bounce from a
    fresh engine state.
  - Offline: the wait back-off starts over for each bounce and after a peer
    arrives in time, so one slow moment no longer costs exactness for the
    rest of the session.
  - The channel list shows a group change at once, also while the
    transport is stopped.
  - Long channel names are no longer cut in the middle of a letter.
  - Help and README: bounce the whole mix; single-track bounces, exports
    and freezes are rendered without the other channels.

## [0.1.0] — development skeleton (not released)

- JUCE 9.0.3 project building AU, VST3 and AAX (AAX unsigned, so it loads
  only in Pro Tools Developer).
- SGTM Automix: mono or stereo insert with 0 samples latency, Weight and
  Output Gain parameters, and input / automix gain / output meters. The
  gain-sharing engine is in place, but instances do not see each other yet,
  so each one runs solo at 0 dB automix gain.
- SGTM Host Probe: development plugin that logs each host's processBlock
  scheduling per instance, plus `tools/analyse_probe.py` to summarise the logs.
- Verification: `SGTMAutomixVerify` (engine) and `SGTMAutomixSmokeTest`
  (loads the built VST3s like a host).
