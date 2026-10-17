#!/bin/bash
# Refresh the public Sonic-CD-PS1-ISO-Builder (docs/28 phase 11) from this port: the prebuilt executable,
# the build-time converters tools/build_assets.py runs, the RSDKv3 sources the host manifest tool compiles,
# the psxpress MDEC decoder source (MPL 2.0: its modified source ships with the binary). No game data.
# The executable must be a natural build (not `make BOOT=`): rebuild with `make` first.
# Usage: tools/export_builder.sh DEST   (e.g. ../Sonic-CD-PS1-ISO-Builder)
set -e
DEST=${1:?usage: tools/export_builder.sh DEST}
mkdir -p "$DEST" && DEST=$(cd "$DEST" && pwd)
cd "$(dirname "$0")/.."
if mipsel-none-elf-nm rsdkv3.elf | grep -q g_ps1BootBuild; then
  echo "rsdkv3.ps-exe is a BOOT= test build: run make first"; exit 1
fi
B=$DEST/builder
rm -rf "$B"
mkdir -p "$B/tools" "$B/RSDKv3" "$DEST/bin" "$DEST/sources/psxpress"
for f in build_assets.py extract_rsdk.py bytecode_scan.py bytecode_dis.py; do cp tools/$f "$B/tools/"; done
for d in scripts font atlas tiles bgstrips floor audio fmv disc; do
  mkdir -p "$B/tools/$d"
  cp tools/$d/*.py "$B/tools/$d/"
done
mkdir -p "$B/tools/rsdkmanifest"
cp tools/rsdkmanifest/Makefile tools/rsdkmanifest/main.cpp tools/rsdkmanifest/host_stubs.cpp "$B/tools/rsdkmanifest/"
cp RSDKv3/*.hpp "$B/RSDKv3/"
[ -e RSDKv3/theoraplay.h ] && cp RSDKv3/theoraplay.h "$B/RSDKv3/"
for f in Script Reader String Object Animation Math; do cp RSDKv3/$f.cpp "$B/RSDKv3/"; done
mkdir -p "$B/ps1" && cp ps1/video.hh "$B/ps1/"   # Script.cpp's PS1 profiling clock declarations
cp ps1/mdec.cpp ps1/mdec.hh "$DEST/sources/psxpress/"
cp rsdkv3.ps-exe "$DEST/bin/SonicCD-PS1.exe"
( cd "$DEST/bin" && shasum -a 256 SonicCD-PS1.exe > SHA256SUMS )
# bin/README.md from the executable just exported (size, SHA-256, port commit), as RSDKv4-ps1's exporter does
SIZE=$(stat -f%z "$DEST/bin/SonicCD-PS1.exe" 2>/dev/null || stat -c%s "$DEST/bin/SonicCD-PS1.exe")
SUM=$(cut -d' ' -f1 "$DEST/bin/SHA256SUMS")
REV=$(git rev-parse --short HEAD); REVDATE=$(git log -1 --format=%ad --date=short HEAD)
cat > "$DEST/bin/README.md" <<EOF_README
# SonicCD-PS1.exe

The PlayStation executable (PS-X EXE, $SIZE bytes) that \`build_iso.py\` puts on the disc as \`SONICCD.EXE\`:
Retro Engine v3 (RSDKv3 decompilation) ported to the PS1 with psyqo.

- Built from the RSDKv3-ps1 port at commit \`$REV\` ($REVDATE), natural boot (\`make\`), retail 2 MB RAM.
- SHA-256 \`$SUM\` (also in \`SHA256SUMS\`).
- One executable for both languages: it reads the disc's language (\`Data/Game/PS1Language.bin\`, written by
  the builder's \`--lang\`).
- It reads the converted assets from the disc (\`Data/...\`); the converters in \`../builder/\` must match this
  executable's formats, so use the builder from the same release.
- Real hardware (tested on a PSone, SCPH-101):
  - The CPU drives the MDEC for the videos, because the MDEC's output DMA failed on that console.
  - It guards against CD and DMA interrupts lost by psyqo's interrupt acknowledge.
  - During a video, SELECT shows a diagnostics line at the bottom.
EOF_README
echo "exported $(git rev-parse --short HEAD) -> $DEST ($(find "$B" -type f | wc -l | tr -d ' ') builder files)"
cat "$DEST/bin/SHA256SUMS"
