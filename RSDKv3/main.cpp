#include "RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_PS1
/*
 * PS1 app (docs/28 phase 2): psyqo owns the loop. start() boots the engine (Engine.Init: GameConfig,
 * first stage) inside a bracketed load (loading icon); frame() runs one engine frame
 * (RetroEngine::RunFrame) and updates the GDB counters below. Rendering, audio, video and the
 * real pad arrive in phases 3-6; until then the game logic runs headless.
 *
 * `make BOOT=<stage folder>` (e.g. R11A): start in that stage (Engine.startSceneFolder / ID, the
 * same fields the PC `stage=` argument sets) for the test harness.
 */
#include <psyqo/advancedpad.hh>
#include <psyqo/hardware/gpu.hh>
#include "../ps1/pad.hh"
#include <psyqo/alloc.h>
#include <psyqo/application.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>

#include "../ps1/loading.hh"
#include "../ps1/video.hh"

extern "C" char __heap_start, __stack_start;
volatile uint32_t g_ps1PerfFrames          = 0; // frames run (engine RunFrame calls)
volatile uint32_t g_ps1PerfVsyncs          = 0; // vsyncs elapsed across those frames
volatile uint32_t g_ps1PerfLastFrameVsyncs = 0; // vsyncs taken by the last frame (1 = 60 fps)
volatile uint32_t g_ps1PerfLastHbl         = 0; // RunFrame CPU time of the last frame (hblanks, 262 = 1 frame)
volatile uint32_t g_ps1PerfMaxHbl          = 0; // longest RunFrame CPU time (hblanks)
volatile uint32_t g_ps1PerfOverHbl         = 0; // frames whose CPU time exceeded one frame
volatile uint32_t g_ps1HeapTotal           = 0; // bytes between bss end and the stack
volatile uint32_t g_ps1HeapHeadroom        = 0; // bytes never reached by the heap high-water mark
volatile uint32_t g_ps1InitDone            = 0; // Engine.Init() returned
static uint32_t s_ps1PerfLastCount         = 0;
// Measurement window (docs/28 speed pass 2): frames g_ps1PerfWinFrom <= n < g_ps1PerfWinTo (set from GDB) add their
// CPU time and vsyncs here (tools/play/scene_run.sh).
volatile uint32_t g_ps1PerfWinFrom   = 0xFFFFFFFF;
volatile uint32_t g_ps1PerfWinTo     = 0;
volatile uint32_t g_ps1PerfWinFrames = 0; // frames counted
volatile uint32_t g_ps1PerfWinHbl    = 0; // sum of their CPU hblanks
volatile uint32_t g_ps1PerfWinVbl    = 0; // sum of their vsyncs (1 a frame = 60 fps)
volatile uint32_t g_ps1PerfWinMaxHbl = 0; // worst CPU hblanks
volatile uint32_t g_ps1PerfWinMaxAt  = 0; // the frame it was
volatile uint32_t g_ps1PerfWinMaxVbl = 0; // worst vsyncs
volatile uint32_t g_ps1PerfWinOver   = 0; // frames over one vsync
// Only frames of the scene the window started in, without a load, count: a reload (a lost life), a time warp or the
// stage's end (its stage index changes before the results screen) stops the count (g_ps1PerfWinSkipped: frames left out).
volatile uint32_t g_ps1PerfWinSkipped = 0;
#ifdef PS1_PERF_SPLIT
// `make PERF_SPLIT=1` (docs/43, harness only): the frame time besides the CPU's. psyqo sends a frame's chain right after
// the flip, so the GPU draws it while the CPU builds the next frame, and the flip waits for that chain and a new vblank.
// GpuTail: hblanks the GPU was still drawing the previous frame's chain (its DMA, up to the GPU FIFO's last 16 words)
// after this frame's CPU work ended; 0 in PCSX-Redux, which draws instantly, modelled by DuckStation. Flip: hblanks from
// the end of frame() to the next frame() (the vblank wait; the tail is already waited for). Window sums as above, plus
// the window sums of the per-frame section counters (g_ps1PerfWinSec[], order as s_ps1PerfSec).
volatile uint32_t g_ps1PerfGpuTail = 0, g_ps1PerfFlip = 0;
volatile uint32_t g_ps1PerfWinTail = 0, g_ps1PerfWinMaxTail = 0, g_ps1PerfWinTailFrames = 0; // sum / worst / frames > 0
volatile uint32_t g_ps1PerfWinFlip = 0, g_ps1PerfWinMaxFlip = 0;
static uint32_t s_ps1FrameEnd = 0;
// Framebuffer checksums for exact A/B checks where the harness can't read VRAM (DuckStation, docs/43 G3): with
// g_ps1VramCrcOn set (GDB), each frame end hashes every line of the buffer the previous frame's chain drew (the picture
// on screen; the GPU is idle after waitChainIdle) into g_ps1VramCrc[] by a GP0(C0h) VRAM read. Harness only (slow).
int PS1ChainBufferY(); // ps1/gpu_chain.cpp
volatile uint32_t g_ps1VramCrcOn = 0;
volatile uint32_t g_ps1VramCrc[240];
volatile uint32_t g_ps1VramLineY = 0xFFFF; // GDB: this line's 320 pixels also go to g_ps1VramLine
volatile uint16_t g_ps1VramLine[320];
static void PS1VramCrc()
{
    using namespace psyqo::Hardware;
    const int y0 = PS1ChainBufferY() ? 0 : 256;
    while (!(GPU::Ctrl & (1 << 26))) {} // ready for a command word
    GPU::Data = 0x01000000;               // clear the texture cache
    while (!(GPU::Ctrl & (1 << 26))) {}
    GPU::Data = 0xC0000000;               // VRAM -> CPU
    GPU::Data = (uint32_t)y0 << 16;
    GPU::Data = (240u << 16) | 320u;
    while (!(GPU::Ctrl & (1 << 27))) {}   // ready to send VRAM
    for (int y = 0; y < 240; ++y) {
        uint32_t h = 0;
        for (int k = 0; k < 160; ++k) {
            uint32_t px = (uint32_t)GPU::Data;
            h = h * 31u + px;
            if ((uint32_t)y == g_ps1VramLineY)
                g_ps1VramLine[2 * k] = (uint16_t)px, g_ps1VramLine[2 * k + 1] = (uint16_t)(px >> 16);
        }
        g_ps1VramCrc[y] = h;
    }
}
extern volatile uint32_t g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1ProfObjDrawHbl, g_ps1ProfFloorsHbl, g_ps1ProfLayer[5];
extern volatile uint32_t g_ps1Prof3DTransformHbl, g_ps1Prof3DSortHbl, g_ps1Prof3DDrawHbl;
// objects, draw (all of DrawStageGFX), object draw subs, 3D floors, FG layers, per-line passes, tile bands, strip bands,
// 3D transform, sort, draw (the last Draw3DScene: stale outside 3D scenes)
static volatile uint32_t *const s_ps1PerfSec[11] = {
    &g_ps1ProfObjectsHbl, &g_ps1ProfDrawHbl, &g_ps1ProfObjDrawHbl, &g_ps1ProfFloorsHbl, &g_ps1ProfLayer[0],
    &g_ps1ProfLayer[1], &g_ps1ProfLayer[2], &g_ps1ProfLayer[3], &g_ps1Prof3DTransformHbl, &g_ps1Prof3DSortHbl,
    &g_ps1Prof3DDrawHbl};
