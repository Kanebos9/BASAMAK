#!/usr/bin/env bash
#==============================================================================
# Make a clean "source code" zip to hand to other developers. It contains
# everything needed to open the project in VS Code and build it - and NOTHING
# that is machine-specific (no build folders, no backups, no large binaries).
#
#   Output: Distribution/BASAMAK-Source.zip
#==============================================================================
set -e
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJ"

OUT="BASAMAK-Source"
rm -rf "/tmp/$OUT" "Distribution/BASAMAK-Source.zip"
mkdir -p "/tmp/$OUT"

# Copy the things a developer needs.
cp -R Source CMakeLists.txt rebuild.sh README.md docs .vscode .github "/tmp/$OUT/" 2>/dev/null || true
mkdir -p "/tmp/$OUT/Distribution"
cp -R Distribution/scripts "/tmp/$OUT/Distribution/"
cp Distribution/*.md       "/tmp/$OUT/Distribution/" 2>/dev/null || true

# Tidy macOS cruft.
find "/tmp/$OUT" -name '.DS_Store' -delete 2>/dev/null || true

( cd /tmp && zip -qr "$PROJ/Distribution/BASAMAK-Source.zip" "$OUT" )
rm -rf "/tmp/$OUT"

echo "==> Done: Distribution/BASAMAK-Source.zip"
echo "    Hand this to a developer. They unzip it, open the folder in VS Code,"
echo "    and read README.md / docs/ARCHITECTURE.md to get started."
