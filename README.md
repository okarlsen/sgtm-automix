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

Early development (0.1.0). Instances on the same computer find each other
and share gain, live and in offline bounces. Not released.

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
- **Bypass** puts the channel at unity gain and takes it out of the gain
  sharing, so the other channels share as if it were not there. It fades
  over 20 ms and follows the host's own bypass button.
- **No signal:** a channel whose level stays below −81 dBFS for a second
  (fader down, muted, nothing connected) drops out of the sharing and sits at
  unity, so it doesn't take a share from the others. It rejoins as soon as
  its level reaches −75 dBFS, fading in over 20 ms. An open mic's room tone
  sits above that, so quiet open mics still count. The channel list marks
  these channels NO SIGNAL.
- **Group** (A, B or C) picks which automix the channel belongs to. Gain is
  shared only among channels in the same group, so up to three automixes
  can run at once, for example one per panel. Changing it fades over 20 ms.
- **Automix on (all channels)** switches the automix off or on for every
  channel at once, for A/B comparison. Switching it on any instance switches
  them all. It is not saved with a session, and turns itself back on when a
  new session starts.
- Meters: **IN** input level, **GAIN** the automix gain, **OUT** output level.
- The channel list shows every running channel, sorted by group: its group,
  name, input level, gain (full at 0 dB, empty at −15 dB) and weight, with
  this instance highlighted. Click the name
  field to name the channel; left empty, it uses the host's track name where
  the host provides one.

All instances on the computer link up, in any host and any number of host
processes (up to 64 channels). Run one host session at a time while using
it: two hosts open at once would link with each other.

## Building from source

See [BUILDING.md](BUILDING.md).

## Verified so far

- VST3 builds on Linux; the engine tests, the VST3 smoke test and pluginval
  (strictness 10) pass there.
- macOS universal build: the engine and instance-link tests, the VST3
  smoke test and AU validation (`auval`) pass.
- Not yet verified: the instance link in real hosts (live and offline
  bounces), AAX in Pro Tools Developer, and the signing and packaging
  scripts.

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
