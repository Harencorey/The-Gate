#!/bin/bash
# Build TheGate-Installer.pkg from the built AU + VST3.
#   make_pkg.sh <Release artefacts dir> <version> <output dir>
set -euo pipefail
ART="$1"; VERSION="$2"; OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
mkdir -p "$OUT"

make_component () {   # name  source-bundle  install-location  identifier  [scripts-dir]
    local name="$1" src="$2" loc="$3" id="$4"
    mkdir -p "$WORK/$name-root"
    ditto "$src" "$WORK/$name-root/$(basename "$src")"
    # Never "relocate": always install to the stated folder, even if another copy exists elsewhere.
    pkgbuild --analyze --root "$WORK/$name-root" "$WORK/$name.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/$name.plist"
    pkgbuild --root "$WORK/$name-root" --component-plist "$WORK/$name.plist" \
             --install-location "$loc" --identifier "$id" --version "$VERSION" \
             ${5:+--scripts "$5"} "$WORK/$name.pkg"
}

make_component au   "$ART/AU/The Gate.component" "/Library/Audio/Plug-Ins/Components" "com.norkadsp.thegate.au" "$HERE/scripts"
make_component vst3 "$ART/VST3/The Gate.vst3"    "/Library/Audio/Plug-Ins/VST3"       "com.norkadsp.thegate.vst3"

cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>The Gate ${VERSION} (Norka DSP)</title>
    <welcome mime-type="text/plain"><![CDATA[The Gate source-aware gate, version ${VERSION}.

This installs the Audio Unit and VST3 into /Library/Audio/Plug-Ins for all users. Older copies are replaced.
Rescan plug-ins in your DAW afterwards and look for Norka DSP > The Gate.
The Standalone app (for live use) is a separate download on the release page.]]></welcome>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
    </choices-outline>
    <choice id="au" title="Audio Unit (Logic, GarageBand, Ableton, ...)"><pkg-ref id="com.norkadsp.thegate.au"/></choice>
    <choice id="vst3" title="VST3 (Ableton, Reaper, Cubase, Studio One, ...)"><pkg-ref id="com.norkadsp.thegate.vst3"/></choice>
    <pkg-ref id="com.norkadsp.thegate.au" version="${VERSION}">au.pkg</pkg-ref>
    <pkg-ref id="com.norkadsp.thegate.vst3" version="${VERSION}">vst3.pkg</pkg-ref>
</installer-gui-script>
XML

productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$OUT/TheGate-Installer.pkg"
echo "built $OUT/TheGate-Installer.pkg"
