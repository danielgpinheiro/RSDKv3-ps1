#!/bin/bash
# Frame-time survey across stages (phase 3.6): for each stage folder, build `make BOOT=<stage>`, hold
# Right from frame F0, reset the frame-time max/over counters there, and report at F1: worst frame
# (hblanks, 262 = one 60 Hz frame), frames over budget, last frame's section profile, chain use.
# Also dumps OUT_<stage>_fb0/1.png. Emulator runs one stage at a time (background + timeout).
# Usage: tools/perf_zones.sh OUT_PREFIX STAGE...   (F0=240 F1=600 MASK=8 env overrides)
cd "$(dirname "$0")/.."
OUT=$1; shift
F0=${F0:-240}; F1=${F1:-600}; MASK=${MASK:-8}
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
for ST in "$@"; do
  touch RSDKv3/main.cpp RSDKv3/RetroEngine.cpp
  make -s -j8 BOOT=$ST >/dev/null 2>&1 || { echo "$ST build failed"; continue; }
  cat > "${OUT}_$ST.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv3.elf
break PS1PerfTick if g_ps1PerfFrames == $F0
continue
set var g_ps1PadTestMask = $MASK
set var g_ps1PerfMaxHbl = 0
set var g_ps1PerfOverHbl = 0
set var g_ps1ChainHighWater = 0
delete
break PS1PerfTick if g_ps1PerfFrames == $F1
continue
printf "ZONE %-6s max=%u over=%u/%u last=%u | objects=%u drawSubs=%u layers=%u strips=%u | chain high=%u overflow=%u | miss=%u unbound=%u todo=%u heap=%u\\n", "$ST", g_ps1PerfMaxHbl, g_ps1PerfOverHbl, $F1 - $F0, g_ps1PerfLastHbl, g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1LayerHbl, g_ps1ProfLayer[3], g_ps1ChainHighWater, g_ps1ChainOverflow, g_ps1AtlasDrawMiss, g_ps1SheetUnbound, g_ps1DrawTodo, g_ps1HeapHeadroom
shell python3 tools/vramshot.py ${OUT}_$ST >/dev/null
detach
GDBEOF
  pkill -f PCSX-Redux 2>/dev/null; sleep 1
  $PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
  sleep 4
  timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "${OUT}_$ST.gdb" 2>&1 | grep -oE "ZONE .*" || echo "ZONE $ST (no result)"
  pkill -f PCSX-Redux
done
touch RSDKv3/main.cpp RSDKv3/RetroEngine.cpp
