#!/bin/bash
#
# Builds the SGTM Automix macOS installer package: signed with a Developer ID,
# notarized by Apple, and stapled.
#
# Produces packaging/build/SGTM-Automix-<version>.pkg from existing Release
# builds (see BUILDING.md): the AU and VST3 from plugin/build and the AAX from
# plugin/build-aax. Three pkgbuild components are combined with productbuild
# into a single installer that installs for all users of the Mac, into
# /Library/Audio/Plug-Ins and, for the AAX, the folder Pro Tools scans. Those
# are system folders, so the installer asks for an administrator password.
#
# The plugin bundles are signed, notarized and stapled *before* being staged
# into the package, so plugins installed from this .pkg carry their own
# notarization ticket and load without any network lookup. The finished .pkg
# is then signed with the Developer ID Installer certificate and notarized in
# its own right.
#
# Requires the Developer ID certificates and a notarytool keychain profile --
# see the "Code signing" section of BUILDING.md.
#
# The AAX must already be PACE-signed, notarized and stapled: Pro Tools only
# loads an AAX signed with PACE's tools, which are not part of this
# repository, and this script refuses an AAX that is not.
#
# Usage: ./packaging/build_installer.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# shellcheck source=packaging/signing.sh
source "$HERE/signing.sh"

ARTEFACTS="$REPO_ROOT/plugin/build/SGTMAutomix_artefacts/Release"
AU_BUNDLE="$ARTEFACTS/AU/SGTM Automix.component"
VST3_BUNDLE="$ARTEFACTS/VST3/SGTM Automix.vst3"
AAX_BUNDLE="${SGTM_AAX_BUNDLE:-$REPO_ROOT/plugin/build-aax/SGTMAutomix_artefacts/Release/AAX/SGTM Automix.aaxplugin}"

BUILD_DIR="$HERE/build"
STAGE_DIR="$BUILD_DIR/stage"

PKG_ID_BASE="com.sgtm.Automix"

# Single source of truth for the version: the project() line in CMakeLists.
VERSION="$(sed -n 's/^project(SGTMAutomix VERSION \([0-9.]*\)).*/\1/p' \
    "$REPO_ROOT/plugin/CMakeLists.txt")"

if [[ -z "$VERSION" ]]; then
    echo "error: could not read the version from plugin/CMakeLists.txt" >&2
    exit 1
fi

for bundle in "$AU_BUNDLE" "$VST3_BUNDLE" "$AAX_BUNDLE"; do
    if [[ ! -d "$bundle" ]]; then
        echo "error: missing $bundle" >&2
        echo "       Build the Release targets first -- see BUILDING.md." >&2
        exit 1
    fi
done

echo "SGTM Automix $VERSION -- building installer"

sgtm_require_signing_identities --with-installer
sgtm_check_no_stray_dylibs "$AU_BUNDLE" "$VST3_BUNDLE" "$AAX_BUNDLE"

# Refuse an AAX that would not load: unsigned (no PACE signature), or signed
# but not notarized and stapled. It is never re-signed here: a second
# codesign pass would strip the PACE signature.
if [[ ! -d "$AAX_BUNDLE/Contents/__Pace_Eden.bundle" ]]; then
    echo "error: $AAX_BUNDLE is not PACE-signed" >&2
    exit 1
fi
codesign --verify --deep --strict "$AAX_BUNDLE"
xcrun stapler validate -q "$AAX_BUNDLE"

# Sign, notarize and staple the plugins before they go into the package.
sgtm_prepare_bundles "$AU_BUNDLE" "$VST3_BUNDLE"

# Remove only this script's own outputs, not the whole build directory --
# build_zip.sh writes its zip here too, and blowing that away depending
# on which script ran last is a needless footgun.
rm -rf "$STAGE_DIR" "$BUILD_DIR/resources" "$BUILD_DIR/distribution.xml" \
    "$BUILD_DIR/SGTMAutomix-AU.pkg" "$BUILD_DIR/SGTMAutomix-VST3.pkg" \
    "$BUILD_DIR/SGTMAutomix-AAX.pkg" "$BUILD_DIR"/component-*.plist
mkdir -p "$STAGE_DIR/au" "$STAGE_DIR/vst3" "$STAGE_DIR/aax"

# pkgbuild wants a directory whose contents get copied into --install-location,
# so stage each bundle on its own.
cp -R "$AU_BUNDLE" "$STAGE_DIR/au/"
cp -R "$VST3_BUNDLE" "$STAGE_DIR/vst3/"
cp -R "$AAX_BUNDLE" "$STAGE_DIR/aax/"

