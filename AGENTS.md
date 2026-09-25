# AGENTS.md — RSDKv3-ps1 (Sonic CD on PlayStation)

Operating manual for this repo. Workspace rules: `../AGENTS.md`. Knowledge base: `../docs/`
(start at `../docs/README.md`). **Plan and phase table: `../docs/28-rsdkv3-sonic-cd-port-plan.md`.**
The finished Nexus port (`../RSDKv2-ps1`, `../docs/12`, `../docs/20`, `../docs/26`) is the reference
for every PS1 subsystem.

## What this is
RSDKv3-Decompilation (Retro Engine v3, Sonic CD 2011) being ported to the PS1 with **psyqo**
(C++20) on branch `ps1`. Remotes: `origin` = upstream RSDKModding (read-only), `fork` =
github.com/danielgpinheiro/RSDKv3-ps1 (**private**; push here).

## First checks (every session)
- `git status` / `git log --oneline -10` (uncommitted work may be another agent's).
- Phase + status in `../docs/28` (and the `rsdkv3-ps1-port` skill).
- Game data: `../SonicCD_Extracted/` (Steam `Data.rsdk`, extracted `Data/` tree, `videos/*.ogv`).
  Never commit game data.
- `../nugget` stays plain upstream.

## User decisions (2026-09-24)
- `ps1/` was **copied** from `RSDKv2-ps1` (`d5e054b`) and is adapted freely here; no shared
  library until the v4 port.
- Both soundtracks (US + JP) as CD-XA, chosen in the menu. Screen **320 px** (decomp default is
  424). Video 320×176 @ 15 fps to start (tune in phase 6).
- Loading icon: `../Memcard icons/sonic-1-loading.gif`. Save icon: `../Memcard icons/cd-mc-icon.png`.

## Build / run / debug
```bash
make -j8                        # rsdkv3.ps-exe (+ .elf, .map); must fit 2 MB
tools/smoke_disc.sh             # phase 0 test disc (subset of SonicCD_Extracted) + make disc
tools/smoke_test.sh [OUT]       # phase 0 smoke test over GDB (files CRC, SPU, icon, pad, fps)
tools/data_disc.sh              # build_assets (Data.rsdk -> manifests, atlases, tiles, BG strips) + disc
tools/frame_capture.sh N OUT    # VRAM + framebuffer PNGs at frame N + renderer profile (MASK/FROM/GDBSET)
tools/vram_test.sh STAGE[_t]    # tile pages, atlas, BG strips, palette in VRAM == the build files
tools/perf_zones.sh OUT STAGE.. # frame-time survey across stages (worst frame vs 262 hblanks)
tools/boot_test.sh [OUT F1 F2]  # headless boot: loads (vblanks), perf, profile, heap, guards
make BOOT=R11A && tools/logic_test.sh   # start in a stage; drive the player via g_ps1PadTestMask
tools/scene3d/sort3d_check.sh   # host: PS1 3D draw-list sort == upstream order
python3 tools/bytecode_scan.py DATA --find X --values VAR [--where RS000.bin]   # who uses what
```
Time loads with the vblank IRQ count (`g_ps1GPU->m_frameCount`): psyqo `GPU::now()` stalls while
callbacks aren't pumped.
PCSX-Redux in the background with a timeout, GDB for evidence (`../docs/21`). Start it
**without `-run`** when GDB sets breakpoints before the first `continue`. Back up
`~/.config/pcsx-redux/pcsx.json` around `-bios` / `-memcard*` runs.

## Layout
| Path | Role |
|---|---|
| `RSDKv3/` | the engine (upstream decompilation; PS1 code arrives in phase 2 behind `RETRO_PLATFORM == RETRO_PS1`) |
| `ps1/` | PS1 platform layer (see `ps1/README.md`); `ps1/platform_test.cpp` = phase 0 app until the engine's `main.cpp` takes over |
| `tools/disc/` | mkpsxiso BIN/CUE builder + byte-level verifier (from the Nexus port; license `none` by default here) |
| `tools/loading/` | loading icon builder (mode downsample of any GIF) + VRAM pixel checker |
| `tools/audio/` | SFX → `.vag` (v2 version; reworked for Sonic CD in phase 5), XA helpers |
| `tools/smoke_*.sh`, `vramshot.py`, `pad.lua` | phase 0 harness, VRAM dumps, pad input via the web server |
| `tools/build_assets.py`, `tools/tiles/`, `tools/bgstrips/`, `tools/atlas/`, `tools/rsdkmanifest/` | build-time asset pipeline (per-player atlases + strips) |
| `ps1/render.cpp` | the PS1 renderer (replaces `RSDKv3/Drawing.cpp`; upstream side effects ported explicitly) |

## Invariants (inherited from the Nexus port — don't break these)
- **2 MB RAM** (`RAM_SIZE=2M`); track heap high-water incl. load peaks.
- **Golden rule**: no runtime asset decoding (GIF, Ogg, Theora, scripts): convert at build time,
  and every converter decodes its output back and compares.
- Per-frame drawing is **DMA-chained** (`ps1/gpu_chain.hh`); fragments ≤ 14 words; immediate
  uploads call `PS1GpuImmediate()` first; `uploadToVRAM` widths even.
- **Pads = `psyqo::AdvancedPad`**, never `SimplePad`.
- **One CD action at a time** (XA music / video streams vs file reads).
- CLUT index 0 = transparent (0x0000); visible black = 0x8000.
- Hardware access in `start()` / `frame()`, not `prepare()`.
- `ps1/libc` maths is ours: check it against PC values.

## Definition of done for a step
Builds cleanly · fits 2 MB · verified in PCSX-Redux with evidence (GDB counters, VRAM dumps,
byte/pixel comparisons, measured numbers) · `../docs/28` status updated · commit only when the user
asks (prefixes `feature:` `fix:` `chore:` `housekeeping:` `FMV:`, push `fork ps1`).