volatile uint32_t g_ps1PerfWinSec[11] = {};
#endif
static uint32_t s_ps1PerfWinLoads = 0, s_ps1PerfLoadsBefore = 0;
static bool s_ps1PerfWinStopped = false;
static int s_ps1PerfWinStage = 0;
extern volatile uint32_t g_ps1LoadingLoads; // ps1/loading.cpp: outermost loads begun
// Test stops: engine frames (ascending) at which the frame end calls ps1TestStop, where GDB breaks (one stop per listed
// frame instead of a conditional breakpoint evaluated every frame over the stub).
volatile uint32_t g_ps1TestStops[8] = {};
volatile uint32_t g_ps1TestStopCount = 0, g_ps1TestStopPos = 0;
extern "C" __attribute__((noinline)) void ps1TestStop() { __asm__ volatile(""); }

psyqo::GPU *g_ps1GPU            = nullptr;
PS1Pad *g_ps1Pad    = nullptr;
psyqo::CDRomDevice *g_ps1CDRom  = nullptr;

void PS1RenderBeginFrame(psyqo::GPU &gpu); // ps1/render.cpp
void PS1VideoTestService();                 // RSDKv3/Video.cpp (GDB test hook)
void PS1SaveTestService();                  // RSDKv3/Userdata.cpp (GDB test hook)
void PS1McPrepare();                        // ps1/memcard.cpp
void PS1RenderEndFrame();
void PS1DetectGpuVersion(); // ps1/render.cpp
void PS1IrqGuardInstall();                  // ps1/irq_guard.cpp
void PS1SamplerInstall();                   // ps1/pc_sampler.cpp (`make SAMPLE=1`; empty otherwise)

