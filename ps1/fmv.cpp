/*
 * Full-screen FMV — see fmv.hh.
 */
#include "fmv.hh"
#include "cdrom_stream.hh"
#include "gpu_chain.hh"
#include "irq_guard.hh"
#include "loading.hh"
#include "video.hh"
#include "xa_music.hh"

#include <stdio.h>
#include <string.h>

#include <psyqo/advancedpad.hh>
#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>

extern psyqo::GPU *g_ps1GPU;
#include "pad.hh"
extern PS1Pad *g_ps1Pad; // docs/43 C11

void PS1RenderFmvFrame(int mode);           // ps1/render.cpp: 0 = black, 1 = new picture (bars only), 2 = repeat
void PS1RenderFmvFade(int step);            // ps1/render.cpp: darken the whole frame by step / 255
void PS1RenderFmvKeepRows(int y0, int y1, int y2, int y3); // ps1/render.cpp: two bands the FMV frame leaves alone

// Video / stream counters (ps1/video.cpp) shown by the diagnostics strip.
extern volatile uint32_t g_ps1VideoFrames, g_ps1VideoPictures, g_ps1VideoResync, g_ps1VideoDropped;
extern volatile uint32_t g_ps1VideoSyncFail, g_ps1VideoCdDeferred, g_ps1VideoVlcHbl, g_ps1VideoMdecHbl;
extern volatile uint32_t g_ps1MdecMode, g_ps1MdecFaults, g_ps1MdecSwitches, g_ps1MdecSnap[8], g_ps1MdecSnapTaken;
extern volatile int32_t g_ps1MdecOrder;
extern volatile uint32_t g_ps1MdecFail[6];
extern volatile int32_t g_ps1MdecPadMode;
uint32_t PS1VideoDataSectors();

volatile uint32_t g_ps1FmvPlays   = 0;
volatile uint32_t g_ps1FmvFails   = 0;
volatile uint32_t g_ps1FmvFrames  = 0;
volatile uint32_t g_ps1FmvSkipped = 0;
volatile uint32_t g_ps1FmvTimeouts = 0; // videos closed by the length watchdog
volatile uint32_t g_ps1FmvOverlayForce = 0; // GDB test hook: 1 = diagnostics strip on (as if SELECT toggled it)

#define FMV_ARENA_WORDS 256 // per parity while a video plays: fills, a copy and the fade
#define FMV_Y           32  // 176 lines letterboxed in 240
#define FMV_FADE_STEP   8   // as upstream ProcessVideo: fadeMode += 8 per frame after a skip
// Length watchdog: a video never plays longer than its frames at 15 fps (4 vblanks each at 60 Hz) plus
// 5 s, whatever the stream does: the game always gets back control (the PSone FMV hang, docs/28).
#define FMV_VBLANKS_PER_FRAME 4
#define FMV_SLACK_VBLANKS     300

