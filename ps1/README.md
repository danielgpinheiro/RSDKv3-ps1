# ps1/ — PlayStation platform layer

Copied from `RSDKv2-ps1/ps1/` at commit `d5e054b` (2026-09-24) and adapted freely for Sonic CD
(user decision, `../../docs/28`: no shared library until the RSDKv4 port). Each file's header
describes it; the Nexus-era notes in them still apply unless changed here.

Differences from the RSDKv2 copy:
- `loading_icon.h` is generated from `PS1/Memcard icons/sonic-1-loading.gif` (Sonic running,
  4 frames, reduced to 48x48 cells, 83 ms/frame) by `tools/loading/build_icon.py`.
- `platform_test.cpp` (phase 0 smoke test) replaces the engine's `main.cpp` until phase 2.
