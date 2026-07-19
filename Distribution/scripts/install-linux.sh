#!/usr/bin/env bash
#==============================================================================
# BASAMAK - Linux installer.  Run:  bash install-linux.sh
#
# Installs the VST3 for the current user and seeds the factory sample library.
# Your samples/sounds/presets live in ~/Documents/BASAMAK and are never
# touched, so this doubles as an in-place update.
#==============================================================================
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

VST3_DST="$HOME/.vst3"
DATA="$HOME/Documents/BASAMAK"
SAMPLES_DST="$DATA/Samples"
MULTI_DST="$DATA/Multisamples"   # [2026-07-19] factory multisample instruments

echo "==> Installing BASAMAK ..."
mkdir -p "$VST3_DST" "$SAMPLES_DST" "$DATA/Sound Bank" "$DATA/Presets"

NEW_VER="unknown"; [ -f "$HERE/VERSION" ] && NEW_VER="$(cat "$HERE/VERSION")"

# Replace any previous plugin binary (user data is elsewhere, left intact).
rm -rf "$VST3_DST/BASAMAK.vst3"
cp -R "$HERE/BASAMAK.vst3" "$VST3_DST/"

# Seed factory samples - only adds files that aren't already present.
if [ -d "$HERE/Samples" ]; then
  echo "==> Adding factory samples (existing files are kept) ..."
  cp -rn "$HERE/Samples/." "$SAMPLES_DST/" 2>/dev/null || true
fi
if [ -d "$HERE/Multisamples" ]; then
  echo "==> Adding factory multisample instruments (existing files are kept) ..."
  mkdir -p "$MULTI_DST"; cp -rn "$HERE/Multisamples/." "$MULTI_DST/" 2>/dev/null || true
fi

echo ""
echo "==> Done. Installed BASAMAK $NEW_VER"
echo "    VST3:    $VST3_DST/BASAMAK.vst3"
echo "    Library: $DATA"
echo ""
echo "Restart your DAW and rescan plugins."
