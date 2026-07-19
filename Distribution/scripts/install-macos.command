#!/bin/bash
#==============================================================================
# BASAMAK - macOS installer.  Double-click this file in Finder to run.
#
# Installs the VST3 + Audio Unit for the current user and seeds the factory
# sample library. Your own samples, sounds and presets are never touched
# (they live in ~/Documents/BASAMAK and are independent of the plugin binary),
# so this also works as an in-place update.
#==============================================================================
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

VST3_DST="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DST="$HOME/Library/Audio/Plug-Ins/Components"
DATA="$HOME/Documents/BASAMAK"
SAMPLES_DST="$DATA/Samples"
MULTI_DST="$DATA/Multisamples"   # [2026-07-19] factory multisample instruments

echo "==> Installing BASAMAK ..."
mkdir -p "$VST3_DST" "$AU_DST" "$SAMPLES_DST" "$DATA/Sound Bank" "$DATA/Presets"

# Report version change if a previous copy is installed (informational only).
NEW_VER="unknown"; [ -f "$HERE/VERSION" ] && NEW_VER="$(cat "$HERE/VERSION")"
OLD_PLIST="$VST3_DST/BASAMAK.vst3/Contents/Info.plist"
if [ -f "$OLD_PLIST" ]; then
  OLD_VER="$(defaults read "$OLD_PLIST" CFBundleShortVersionString 2>/dev/null || echo unknown)"
  echo "    Replacing installed version $OLD_VER with $NEW_VER"
fi

# Remove the old plugin binaries (user data is elsewhere and is left intact).
rm -rf "$VST3_DST/BASAMAK.vst3" "$AU_DST/BASAMAK.component"

cp -R "$HERE/BASAMAK.vst3"      "$VST3_DST/"
cp -R "$HERE/BASAMAK.component" "$AU_DST/"

# The downloaded binaries are not notarized; clear the quarantine flag so macOS
# lets the DAW load them.
xattr -dr com.apple.quarantine "$VST3_DST/BASAMAK.vst3"      2>/dev/null || true
xattr -dr com.apple.quarantine "$AU_DST/BASAMAK.component"   2>/dev/null || true

# Seed factory samples - only ADDS files that aren't already there, so anything
# you have added yourself is preserved.
if [ -d "$HERE/Samples" ]; then
  echo "==> Adding factory samples (existing files are kept) ..."
  cp -Rn "$HERE/Samples/." "$SAMPLES_DST/" 2>/dev/null || true
fi
if [ -d "$HERE/Multisamples" ]; then
  echo "==> Adding factory multisample instruments (existing files are kept) ..."
  mkdir -p "$MULTI_DST"; cp -Rn "$HERE/Multisamples/." "$MULTI_DST/" 2>/dev/null || true
fi

echo ""
echo "==> Done. Installed BASAMAK $NEW_VER"
echo "    VST3: $VST3_DST/BASAMAK.vst3"
echo "    AU:   $AU_DST/BASAMAK.component"
echo "    Library: $DATA"
echo ""
echo "Restart your DAW and rescan plugins. You can close this window."
