#!/bin/bash
#
# Builds the SGTM Automix AAX (Pro Tools) installer package: signed with a
# Developer ID, notarized by Apple, and stapled.
#
# Produces packaging/build/SGTM-Automix-<version>-AAX.pkg from an AAX bundle
# that has already been PACE-signed, notarized and stapled. This script does
# not sign the plugin itself: Pro Tools only loads an AAX signed with PACE's
# tools, which are not part of this repository.
#
# The AAX gets its own installer rather than a third choice in
# build_installer.sh because Pro Tools only scans
# /Library/Application Support/Avid/Audio/Plug-Ins. That is a system folder,
# so this package asks for an administrator password, while the AU/VST3
# installer stays a per-user install that needs none.
#
# Requires the Developer ID Installer certificate and a notarytool keychain
# profile -- see the "Code signing" section of BUILDING.md.
#
# Usage: ./packaging/build_aax_installer.sh [path/to/SGTM Automix.aaxplugin]

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# shellcheck source=packaging/signing.sh
source "$HERE/signing.sh"

AAX_BUNDLE="${1:-$REPO_ROOT/plugin/build-aax/SGTMAutomix_artefacts/Release/AAX/SGTM Automix.aaxplugin}"

BUILD_DIR="$HERE/build"
STAGE_DIR="$BUILD_DIR/stage-aax"

PKG_ID="com.sgtm.Automix.aax"

# Single source of truth for the version: the project() line in CMakeLists.
VERSION="$(sed -n 's/^project(SGTMAutomix VERSION \([0-9.]*\)).*/\1/p' \
    "$REPO_ROOT/plugin/CMakeLists.txt")"

if [[ -z "$VERSION" ]]; then
    echo "error: could not read the version from plugin/CMakeLists.txt" >&2
    exit 1
fi

if [[ ! -d "$AAX_BUNDLE" ]]; then
    echo "error: missing $AAX_BUNDLE" >&2
    echo "       Build the AAX target first -- see BUILDING.md." >&2
    exit 1
fi

echo "SGTM Automix $VERSION -- building AAX installer"

sgtm_require_signing_identities --with-installer
sgtm_check_no_stray_dylibs "$AAX_BUNDLE"

# Refuse an AAX that would not load: unsigned (no PACE signature), or signed
# but not notarized and stapled.
if [[ ! -d "$AAX_BUNDLE/Contents/__Pace_Eden.bundle" ]]; then
    echo "error: $AAX_BUNDLE is not PACE-signed" >&2
    exit 1
fi
codesign --verify --deep --strict "$AAX_BUNDLE"
xcrun stapler validate -q "$AAX_BUNDLE"

mkdir -p "$BUILD_DIR"
rm -rf "$STAGE_DIR" "$BUILD_DIR/resources-aax" "$BUILD_DIR/distribution-aax.xml" \
    "$BUILD_DIR/SGTMAutomix-AAX.pkg" "$BUILD_DIR/component-aax.plist"
mkdir -p "$STAGE_DIR"

cp -R "$AAX_BUNDLE" "$STAGE_DIR/"

# pkgbuild marks bundles relocatable by default, which lets the installer
# "update" any other copy of the bundle it finds on disk (a build folder,
# say) instead of writing to the install location. Pin it.
pkgbuild --analyze --root "$STAGE_DIR" "$BUILD_DIR/component-aax.plist" > /dev/null
/usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$BUILD_DIR/component-aax.plist"

pkgbuild \
    --quiet \
    --root "$STAGE_DIR" \
    --component-plist "$BUILD_DIR/component-aax.plist" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location "/Library/Application Support/Avid/Audio/Plug-Ins" \
    "$BUILD_DIR/SGTMAutomix-AAX.pkg"

echo "  built component package"

RESOURCES="$BUILD_DIR/resources-aax"
mkdir -p "$RESOURCES"

cat > "$RESOURCES/welcome.txt" <<TXT
SGTM Automix for Pro Tools (AAX)

SGTM Automix is a gain-sharing automixer for speech. Put one instance on
every talker's track; the instances find each other and keep the total gain
at one open microphone, so whoever is speaking comes up and the others duck.

This installer places the AAX plugin where Pro Tools looks for it:

  /Library/Application Support/Avid/Audio/Plug-Ins/SGTM Automix.aaxplugin

That is a system folder, so the installer asks for an administrator
password. The AU and VST3 versions are a separate download.

Requirements: macOS 13 or newer, on Apple Silicon or Intel, and Pro Tools.


Signing

This installer is signed with an Apple Developer ID (Sounds Good To Me AS)
and notarized by Apple, and the plugin is signed for Pro Tools with PACE.


After installing

Restart Pro Tools so it picks up the new plugin. SGTM Automix is listed under
Dynamics. Insert it on each speech track. Click the "?" button for the full
control reference.

SGTM Automix is free software under the AGPLv3, provided as-is with no
warranty of any kind. Source: https://github.com/okarlsen/sgtm-automix
TXT

BACKGROUND_XML=""
LOGO="$REPO_ROOT/plugin/Resources/sgtm_logo.png"
if [[ -f "$LOGO" ]]; then
    cp "$LOGO" "$RESOURCES/background.png"
    BACKGROUND_XML='<background file="background.png" alignment="bottomleft" scaling="proportional"/>'
fi

cat > "$BUILD_DIR/distribution-aax.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>SGTM Automix $VERSION for Pro Tools</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    $BACKGROUND_XML
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline>
        <line choice="aax"/>
    </choices-outline>
    <choice id="aax" title="AAX (Pro Tools)">
        <pkg-ref id="$PKG_ID"/>
    </choice>
    <pkg-ref id="$PKG_ID" version="$VERSION" onConclusion="none">SGTMAutomix-AAX.pkg</pkg-ref>
</installer-gui-script>
XML

FINAL_PKG="$BUILD_DIR/SGTM-Automix-$VERSION-AAX.pkg"

productbuild \
    --quiet \
    --distribution "$BUILD_DIR/distribution-aax.xml" \
    --package-path "$BUILD_DIR" \
    --resources "$RESOURCES" \
    --sign "$SGTM_INSTALLER_IDENTITY" \
    "$FINAL_PKG"

echo "  signed the installer package"

sgtm_notarize_artifact "$FINAL_PKG"
xcrun stapler staple "$FINAL_PKG"

# Tidy up the intermediates so only the shippable .pkg is left behind.
rm -rf "$STAGE_DIR" "$RESOURCES" "$BUILD_DIR/SGTMAutomix-AAX.pkg" \
    "$BUILD_DIR/distribution-aax.xml" "$BUILD_DIR/component-aax.plist"

echo
echo "Verification:"
pkgutil --check-signature "$FINAL_PKG" | sed 's/^/  /'
xcrun stapler validate "$FINAL_PKG" | sed 's/^/  /'
spctl -a -vvv -t install "$FINAL_PKG" 2>&1 | sed 's/^/  /'

echo
echo "Installer: $FINAL_PKG"
echo "Size:      $(du -h "$FINAL_PKG" | cut -f1)"
