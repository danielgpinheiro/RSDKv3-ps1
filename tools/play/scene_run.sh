#!/bin/bash
# A scene's scripted run, timed (docs/28 speed pass 2): builds `make BOOT=SCENE [BOOT_ID=ID]` (SAMPLE=1 with SAMPLE=1),
# boots it on the harness emulator (tools/redux_env.sh: redux16mb, stock 2 MB / 1 MB VRAM, muted), plays the pad
# script and reports engine frames FROM..TO-1: CPU hblanks a frame (avg / worst; 262 = one 60 Hz frame) and vsyncs a
# frame (avg / worst; 1.00 = 60 fps). Prints "RUN scene avgHbl maxHbl avgVbl maxVbl over frames" and guards.
# Usage: tools/play/scene_run.sh OUT_PREFIX SCENE[:BOOT_ID] FROM TO
#   PAD="frame:mask ..."  pad script (engine frames since boot; masks: 1 Up 2 Down 4 Left 8 Right 16 A 32 B 64 C
#                         128 Start); default: none
#   SAMPLE=1              PC sampler over the same frames -> OUT_samples.bin + OUT_scripts.txt + OUT_code.bin
#                         + OUT_funcs.bin (tools/play/sample_report.py OUT)
#   CHECK="f1 f2 ..."     at these frames dump the logic state (OUT_state_F.bin: entities, players, script engine,
#                         globals) and VRAM (OUT_F_vram.bin) for A/B checks (tools/play/ab_compare.py)
#   EXTRA="gdb lines"     also run at each CHECK frame (\n-separated), e.g. a printf of counters
#   POKE="gdb lines"      run at each CHECK frame after the dump, in the reference run too (tools/play/ab_run.sh):
#                         a state change both builds then run from (e.g. forcing a path the pad script never takes)
#   DISC=path.cue         the disc (default build/game.cue; A/B reference runs use the reference data's disc)
#   NOBUILD=1             run the EXE as it is (an A/B snapshot: EXE=path ELF=path)
#   EMU=ds                DuckStation (retail BIOS; only when no other session runs it): timing only
#   GPU="f1 f2 ..."       at these frames dump RAM + the frame's chain head and price the chain with
#                         tools/play/gpu_cost.py (docs/43: DuckStation's GPU model) -> OUT_gpu_F.ram / .txt, "GPUCOST" lines
#   PERF_SPLIT=1          build with the frame-split counters (make PERF_SPLIT=1): "SPLIT" line = GPU tail (GPU still
#                         drawing the previous chain after the CPU's work; only DuckStation models it), flip gap, and the
#                         section averages (objects, draw, object draw, floors, FG, per-line, tile bands, strips, 3D
#                         transform / sort / draw), all hblanks a frame over the window
set -u
cd "$(dirname "$0")/../.."
OUT=$1; SC=$2; FROM=$3; TO=$4
SCENE=${SC%%:*}; BID=; [ "$SC" != "$SCENE" ] && BID=${SC#*:}
EXE=${EXE:-rsdkv3.ps-exe}; ELF=${ELF:-rsdkv3.elf}
mkdir -p "$(dirname "$OUT")"
if [ -z "${NOBUILD:-}" ]; then
  rm -f RSDKv3/main.o RSDKv3/RetroEngine.o
  make -j8 BOOT=$SCENE BOOT_ID=$BID SAMPLE=${SAMPLE:-} PERF_SPLIT=${PERF_SPLIT:-} > "$OUT.build.log" 2>&1 || { echo "BUILD FAIL ($OUT.build.log)"; exit 1; }
fi
# pad script -> GDB sets
sets=""; n=0
for st in ${PAD:-}; do
  f=${st%%:*}; m=${st#*:}
  sets="${sets}set var g_ps1PadScript[$n] = $(( (f << 8) | m ))\n"; n=$((n + 1))
done
[ $n -le 64 ] || { echo "PAD: $n steps (g_ps1PadScript holds 64)"; exit 1; }
sets="${sets}set var g_ps1PadScriptSteps = $n\nset var g_ps1PerfWinFrom = $FROM\nset var g_ps1PerfWinTo = $TO\n"
# test stops (ascending, <= 8): FROM, the CHECK and GPU frames, TO
stops=$(echo $FROM ${CHECK:-} ${GPU:-} $TO | tr ' ' '\n' | sort -n | uniq)
k=0
for c in $stops; do sets="${sets}set var g_ps1TestStops[$k] = $c\n"; k=$((k + 1)); done
[ $k -le 8 ] || { echo "CHECK: too many stops"; exit 1; }
sets="${sets}set var g_ps1TestStopCount = $k\n"
EMU=${EMU:-redux}
{
  echo "set pagination off"; echo "set confirm off"; echo "set height 0"
  echo "file $ELF"; echo "set architecture mips:3000"
  if [ "$EMU" = ds ]; then echo "target remote localhost:2345"; else echo "target remote localhost:\$PORT"; fi
  printf 'tbreak main\ncommands\nsilent\n%bcontinue\nend\n' "$sets"
  if [ -n "${SAMPLE:-}" ]; then
    printf 'break PS1SampleDrain\ncommands\nsilent\nappend binary memory %s_samples.bin &g_ps1SampleRing[g_ps1SampleHalf*512] &g_ps1SampleRing[g_ps1SampleHalf*512+512]\ncontinue\nend\n' "$OUT"
  fi
  echo "break ps1TestStop"
  echo "commands"; echo "silent"
  for c in ${CHECK:-}; do
    echo "if g_ps1PerfFrames == $c"
    echo "  dump binary memory ${OUT}_state_$c.bin &objectEntityList[0] &objectEntityList[sizeof(objectEntityList)/sizeof(objectEntityList[0])]"
    echo "  append binary memory ${OUT}_state_$c.bin &playerList[0] &playerList[sizeof(playerList)/sizeof(playerList[0])]"
    echo "  append binary memory ${OUT}_state_$c.bin &scriptEng (&scriptEng+1)"
    echo "  append binary memory ${OUT}_state_$c.bin &globalVariables[0] &globalVariables[sizeof(globalVariables)/sizeof(globalVariables[0])]"
    echo "  printf \"CHECK %u stage=%s cam=%d,%d\\n\", g_ps1PerfFrames, stageList[activeStageList][stageListPosition].folder, xScrollOffset, yScrollOffset"
    [ -n "${EXTRA:-}" ] && printf '%b\n' "$EXTRA"
    [ -n "${POKE:-}" ] && printf '%b\n' "$POKE"
    [ "$EMU" != ds ] && echo "  shell REDUX_WEB=\$WEB python3 tools/vramshot.py ${OUT}_$c >/dev/null"
    echo "end"
  done
  for c in ${GPU:-}; do
    echo "if g_ps1PerfFrames == $c"
    echo "  dump binary memory ${OUT}_gpu_$c.ram 0x80000000 0x80200000"
    echo "  printf \"GPUHEAD %u %08x %08x %08x %d\\n\", g_ps1PerfFrames, (unsigned)g_ps1GPU->m_chainHead, (unsigned)g_ps1GPU->m_chainTail, (unsigned)g_ps1GPU->m_chainTailCount, (int)g_ps1GPU->m_chainHeadOversized"
    echo "end"
  done
  echo "if g_ps1PerfFrames == $FROM"
  [ -n "${SAMPLE:-}" ] && echo "  set var g_ps1SamplePos = 0" && echo "  set var g_ps1SampleOn = 1"
  echo "  printf \"FROM %u stage=%s\\n\", g_ps1PerfFrames, stageList[activeStageList][stageListPosition].folder"
  echo "end"
  echo "if g_ps1PerfFrames == $TO"
  if [ -n "${SAMPLE:-}" ]; then
    echo "  set var g_ps1SampleOn = 0"
    echo "  append binary memory ${OUT}_samples.bin &g_ps1SampleRing[(g_ps1SamplePos/256)*512] &g_ps1SampleRing[g_ps1SamplePos*2]"
    echo "  dump binary memory ${OUT}_code.bin &scriptCode[0] &scriptCode[scriptCodePos]"
    echo "  set \$i = 0"
    echo "  while \$i < 256"
    echo "    if typeNames[\$i][0] != 0"
    echo "      printf \"OBJ %d %s %d %d %d %d\\n\", \$i, typeNames[\$i], objectScriptList[\$i].subMain.scriptCodePtr, objectScriptList[\$i].subPlayerInteraction.scriptCodePtr, objectScriptList[\$i].subDraw.scriptCodePtr, objectScriptList[\$i].subStartup.scriptCodePtr"
    echo "    end"
    echo "    set \$i = \$i + 1"
    echo "  end"
    echo "  dump binary memory ${OUT}_funcs.bin &scriptFunctionList[0] &scriptFunctionList[sizeof(scriptFunctionList)/sizeof(scriptFunctionList[0])]"
    echo "  printf \"SAMPLES count=%u period=%u drains=%u\\n\", g_ps1SampleCount, g_ps1SamplePeriod, g_ps1SampleDrains"
  fi
  echo "  printf \"RUN %s avgHbl=%u.%02u maxHbl=%u avgVbl=%u.%02u maxVbl=%u over=%u frames=%u\\n\", stageList[activeStageList][stageListPosition].folder, g_ps1PerfWinHbl / g_ps1PerfWinFrames, g_ps1PerfWinHbl * 100 / g_ps1PerfWinFrames % 100, g_ps1PerfWinMaxHbl, g_ps1PerfWinVbl / g_ps1PerfWinFrames, g_ps1PerfWinVbl * 100 / g_ps1PerfWinFrames % 100, g_ps1PerfWinMaxVbl, g_ps1PerfWinOver, g_ps1PerfWinFrames"
  echo "  printf \"SKIPPED %u\\n\", g_ps1PerfWinSkipped"
  if mipsel-none-elf-nm "$ELF" 2>/dev/null | grep -q g_ps1PerfGpuTail; then
    echo "  printf \"SPLITRAW %u %u %u %u %u %u\", g_ps1PerfWinFrames, g_ps1PerfWinTail, g_ps1PerfWinMaxTail, g_ps1PerfWinTailFrames, g_ps1PerfWinFlip, g_ps1PerfWinMaxFlip"
    echo "  printf \" %u %u %u %u %u %u %u %u %u %u %u\\n\", g_ps1PerfWinSec[0], g_ps1PerfWinSec[1], g_ps1PerfWinSec[2], g_ps1PerfWinSec[3], g_ps1PerfWinSec[4], g_ps1PerfWinSec[5], g_ps1PerfWinSec[6], g_ps1PerfWinSec[7], g_ps1PerfWinSec[8], g_ps1PerfWinSec[9], g_ps1PerfWinSec[10]"
  fi
  echo "  printf \"GUARDS badOpcode=%u divZero=%u badPtr=%u chainOverflow=%u gpuLimit=%u atlasMiss=%u faceTexMiss=%u scene3D=%u heapHeadroom=%u\\n\", g_ps1ScriptBadOpcode, g_ps1ScriptDivZero, g_ps1ScriptBadPtr, g_ps1ChainOverflow, g_ps1GpuLimit, g_ps1AtlasDrawMiss, g_ps1FaceTexMiss, g_ps1Scene3DOverflow, g_ps1HeapHeadroom"
  echo "  kill"
  echo "  quit"
  echo "end"
  echo "continue"; echo "end"
  echo "continue"
} > "$OUT.gdb.in"
rm -f "${OUT}_samples.bin"
if [ "$EMU" = ds ]; then
  . tools/ds_env.sh
  ds_start "$OUT" "$EXE"
  timeout ${TIMEOUT:-900} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb.in" > "$OUT.gdb.log" 2>&1
  ds_stop
else
  . tools/redux_env.sh
  sed -e "s/\$PORT/$REDUX_PORT/" -e "s/\$WEB/$REDUX_WEB/" "$OUT.gdb.in" > "$OUT.gdb"; mv "$OUT.gdb" "$OUT.gdb.in"
  for attempt in 1 2; do # a rare GDB race ("target is running") loses a run: once more
    rm -f "${OUT}_samples.bin"
    redux_wait_free
    "$REDUX_BIN" $REDUX_ARGS -iso "${DISC:-build/game.cue}" -exe "$EXE" >/dev/null 2>&1 &
    REDUX_PID=$!
    sleep 4
    timeout ${TIMEOUT:-900} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb.in" > "$OUT.gdb.log" 2>&1
    redux_stop
    grep -q "^RUN " "$OUT.gdb.log" && break
  done
fi
grep -E "^(RUN|GUARDS|SAMPLES|CHECK|FROM|SKIPPED|X) " "$OUT.gdb.log" || { echo "NO RESULT ($OUT.gdb.log)"; tail -5 "$OUT.gdb.log"; }
grep -E "^OBJ " "$OUT.gdb.log" > "${OUT}_scripts.txt" 2>/dev/null
# the frame split (PERF_SPLIT=1), as averages a frame
grep -E "^SPLITRAW " "$OUT.gdb.log" | head -1 | awk '{ n = ($2 > 0) ? $2 : 1
  printf "SPLIT tail=%.1f maxTail=%d tailFrames=%d flip=%.1f maxFlip=%d | objects=%.1f draw=%.1f objDraw=%.1f floors=%.1f fg=%.1f perLine=%.1f tiles=%.1f strips=%.1f 3dT=%.1f 3dS=%.1f 3dD=%.1f\n",
    $3/n, $4, $5, $6/n, $7, $8/n, $9/n, $10/n, $11/n, $12/n, $13/n, $14/n, $15/n, $16/n, $17/n, $18/n }'
# the chains priced (GPU=)
for c in ${GPU:-}; do
  h=$(grep -E "^GPUHEAD $c " "$OUT.gdb.log" | head -1 | awk '{print "0x" $3, "0x" $4, "0x" $5, $6}')
  [ -n "$h" ] && [ -f "${OUT}_gpu_$c.ram" ] || { echo "GPUCOST $c none"; continue; }
  /usr/bin/python3 tools/play/gpu_cost.py "${OUT}_gpu_$c.ram" $h --top 12 > "${OUT}_gpu_$c.txt"
  echo "GPUCOST $c $(grep '^SUM ' "${OUT}_gpu_$c.txt" | cut -c5-)"
  [ "${KEEP_RAM:-0}" = 1 ] || rm -f "${OUT}_gpu_$c.ram"
done
true
