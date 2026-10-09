#!/bin/bash
# Which engine calls a frame makes, and when (docs/28 speed pass 2): a BOOT=SCENE build (the working tree's current
# EXE: build it first), the survey's pad script, stopped at the end of frame F - 1; then breakpoints on FUNCS print the
# frame, root counter 1 (hblanks) and the vblank count at each call (with 3 frames of backtrace) until frame F ends.
# Usage: tools/play/call_trace.sh SCENE F "FUNC ..." OUT.log
set -u
cd "$(dirname "$0")/../.."
SC=$1; F=$2; FUNCS=$3; LOG=$4
eval "$(sed -n '/^act_pad()/p;/^ss_pad()/p;/^pad_for()/,/^}/p' tools/play/survey.sh)"
. tools/redux_env.sh
G=$LOG.gdb
{
  echo "set pagination off"; echo "set confirm off"; echo "file rsdkv3.elf"; echo "set architecture mips:3000"
  echo "target remote localhost:$REDUX_PORT"
  echo "tbreak main"; echo "commands"; echo "silent"
  n=0
  for st in $(pad_for $SC); do echo "set var g_ps1PadScript[$n] = $(( (${st%%:*} << 8) | ${st#*:} ))"; n=$((n + 1)); done
  echo "set var g_ps1PadScriptSteps = $n"
  echo "set var g_ps1TestStops[0] = $((F - 1))"; echo "set var g_ps1TestStops[1] = $F"; echo "set var g_ps1TestStopCount = 2"
  echo "continue"; echo "end"
  echo "break ps1TestStop"; echo "continue"
  for f in $FUNCS; do
    echo "break $f"; echo "commands"; echo "silent"
    echo "printf \"HIT %s frame=%u hbl=%u vbl=%u\\n\", \"$f\", g_ps1PerfFrames, *(volatile unsigned short *)0x1F801110, g_ps1GPU->m_frameCount"
    echo "bt 3"; echo "continue"; echo "end"
  done
  echo "continue"
  echo "printf \"END frame=%u vbl=%u\\n\", g_ps1PerfFrames, g_ps1GPU->m_frameCount"
  echo "kill"; echo "quit"
} > "$G"
redux_wait_free
"$REDUX_BIN" $REDUX_ARGS -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
REDUX_PID=$!
sleep 5
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$G" > "$LOG" 2>&1
redux_stop
grep -E "^(HIT|END|#)" "$LOG"
