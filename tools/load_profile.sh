#!/bin/bash
# Load-time profile (docs/28 phase 9): every file-system CD request (PS1FsProfileHook: LBA, sectors, time),
# every fopen and every load end (PS1LoadingDoneHook), then per-load / per-file tables (load_profile.py,
# from the Nexus port). The emulated CD timing is PCSX-Redux's model: compare request counts first.
# Usage: tools/load_profile.sh OUT.log [STOP_FRAME=3] [--order ORDER.txt]   (build: e.g. make BOOT=R11A)
OUT=${1:-/tmp/rsdkv3_load.log}; STOP=${2:-3}; shift 2 2>/dev/null; EXTRA=("$@")
cd "$(dirname "$0")/.."
. tools/redux_env.sh
G=$(dirname "$OUT")/load_profile.gdb
cat > $G <<GDBEOF
set pagination off
set confirm off
target remote localhost:$REDUX_PORT
symbol-file rsdkv3.elf
break PS1FsProfileHook
commands
silent
printf "REQ lba=%u n=%u us=%u frame=%u\n", lba, count, us, g_ps1PerfFrames
continue
end
break fopen
commands
silent
printf "OPEN %s\n", path
continue
end
break PS1LoadingDoneHook
commands
silent
printf "LOAD us=%u frame=%u requests=%u sectors=%u readUs=%u headroom=%u\n", us, g_ps1PerfFrames, g_ps1FsRequests, g_ps1FsSectors, g_ps1FsReadUs, g_ps1LoadingHeadroom
continue
end
tbreak PS1PerfTick if g_ps1PerfFrames == $STOP
continue
printf "DONE frame=%u requests=%u sectors=%u readUs=%u\n", g_ps1PerfFrames, g_ps1FsRequests, g_ps1FsSectors, g_ps1FsReadUs
printf "STREAM starts=%u stale=%u skipped=%u errors=%u fallback=%u\n", g_ps1FsStreamStarts, g_ps1FsStreamStale, g_ps1FsStreamSkipped, g_ps1FsStreamErrors, g_ps1FsFallbackReads
printf "FS indexHits=%u dirReads=%u dirHits=%u\n", g_ps1FsIndexHits, g_ps1FsDirReads, g_ps1FsDirHits
detach
GDBEOF
redux_wait_free
"$REDUX_BIN" $REDUX_ARGS -iso build/game.cue -exe rsdkv3.ps-exe >/dev/null 2>&1 &
REDUX_PID=$!
sleep 4
timeout ${TIMEOUT:-400} ${GDB:-tools/gdbw} --batch -x $G 2>&1 | grep -E "^(REQ|OPEN|LOAD|DONE|STREAM|FS)" > "$OUT"
redux_stop
/usr/bin/python3 tools/load_profile.py "$OUT" build/game.bin "${EXTRA[@]}"
