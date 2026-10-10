# Changelog

All notable changes to SGTM Automix are documented here. Versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.0] — 2026-10-09

First release: one installer with the AU, VST3 and AAX (Pro Tools),
installed for all users, plus a zip with the AU and VST3. Universal for
Apple Silicon and Intel, signed and notarized.

- Automatic linking: every SGTM Automix on the computer finds the others and
  shares gain with them, in any host and across host processes, up to 64
  channels. Live, the audio thread never waits; offline bounces of the whole
  mix match sample positions exactly when the host renders the tracks
  together.
- Channel list: every running channel in one window, sorted by group and
  name, with its input level, gain and weight. Channels can be named, and
  the window can be resized.
- Groups A, B and C: gain is shared only within a group, so three automixes
  can run at once.
- Weight per channel, to give a talker priority, and an Output trim.
- Bypass per channel (also the host's bypass), passing audio unchanged, and
  an all-channels automix on/off switch for A/B comparison.
- The level detector listens to the voice band only (150 Hz to 5 kHz); the
  audio itself is not filtered. Rumble, handling noise and hiss take no
  share.
- Channels without signal (below −81 dBFS for a second; back in at −75 dBFS)
  take no share, so a faded-down channel doesn't turn the others down.
- Mono and stereo tracks, with one gain for both sides of a stereo track.
  IN and OUT meters show left and right on stereo tracks; the GAIN meter
  shows the gain each channel lets through.
- Help window ("?" in the title bar) with a control reference and the
  disclaimer.
- Zero added latency in every format.