__attribute__((noinline)) static void PS1PerfTick(psyqo::GPU &gpu) {
    uint32_t now = gpu.getFrameCount();
    if (g_ps1PerfFrames) {
        uint32_t d               = now - s_ps1PerfLastCount;
        g_ps1PerfVsyncs          = g_ps1PerfVsyncs + d;
        g_ps1PerfLastFrameVsyncs = d;
        uint32_t n = g_ps1PerfFrames;
        int stage = activeStageList * 0x100 + stageListPosition;
        if (n == g_ps1PerfWinFrom)
            s_ps1PerfWinLoads = s_ps1PerfLoadsBefore, s_ps1PerfWinStopped = false, s_ps1PerfWinStage = stage;
        if (n >= g_ps1PerfWinFrom && n < g_ps1PerfWinTo && (g_ps1LoadingLoads != s_ps1PerfWinLoads || stage != s_ps1PerfWinStage))
            s_ps1PerfWinStopped = true;
        if (n >= g_ps1PerfWinFrom && n < g_ps1PerfWinTo && s_ps1PerfWinStopped)
            g_ps1PerfWinSkipped = g_ps1PerfWinSkipped + 1;
        else if (n >= g_ps1PerfWinFrom && n < g_ps1PerfWinTo) {
            uint32_t cpu       = g_ps1PerfLastHbl;
            g_ps1PerfWinFrames = g_ps1PerfWinFrames + 1;
            g_ps1PerfWinHbl    = g_ps1PerfWinHbl + cpu;
            g_ps1PerfWinVbl    = g_ps1PerfWinVbl + d;
            if (cpu > g_ps1PerfWinMaxHbl)
                g_ps1PerfWinMaxHbl = cpu, g_ps1PerfWinMaxAt = n;
            if (d > g_ps1PerfWinMaxVbl)
                g_ps1PerfWinMaxVbl = d;
            if (d > 1)
                g_ps1PerfWinOver = g_ps1PerfWinOver + 1;
#ifdef PS1_PERF_SPLIT
            uint32_t t = g_ps1PerfGpuTail, fl = g_ps1PerfFlip;
            g_ps1PerfWinTail = g_ps1PerfWinTail + t;
            g_ps1PerfWinFlip = g_ps1PerfWinFlip + fl;
            if (t)
                g_ps1PerfWinTailFrames = g_ps1PerfWinTailFrames + 1;
            if (t > g_ps1PerfWinMaxTail)
                g_ps1PerfWinMaxTail = t;
            if (fl > g_ps1PerfWinMaxFlip)
                g_ps1PerfWinMaxFlip = fl;
            for (int k = 0; k < 11; ++k)
                g_ps1PerfWinSec[k] = g_ps1PerfWinSec[k] + *s_ps1PerfSec[k];
#endif
        }
    }
    s_ps1PerfLastCount   = now;
    s_ps1PerfLoadsBefore = g_ps1LoadingLoads; // loads begun up to the end of this frame
    if (g_ps1TestStopPos < g_ps1TestStopCount && g_ps1PerfFrames == g_ps1TestStops[g_ps1TestStopPos]) {
        g_ps1TestStopPos = g_ps1TestStopPos + 1;
        ps1TestStop(); // g_ps1PerfFrames = the listed frame here
    }
    g_ps1PerfFrames    = g_ps1PerfFrames + 1;
    uintptr_t hs = (uintptr_t)&__heap_start, ss = (uintptr_t)&__stack_start;
    uintptr_t top = (uintptr_t)psyqo_heap_end();
    if (top < hs)
        top = hs;
    g_ps1HeapTotal    = ss - hs;
    g_ps1HeapHeadroom = ss - top;
}

#ifdef PS1_BOOT_FOLDER
extern "C" const char g_ps1BootBuild[] = PS1_BOOT_FOLDER; // marks a `make BOOT=` test build (make dist refuses it)
#endif

namespace {

class RSDKv3PS1App final : public psyqo::Application {
    void prepare() override {
        gpu().initialize(psyqo::GPU::Configuration{}
                             .set(psyqo::GPU::Resolution::W320)
                             .set(psyqo::GPU::VideoMode::AUTO)
                             .set(psyqo::GPU::ColorMode::C15BITS)
                             .set(psyqo::GPU::Interlace::PROGRESSIVE)
                             .set(psyqo::GPU::MiscSetting::CLEAR_VRAM));
        g_ps1GPU = &gpu();
        m_cdrom.prepare();
        PS1McPrepare(); // memory card saves (RSDKv3/Userdata.cpp, docs/28 phase 8)
    }

