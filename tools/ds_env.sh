# DuckStation for the timing harness (sourced by tools/play/scene_run.sh EMU=ds), on the model of RSDKv5-ps1's
# tools/emu.sh: retail SCPH-101 BIOS, Interpreter, no overclock, CD speedups 1x (the user's settings), GDB 2345.
# DuckStation is shared with other sessions: it starts only when no DuckStation runs at all (otherwise exit 2), and
# the settings a run changes (a non-persistent card, StartPaused so GDB's first continue starts the boot, the output
# muted: user 2026-10-09) are backed up and restored. It boots a disc built for the run (build/ds/game: the EXE given
# + the data tree, DS_ISO or build/iso; ~574 MB, rewritten each run), never the user's own card.
DS_BIN=/Applications/DuckStation.app/Contents/MacOS/DuckStation
DS_SET="$HOME/Library/Application Support/DuckStation/settings.ini"
DS_PID=
DS_BAK=
# The user's settings.ini while a harness run holds DuckStation (every harness that edits it, 2026-10-23: a run killed
# before its restore left a throwaway memory card and StartPaused in the user's settings for days). A run that finds it
# at start puts it back first (no DuckStation runs then: its holder was killed); the restore removes it.
DS_HELD="$DS_SET.harness"

ds_restore() {
    [ -n "$DS_BAK" ] || return 0
    if [ -f "$DS_HELD" ]; then cp "$DS_HELD" "$DS_SET" && rm -f "$DS_HELD"
    elif [ -f "$DS_BAK" ]; then cp "$DS_BAK" "$DS_SET"; fi
    DS_BAK=
}

# ds_start OUT_PREFIX EXE
ds_start() {
    pgrep -f "DuckStation.app/Contents/MacOS" >/dev/null && { echo "DuckStation is running (another session): not starting it" >&2; exit 2; }
    mkdir -p build/ds
    /usr/bin/python3 tools/disc/build_disc.py "${DS_ISO:-build/iso}" "$2" none build/ds/game > "$1.ds_disc.log" 2>&1 || { echo "ds disc build failed ($1.ds_disc.log)" >&2; exit 1; }
    if [ -f "$DS_HELD" ]; then
        echo "DuckStation settings: the user's put back from $DS_HELD (a killed run left them)" >&2
        cp "$DS_HELD" "$DS_SET"
    fi
    cp "$DS_SET" "$DS_HELD"
    # the exit trap stops our DuckStation before restoring: it writes its settings (the run's) when it quits
    DS_BAK="$1.settings.bak"; cp "$DS_SET" "$DS_BAK"; trap ds_stop EXIT
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

ds_stop() {
    if [ -n "$DS_PID" ]; then
        for _ in $(seq 1 15); do kill $DS_PID 2>/dev/null || break; sleep 1; done
        kill -9 $DS_PID 2>/dev/null
        wait $DS_PID 2>/dev/null
    fi
    DS_PID=; ds_restore
}
