# SGTM Automix

A gain-sharing automixer for speech: panels, talk shows, conferences. Put
one instance on every talker's track. The instances find each other and keep
the total gain at one open microphone, so whoever is speaking comes up
instantly and the others duck, with no thresholds to set and no added
latency.

It runs live (MainStage, Logic, Cubase) with 0 samples of reported latency,
and is designed to give the same result in faster-than-real-time offline
bounces of the whole mix as in playback.

Built by SGTM on top of [JUCE](https://juce.com).

## Status

Version 1.0.0, the first release. Instances on the same computer find each
other and share gain, live and in offline bounces. Downloads are on the
[releases page](https://github.com/okarlsen/sgtm-automix/releases): an
installer with the AU, VST3 and AAX (Pro Tools), and a zip with the AU and
VST3 for installing by hand.

## Requirements

- macOS 13 (Ventura) or newer, on Apple Silicon or Intel.
- A host supporting AU, VST3 or AAX.
- Mono or stereo tracks.

## Using it

Insert SGTM Automix on each speech track, after EQ and before any
compressor (compression flattens the level differences it relies on).
It is meant to be inserted post-fader, and works best with one microphone
per talker.
Put it on the talkers' own tracks, not on a bus or master that carries their
sum in the same group: the bus would take a large share and turn every track
down further.

- **Weight** sets a channel's priority. It changes how loud the channel looks
  to the automixer, not its audio level. Balance the weights so all GAIN
  meters read about the same when nobody is talking.
- **Output** is a plain output trim.
- **Bypass** passes the channel's audio through unchanged (Output trim
  included) and takes it out of the gain sharing, so the other channels share
  as if it were not there. It fades over 20 ms and follows the host's own
  bypass button.
- The automixer judges levels in the voice band only (150 Hz to 5 kHz), so
  stage rumble, handling noise and hiss don't take a share. The audio itself
  is not filtered.
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
- Meters: **IN** input level and **OUT** output level (left and right on a
  stereo track), **GAIN** the gain the channel lets through. One gain applies
  to both sides of a stereo track, set from the level of both sides together,
  so the stereo image does not shift.
- The channel list shows every running channel, sorted by group and then
  by name: its group,
  name, input level, gain (full at 0 dB, empty at −15 dB) and weight, with
  this instance highlighted. Click the name
  field to name the channel; left empty, it uses the host's track name where
  the host provides one.
- The window can be resized from its bottom-right corner; the channel list
  takes the extra height.

All instances on the computer link up, in any host and any number of host
processes (up to 64 channels). Run one host session at a time while using
it: two hosts open at once would link with each other.

Bounce or export the whole mix. Bouncing in place, exporting or freezing a
single track renders that track without the other channels, so the automix
is not applied to it. Offline bounces are calculated sample-exactly across
all channels when the host renders the tracks together.

## Building from source

See [BUILDING.md](BUILDING.md).

## Verified

- VST3 builds on Linux; the engine tests, the VST3 smoke test and pluginval
  (strictness 10) pass there.
- macOS universal build: the engine and instance-link tests, the VST3
  smoke test and AU validation (`auval`) pass.
- Used live in Logic and LiveProfessor on macOS (AU), with linked
  instances, groups and the channel list.
- The AAX passes Avid's AAX Validator (description, parameters, data model,
  load and unload).

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
