# DuckStation for the timing harness (sourced by tools/play/scene_run.sh EMU=ds), on the model of RSDKv5-ps1's
# tools/emu.sh: retail SCPH-101 BIOS, Interpreter, no overclock, CD speedups 1x (the user's settings), GDB 2345.
# DuckStation is shared with other sessions: it starts only when no DuckStation runs at all (otherwise exit 2), and
# the settings a run changes (a non-persistent card, StartPaused so GDB's first continue starts the boot, the output
# muted: user 2026-10-09) are backed up and restored. It boots a disc built for the run (build/ds/game: the EXE given
# + the data tree; ~574 MB, rewritten each run), never the user's own card.
DS_BIN=/Applications/DuckStation.app/Contents/MacOS/DuckStation
DS_SET="$HOME/Library/Application Support/DuckStation/settings.ini"
DS_PID=
DS_BAK=

ds_restore() { [ -n "$DS_BAK" ] && [ -f "$DS_BAK" ] && cp "$DS_BAK" "$DS_SET"; DS_BAK=; }

# ds_start OUT_PREFIX EXE
ds_start() {
    pgrep -f "DuckStation.app/Contents/MacOS" >/dev/null && { echo "DuckStation is running (another session): not starting it" >&2; exit 2; }
    mkdir -p build/ds
    /usr/bin/python3 tools/disc/build_disc.py build/iso "$2" none build/ds/game > "$1.ds_disc.log" 2>&1 || { echo "ds disc build failed ($1.ds_disc.log)" >&2; exit 1; }
    DS_BAK="$1.settings.bak"; cp "$DS_SET" "$DS_BAK"; trap ds_restore EXIT
    /usr/bin/python3 - "$DS_SET" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r'^\[MemoryCards\]\n((?:(?!\[).*\n)*)', s, re.M)
body = [l for l in m.group(1).splitlines() if not l.startswith(('Card1Type', 'Card1Path'))]
s = s[:m.start(1)] + '\n'.join(['Card1Type = NonPersistent'] + body) + '\n' + s[m.end(1):]
s = re.sub(r'^StartPaused = .*$', 'StartPaused = true', s, flags=re.M)
s = re.sub(r'^OutputMuted = .*$', 'OutputMuted = true', s, flags=re.M)
open(p, 'w').write(s)
PYEOF
    grep -q "^OutputMuted = true" "$DS_SET" || { echo "ds_env: could not mute the output" >&2; ds_restore; exit 1; }
    "$DS_BIN" -batch -fastboot -nofullscreen -- "$PWD/build/ds/game.cue" >/dev/null 2>&1 &
    DS_PID=$!
    sleep 4
    for _ in $(seq 1 26); do nc -z localhost 2345 2>/dev/null && break; sleep 1; done
}

ds_stop() { [ -n "$DS_PID" ] && { kill $DS_PID 2>/dev/null; wait $DS_PID 2>/dev/null; }; DS_PID=; ds_restore; }
