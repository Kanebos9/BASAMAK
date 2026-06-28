#!/usr/bin/env bash
#==============================================================================
# Build BASAMAK from source on macOS (clean release build, no auto-install).
# For the day-to-day dev loop use ../../rebuild.sh instead (it auto-installs).
#
# JUCE is cloned to ~/JUCE if not present. Override with JUCE_DIR=/path ...
#==============================================================================
set -e
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJ"

if [ -z "$JUCE_DIR" ] && [ ! -d "$HOME/JUCE" ]; then
  echo "==> Cloning JUCE 8.x ..."
  git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git "$HOME/JUCE"
fi

echo "==> Configuring (Release) ..."
cmake -B build-release -G Xcode -DDAVULSEQ_RELEASE=ON

echo "==> Building ..."
cmake --build build-release --config Release

echo ""
echo "==> Built. VST3/AU are under build-release/DrumSequencer_artefacts/Release/"
echo "Run Distribution/scripts/install-macos.command to install, or"
echo "Distribution/scripts/package-macos.sh to make the distributable zip."
