#!/usr/bin/env bash
#==============================================================================
# Build BASAMAK from source on Linux.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt update && sudo apt install -y build-essential cmake git \
#        libasound2-dev libjack-jackd2-dev libfreetype6-dev libfontconfig1-dev \
#        libgl1-mesa-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev \
#        libxcursor-dev libcurl4-openssl-dev libwebkit2gtk-4.1-dev
#
# JUCE: cloned automatically next to the project if not found. Override with
#   JUCE_DIR=/path/to/JUCE bash build-linux.sh
#==============================================================================
set -e
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJ"

if [ -z "$JUCE_DIR" ] && [ ! -d "$HOME/JUCE" ]; then
  echo "==> Cloning JUCE (8.x) ..."
  git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git "$HOME/JUCE"
fi

echo "==> Configuring (Release) ..."
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DDAVULSEQ_RELEASE=ON

echo "==> Building ..."
cmake --build build-release --config Release -j"$(nproc)"

echo ""
echo "==> Built VST3:"
find build-release -name "BASAMAK.vst3" -maxdepth 6 -print
echo "Run Distribution/scripts/install-linux.sh to install it."