namespace {
enum class State : uint8_t { IDLE, REQUESTED, PLAYING, CLOSING, CLOSED };
State s_state = State::IDLE;
char s_path[64];
int s_audio      = 0;
bool s_picture   = false; // a picture has been shown (else frames stay black)
bool s_skipped   = false;
int s_fade       = 0;
uint32_t s_openVbl  = 0; // frame count when the video opened
uint32_t s_limitVbl = 0; // the watchdog's limit (vblanks since s_openVbl)

void swapArenas(uint32_t words) {
    PS1ChainSuspend(); // nothing chained yet this frame: the arenas go back to the heap
    PS1ChainSetArenaWords(words);
}

// ---- Diagnostics strip (SELECT toggles it during a video; an MDEC fault shows it) -------------------
// For real hardware, where there is no debugger: three text lines in the bottom letterbox and two in the
// top one, in both buffers, drawn by the CPU into a 320 x 8 line buffer (heap, only while shown) and
// uploaded. The values count from the video's start. If the game hangs, the last strip stays on screen.
#define OVL_Y     212 // 3 lines of 8 rows in the bottom bar (208..240)
#define OVL_LINES 3
#define OVL_TOP_Y 8   // 2 lines in the top bar (0..32): MDEC mode and the register snapshot
#define OVL_TOP_LINES 2
#define OVL_W     320
uint16_t *s_ovl   = nullptr;
bool s_ovlOn      = false;
bool s_selectWas  = false;
bool s_autoShown  = false; // shown by a fault once this video
struct Base { // counters when the video opened
    uint32_t int1, discarded, frames, pictures, resync, dropped, syncFail, deferred, cdKicks, dmaKicks, cdStale;
    uint32_t mdecFaults, mdecSwitches;
} s_base;

void takeBase() {
    s_base = { g_ps1StreamInt1, g_ps1StreamDiscarded, g_ps1VideoFrames, g_ps1VideoPictures, g_ps1VideoResync,
               g_ps1VideoDropped, g_ps1VideoSyncFail, g_ps1VideoCdDeferred, g_ps1CdIrqKicks, g_ps1DmaIrqKicks,
               g_ps1CdIrqStale, g_ps1MdecFaults, g_ps1MdecSwitches };
    s_autoShown = false;
}

// 5 x 7 glyphs, row-major, bit 4 = leftmost (generated from glyph art, round-trip checked).
static const uint8_t s_font[40][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}, // '-'
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, // '/'
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, // '0'
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, // '1'
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, // '2'
    {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}, // '3'
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, // '4'
    {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}, // '5'
    {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, // '6'
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // '7'
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, // '8'
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}, // '9'
    {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}, // ':'
    {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // 'A'
    {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, // 'B'
    {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, // 'C'
    {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c}, // 'D'
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, // 'E'
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, // 'F'
    {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, // 'G'
    {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // 'H'
    {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, // 'I'
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}, // 'J'
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // 'K'
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, // 'L'
    {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, // 'M'
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, // 'N'
    {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // 'O'
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, // 'P'
    {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, // 'Q'
    {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, // 'R'
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, // 'S'
    {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // 'T'
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // 'U'
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, // 'V'
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, // 'W'
    {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, // 'X'
    {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, // 'Y'
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, // 'Z'
};

const uint8_t *glyph(char c) {
    if (c == '-')
        return s_font[1];
    if (c == '/')
        return s_font[2];
    if (c >= '0' && c <= ':')
        return s_font[3 + (c - '0')];
    if (c >= 'A' && c <= 'Z')
        return s_font[14 + (c - 'A')];
    return s_font[0];
}

// One text line into s_ovl (6-pixel cells, white on black; STP set as on every FMV texel).
void drawLine(const char *text) {
    for (int i = 0; i < OVL_W * 8; i++)
        s_ovl[i] = 0x8000;
    for (int col = 0; *text && col + 6 <= OVL_W; text++, col += 6) {
        const uint8_t *g = glyph(*text);
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x))
                    s_ovl[y * OVL_W + col + x] = 0xffff;
    }
}

void closeOverlay() {
    psyqo_free(s_ovl);
    s_ovl   = nullptr;
    s_ovlOn = false;
    PS1RenderFmvKeepRows(0, 0, 0, 0);
}

void updateOverlay() {
    bool select = g_ps1Pad && g_ps1Pad->isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Select);
    bool toggle = select && !s_selectWas;
    s_selectWas = select;
    static uint32_t s_forceWas = 0; // GDB hook g_ps1FmvOverlayForce: a change to 1 / 0 shows / hides it
    if (g_ps1FmvOverlayForce != s_forceWas) {
        s_forceWas = g_ps1FmvOverlayForce;
        toggle     = (s_forceWas != 0) != s_ovlOn;
    }
    if (!s_autoShown && g_ps1MdecFaults != s_base.mdecFaults) { // the MDEC got stuck: show it without SELECT
        s_autoShown = true;
        toggle      = !s_ovlOn;
    }
    if (toggle) {
        if (s_ovlOn)
            closeOverlay();
        else if ((s_ovl = (uint16_t *)psyqo_malloc(OVL_W * 8 * sizeof(uint16_t))) != nullptr) {
            s_ovlOn = true;
            PS1RenderFmvKeepRows(OVL_Y, OVL_Y + OVL_LINES * 8, OVL_TOP_Y, OVL_TOP_Y + OVL_TOP_LINES * 8);
        }
    }
    if (!s_ovlOn)
        return;
    char line[OVL_LINES][56];
    snprintf(line[0], sizeof(line[0]), "VB %5u  INT1 %5u  SECT %5u/%5u  DISC %u",
             (unsigned)(g_ps1GPU->getFrameCount() - s_openVbl), (unsigned)(g_ps1StreamInt1 - s_base.int1),
             (unsigned)PS1StreamDelivered(), (unsigned)PS1VideoDataSectors(), (unsigned)(g_ps1StreamDiscarded - s_base.discarded));
    snprintf(line[1], sizeof(line[1]), "FRM %4u/%4u  PIC %4u  RS %u  DROP %u  SF %u",
             (unsigned)(g_ps1VideoFrames - s_base.frames), (unsigned)PS1VideoFrameCount(),
             (unsigned)(g_ps1VideoPictures - s_base.pictures), (unsigned)(g_ps1VideoResync - s_base.resync),
             (unsigned)(g_ps1VideoDropped - s_base.dropped), (unsigned)(g_ps1VideoSyncFail - s_base.syncFail));
    snprintf(line[2], sizeof(line[2]), "VLC %3u  MDEC %3u  DEF %5u  KICK %u/%u/%u  ERR %u",
             (unsigned)g_ps1VideoVlcHbl, (unsigned)g_ps1VideoMdecHbl, (unsigned)(g_ps1VideoCdDeferred - s_base.deferred),
             (unsigned)(g_ps1CdIrqKicks - s_base.cdKicks), (unsigned)(g_ps1DmaIrqKicks - s_base.dmaKicks),
             (unsigned)(g_ps1CdIrqStale - s_base.cdStale), PS1StreamError() ? 1u : 0u);
    // Top: MDEC mode (PIO / DMA), PIO output order (8x8 blocks / rows), faults and switches this video,
    // then the registers at the first fault: MDEC1 status, DMA0 BCR + CHCR, DMA1 BCR + CHCR, DPCR (hex).
    char top[OVL_TOP_LINES][56];
    const volatile uint32_t *r = g_ps1MdecSnap;
    // First fault: kind 1 setup / 2 command / 3 pixels / 4 padding / 5-6 DMA, column, macroblock, word,
    // MDEC1 at that instant.
    const volatile uint32_t *f = g_ps1MdecFail;
    snprintf(top[0], sizeof(top[0]), "%s%s %s F%u S%u K%u %u/%u/%u %08x", g_ps1MdecMode == 1 ? "DMA" : g_ps1MdecMode == 2 ? "HYB" : "PIO",
             g_ps1MdecPadMode >= 0 ? "-PAD" : "",
             g_ps1MdecOrder == 1 ? "BLK" : g_ps1MdecOrder == 0 ? "ROW" : "-",
             (unsigned)(g_ps1MdecFaults - s_base.mdecFaults), (unsigned)(g_ps1MdecSwitches - s_base.mdecSwitches),
             (unsigned)f[0], (unsigned)f[1], (unsigned)f[2], (unsigned)f[3], (unsigned)f[5]);
    // Registers after the first fault: MDEC1, DMA0 BCR + CHCR, DMA1 CHCR, DPCR.
    snprintf(top[1], sizeof(top[1]), "M%08x I%08x %08x O%08x P%08x", (unsigned)r[0], (unsigned)r[2], (unsigned)r[3],
             (unsigned)r[6], (unsigned)r[7]);
    for (int i = 0; i < OVL_TOP_LINES; i++)
        for (char *c = top[i]; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c -= 32; // the font has capitals only
    PS1GpuImmediate(); // immediate uploads: the in-flight chain first
    for (int i = 0; i < OVL_LINES + OVL_TOP_LINES; i++) {
        drawLine(i < OVL_LINES ? line[i] : top[i - OVL_LINES]);
        int16_t y = i < OVL_LINES ? (int16_t)(OVL_Y + 8 * i) : (int16_t)(OVL_TOP_Y + 8 * (i - OVL_LINES));
        for (int16_t by = 0; by <= 256; by += 256) // both buffers: the chain leaves these rows alone
            g_ps1GPU->uploadToVRAM(s_ovl, psyqo::Rect{{0, (int16_t)(by + y)}, {OVL_W, 8}});
    }
}
} // namespace

bool PS1FmvRequest(const char *name, int soundtrack) {
    snprintf(s_path, sizeof(s_path), "Data/Videos/%s.str", name);
    s_audio   = soundtrack ? 1 : 0;
    s_state   = State::REQUESTED;
    s_picture = false;
    s_skipped = false;
    s_fade    = 0;
    return true;
}

void PS1FmvBeginFrame() {
    if (s_state == State::REQUESTED) {
        PS1XaStop(); // the video owns the CD
        swapArenas(FMV_ARENA_WORDS);
        static bool s_mdecReady = false;
        if (!s_mdecReady) // MDEC reset + its DMA channels + tables, once
            s_mdecReady = PS1VideoInit();
        PS1LoadingBegin(); // opening reads the sidecar + directory: no loading icon needed, but the CD windows
        bool ok = PS1VideoOpen(s_path, s_audio);
        PS1LoadingEnd(); // (resumes the chain with the small arenas)
        if (ok) {
            g_ps1FmvPlays = g_ps1FmvPlays + 1;
            s_state       = State::PLAYING;
            s_openVbl     = g_ps1GPU->getFrameCount();
            s_limitVbl    = (uint32_t)PS1VideoFrameCount() * FMV_VBLANKS_PER_FRAME + FMV_SLACK_VBLANKS;
            takeBase();
            // Hardware test without a debugger (docs/28 appendix 12): R1 held as the video opens = hybrid
            // MDEC (DMA0 in, CPU out), L1 = DMA in and out, neither = automatic (PIO, falling back on faults).
            using Pad = psyqo::AdvancedPad;
            bool r1 = g_ps1Pad && g_ps1Pad->isButtonPressed(Pad::Pad::Pad1a, Pad::Button::R1);
            bool l1 = g_ps1Pad && g_ps1Pad->isButtonPressed(Pad::Pad::Pad1a, Pad::Button::L1);
            g_ps1MdecPadMode = r1 ? 2 : l1 ? 1 : -1;
        } else {
            g_ps1FmvFails = g_ps1FmvFails + 1;
            s_state       = State::CLOSING;
        }
    }
    if (s_state == State::CLOSING) {
        closeOverlay();
        PS1VideoClose();
        swapArenas(0);
        PS1ChainResume();
        s_state = State::CLOSED;
    }
}

int PS1FmvUpdate(bool skip) {
    if (s_state == State::CLOSED) {
        s_state = State::IDLE;
        return 1;
    }
    if (s_state == State::IDLE)
        return 1;
    g_ps1FmvFrames = g_ps1FmvFrames + 1;
    if (s_state != State::PLAYING) {
        PS1RenderFmvFrame(0);
        return 0;
    }
    if (skip && !s_skipped) {
        s_skipped     = true;
        g_ps1FmvSkipped = g_ps1FmvSkipped + 1;
    }
    int r = 0;
    if (!s_skipped)
        // Into the buffer shown after this frame's flip: psyqo sends the chain after flip(), so during the
        // frame PS1ChainBufferY() is the buffer on screen (an upload there showed the new picture, then the
        // old one for a frame). The frame's chain copies it into its own buffer (PS1RenderFmvFrame mode 1).
        r = PS1VideoDecodeTo(PS1Hblanks(), 0, (256 - PS1ChainBufferY()) + FMV_Y);
    if (r > 0)
        s_picture = true;
    PS1RenderFmvFrame(r > 0 ? 1 : s_picture ? 2 : 0);
    if (s_skipped) { // fade the last picture out, then close
        s_fade += FMV_FADE_STEP;
        PS1RenderFmvFade(FMV_FADE_STEP);
        if (s_fade >= 0xFF)
            s_state = State::CLOSING;
    } else if (r < 0) {
        s_state = State::CLOSING;
    } else if (g_ps1GPU->getFrameCount() - s_openVbl > s_limitVbl) {
        g_ps1FmvTimeouts = g_ps1FmvTimeouts + 1;
        s_state          = State::CLOSING;
    }
    updateOverlay();
    return 0;
}

bool PS1FmvActive() { return s_state == State::PLAYING; }
