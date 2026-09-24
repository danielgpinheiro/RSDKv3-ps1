#!/bin/bash
# Phase 0 test disc: a hand-picked subset of ../SonicCD_Extracted/Data + Jump.vag (psxavenc), then
# `make disc` (mkpsxiso + verify_disc, no license). Replaced by the real pipeline in phase 1.
set -eu
cd "$(dirname "$0")/.."
SRC=../SonicCD_Extracted/Data
PSXAVENC=${PSXAVENC:-../psxavenc/build/psxavenc}
rm -rf build/iso && mkdir -p build/iso/Data/Game build/iso/Data/Stages/R11A build/iso/Data/SoundFX/Global
cp "$SRC/Game/GameConfig.bin" build/iso/Data/Game/
for f in StageConfig.bin Act1.bin 128x128Tiles.bin CollisionMasks.bin Backgrounds.bin; do
  cp "$SRC/Stages/R11A/$f" build/iso/Data/Stages/R11A/
done
"$PSXAVENC" -q -t vag -f 44100 "$SRC/SoundFX/Global/Jump.wav" build/iso/Data/SoundFX/Global/Jump.vag
make -s disc
