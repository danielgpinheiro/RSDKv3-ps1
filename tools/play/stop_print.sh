#!/bin/bash
# Values at a frame of a scene's scripted run (docs/28 speed pass 2: e.g. branch counters of a native opcode, to see
# that the A/B run went through every path; breakpoints inside ProcessScript don't stop here). A BOOT=SCENE build of the
# working tree, the survey's pad script, then GDB prints each EXPR at the end of frame F.
# Usage: tools/play/stop_print.sh SCENE F OUT.log EXPR...
set -u
cd "$(dirname "$0")/../.."
SC=$1; F=$2; LOG=$3; shift 3
eval "$(sed -n '/^act_pad()/p;/^ss_pad()/p;/^pad_for()/,/^}/p' tools/play/survey.sh)"
. tools/redux_env.sh
rm -f RSDKv3/main.o RSDKv3/RetroEngine.o  # BOOT= lives there (make 3.81 timestamps: rebuild them)
make -j8 BOOT=$SC SAMPLE= > $LOG.build.log 2>&1 || { echo "BUILD FAIL ($LOG.build.log)"; exit 1; }
G=$LOG.gdb
{
  echo "set pagination off"; echo "set confirm off"; echo "file rsdkv3.elf"; echo "set architecture mips:3000"
  echo "target remote localhost:$REDUX_PORT"
  echo "tbreak main"; echo "commands"; echo "silent"
  n=0
  for st in $(pad_for $SC); do echo "set var g_ps1PadScript[$n] = $(( (${st%%:*} << 8) | ${st#*:} ))"; n=$((n + 1)); done
  echo "set var g_ps1PadScriptSteps = $n"
  echo "set var g_ps1TestStops[0] = $F"; echo "set var g_ps1TestStopCount = 1"
  echo "continue"; echo "end"
  echo "break ps1TestStop"; echo "continue"
  for e in "$@"; do case "$e" in source*) echo "$e";; *) echo "print $e";; esac; done
  echo "kill"; echo "quit"
} > "$G"
redux_wait_free
"$REDUX_BIN" $REDUX_ARGS -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
REDUX_PID=$!
sleep 5
timeout ${TIMEOUT:-900} ${GDB:-tools/gdbw} --batch -x "$G" > "$LOG" 2>&1
redux_stop
grep -E '^(\$[0-9]+ = |TYPES)' "$LOG"
