# AGENTS.md — RSDKv3-ps1 (Sonic CD on PlayStation)

Operating manual for this repo. Workspace rules: `../AGENTS.md`. Knowledge base: `../docs/`
(start at `../docs/README.md`). **Plan and phase table: `../docs/28-rsdkv3-sonic-cd-port-plan.md`.**
Performance at HEAD: `../docs/43`. Lessons for other ports: `../docs/29` (§12a = this port's speed pass 2).
The finished Nexus port (`../RSDKv2-ps1`, `../docs/12`, `../docs/20`, `../docs/26`) is the reference
for every PS1 subsystem.

## What this is
RSDKv3-Decompilation (Retro Engine v3, Sonic CD 2011) ported to the PS1 with **psyqo**
(C++20) on branch `ps1`. Remotes: `origin` = upstream RSDKModding (read-only), `fork` =
github.com/danielgpinheiro/RSDKv3-ps1 (public on GitHub as of 2026-10-16; push here).

**Status (2026-10-16): done.** All 12 phases; the game and its videos run on a PSone (SCPH-101, `../docs/31`).
Public builder https://github.com/danielgpinheiro/Sonic-CD-PS1-ISO-Builder (head `7e61cb1`, executable from `88949da`,
the title screen's build stamp; update notes in its `CHANGELOG.md`). The disc boots `SONICCD.EXE` (`4d45fc0`). Since then only
fixes, tools and harness changes.

## First checks (every session)
- `git status` / `git log --oneline -10` (uncommitted work may be another agent's).
- Status in `../docs/28` (and the `rsdkv3-ps1-port` skill).
- Game data: `../SonicCD_Extracted/` (Steam `Data.rsdk`, extracted `Data/` tree, `videos/*.ogv`, optional
  `videos JPN/`). Never commit game data.
- `../nugget` stays plain upstream.

## User decisions (2026-09-24)
- `ps1/` was **copied** from `RSDKv2-ps1` (`e7cc7d4`; older docs cite `d5e054b`, the same commit before a history rewrite) and is
  adapted freely here; no shared library (the v4 port copied this one in turn).
- Both soundtracks (US + JP) as CD-XA, chosen in the menu. Screen **320 px** (decomp default is
  424). Video 320×176 @ 15 fps.
- Loading icon: `../Memcard icons/sonic-1-loading.gif`. Save icon: `../Memcard icons/cd-mc-icon.png`.

## Build / run / debug
```bash
make -j8                        # rsdkv3.ps-exe (+ .elf, .map); must fit 2 MB. The EXE is always re-made from the ELF
make BOOT=R11A [BOOT_ID=n]      # start in a stage; delete RSDKv3/main.o RSDKv3/RetroEngine.o when switching BOOT
                                # (make 3.81 timestamps; scene_run.sh does it itself)
tools/data_disc.sh              # build_assets (Data.rsdk -> manifests, atlases, tiles, BG strips, audio, STR) + disc;
                                # the JP vocal FMV audio from ../SonicCD_Extracted/videos JPN when present
JP_VIDEOS= tools/data_disc.sh   # without the JP vocals (the public builder's image)
make dist                       # natural build only: licensed (../LICENSE.DAT by path; LICENSE=none for none), verified,
                                # build/dist/SonicCD-PS1-<EN|JP>.bin/.cue + SHA256SUMS; SYSTEM.CNF boots SONICCD.EXE
tools/export_builder.sh ../Sonic-CD-PS1-ISO-Builder   # natural build: EXE, SHA256SUMS, bin/README.md, converters
# Build stamp: every disc gets VERSION.TXT (tools/gen_version.py: commit, -dirty; the builder: bin/VERSION.TXT from the
# export), drawn bottom-left on the title (ps1/version_stamp.*). PS1_VERSION_FILE=path / none: that file / no file
tools/smoke_disc.sh             # phase 0 test disc (subset of SonicCD_Extracted) + make disc
tools/smoke_test.sh [OUT]       # phase 0 smoke test over GDB (files CRC, SPU, icon, pad, fps); make PLATFORM_TEST=1
tools/frame_capture.sh N OUT    # VRAM + framebuffer PNGs at frame N + renderer profile (MASK/FROM/GDBSET)
tools/vram_test.sh STAGE[_t]    # tile pages, atlas, BG strips, palette in VRAM == the build files
tools/perf_zones.sh OUT STAGE.. # frame-time survey across stages (worst frame vs 262 hblanks)
tools/boot_test.sh [OUT F1 F2]  # headless boot: loads (vblanks), perf, profile, heap, guards
make BOOT=R11A && tools/logic_test.sh   # start in a stage; drive the player via g_ps1PadTestMask
tools/load_profile.sh OUT.log [STOP]    # CD requests / files per load (a BOOT= build)
tools/memcard/save_test.sh [WORKDIR]    # save / reload on temporary card images
tools/scene3d/sort3d_check.sh   # host: PS1 3D draw-list sort == upstream order
# Speed pass 2 kit (docs/28 appendix "speed pass 2") and the docs/43 study:
tools/play/scene_run.sh OUT SCENE[:ID] FROM TO   # one scripted run: CPU hblanks / vsyncs (avg, worst); PAD=, SAMPLE=1,
        # CHECK="f.." (state + VRAM dumps), POKE= (forced paths), PERF_SPLIT=1 (SPLIT line: GPU tail, flip gap, sections),
        # GPU="f.." (chain priced by tools/play/gpu_cost.py), KEEP_RAM=1 (keep those RAM dumps), EMU=ds, NOBUILD=1
tools/play/survey.sh OUT SCENE...          # scene_run.sh over scenes with their pad scripts (same switches, SNAP=1)
python3 tools/play/gpu_cost.py RAM.bin HEAD [TAIL ...]   # DuckStation's GPU model on a frame's chain (clipped to the screen)
tools/play/ab_run.sh SCENE [FROM TO]       # reference build (worktree build/refwt) vs working tree: timing + exactness;
                                           # REF_DISC= the reference data's disc (its cache build/perf2 was deleted 10-16)
tools/play/sample_scene.sh OUT SCENE [FROM TO] [--lines FUNC]   # PC sampler profile (functions, script subs, instructions)
tools/play/vm_step.sh SCENE [FRAME N]      # machine instructions per script VM instruction (GDB single steps)
tools/play/call_trace.sh SCENE F "FUNC ..." OUT.log   # which engine calls frame F makes, with hblank stamps
tools/play/stop_print.sh SCENE F OUT.log EXPR...       # values (e.g. a native's path counters) at the end of frame F
python3 tools/bytecode_scan.py DATA --find X --values VAR [--where RS000.bin]   # who uses what
```
Time loads with the vblank IRQ count (`g_ps1GPU->m_frameCount`): psyqo `GPU::now()` stalls while
callbacks aren't pumped. `make dist` refuses `BOOT=`, `SAMPLE=1` and `PERF_SPLIT=1` builds, the exporter `BOOT=` ones:
rebuild the natural EXE first.

**Emulators** (user 2026-10-16):
- Default: the **stock PCSX-Redux app** (`/Applications/PCSX-Redux.app`), `-2mb` stock console, on this port's own
  portable instance `../pcsx-redux-cd` (GDB 3338, web 8088, OpenBIOS, muted, fresh cards each run), via
  `tools/redux_env.sh` (`354fbc7`; 2026-10-09..16 it was the patched `../pcsx-redux-16mb/redux16mb` copy, now the
  GTA III project's). Never `-8mb` / `-16mb` / `-2mbvram`. The scripts wait while 3338 / 8088 are taken and kill only the PID
  they started; never `pkill`. Launch emulators only from bash scripts (the Bash tool is zsh: `$REDUX_ARGS` doesn't
  split). Background + timeout; check the run really started. The `-portable` instance keeps `-bios` / `-memcard*` out
  of the shared `~/.config/pcsx-redux/pcsx.json` (no backup needed any more).
- DuckStation (`tools/ds_env.sh`, `EMU=ds`; SCPH-101, interpreter, GDB 2345): allowed whenever no other session runs it
  (`pgrep` first, exit 2; never a second instance), muted, own PID only, `settings.ini` backed up and restored by the
  script.
- GDB: Homebrew gdb 18.1 can't attach to PCSX-Redux's stub ("invalid remote ptid: p1.t1"): every harness script runs it
  through `tools/gdbw` + `tools/gdbrelay.py` (`73c441a`, `5474700`; `GDB=` overrides). Write new `-x` scripts with a
  plain `target remote localhost:N` line (the one line `gdbw` rewrites). Start PCSX-Redux **without `-run`** when GDB
  sets breakpoints before the first `continue`. Evidence rules: `../docs/21`.

## docs/43 fix pass (2026-10-16/17)
Results per issue: `docs/perf-43-report.md` (log: `docs/perf-43-tracker.md`; hardware facts: `docs/ps1-perf-findings.md`).
New in RSDKv3-ps1: `ps1/pad.cpp` (port 1 only), natives `PS1HudDraw` / `PS1RingTouch` (GS000), per-cell cluster lists
and the off-screen textured-face cull (Title), flip-bit tile sprites after a boot self-test (`PS1DetectGpuVersion`;
GDB `g_ps1FlipRectsOn` / `g_ps1FlipRectsForce`). Harness (PERF_SPLIT builds): `g_ps1VramCrcOn` hashes the displayed
buffer per line (GP0(C0h)) for DuckStation checks, `g_ps1VramLineY` dumps one line.

## Layout
| Path | Role |
|---|---|
| `RSDKv3/` | the engine (upstream decompilation) + PS1 code behind `RETRO_PLATFORM == RETRO_PS1` (`main.cpp` = the psyqo app) |
| `ps1/` | PS1 platform layer (see `ps1/README.md`); `ps1/platform_test.cpp` = the phase 0 smoke test (`make PLATFORM_TEST=1`) |
| `tools/disc/` | mkpsxiso BIN/CUE builder (load order, file index, `SONICCD.EXE`) + byte-level verifier |
| `tools/loading/` | loading icon builder (mode downsample of any GIF) + VRAM pixel checker |
| `tools/audio/`, `tools/fmv/` | SFX → SPU-ADPCM banks, music → CD-XA (`build_music.py`), videos → STR (`build_str.py`, `--jp`) |
| `tools/smoke_*.sh`, `vramshot.py`, `pad.lua` | phase 0 harness, VRAM dumps, pad input via the web server |
| `tools/build_assets.py`, `tools/tiles/`, `tools/bgstrips/`, `tools/atlas/`, `tools/floor/`, `tools/rsdkmanifest/` | build-time asset pipeline (per-player atlases + strips; the public builder runs the same) |
| `tools/scripts/patch_bytecode.py` | build-time bytecode edits: options window, native opcodes, `patch_unlocks` |
| `tools/play/` | speed-pass and docs/43 harness (above) |
| `tools/redux_env.sh`, `tools/ds_env.sh`, `tools/gdbw`, `tools/gdbrelay.py` | emulator instances and the GDB 18 relay |
| `ps1/render.cpp` | the PS1 renderer (replaces `RSDKv3/Drawing.cpp`; upstream side effects ported explicitly) |

## Invariants (inherited from the Nexus port — don't break these)
- **2 MB RAM** (`RAM_SIZE=2M`); track heap high-water incl. load peaks.
- **Golden rule**: no runtime asset decoding (GIF, Ogg, Theora, scripts): convert at build time,
  and every converter decodes its output back and compares.
- Per-frame drawing is **DMA-chained** (`ps1/gpu_chain.hh`); fragments ≤ 14 words; immediate
  uploads call `PS1GpuImmediate()` first; `uploadToVRAM` widths even.
- **Pads = `psyqo::AdvancedPad`**, never `SimplePad`.
- **One CD action at a time** (XA music / video streams vs file reads); every CD wait bounded (`../docs/40`).
- CLUT index 0 = transparent (0x0000); visible black = 0x8000.
- Hardware access in `start()` / `frame()`, not `prepare()`.
- `ps1/libc` maths is ours: check it against PC values.

## Definition of done for a step
Builds cleanly · fits 2 MB · verified in the emulator with evidence (GDB counters, VRAM dumps,
byte/pixel comparisons, measured numbers) · `../docs/28` status updated · commit only when the user
asks (prefixes `feature:` `fix:` `chore:` `housekeeping:` `FMV:`, push `fork ps1`; no Claude / AI attribution, no
`Co-Authored-By` line, here or in the builder).
