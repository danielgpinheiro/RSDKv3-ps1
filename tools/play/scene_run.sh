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
#   NOBUILD=1             run the EXE as it is (an A/B snapshot: EXE=path ELF=path)
#   EMU=ds                DuckStation (retail BIOS; only when no other session runs it): timing only
set -u
cd "$(dirname "$0")/../.."
OUT=$1; SC=$2; FROM=$3; TO=$4
SCENE=${SC%%:*}; BID=; [ "$SC" != "$SCENE" ] && BID=${SC#*:}
EXE=${EXE:-rsdkv3.ps-exe}; ELF=${ELF:-rsdkv3.elf}
mkdir -p "$(dirname "$OUT")"
if [ -z "${NOBUILD:-}" ]; then
  rm -f RSDKv3/main.o RSDKv3/RetroEngine.o
  make -j8 BOOT=$SCENE BOOT_ID=$BID SAMPLE=${SAMPLE:-} > "$OUT.build.log" 2>&1 || { echo "BUILD FAIL ($OUT.build.log)"; exit 1; }
fi
# pad script -> GDB sets
sets=""; n=0
for st in ${PAD:-}; do
  f=${st%%:*}; m=${st#*:}
  sets="${sets}set var g_ps1PadScript[$n] = $(( (f << 8) | m ))\n"; n=$((n + 1))
done
[ $n -le 64 ] || { echo "PAD: $n steps (g_ps1PadScript holds 64)"; exit 1; }
sets="${sets}set var g_ps1PadScriptSteps = $n\nset var g_ps1PerfWinFrom = $FROM\nset var g_ps1PerfWinTo = $TO\n"
# test stops (ascending, <= 8): FROM, the CHECK frames, TO
stops=$(echo $FROM ${CHECK:-} $TO | tr ' ' '\n' | sort -n | uniq)
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
    [ "$EMU" != ds ] && echo "  shell REDUX_WEB=\$WEB python3 tools/vramshot.py ${OUT}_$c >/dev/null"
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
  redux_wait_free
  "$REDUX_BIN" $REDUX_ARGS -iso build/game.cue -exe "$EXE" >/dev/null 2>&1 &
  REDUX_PID=$!
  sleep 4
  timeout ${TIMEOUT:-900} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb.in" > "$OUT.gdb.log" 2>&1
  redux_stop
fi
grep -E "^(RUN|GUARDS|SAMPLES|CHECK|FROM|SKIPPED|X) " "$OUT.gdb.log" || { echo "NO RESULT ($OUT.gdb.log)"; tail -5 "$OUT.gdb.log"; }
grep -E "^OBJ " "$OUT.gdb.log" > "${OUT}_scripts.txt" 2>/dev/null
true
