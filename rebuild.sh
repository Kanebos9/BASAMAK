#!/bin/bash
# Clean rebuild of the Drum Sequencer plugin.
# Removes ALL previously installed copies first, because every build shares the
# same VST3/AU class UID — two installed copies with the same UID make Reaper's
# scanner hang ("plug-in is not responding"). Only one copy should exist at a time.
set -e

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
APP_DIR="/Applications"

echo "==> Removing old installed copies..."
rm -rf "$VST3_DIR/BASAMAK"*.vst3 "$VST3_DIR/DavulSEQ"*.vst3 "$VST3_DIR/samSEQ"*.vst3 "$VST3_DIR/DrumSeq"*.vst3 "$VST3_DIR/Drum Sequencer.vst3"
rm -rf "$AU_DIR/BASAMAK"*.component "$AU_DIR/DavulSEQ"*.component "$AU_DIR/samSEQ"*.component "$AU_DIR/DrumSeq"*.component "$AU_DIR/Drum Sequencer.component"
rm -rf "$APP_DIR/BASAMAK"*.app "$APP_DIR/DavulSEQ"*.app

cd "$HOME/DrumSequencer"
echo "==> Clean configure (new timestamp)..."
rm -rf build
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/dev/null

echo "==> Building VST3..."
cmake --build build --config Release --target DrumSequencer_VST3 >/dev/null

echo "==> Building AU..."
cmake --build build --config Release --target DrumSequencer_AU >/dev/null

echo "==> Building + installing Standalone to /Applications..."
cmake --build build --config Release --target DrumSequencer_Standalone >/dev/null
SA_APP=$(ls -dt build/DrumSequencer_artefacts/Release/Standalone/*.app 2>/dev/null | head -1)
[ -n "$SA_APP" ] && cp -R "$SA_APP" "$APP_DIR/"

echo "==> Done. Installed:"
ls -1 "$VST3_DIR" | grep -i davulseq || true
ls -1 "$AU_DIR"   | grep -i davulseq || true
ls -1d "$APP_DIR/DavulSEQ"*.app 2>/dev/null || true
