# The PCSX-Redux instance the test harness drives (sourced by every emulator script, from the repo root).
# User 2026-10-16: the stock PCSX-Redux app (/Applications/PCSX-Redux.app), not the patched 16 MB copy (that one is the
# GTA III project's), with the stock console (-2mb: 2 MB RAM, 1 MB VRAM), on its own portable settings
# (../pcsx-redux-cd/pcsx.json), GDB port 3338 and web port 8088, so it runs next to other sessions' emulators without
# touching them (2026-10-09..16 it was ../pcsx-redux-16mb/redux16mb in stock mode: the same numbers within 0.05 %).
# OpenBIOS from ../pcsx-redux-16mb/openbios.bin (a BIOS image only). Scripts kill only the PID they started (redux_stop).
# REDUX_BIN / REDUX_PORT / REDUX_WEB / REDUX_PORTABLE override; REDUX_CARDS=0: the script passes its own cards.
REDUX_BIN=${REDUX_BIN:-/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux}
REDUX_PORTABLE=${REDUX_PORTABLE:-$(cd .. && pwd)/pcsx-redux-cd}
REDUX_PORT=${REDUX_PORT:-3338}
REDUX_WEB=${REDUX_WEB:-8088}
export REDUX_PORT REDUX_WEB
mkdir -p "$REDUX_PORTABLE"
[ -f "$REDUX_PORTABLE/pcsx.json" ] || echo '{}' > "$REDUX_PORTABLE/pcsx.json"
# User 2026-10-09: no sound from the test runs. SPU.Mute only silences the output: the SPU (IRQs, timing) still runs.
python3 - "$REDUX_PORTABLE/pcsx.json" <<'PYEOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d.setdefault('SPU', {})['Mute'] = True
json.dump(d, open(p, 'w'), indent=4)
PYEOF
REDUX_ARGS="-2mb -fastboot -interpreter -debugger -gdb -gdb-port $REDUX_PORT -webserver -webserver-port $REDUX_WEB -portable $REDUX_PORTABLE"
REDUX_ARGS="$REDUX_ARGS -bios $(cd .. && pwd)/pcsx-redux-16mb/openbios.bin"
if [ "${REDUX_CARDS:-1}" = 1 ]; then
    # freshly formatted cards every run: a run that saves must not change the next run's game
    python3 tools/memcard/mcd.py format "$REDUX_PORTABLE/harness_mc1.mcd" >/dev/null
    python3 tools/memcard/mcd.py format "$REDUX_PORTABLE/harness_mc2.mcd" >/dev/null
    REDUX_ARGS="$REDUX_ARGS -memcard1 $REDUX_PORTABLE/harness_mc1.mcd -memcard2 $REDUX_PORTABLE/harness_mc2.mcd"
fi
REDUX_PID=
case " $REDUX_ARGS " in *" -8mb "*|*" -16mb "*|*" -2mbvram "*) echo "redux_env: stock RAM / VRAM only" >&2; exit 1 ;; esac

# Our instance's ports are taken (a run of ours still up, or someone else on them): wait up to 10 minutes.
redux_wait_free() {
    for _ in $(seq 1 120); do
        nc -z localhost "$REDUX_PORT" 2>/dev/null || nc -z localhost "$REDUX_WEB" 2>/dev/null || return 0
        sleep 5
    done
    echo "redux_env: ports $REDUX_PORT / $REDUX_WEB still busy" >&2
    exit 2
}
# Kill only the emulator this script started.
redux_stop() {
    [ -n "$REDUX_PID" ] && kill "$REDUX_PID" 2>/dev/null
    REDUX_PID=
}
