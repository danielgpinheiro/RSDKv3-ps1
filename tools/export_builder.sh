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
echo "exported $(git rev-parse --short HEAD) -> $DEST ($(find "$B" -type f | wc -l | tr -d ' ') builder files)"
cat "$DEST/bin/SHA256SUMS"
