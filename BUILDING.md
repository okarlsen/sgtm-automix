# Building SGTM Automix from source

SGTM Automix builds on macOS and produces an AU component, a VST3 bundle and
an AAX plugin, as a universal binary (Apple Silicon + Intel). The VST3 also
builds on Linux and Windows, which is handy for running the tests.

## Prerequisites

- **Xcode Command Line Tools**: `xcode-select --install`
- **CMake** 3.22 or newer and **Ninja**:

  ```sh
  brew install cmake ninja
  ```

## Get the source

JUCE is a git submodule, so clone recursively:

```sh
git clone --recurse-submodules https://github.com/okarlsen/sgtm-automix.git
cd sgtm-automix
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init
```

## Build the plugins

```sh
cd plugin
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

This builds everything: SGTM Automix in every format, the SGTM Host Probe
development plugin, and the two test programs. To build only what you need:

```sh
cmake --build build --target SGTMAutomix_AU SGTMAutomix_VST3 -j8
```

The build products land in:

```
plugin/build/SGTMAutomix_artefacts/Release/AU/SGTM Automix.component
plugin/build/SGTMAutomix_artefacts/Release/VST3/SGTM Automix.vst3
plugin/build/SGTMAutomix_artefacts/Release/AAX/SGTM Automix.aaxplugin
```

`COPY_PLUGIN_AFTER_BUILD` is enabled, so they are also copied to
`~/Library/Audio/Plug-Ins/Components`, `~/Library/Audio/Plug-Ins/VST3` and
`/Library/Application Support/Avid/Audio/Plug-Ins` at the end of the build.
If that last folder is not writable for you, the AAX copy step fails; either
make it writable, or configure with `-DSGTM_BUILD_AAX=OFF`. A DAW may need a
rescan to pick up a newly built version.

Configure options:

| Option | Default | Effect |
|---|---|---|
| `SGTM_BUILD_AAX` | ON on macOS/Windows | Build the AAX format |
| `SGTM_BUILD_PROBE` | ON | Build SGTM Host Probe |
| `CMAKE_OSX_ARCHITECTURES` | `arm64;x86_64` | Use `arm64` for a faster local build |

### AAX and Pro Tools

The AAX builds from the AAX SDK that ships inside JUCE, so nothing extra
needs downloading. It is not PACE-signed, so retail Pro Tools will not load
it. For development, use **Pro Tools Developer**, which loads unsigned
plugins and needs a free Avid developer account. Shipping AAX needs Avid's
PACE signing tools (wraptool and an iLok signing certificate), requested
from Avid.

To build and install an unsigned AAX for testing in Pro Tools Developer
(development only; never package or publish it):

```sh
cd plugin
cmake -B build-aax -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSGTM_BUILD_AAX=ON -DSGTM_BUILD_PROBE=OFF
cmake --build build-aax --target SGTMAutomix_AAX -j8
```

The build copies `SGTM Automix.aaxplugin` into
`/Library/Application Support/Avid/Audio/Plug-Ins`. If that folder is not
writable for you, copy it there yourself with
`sudo cp -R "build-aax/SGTMAutomix_artefacts/Release/AAX/SGTM Automix.aaxplugin" "/Library/Application Support/Avid/Audio/Plug-Ins/"`.
Retail Pro Tools scans the same folder and will report the unsigned plugin
as invalid; remove it from there when you are done testing.

The AAX is set up for automixing, untested until it runs in Pro Tools:
multi-mono is off (a stereo track gets one instance and one gain), AudioSuite
is off (it processes a clip with no other channels to share with), and Pro
Tools' dynamic plug-in processing is off (silent tracks keep running, so a
bounce never waits on them). See `plugin/CMakeLists.txt`.

## Verifying a build

Two test programs, both built by default:

```sh
./build/SGTMAutomixVerify
./build/SGTMAutomixSmokeTest_artefacts/Release/SGTMAutomixSmokeTest
```

- **SGTMAutomixVerify** drives the automix engine directly: the gain law
  on known cases (4 equal mics at −6 dB each, one mic 20 dB
  louder at about 0 dB with the others at about −20 dB, two loud mics at
  −3 dB, weight, silence), bit-exact pass-through when solo, and identical
  output for any block size. It also runs several channels through the
  instance link: live groups, an offline bounce on 4 threads with random
  block sizes that must match an exact reference bit for bit (twice), tracks
  rendered serially on one thread without stalling, groups (independent
  sharing, a lone channel per group, moving between groups, and an offline
  bounce with two groups against an exact reference), channels without
  signal (not diluting the others, waking up smoothly, hysteresis, ten open
  mics at room tone each at −10 dB), bypass, the
  all-channels switch, peers dropping out, and a crashed peer process.
- **SGTMAutomixSmokeTest** loads the built VST3s the way a host does and
  checks 0 samples latency, pass-through, the Output Gain parameter, state
  save/restore, the editor, two linked instances sharing gain and Bypass,
  and that the probe logs every block. It links its instances under a
  private name, so a host running at the same time is not disturbed.

Both must end with `ALL TESTS PASS`.

For the AU, Apple's own validation tool should also succeed:

```sh
auval -v aufx Smix Sgtm
```

Look for `AU VALIDATION SUCCEEDED`.

## Running the host probe

SGTM Host Probe logs, per instance, every processBlock call: wall-clock time,
thread, timeline position, block size and the offline flag. It shows how a
host schedules tracks against each other, which the instance link has to
cope with.

1. Put **SGTM Host Probe** on 4 or more tracks.
2. Play back with no track record-enabled; play again with one track
   record-enabled or input-monitored; then do an offline bounce. In
   MainStage, play with live inputs.
3. Summarise the logs:

   ```sh
   python3 tools/analyse_probe.py --last-minutes 30
   ```

Logs are in `~/Library/Logs/SGTM Host Probe/`, one CSV per instance (the
probe window has an "Open log folder" button). Delete old logs between
hosts, or filter with `--pid`.

## Linux (tests only)

```sh
sudo apt install cmake ninja-build g++ libasound2-dev libx11-dev libxrandr-dev \
    libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev libxi-dev \
    libfreetype-dev libfontconfig-dev libgl-dev libgtk-3-dev
