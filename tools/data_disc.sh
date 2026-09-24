#!/bin/bash
# Game disc: tools/build_assets.py (Data.rsdk or an extracted Data/ tree -> build/iso/Data with the
# converted assets) then `make disc` (mkpsxiso + verify_disc, no license; build_disc leaves the PC
# sources .gif .wav .ogg off the disc, .txt stays: Sonic CD reads it at runtime).
# RSDK = the Steam Data.rsdk (default ../SonicCD_Extracted/Data.rsdk), or DATA = a Data/ tree.
set -eu
cd "$(dirname "$0")/.."
if [ -n "${DATA:-}" ]; then
  /usr/bin/python3 tools/build_assets.py --data "$DATA"
else
  /usr/bin/python3 tools/build_assets.py --rsdk "${RSDK:-../SonicCD_Extracted/Data.rsdk}"
fi
make -s disc