    void start() override {
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        PS1SamplerInstall();
        PS1IrqGuardInstall(); // re-raises CD / DMA IRQ edges lost to psyqo's I_STAT read-modify-write (ps1/irq_guard.hh)
        m_pad.initialize(); // psyqo AdvancedPad's driver on port 1 only (ps1/pad.hh, docs/43 C11); never SimplePad (Nexus docs/26 F14)
        g_ps1Pad = &m_pad;
#ifdef PS1_BOOT_FOLDER
        StrCopy(Engine.startSceneFolder, g_ps1BootBuild);
        StrCopy(Engine.startSceneID, PS1_BOOT_ID);
#endif
        PS1DetectGpuVersion(); // flip-bit sprites for flipped tiles on the new GPU (ps1/render.cpp, docs/43 G3)
        PS1LoadingBegin(); // boot load: GameConfig, global scripts, first stage
        Engine.Init();
#ifdef PS1_BOOT_FOLDER
        // Sonic CD's scripts set Options.GameMode before starting a stage (tools/bytecode_scan.py:
        // Title / Stage Select 0, Menu 0 or 1, Time Attack 2). A `BOOT=` stage starts like the
        // Stage Select does (BOOT_GAMEMODE, default 0).
        SetGlobalVariableByName("Options.GameMode", PS1_BOOT_GAMEMODE);
        playerListPos = PS1_BOOT_PLAYER; // 0 Sonic, 1 Tails (the menu sets it; per-player atlas, docs/28 3.2)
#endif
        PS1LoadingEnd();
        g_ps1InitDone = 1;
    }

    void frame() override {
        uint32_t t0 = PS1Hblanks();
#ifdef PS1_PERF_SPLIT
        if (s_ps1FrameEnd)
            g_ps1PerfFlip = (t0 - s_ps1FrameEnd) & 0xffff;
#endif
        PS1AudioUpdate();     // frees SFX channels whose sample ended (before the scripts run), music volume
        PS1SpuDebugService();
        PS1VideoTestService(); // GDB: g_ps1VideoTestPlay
        PS1SaveTestService();  // GDB: g_ps1SaveTest
        PS1RenderBeginFrame(gpu()); // all drawing is DMA-chained; psyqo sends it after frame() returns
        Engine.RunFrame();
        PS1RenderEndFrame();
        uint32_t cpu     = PS1HblanksSince(t0);
        g_ps1PerfLastHbl = cpu;
        if (cpu > g_ps1PerfMaxHbl)
            g_ps1PerfMaxHbl = cpu;
        if (cpu > 262)
            g_ps1PerfOverHbl = g_ps1PerfOverHbl + 1;
#ifdef PS1_PERF_SPLIT
        uint32_t tw = PS1Hblanks();
        gpu().waitChainIdle(); // psyqo's flip waits for it too
        g_ps1PerfGpuTail = PS1HblanksSince(tw);
        if (g_ps1VramCrcOn)
            PS1VramCrc();
#endif
        PS1PerfTick(gpu());
#ifdef PS1_PERF_SPLIT
        s_ps1FrameEnd = PS1Hblanks();
#endif
    }

    psyqo::CDRomDevice m_cdrom;
    PS1Pad m_pad;
};

RSDKv3PS1App s_app;

} // namespace

int main() { return s_app.run(); }

#else // PC and other platforms: the upstream main

#if !RETRO_USE_ORIGINAL_CODE

#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
#include "Windows.h"
#endif

void parseArguments(int argc, char *argv[])
{
    for (int a = 0; a < argc; ++a) {
        const char *find = "";

        find = strstr(argv[a], "stage=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneFolder[b++] = find[c++];
            Engine.startSceneFolder[b] = 0;
        }

        find = strstr(argv[a], "scene=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneID[b++] = find[c++];
            Engine.startSceneID[b] = 0;
        }

        find = strstr(argv[a], "console=true");
        if (find) {
            engineDebugMode       = true;
            Engine.devMenu        = true;
            Engine.consoleEnabled = true;
#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
            AllocConsole();
            freopen_s((FILE **)stdin, "CONIN$", "w", stdin);
            freopen_s((FILE **)stdout, "CONOUT$", "w", stdout);
            freopen_s((FILE **)stderr, "CONOUT$", "w", stderr);
#endif
        }

        find = strstr(argv[a], "usingCWD=true");
        if (find) {
            usingCWD = true;
        }
    }
}
#endif

int main(int argc, char *argv[])
{
#if !RETRO_USE_ORIGINAL_CODE
    parseArguments(argc, argv);
#endif

    Engine.Init();
    Engine.Run();

#if !RETRO_USE_ORIGINAL_CODE
    if (Engine.consoleEnabled) {
#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
        FreeConsole();
#endif
    }
#endif

    return 0;
}
#endif // RETRO_PLATFORM == RETRO_PS1
