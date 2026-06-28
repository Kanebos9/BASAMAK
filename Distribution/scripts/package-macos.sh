#!/usr/bin/env bash
#==============================================================================
# Build a clean release and assemble the macOS distribution zip:
#
#   Distribution/BASAMAK-macOS/
#       BASAMAK.vst3
#       BASAMAK.component
#       Samples/                 <- factory sample library
#       Install BASAMAK.command <- double-click installer
#       VERSION
#       INSTALL.md
#   Distribution/BASAMAK-macOS.zip
#
# Factory samples are taken from $SAMPLES_SRC (default: the curated library in
# ~/Documents/DrumSequencer Samples). Override with SAMPLES_SRC=/path ...
#==============================================================================
set -e
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJ"

VER="$(grep -m1 'project(DrumSequencer VERSION' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')"
SAMPLES_SRC="${SAMPLES_SRC:-$HOME/Documents/DrumSequencer Samples}"
DST="Distribution/BASAMAK-macOS"

echo "==> Building BASAMAK $VER (Release) ..."
cmake -B build-release -G Xcode -DDAVULSEQ_RELEASE=ON >/dev/null
cmake --build build-release --config Release >/dev/null

VST3="$(find build-release -name 'BASAMAK.vst3' -maxdepth 6 | head -1)"
COMP="$(find build-release -name 'BASAMAK.component' -maxdepth 6 | head -1)"
[ -d "$VST3" ] || { echo "ERROR: VST3 not found"; exit 1; }

echo "==> Assembling $DST ..."
rm -rf "$DST"; mkdir -p "$DST"
cp -R "$VST3" "$DST/"
[ -d "$COMP" ] && cp -R "$COMP" "$DST/"
cp "Distribution/scripts/install-macos.command" "$DST/Install BASAMAK.command"
chmod +x "$DST/Install BASAMAK.command"
echo "$VER" > "$DST/VERSION"
[ -f "Distribution/INSTALL.md" ] && cp "Distribution/INSTALL.md" "$DST/"

if [ -d "$SAMPLES_SRC" ]; then
  echo "==> Bundling factory samples from: $SAMPLES_SRC"
  rm -rf "$DST/Samples"; cp -R "$SAMPLES_SRC" "$DST/Samples"
  find "$DST/Samples" -name '.DS_Store' -delete 2>/dev/null || true
else
  echo "WARNING: no factory samples at '$SAMPLES_SRC' - zip will ship without them."
fi

echo "==> Zipping ..."
( cd Distribution && rm -f BASAMAK-macOS.zip && zip -qr BASAMAK-macOS.zip BASAMAK-macOS )

echo ""
echo "==> Done: Distribution/BASAMAK-macOS.zip  (BASAMAK $VER)"
