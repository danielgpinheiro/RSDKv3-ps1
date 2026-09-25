#!/bin/bash
# Game disc: tools/build_assets.py (Data.rsdk or an extracted Data/ tree -> build/iso/Data with the
# converted assets) then `make disc` (mkpsxiso + verify_disc, no license; build_disc leaves the PC
# sources .gif .wav .ogg off the disc, .txt stays: Sonic CD reads it at runtime).
# RSDK = the Steam Data.rsdk (default ../SonicCD_Extracted/Data.rsdk), or DATA = a Data/ tree;
# LANG_DISC = en (default) or jp: one disc per language (docs/28 phase 4).
set -eu
cd "$(dirname "$0")/.."
if [ -n "${DATA:-}" ]; then
  /usr/bin/python3 tools/build_assets.py --data "$DATA" --lang "${LANG_DISC:-en}"
else
  /usr/bin/python3 tools/build_assets.py --rsdk "${RSDK:-../SonicCD_Extracted/Data.rsdk}" --lang "${LANG_DISC:-en}"
fi
/usr/bin/python3 tools/disc/gen_load_order.py build/iso/Data  # disc layout in load order (docs/28 phase 9)
make -s disc
