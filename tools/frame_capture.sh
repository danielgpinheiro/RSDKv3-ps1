#!/bin/bash
# Halt the current rsdkv3.ps-exe right after game frame N (PS1PerfTick) and dump VRAM + the two
# framebuffers as PNGs (tools/vramshot.py). At that point the chain of frame N hasn't been sent yet:
# the displayed buffer holds frame N-1 complete (the other one is older). Prints renderer counters.
# Usage: tools/frame_capture.sh N OUT_PREFIX   (MASK=bits FROM=frame: hold g_ps1PadTestMask from that frame;
#        GDBSET='var=value' also applied at FROM, e.g. GDBSET='waterLevel=1900')
cd "$(dirname "$0")/.."
N=$1; OUT=$2
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv3.elf
break PS1PerfTick if g_ps1PerfFrames == ${FROM:-0}
continue
set var g_ps1PadTestMask = ${MASK:-0}
${GDBSET:+set var $GDBSET}
delete
break PS1PerfTick if g_ps1PerfFrames == $N
continue
printf "ACTIVE %d %d %d %d mid %d | layer1 type %d %dx%d\n", activeTileLayers[0], activeTileLayers[1], activeTileLayers[2], activeTileLayers[3], tLayerMidPoint, stageLayouts[1].type, stageLayouts[1].xsize, stageLayouts[1].ysize
printf "FRAME %u chainOverflow=%u parity=%d folder=%s | sprites=%u quads=%u layerPrims=%u layerHbl=%u lastHbl=%u | chainWords=%u high=%u overflow=%u | miss=%u unbound=%u todo=%u draws=%u repeats=%u\n", g_ps1PerfFrames, g_ps1ChainOverflow, g_ps1GPU->m_parity, stageList[activeStageList][stageListPosition].folder, g_ps1DrawSprites, g_ps1DrawQuads, g_ps1LayerPrims, g_ps1LayerHbl, g_ps1PerfLastHbl, g_ps1ChainLastWords, g_ps1ChainHighWater, g_ps1ChainOverflow, g_ps1AtlasDrawMiss, g_ps1SheetUnbound, g_ps1DrawTodo, g_ps1DrawFrames, g_ps1RepeatFrames
shell python3 tools/vramshot.py ${OUT} >/dev/null
printf "LAYERS uniform=%u lines=%u tiles=%u strips=%u (hblanks) calls=%u stripRegions=%u objects=%u drawSubs=%u | ink pieces=%u clutFades=%u fadeMode=%d fadeA=%d | banks regions=%u mask=%x water=%d\n", g_ps1ProfLayer[0], g_ps1ProfLayer[1], g_ps1ProfLayer[2], g_ps1ProfLayer[3], g_ps1ProfLayer[4], g_ps1StripRegions, g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1InkPieces, g_ps1ClutFades, fadeMode, fadeA, g_ps1BankRegions, g_ps1BankMask, waterDrawPos
detach
GDBEOF
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
sleep 4
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(FRAME|LAYERS|ACTIVE) .*"
pkill -f PCSX-Redux
