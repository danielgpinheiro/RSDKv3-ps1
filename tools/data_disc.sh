#!/bin/bash
# Game disc from a loose Sonic CD Data/ tree (phase 2): copy it into build/iso without the PC
# source assets (tools/disc/build_disc.py SOURCE_ONLY: .gif .wav .ogg; .txt stays, the game reads
# it) and macOS metadata, then `make disc` (mkpsxiso + verify_disc, no license).
# DATA = a Data/ tree: ../SonicCD_Extracted/Data, or tools/extract_rsdk.py's output from Data.rsdk.
# Converted assets (atlases, tiles, SFX, music) join in later phases.
set -eu
cd "$(dirname "$0")/.."
DATA=${DATA:-../SonicCD_Extracted/Data}
rm -rf build/iso && mkdir -p build/iso
rsync -a --exclude '*.gif' --exclude '*.wav' --exclude '*.ogg' --exclude '.DS_Store' "$DATA/" build/iso/Data/
echo "iso root: $(find build/iso -type f | wc -l | tr -d ' ') files, $(du -sh build/iso | cut -f1)"
make -s disc