cd plugin
cmake -B build -S . -G Ninja
cmake --build build -j8
./build/SGTMAutomixVerify
xvfb-run -a ./build/SGTMAutomixSmokeTest_artefacts/Release/SGTMAutomixSmokeTest
```

## Code signing

A plain local build is ad-hoc signed, which is fine for development on the
machine that built it. Released artifacts are signed with an Apple Developer
ID and notarized by Apple; the packaging scripts do that automatically.

### What a release needs

Two certificates in the login keychain, both from the same team
(`ZVP9U3LWAJ`, "Sounds Good To Me AS"):

| Certificate | Signs |
| --- | --- |
| Developer ID **Application** | the `.component` and `.vst3` bundles |
| Developer ID **Installer** | the final `.pkg` |

Check they are present with `security find-identity -v -p basic` (the
Installer certificate does not show up under `-p codesigning`).

Plus a `notarytool` keychain profile, created once:

```sh
xcrun notarytool store-credentials notarytool \
    --apple-id <apple-id> --team-id ZVP9U3LWAJ --password <app-specific-password>
```

The identities and profile can be overridden with the `SGTM_SIGN_IDENTITY`,
`SGTM_INSTALLER_IDENTITY` and `SGTM_NOTARY_PROFILE` environment variables.

The scripts work exactly as in Less PA: `packaging/signing.sh` checks the
bundles link nothing outside `/System/Library` and `/usr/lib`, signs them
with the hardened runtime and no entitlements, notarizes both in a single
submission and staples each. That step is idempotent, so the two packaging
scripts can run in either order.

## Building a release

With a completed Release build in place:

```sh
./packaging/build_installer.sh   # packaging/build/SGTM-Automix-<version>.pkg
./packaging/build_zip.sh         # packaging/build/SGTM-Automix-<version>.zip
```

The `.pkg` installs the AU and VST3 into the current user's
`~/Library/Audio/Plug-Ins`, no administrator password needed. The `.zip`
holds the same two bundles plus `INSTALL.txt`. The version comes from the
`project()` line in `plugin/CMakeLists.txt`. Releases are tagged
`v<version>` and published on the GitHub releases page with both files
attached, with the notes taken from `CHANGELOG.md`.

AAX is not in the release downloads until the plugin can be PACE-signed. The
host probe is a development tool and is never packaged.

`COPY_PLUGIN_AFTER_BUILD` copies ad-hoc signed builds into your plug-in
folders; re-copy from `plugin/build/SGTMAutomix_artefacts/Release/` after
running a packaging script to test the signed build locally.