# The distribution below enables only enable_localSystem, so the install
# locations are the system-wide folders, shared by every user of the Mac.
#
# pkgbuild marks bundles relocatable by default, which lets the installer
# "update" any other copy of the bundle it finds on disk (a build folder, or
# an older per-user install) instead of writing to the install location. Pin
# each one.
sgtm_build_component() {
    local name="$1" id="$2" location="$3" out="$4"
    pkgbuild --analyze --root "$STAGE_DIR/$name" "$BUILD_DIR/component-$name.plist" > /dev/null
    # The key is only present for some bundle types; set it or add it.
    /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$BUILD_DIR/component-$name.plist" 2>/dev/null \
        || /usr/libexec/PlistBuddy -c "Add :0:BundleIsRelocatable bool false" "$BUILD_DIR/component-$name.plist"
    pkgbuild \
        --quiet \
        --root "$STAGE_DIR/$name" \
        --component-plist "$BUILD_DIR/component-$name.plist" \
        --identifier "$id" \
        --version "$VERSION" \
        --install-location "$location" \
        "$out"
}

sgtm_build_component au "$PKG_ID_BASE.au" "/Library/Audio/Plug-Ins/Components" "$BUILD_DIR/SGTMAutomix-AU.pkg"
sgtm_build_component vst3 "$PKG_ID_BASE.vst3" "/Library/Audio/Plug-Ins/VST3" "$BUILD_DIR/SGTMAutomix-VST3.pkg"
sgtm_build_component aax "$PKG_ID_BASE.aax" "/Library/Application Support/Avid/Audio/Plug-Ins" "$BUILD_DIR/SGTMAutomix-AAX.pkg"

echo "  built component packages"

# Installer resources: the welcome text, plus the SGTM wordmark as background.
RESOURCES="$BUILD_DIR/resources"
mkdir -p "$RESOURCES"
cp "$HERE/welcome.txt" "$RESOURCES/welcome.txt"

BACKGROUND_XML=""
LOGO="$REPO_ROOT/plugin/Resources/sgtm_logo.png"
if [[ -f "$LOGO" ]]; then
    cp "$LOGO" "$RESOURCES/background.png"
    BACKGROUND_XML='<background file="background.png" alignment="bottomleft" scaling="proportional"/>'
fi

cat > "$BUILD_DIR/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>SGTM Automix $VERSION</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    $BACKGROUND_XML
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
        <line choice="aax"/>
    </choices-outline>
    <choice id="au" title="Audio Unit (AU)"
            description="Installs SGTM Automix.component into /Library/Audio/Plug-Ins/Components. For Logic Pro, MainStage and other AU hosts.">
        <pkg-ref id="$PKG_ID_BASE.au"/>
    </choice>
    <choice id="vst3" title="VST3"
            description="Installs SGTM Automix.vst3 into /Library/Audio/Plug-Ins/VST3. For Cubase, Nuendo, Reaper and other VST3 hosts.">
        <pkg-ref id="$PKG_ID_BASE.vst3"/>
    </choice>
    <choice id="aax" title="AAX (Pro Tools)"
            description="Installs SGTM Automix.aaxplugin into /Library/Application Support/Avid/Audio/Plug-Ins. For Pro Tools.">
        <pkg-ref id="$PKG_ID_BASE.aax"/>
    </choice>
    <pkg-ref id="$PKG_ID_BASE.au" version="$VERSION" onConclusion="none">SGTMAutomix-AU.pkg</pkg-ref>
    <pkg-ref id="$PKG_ID_BASE.vst3" version="$VERSION" onConclusion="none">SGTMAutomix-VST3.pkg</pkg-ref>
    <pkg-ref id="$PKG_ID_BASE.aax" version="$VERSION" onConclusion="none">SGTMAutomix-AAX.pkg</pkg-ref>
</installer-gui-script>
XML

FINAL_PKG="$BUILD_DIR/SGTM-Automix-$VERSION.pkg"

# Only the final combined product is signed; the three component packages above
# are intermediates that get embedded into it, so signing them buys nothing.
productbuild \
    --quiet \
    --distribution "$BUILD_DIR/distribution.xml" \
    --package-path "$BUILD_DIR" \
    --resources "$RESOURCES" \
    --sign "$SGTM_INSTALLER_IDENTITY" \
    "$FINAL_PKG"

echo "  signed the installer package"

sgtm_notarize_artifact "$FINAL_PKG"
xcrun stapler staple "$FINAL_PKG"

# Tidy up the intermediates so only the shippable .pkg is left behind.
rm -rf "$STAGE_DIR" "$RESOURCES" \
    "$BUILD_DIR/SGTMAutomix-AU.pkg" "$BUILD_DIR/SGTMAutomix-VST3.pkg" \
    "$BUILD_DIR/SGTMAutomix-AAX.pkg" "$BUILD_DIR"/component-*.plist \
    "$BUILD_DIR/distribution.xml"

echo
echo "Verification:"
pkgutil --check-signature "$FINAL_PKG" | sed 's/^/  /'
xcrun stapler validate "$FINAL_PKG" | sed 's/^/  /'
spctl -a -vvv -t install "$FINAL_PKG" 2>&1 | sed 's/^/  /'

echo
echo "Installer: $FINAL_PKG"
echo "Size:      $(du -h "$FINAL_PKG" | cut -f1)"
