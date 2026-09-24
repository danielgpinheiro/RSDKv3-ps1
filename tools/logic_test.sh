#!/bin/bash
# Phase 2.3 gameplay-logic test (headless): boots a `make BOOT=<folder>` build, drives the player
# with g_ps1PadTestMask (bit = INPUT_* index: 0 up 1 down 2 left 3 right 4 A 5 B 6 C 7 start) and
# samples playerList[0], the stage timer, perf and guards at fixed frames (PS1PerfTick). Entity
# types are dumped at frame S0 (build/…_ents.bin, 68-byte entities, type at +56) and counted.
#   S0  settle (title card)        -> hold Right
#   S1  running                    -> Right + A (jump)
#   S2  airborne                   -> release all
#   S3  landed / stopped           -> end
# Usage: make BOOT=R11A && tools/logic_test.sh [OUT_PREFIX] [S0=240] [S1=420] [S2=432] [S3=600]
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/rsdkv3_logic}; S0=${2:-240}; S1=${3:-420}; S2=${4:-432}; S3=${5:-600}
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv3.elf
break PS1LoadingDoneHook
commands
silent
printf "LOAD vbl=%u sectors=%u list=%d pos=%d folder=%s\n", g_ps1GPU->m_frameCount, g_ps1FsSectors, activeStageList, stageListPosition, stageList[activeStageList][stageListPosition].folder
continue
end
break PS1PerfTick if g_ps1PerfFrames == 1 || g_ps1PerfFrames == $S0 || g_ps1PerfFrames == $S1 || g_ps1PerfFrames == $S2 || g_ps1PerfFrames == $S3
commands
silent
printf "F%u mask=%02x P0 x=%d y=%d xv=%d yv=%d speed=%d gravity=%d | time %d:%02d.%02d enabled=%d stageMode=%d | lastHbl=%u maxHbl=%u vsyncs=%u\n  ctrl mode=%d lock=%d in L%dR%dU%dD%d jP%d jH%d keyDown R%d A%d | entity %d type %d\n", g_ps1PerfFrames, g_ps1PadButtons, playerList[0].XPos >> 16, playerList[0].YPos >> 16, playerList[0].XVelocity, playerList[0].YVelocity, playerList[0].speed, playerList[0].gravity, stageMinutes, stageSeconds, stageMilliseconds, timeEnabled, stageMode, g_ps1PerfLastHbl, g_ps1PerfMaxHbl, g_ps1PerfVsyncs, playerList[0].controlMode, playerList[0].controlLock, playerList[0].left, playerList[0].right, playerList[0].up, playerList[0].down, playerList[0].jumpPress, playerList[0].jumpHold, keyDown.right, keyDown.A, playerList[0].entityNo, objectEntityList[playerList[0].entityNo].type
if g_ps1PerfFrames == $S0
  dump binary memory ${OUT}_ents.bin &objectEntityList[0] &objectEntityList[0x4A0]
  set var g_ps1PadTestMask = 0x08
end
if g_ps1PerfFrames == $S1
  set var g_ps1PadTestMask = 0x18
end
if g_ps1PerfFrames == $S2
  set var g_ps1PadTestMask = 0
end
if g_ps1PerfFrames == $S3
  printf "PROF objects=%u drawSubs=%u (hblanks, last frame)\n", g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl
  printf "GUARDS scriptOverflow=%u badPtr=%u divZero=%u badOpcode=%u rowsClamped=%u scene3D=%u heapHeadroom=%u\n", g_ps1ScriptOverflow, g_ps1ScriptBadPtr, g_ps1ScriptDivZero, g_ps1ScriptBadOpcode, g_ps1LayerRowsClamped, g_ps1Scene3DOverflow, g_ps1HeapHeadroom
  detach
else
  continue
end
end
continue
GDBEOF
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -iso build/game.cue -exe rsdkv3.ps-exe \
      -stdout -logfile "$OUT.log" >/dev/null 2>&1 &
sleep 4
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(LOAD|F[0-9]+|GUARDS|PROF|  ctrl) .*" | tee "$OUT.txt"
pkill -f PCSX-Redux
[ -f "${OUT}_ents.bin" ] && python3 - "${OUT}_ents.bin" <<'PYEOF'
import sys, collections
d = open(sys.argv[1], 'rb').read()
types = [d[i * 68 + 56] for i in range(len(d) // 68)]
c = collections.Counter(t for t in types if t)
print('ENTITIES %d active (of %d), %d types; most common: %s' % (sum(c.values()), len(types), len(c),
      ', '.join('%d x%d' % kv for kv in c.most_common(8))))
PYEOF
