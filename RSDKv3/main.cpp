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
volatile uint32_t g_ps1PadTestMask         = 0; // GDB: engine input bits to force (phase 4 brings the pad)
static uint32_t s_ps1PerfLastCount         = 0;

psyqo::GPU *g_ps1GPU            = nullptr;
psyqo::AdvancedPad *g_ps1Pad    = nullptr;
psyqo::CDRomDevice *g_ps1CDRom  = nullptr;

__attribute__((noinline)) static void PS1PerfTick(psyqo::GPU &gpu) {
    uint32_t now = gpu.getFrameCount();
    if (g_ps1PerfFrames) {
        uint32_t d               = now - s_ps1PerfLastCount;
        g_ps1PerfVsyncs          = g_ps1PerfVsyncs + d;
        g_ps1PerfLastFrameVsyncs = d;
    }
    s_ps1PerfLastCount = now;
    g_ps1PerfFrames    = g_ps1PerfFrames + 1;
    uintptr_t hs = (uintptr_t)&__heap_start, ss = (uintptr_t)&__stack_start;
    uintptr_t top = (uintptr_t)psyqo_heap_end();
    if (top < hs)
        top = hs;
    g_ps1HeapTotal    = ss - hs;
    g_ps1HeapHeadroom = ss - top;
}

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
    }

    void start() override {
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        m_pad.initialize(psyqo::AdvancedPad::PollingMode::Fast); // never SimplePad (Nexus docs/26 F14)
        g_ps1Pad = &m_pad;
#ifdef PS1_BOOT_FOLDER
        StrCopy(Engine.startSceneFolder, PS1_BOOT_FOLDER);
        StrCopy(Engine.startSceneID, PS1_BOOT_ID);
#endif
        PS1LoadingBegin(); // boot load: GameConfig, global scripts, first stage
        Engine.Init();
        PS1LoadingEnd();
        g_ps1InitDone = 1;
    }

    void frame() override {
        uint32_t t0 = PS1Hblanks();
        Engine.RunFrame();
        uint32_t cpu     = PS1HblanksSince(t0);
        g_ps1PerfLastHbl = cpu;
        if (cpu > g_ps1PerfMaxHbl)
            g_ps1PerfMaxHbl = cpu;
        if (cpu > 262)
            g_ps1PerfOverHbl = g_ps1PerfOverHbl + 1;
        PS1PerfTick(gpu());
    }

    psyqo::CDRomDevice m_cdrom;
    psyqo::AdvancedPad m_pad;
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
