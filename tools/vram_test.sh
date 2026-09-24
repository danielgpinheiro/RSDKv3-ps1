#!/bin/bash
# Phase 3.1 check: the stage's tile pages + sprite atlas in VRAM and the tile palette entries equal the
# build-time files (tools/check_vram.py). Boots the current rsdkv3.ps-exe (e.g. `make BOOT=R11A`),
# dumps activePalette32 right after the stage load and VRAM at frame F (PCSX-Redux web server).
# Usage: tools/vram_test.sh STAGE [OUT_PREFIX] [F=10]
cd "$(dirname "$0")/.."
STAGE=$1; OUT=${2:-/tmp/rsdkv3_vram}; F=${3:-10}
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv3.elf
break PS1LoadingDoneHook if g_ps1InitDone
commands
silent
dump binary memory ${OUT}_pal.bin &activePalette32[0] &activePalette32[256]
printf "LOADED %s tiles=%u (%u B) atlas=%u (%u B)\n", stageList[activeStageList][stageListPosition].folder, g_ps1TileSetLoads, g_ps1TileSetBytes, g_ps1AtlasLoads, g_ps1AtlasBlockBytes
continue
end
break PS1PerfTick if g_ps1PerfFrames == $F
commands
silent
shell python3 tools/vramshot.py ${OUT} >/dev/null
printf "FRAME %u heapHeadroom=%u\n", g_ps1PerfFrames, g_ps1HeapHeadroom
detach
end
continue
GDBEOF
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
sleep 4
timeout ${TIMEOUT:-240} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(LOADED|FRAME) .*"
pkill -f PCSX-Redux
/usr/bin/python3 tools/check_vram.py "${OUT}_vram.bin" build/iso/Data "$STAGE" "${OUT}_pal.bin"
