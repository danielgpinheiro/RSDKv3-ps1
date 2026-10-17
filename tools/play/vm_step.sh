#!/bin/bash
# Machine instructions per VM instruction (docs/28 speed pass 2): a SAMPLE=1 BOOT=SCENE build stopped at engine frame
# FRAME (the survey's pad script), then N single steps from the next ProcessScript call; prints the steps each VM
# instruction took and the source lines they ran. Usage: tools/play/vm_step.sh SCENE [FRAME=600] [N=900]
set -u
cd "$(dirname "$0")/../.."
SC=$1; F=${2:-600}; N=${3:-900}; OUT=build/perf2/prof/vmstep_$SC
eval "$(sed -n '/^act_pad()/p;/^ss_pad()/p;/^pad_for()/,/^}/p' tools/play/survey.sh)"
rm -f RSDKv3/main.o RSDKv3/RetroEngine.o
make -j8 BOOT=$SC SAMPLE=1 > $OUT.build.log 2>&1 || { echo BUILD FAIL; exit 1; }
. tools/redux_env.sh
{ echo "set pagination off"; echo "set confirm off"; echo "file rsdkv3.elf"; echo "set architecture mips:3000"
  echo "target remote localhost:$REDUX_PORT"; echo "tbreak main"; echo "commands"; echo "silent"
  n=0; for st in $(pad_for $SC); do echo "set var g_ps1PadScript[$n] = $(( (${st%%:*} << 8) | ${st#*:} ))"; n=$((n + 1)); done
  echo "set var g_ps1PadScriptSteps = $n"; echo "set var g_ps1TestStops[0] = $F"; echo "set var g_ps1TestStopCount = 1"
  echo "continue"; echo "end"; echo "break ps1TestStop"; echo "continue"; echo "delete"; echo "break ProcessScript"; echo "continue"; echo "delete"
  echo "set \$k = 0"; echo "while \$k < $N"; echo "  printf \"S %x %u\\n\", \$pc, g_ps1ScriptOps"; echo "  stepi"; echo "  set \$k = \$k + 1"; echo "end"
  echo "kill"; echo "quit"; } > $OUT.gdb
redux_wait_free
"$REDUX_BIN" $REDUX_ARGS -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
REDUX_PID=$!
sleep 5
timeout 1200 ${GDB:-tools/gdbw} --batch -x $OUT.gdb > $OUT.log 2>&1
redux_stop
python3 - $OUT.log <<'PYEOF'
import subprocess, collections, sys
L = [l.split() for l in open(sys.argv[1]) if l.startswith('S ')]
ops = [int(x[2]) for x in L]
idx = [i for i in range(1, len(ops)) if ops[i] != ops[i - 1]]
d = [b - a for a, b in zip(idx, idx[1:])]
print('steps per VM instruction', d, 'avg %.1f' % (sum(d) / max(len(d), 1)))
for a, b in zip(idx, idx[1:]):
    res = subprocess.run(['mipsel-none-elf-addr2line', '-e', 'rsdkv3.elf'] + ['0x' + L[i][1] for i in range(a, b)], capture_output=True, text=True).stdout.splitlines()
    c = collections.OrderedDict()
    for r in res:
        k = r.split('/')[-1].split(' (')[0]
        c[k] = c.get(k, 0) + 1
    print('--- %d: ' % (b - a) + ' '.join('%s:%d' % (k.replace('Script.cpp:', 'S'), v) for k, v in c.items()))
PYEOF
