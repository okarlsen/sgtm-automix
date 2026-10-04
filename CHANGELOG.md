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
- IN and OUT meters show left and right separately on stereo tracks.

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
