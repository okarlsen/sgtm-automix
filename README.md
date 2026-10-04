# SGTM Automix

A gain-sharing automixer for speech: panels, talk shows, conferences. Put
one instance on every talker's track. The instances find each other and keep
the total gain at one open microphone, so whoever is speaking comes up
instantly and the others duck, with no thresholds to set and no added
latency.

It runs live (MainStage, Logic, Cubase) with 0 samples of reported latency,
and gives the same result in faster-than-real-time offline bounces (Pro
Tools, Cubase, Logic).

Built by SGTM on top of [JUCE](https://juce.com).

## Status

Early development (0.1.0). The plugin builds and passes audio, and the
automix engine is in place, but instances do not see each other yet, so each
one runs solo at 0 dB automix gain. Not released.

## Requirements

- macOS 13 (Ventura) or newer, on Apple Silicon or Intel.
- A host supporting AU, VST3 or AAX.
- Mono or stereo tracks.

## Using it

Insert SGTM Automix on each speech track, after EQ and before any
compressor (compression flattens the level differences it relies on).

- **Weight** sets a channel's priority. It changes how loud the channel looks
  to the automixer, not its audio level. Balance the weights so all GAIN
  meters read about the same when nobody is talking.
- **Output** is a plain output trim.
- Meters: **IN** input level, **GAIN** the automix gain, **OUT** output level.

## Building from source

See [BUILDING.md](BUILDING.md).

## Verified so far

- VST3 builds on Linux; the engine tests, the VST3 smoke test and pluginval
  (strictness 10) pass there.
- Not yet verified: the macOS build (universal binary), AU validation, AAX
  in Pro Tools Developer, and the signing and packaging scripts.

## License

SGTM Automix is free to use, provided as-is with no warranty of any kind —
see [LICENSE](LICENSE) (AGPLv3). Third-party components are credited in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Licensing notes:

- **JUCE** is dual-licensed: AGPLv3, or a commercial JUCE licence. This
  project uses the AGPLv3 terms. A closed-source release would need a
  commercial JUCE licence instead.
- **AAX** is built from the AAX SDK bundled with JUCE (GPLv3 or Avid's
  licence). Shipping AAX to retail Pro Tools also needs an Avid developer
  account and PACE signing, so AAX is not in the release downloads yet.
- **VST3 SDK**: MIT licence, bundled with JUCE.

Copyright © 2026 SGTM.
