# Changelog

All notable changes to SGTM Automix are documented here. Versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

- Instance link: every SGTM Automix instance on the computer finds the
  others through shared memory and shares gain with them, also across host
  processes. Live, the audio thread never waits; offline bounces match
  sample positions exactly.
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
- IN and OUT meters show left and right separately on stereo tracks, and the channel list
  shows a left/right input pair for stereo channels.

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
