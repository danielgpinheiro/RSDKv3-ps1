// RSDKv3 (Sonic CD) PS1 renderer: replaces RSDKv3/Drawing.cpp (the PC software/GL renderer) on PS1.
//
// Everything a frame draws is DMA-chained (ps1/gpu_chain.hh, the Nexus port's chain): main.cpp opens the
// chain around Engine.RunFrame(), psyqo sends it after frame() returns. Sprites come from the stage's
// VRAM atlas (ps1/sprite_atlas, tools/atlas), tiles from the stage's tile pages (ps1/tileset,
// tools/tiles): nothing is decoded at runtime.
//
// Upstream Drawing.cpp functions also change game state, not just pixels (phase 2.3: the title card
// works from its draw sub). Every such side effect is ported here explicitly, marked "upstream":
// draw subs (DrawObjectList), draw order + water position (DrawStageGFX), the BG autoscroll and
// parallax line positions (DrawHLineScrollLayer / DrawVLineScrollLayer), the view globals
// (InitRenderDevice), DrawObjectAnimation's dispatch.
//
// Phase 3.2 scope (docs/28): chain + clear + palette, atlas sprites (plain / flipped / scaled / rotated
// / rotozoom), HScroll tile layers. Not yet (counted in g_ps1DrawTodo): ink effects and tint (3.3),
// per-line palette banks + water palette (3.4), VScroll / 3D layers / faces (3.5, 7), menu text (4).
#include "RetroEngine.hpp"

#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>

#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/sprites.hh>

#include "common/syscalls/syscalls.h"
#include "gpu_chain.hh"
#include "sprite_atlas.hh"
#include "tileset.hh"
#include "video.hh" // PS1Hblanks (profiling)

extern psyqo::GPU *g_ps1GPU;

// ---------------------------------------------------------------------------------------------------
// Engine globals owned by Drawing.cpp upstream

int SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE; // 320 (user decision; PC 424)
int SCREEN_CENTERX      = DEFAULT_SCREEN_XSIZE / 2;
int SCREEN_XSIZE_CONFIG = DEFAULT_SCREEN_XSIZE;

DrawListEntry drawListEntries[DRAWLAYER_COUNT];

int gfxDataPosition;
GFXSurface gfxSurface[SURFACE_COUNT];
byte graphicData[GFXDATA_SIZE];

ushort gfxVertexSize       = 0;
ushort gfxVertexSizeOpaque = 0;
ushort gfxIndexSize        = 0;
ushort gfxIndexSizeOpaque  = 0;
ushort vertexSize3D        = 0;
ushort indexSize3D         = 0;
ushort tileUVArray[TILEUV_SIZE];
bool render3DEnabled = false;
byte texBufferMode   = 0;

// ---------------------------------------------------------------------------------------------------
// Verification counters (GDB)

volatile uint32_t g_ps1DrawFrames     = 0; // frames drawn by the stage renderer (clear + scene)
volatile uint32_t g_ps1RepeatFrames   = 0; // frames that kept the last image (nothing or only objects drawn)
volatile uint32_t g_ps1DrawSprites    = 0; // atlas sprite pieces chained this frame
volatile uint32_t g_ps1DrawQuads      = 0; // textured quads chained this frame (flip / scale / rotation)
volatile uint32_t g_ps1LayerPrims     = 0; // tile-layer primitives chained this frame
volatile uint32_t g_ps1LayerHbl       = 0; // CPU time of this frame's tile layers (hblanks)
volatile uint32_t g_ps1ProfLayer[5]   = {}; // this frame: FG (uniform) layers, per-line passes, tile bands, strip bands (hblanks), HScroll calls
volatile uint32_t g_ps1AtlasDrawMiss  = 0; // sprite texels in no atlas cluster
volatile uint32_t g_ps1SheetUnbound   = 0; // draws from a sheet the stage atlas doesn't have
volatile uint32_t g_ps1DrawTodo       = 0; // draw calls of later steps (ink effects, faces, text...), skipped
volatile uint32_t g_ps1SurfaceGen     = 1; // bumped by Sprite.cpp when a sheet name is (un)registered
volatile uint8_t g_ps1DebugDrawMask   = 0; // bit 0: drop object-list primitives (layers-only frames)

// ---------------------------------------------------------------------------------------------------
// Palette: master CLUT (256 x 15-bit) at (0, 240), the atlas' 4-bit CLUTs (16 entries of master
// indices each) where the atlas put them. As on PC (FlipScreen converts the palette after drawing),
// the rows are chained at the start of the frame and filled again at FlipScreen with the palette as
// it stands when drawing is done.

#define PS1_CLUT_X 0
#define PS1_CLUT_Y 240
#define PS1_PAL_ENTRIES (256 + PS1_ATLAS_MAX_CLUTS * 16)

static uint16_t s_pal[PS1_PAL_ENTRIES];
struct PS1PalChunk {
    uint16_t *dst;
    uint16_t src, n;
};
static PS1PalChunk s_palChunks[128];
static int s_palChunkCount = 0;

// Every visible entry carries the STP bit (0x8000): textured primitives drawn semi-transparent then
// blend all their texels (psx-spx: only STP texels blend), opaque draws are unaffected; 0x0000 stays
// transparent (index 0).
static inline uint16_t PS1Conv(int r, int g, int b) { return (uint16_t)(0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }

// A full-screen alpha DrawRectangle that ends the frame (PC's screen fades) is applied to the CLUTs
// and the clear colour at FlipScreen: every pixel is CLUT-based, so this is PC's per-pixel blend
// c * (255 - a) + fade * a, exactly (user decision 2026-09-24). If anything is drawn after it, the
// chain hook draws it as a 4-level alpha rectangle instead (PS1FadeHook).
static bool s_fadePending = false;
static int s_fadeR, s_fadeG, s_fadeB, s_fadeA;
static psyqo::Prim::FastFill *s_clearPrim = nullptr;
volatile uint32_t g_ps1ClutFades = 0; // frames whose fade went into the CLUTs (exact)

static void PS1BuildPalette(int bank)
{
    const PaletteEntry *pal = fullPalette32[bank];
    s_pal[0]                = 0x0000;                          // index 0 = transparent (tiles and sprites)
    if (s_fadePending) {
        int a = s_fadeA, na = 255 - a;
        for (int i = 1; i < 256; ++i)
            s_pal[i] = PS1Conv((pal[i].r * na + s_fadeR * a) / 255, (pal[i].g * na + s_fadeG * a) / 255, (pal[i].b * na + s_fadeB * a) / 255);
    }
    else {
        for (int i = 1; i < 256; ++i) s_pal[i] = PS1Conv(pal[i].r, pal[i].g, pal[i].b);
    }
    for (int k = 0; k < g_ps1AtlasClutCount; ++k) {
        const uint8_t *map = g_ps1AtlasCluts[k].map;
        uint16_t *dst      = &s_pal[256 + k * 16];
        dst[0]             = 0x0000;
        for (int i = 1; i < 16; ++i) dst[i] = s_pal[map[i]];
    }
}

static void PS1ReserveRow(int src, int count, int16_t x, int16_t y)
{
    uint16_t *data[32];
    uint16_t len[32];
    int n = PS1ChainReserveRow(count, x, y, data, len, 32);
    for (int i = 0; i < n && s_palChunkCount < 128; ++i) {
        s_palChunks[s_palChunkCount++] = PS1PalChunk{ data[i], (uint16_t)src, len[i] };
        src += len[i];
    }
}

// Fills chunks [first, s_palChunkCount) with `bank`'s palette.
static void PS1FillPalette(int bank, int first = 0)
{
    PS1BuildPalette(bank);
    for (int i = first; i < s_palChunkCount; ++i) memcpy(s_palChunks[i].dst, &s_pal[s_palChunks[i].src], s_palChunks[i].n * sizeof(uint16_t));
}

// Master + atlas 4-bit CLUT rows, chained here (filled by PS1FillPalette).
static void PS1ReservePaletteRows()
{
    PS1ReserveRow(0, 256, PS1_CLUT_X, PS1_CLUT_Y);
    for (int k = 0; k < g_ps1AtlasClutCount;) { // runs of adjacent CLUT slots in one VRAM row
        int run = 1;
        while (k + run < g_ps1AtlasClutCount && g_ps1AtlasCluts[k + run].y == g_ps1AtlasCluts[k].y
               && g_ps1AtlasCluts[k + run].x == g_ps1AtlasCluts[k].x + 16 * run)
            ++run;
        PS1ReserveRow(256 + k * 16, run * 16, (int16_t)g_ps1AtlasCluts[k].x, (int16_t)g_ps1AtlasCluts[k].y);
        k += run;
    }
}

// ---------------------------------------------------------------------------------------------------
// Frame: main.cpp calls PS1RenderBeginFrame / PS1RenderEndFrame around Engine.RunFrame(). The first
// draw of a frame starts it: DrawStageGFX clears; anything else (the paused stage mode draws only its
// object lists over the last image, like PC's persistent framebuffer) repeats the last frame first.

static bool s_chainOpen  = false;
static uint32_t *s_sceneMark = nullptr; // chain position after the clear and the frame-start CLUT rows
static bool s_frameOpen  = false;
static bool s_frameFresh = false; // this frame started with a clear

static void PS1ClearClusterCache();

static void PS1FrameStart(bool clear)
{
    if (s_frameOpen || !s_chainOpen)
        return;
    s_frameOpen      = true;
    s_frameFresh     = clear;
    g_ps1DrawSprites = 0;
    g_ps1DrawQuads   = 0;
    g_ps1LayerPrims  = 0;
    g_ps1LayerHbl    = 0;
    for (int i = 0; i < 5; ++i) g_ps1ProfLayer[i] = 0;
    s_fadePending = false;
    s_clearPrim   = nullptr;
    if (clear) {
        s_clearPrim = PS1ChainClear(0, 0, 0);
        g_ps1DrawFrames = g_ps1DrawFrames + 1;
    }
    else {
        PS1ChainRepeatLastFrame();
        g_ps1RepeatFrames = g_ps1RepeatFrames + 1;
    }
    s_palChunkCount = 0;
    PS1ReservePaletteRows();
    PS1FillPalette(gfxLineBuffer[0]);
    PS1ChainFlushCache(); // the GPU caches CLUTs
    PS1ChainInvalidateTPage();
    s_sceneMark = PS1ChainMark();
}

void PS1RenderBeginFrame(psyqo::GPU &gpu)
{
    PS1ChainBegin(gpu);
    s_chainOpen = true;
    s_frameOpen = false;
}

void PS1RenderEndFrame()
{
    if (!s_frameOpen)
        PS1FrameStart(false); // nothing drawn: keep the image in both buffers
    PS1ChainEnd();
    s_chainOpen = false;
}

// ---------------------------------------------------------------------------------------------------
// Sheets -> atlas sheets. gfxSurface[] holds the names the scripts registered (Sprite.cpp); the atlas
// sheet of each is looked up again whenever a name or the atlas changes.

static int16_t s_surfAtlas[SURFACE_COUNT];
static uint32_t s_boundGen = 0, s_boundAtlas = 0xFFFFFFFF;

static int PS1SurfaceSheet(int sheetID)
{
    if (s_boundGen != g_ps1SurfaceGen || s_boundAtlas != g_ps1AtlasLoads) {
        s_boundGen   = g_ps1SurfaceGen;
        s_boundAtlas = g_ps1AtlasLoads;
        for (int s = 0; s < SURFACE_COUNT; ++s)
            s_surfAtlas[s] = gfxSurface[s].fileName[0] && g_ps1AtlasSheetCount ? (int16_t)PS1AtlasFindSheet(gfxSurface[s].fileName) : (int16_t)-1;
        PS1ClearClusterCache();
    }
    if ((uint)sheetID >= SURFACE_COUNT)
        return -1;
    return s_surfAtlas[sheetID];
}

struct PS1ClusterCacheEntry {
    int16_t sheet, x, y, index;
};
static PS1ClusterCacheEntry s_clusterCache[64];
static void PS1ClearClusterCache() { memset(s_clusterCache, 0xFF, sizeof(s_clusterCache)); }

static const PS1AtlasCluster *PS1FindCluster(int sheet, int x, int y)
{
    PS1ClusterCacheEntry &e = s_clusterCache[((unsigned)x * 7 + (unsigned)y * 13 + (unsigned)sheet * 31) & 63];
    if (e.sheet == sheet && e.x == x && e.y == y)
        return &g_ps1AtlasClusters[e.index];
    const PS1AtlasSheet &sh = g_ps1AtlasSheets[sheet];
    for (int i = sh.firstCluster; i < sh.firstCluster + sh.clusterCount; ++i) {
        const PS1AtlasCluster &c = g_ps1AtlasClusters[i];
        if (x >= c.srcX && x < c.srcX + c.w && y >= c.srcY && y < c.srcY + c.h) {
            e = PS1ClusterCacheEntry{ (int16_t)sheet, (int16_t)x, (int16_t)y, (int16_t)i };
            return &c;
        }
    }
    g_ps1AtlasDrawMiss = g_ps1AtlasDrawMiss + 1;
    return nullptr;
}

static psyqo::PrimPieces::TPageAttr PS1ClusterTPage(const PS1AtlasCluster *c)
{
    psyqo::PrimPieces::TPageAttr attr;
    attr.set(c->depth == 4 ? psyqo::Prim::TPageAttr::ColorMode::Tex4Bits : psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    attr.setPageX(c->tpageX);
    attr.setPageY(c->tpageY);
    return attr;
}

static psyqo::PrimPieces::ClutIndex PS1ClusterClut(const PS1AtlasCluster *c)
{
    if (c->clut == 0xFFFF)
        return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    const PS1AtlasClut &k = g_ps1AtlasCluts[c->clut];
    return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = (int16_t)k.x, .y = (int16_t)k.y } });
}

// A piece of a frame: texel rect [x, x + w) x [y, y + h) of one cluster, at offset (ox, oy) in the frame.
struct PS1Piece {
    const PS1AtlasCluster *c;
    int16_t ox, oy, w, h, u, v;
};

// Splits frame (sprX, sprY, width, height) of surface sheetID into cluster pieces and calls fn on each.
// Frames past the bottom of their sheet continue into the next loaded sheet of the same width, as on
// PC where every sheet sits back to back in one buffer (Sonic CD's menu headings: MenuGfx1 -> 2).
template <typename Fn> static void PS1ForEachPiece(int sheetID, int width, int height, int sprX, int sprY, Fn fn)
{
    int sheet = PS1SurfaceSheet(sheetID);
    if (sheet < 0) {
        g_ps1SheetUnbound = g_ps1SheetUnbound + 1;
        return;
    }
    int oy = 0;
    while (height > 0) {
        int sh = g_ps1AtlasSheets[sheet].height;
        while (sprY >= sh) { // overflow into the next loaded sheet of the same width
            int w = g_ps1AtlasSheets[sheet].width, next = -1;
            for (int s = sheetID + 1; s < SURFACE_COUNT && next < 0; ++s) {
                int a = PS1SurfaceSheet(s);
                if (a >= 0 && g_ps1AtlasSheets[a].width == w) {
                    next    = a;
                    sheetID = s;
                }
            }
            if (next < 0)
                return;
            sprY -= sh;
            sheet = next;
            sh    = g_ps1AtlasSheets[sheet].height;
        }
        int part = sprY + height > sh ? sh - sprY : height;
        struct Rect16 {
            int16_t x, y, w, h;
        } stack[16];
        int sp      = 0;
        stack[sp++] = Rect16{ (int16_t)sprX, (int16_t)sprY, (int16_t)width, (int16_t)part };
        while (sp) {
            Rect16 r                 = stack[--sp];
            const PS1AtlasCluster *c = PS1FindCluster(sheet, r.x, r.y);
            if (!c)
                continue;
            int iw = c->srcX + c->w - r.x, ih = c->srcY + c->h - r.y;
            if (iw > r.w)
                iw = r.w;
            if (ih > r.h)
                ih = r.h;
            fn(PS1Piece{ c, (int16_t)(r.x - sprX), (int16_t)(oy + r.y - sprY), (int16_t)iw, (int16_t)ih, (int16_t)(c->u + r.x - c->srcX),
                         (int16_t)(c->v + r.y - c->srcY) });
            if (r.w > iw && sp < 16)
                stack[sp++] = Rect16{ (int16_t)(r.x + iw), r.y, (int16_t)(r.w - iw), (int16_t)ih };
            if (r.h > ih && sp < 16)
                stack[sp++] = Rect16{ r.x, (int16_t)(r.y + ih), r.w, (int16_t)(r.h - ih) };
        }
        oy += part;
        sprY += part;
        height -= part;
    }
}

static inline bool PS1ObjectsHidden() { return (g_ps1DebugDrawMask & 1) != 0; }

// Ink state for the next pieces: GPU semi-transparency mode (psx-spx GP0 E1h bits 5-6) and the texel
// modulation (0x80 = unchanged). Set by the ink draw calls around their piece loops.
enum PS1Ink { PS1_INK_OPAQUE, PS1_INK_HALF, PS1_INK_ADD, PS1_INK_SUB };
static uint8_t s_ink    = PS1_INK_OPAQUE;
static uint8_t s_inkMod = 0x80;
volatile uint32_t g_ps1InkPieces = 0; // semi-transparent sprite pieces chained

static inline void PS1ApplyInk(psyqo::PrimPieces::TPageAttr &attr)
{
    static const psyqo::Prim::TPageAttr::SemiTrans modes[4] = { psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront,
                                                                psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront,
                                                                psyqo::Prim::TPageAttr::SemiTrans::FullBackAndFullFront,
                                                                psyqo::Prim::TPageAttr::SemiTrans::FullBackSubFullFront };
    attr.set(modes[s_ink]);
}

// PC alpha 0-255 -> the PS1 levels (user decision 2026-09-24): 0 skip, 1 = 50 %, 2 = 75 % (two 50 %
// draws: B/4 + 3F/4), 3 = opaque.
static inline int PS1AlphaLevel(int a) { return a < 64 ? 0 : a < 160 ? 1 : a < 224 ? 2 : 3; }

static void PS1ChainPieceSprite(const PS1Piece &p, int x, int y)
{
    psyqo::PrimPieces::TPageAttr attr = PS1ClusterTPage(p.c);
    if (s_ink != PS1_INK_OPAQUE)
        PS1ApplyInk(attr);
    PS1ChainTPage(attr);
    psyqo::Prim::Sprite s(psyqo::Color{ { .r = s_inkMod, .g = s_inkMod, .b = s_inkMod } });
    if (s_ink != PS1_INK_OPAQUE) {
        s.setSemiTrans();
        g_ps1InkPieces = g_ps1InkPieces + 1;
    }
    s.position = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)y } };
    s.size     = psyqo::Vertex{ { .w = p.w, .h = p.h } };
    s.texInfo  = psyqo::PrimPieces::TexInfo{ (uint8_t)p.u, (uint8_t)p.v, PS1ClusterClut(p.c) };
    PS1ChainSprite(s);
    g_ps1DrawSprites = g_ps1DrawSprites + 1;
}

// Textured quad: corners A (u0, v0), B (u1, v0), C (u0, v1), D (u1, v1). UVs are texel edges; they
// may run backwards (mirroring). A UV byte can't hold -1 or 256: those edges are pulled in by one texel
// (rare: pieces at a page edge), which drops/duplicates at most one texel column or row.
static void PS1ChainPieceQuad(const PS1Piece &p, const int16_t *xs, const int16_t *ys, int u0, int u1, int v0, int v1)
{
    auto clampUV = [](int t) { return (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t); };
    psyqo::Prim::TexturedQuad q(psyqo::Color{ { .r = s_inkMod, .g = s_inkMod, .b = s_inkMod } });
    q.pointA    = psyqo::Vertex{ { .x = xs[0], .y = ys[0] } };
    q.pointB    = psyqo::Vertex{ { .x = xs[1], .y = ys[1] } };
    q.pointC    = psyqo::Vertex{ { .x = xs[2], .y = ys[2] } };
    q.pointD    = psyqo::Vertex{ { .x = xs[3], .y = ys[3] } };
    q.uvA       = psyqo::PrimPieces::UVCoords{ clampUV(u0), clampUV(v0) };
    q.uvB       = psyqo::PrimPieces::UVCoords{ clampUV(u1), clampUV(v0) };
    q.uvC       = psyqo::PrimPieces::UVCoordsPadded{ clampUV(u0), clampUV(v1), 0 };
    q.uvD       = psyqo::PrimPieces::UVCoordsPadded{ clampUV(u1), clampUV(v1), 0 };
    q.clutIndex = PS1ClusterClut(p.c);
    q.tpage     = PS1ClusterTPage(p.c);
    if (s_ink != PS1_INK_OPAQUE) {
        PS1ApplyInk(q.tpage);
        q.setSemiTrans();
        g_ps1InkPieces = g_ps1InkPieces + 1;
    }
    PS1ChainQuad(q);
    g_ps1DrawQuads = g_ps1DrawQuads + 1;
}

// Axis-aligned piece at screen rect (x, y, w, h) with optional mirroring: 1:1 mirrors sample texel
// u + w - 1 - k at column k (UV edges u + w - 1 .. u - 1), scaled ones the full [u, u + w) range.
static void PS1ChainPieceRect(const PS1Piece &p, int x, int y, int w, int h, bool flipX, bool flipY)
{
    if (!flipX && !flipY && w == p.w && h == p.h) {
        PS1ChainPieceSprite(p, x, y);
        return;
    }
    bool unit = w == p.w && h == p.h;
    int u0 = p.u, u1 = p.u + p.w, v0 = p.v, v1 = p.v + p.h;
    if (flipX) {
        u0 = unit ? p.u + p.w - 1 : p.u + p.w;
        u1 = unit ? p.u - 1 : p.u;
    }
    if (flipY) {
        v0 = unit ? p.v + p.h - 1 : p.v + p.h;
        v1 = unit ? p.v - 1 : p.v;
    }
    int16_t xs[4] = { (int16_t)x, (int16_t)(x + w), (int16_t)x, (int16_t)(x + w) };
    int16_t ys[4] = { (int16_t)y, (int16_t)y, (int16_t)(y + h), (int16_t)(y + h) };
    PS1ChainPieceQuad(p, xs, ys, u0, u1, v0, v1);
}

// ---------------------------------------------------------------------------------------------------
// Upstream API

int InitRenderDevice()
{
    // upstream InitRenderDevice / SetScreenSize: the logic's view (scroll edges, object activity border)
    SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE;
    SCREEN_CENTERX      = SCREEN_XSIZE / 2;
    SCREEN_SCROLL_LEFT  = SCREEN_CENTERX - 8;
    SCREEN_SCROLL_RIGHT = SCREEN_CENTERX + 8;
    OBJECT_BORDER_X2    = SCREEN_XSIZE + 0x80;
    return 1;
}

// PC converts the palette here, after drawing: the CLUT rows chained at the frame start get the
// palette as it is now (a SetFade / palette change from a draw sub shows in the same frame).
volatile uint32_t g_ps1BankRegions = 0; // palette-bank regions (runs of equal gfxLineBuffer) in the last frame
volatile uint32_t g_ps1BankMask    = 0; // banks used by the last frame (bit per bank)
volatile uint32_t g_ps1BankReplays = 0; // regions drawn again with their own bank (total)

// Per-line palette banks (upstream SetActivePalette / gfxLineBuffer; user decision 2026-09-24: replay
// per region). The frame was drawn with line 0's bank; every further run of lines with another bank
// gets its bank's CLUT rows (same VRAM rows), a drawing area of those lines, a fresh base (clear
// colour, or the last frame in a repeated frame) and a replay of the frame's primitives.
static void PS1DrawBankRegions(psyqo::Color clear)
{
    uint32_t *sceneEnd = PS1ChainMark();
    int regions = 1, y0 = 0;
    uint32_t mask = 1u << gfxLineBuffer[0];
    for (int y = 1; y <= SCREEN_YSIZE; ++y) {
        if (y < SCREEN_YSIZE && gfxLineBuffer[y] == gfxLineBuffer[y0])
            continue;
        if (y0 > 0) { // regions after the first: the frame again, with this region's bank
            int bank  = gfxLineBuffer[y0];
            int first = s_palChunkCount;
            PS1ReservePaletteRows();
            PS1FillPalette(bank, first);
            PS1ChainFlushCache();
            PS1ChainDrawArea(0, y0, 320, y);
            if (s_frameFresh)
                PS1ChainFillLines(y0, y, clear.r, clear.g, clear.b);
            else
                PS1ChainCopyLastFrameLines(y0, y);
            PS1ChainReplayLines(s_sceneMark, sceneEnd, y0, y);
            g_ps1BankReplays = g_ps1BankReplays + 1;
            mask |= 1u << bank;
        }
        if (y < SCREEN_YSIZE) {
            ++regions;
            y0 = y;
        }
    }
    if (regions > 1) {
        PS1ChainDrawAreaReset();
        PS1ChainInvalidateTPage();
    }
    g_ps1BankRegions = regions;
    g_ps1BankMask    = mask;
}

// PC converts the palette here, after drawing: the CLUT rows chained at the frame start get the
// palette as it is now (a SetFade / palette change from a draw sub shows in the same frame).
void FlipScreen()
{
    if (!s_frameOpen)
        return;
    PS1ChainSetHook(nullptr); // nothing else comes: a pending fade stays a CLUT fade (the hook would draw it)
    psyqo::Color clear = { { .r = 0, .g = 0, .b = 0 } };
    if (s_fadePending)
        clear = psyqo::Color{ { .r = (uint8_t)(s_fadeR * s_fadeA / 255), .g = (uint8_t)(s_fadeG * s_fadeA / 255),
                                .b = (uint8_t)(s_fadeB * s_fadeA / 255) } };
    int frameChunks = s_palChunkCount;
    PS1DrawBankRegions(clear);
    int end         = s_palChunkCount; // the regions filled their own chunks: refill only the frame-start rows
    s_palChunkCount = frameChunks;
    PS1FillPalette(gfxLineBuffer[0]);
    s_palChunkCount = end;
    if (s_fadePending) {
        g_ps1ClutFades = g_ps1ClutFades + 1;
        if (s_clearPrim) // cleared (undrawn) pixels are black: under the fade, fade colour * a
            s_clearPrim->setColor(clear);
    }
}

void GenerateBlendLookupTable() {}
void UpdateHardwareTextures() {}

void ClearScreen(byte index)
{
    PS1FrameStart(true);
    const PaletteEntry &c = fullPalette32[gfxLineBuffer[0]][index];
    psyqo::Prim::Rectangle r(psyqo::Color{ { .r = c.r, .g = c.g, .b = c.b } });
    r.position = psyqo::Vertex{ { .x = 0, .y = 0 } };
    r.size     = psyqo::Vertex{ { .w = (int16_t)SCREEN_XSIZE, .h = SCREEN_YSIZE } };
    PS1ChainRect(r);
}

// upstream: draw subs are game logic too; only their Draw* calls produce primitives.
void DrawObjectList(int layer)
{
    PS1FrameStart(false);
    g_ps1ChainSkipPrims = PS1ObjectsHidden();
    int size            = drawListEntries[layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectLoop = drawListEntries[layer].entityRefs[i];
        int type   = objectEntityList[objectLoop].type;
        if (type) {
            activePlayer = 0;
            if (scriptCode[objectScriptList[type].subDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].subDraw.scriptCodePtr, objectScriptList[type].subDraw.jumpTablePtr, SUB_DRAW);
        }
    }
    g_ps1ChainSkipPrims = false;
}

void DrawHLineScrollLayer(int layerID);
void DrawVLineScrollLayer(int layerID);

static void PS1DrawLayer(int layerID)
{
    if (activeTileLayers[layerID] >= LAYER_COUNT)
        return;
    switch (stageLayouts[activeTileLayers[layerID]].type) {
        case LAYER_HSCROLL: DrawHLineScrollLayer(layerID); break;
        case LAYER_VSCROLL: DrawVLineScrollLayer(layerID); break;
        case LAYER_3DFLOOR:
        case LAYER_3DSKY: g_ps1DrawTodo = g_ps1DrawTodo + 1; break; // phase 7 (GTE floor)
        default: break;
    }
}

// upstream DrawStageGFX (SW path): water position, then layers and draw lists in upstream order
// (list 7 before 6), then the full-screen fade rectangle.
void DrawStageGFX()
{
    PS1FrameStart(true);
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;

    DrawObjectList(0);
    PS1DrawLayer(0);
    DrawObjectList(1);
    PS1DrawLayer(1);
    DrawObjectList(2);
    PS1DrawLayer(2);
    DrawObjectList(3);
    DrawObjectList(4);
    PS1DrawLayer(3);
    DrawObjectList(5);
    if (forceUseScripts || Engine.usingOrigins)
        DrawObjectList(7);
    DrawObjectList(6);
    if (fadeMode > 0)
        DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
}

void DrawDebugOverlays() {}

// ---------------------------------------------------------------------------------------------------
// Tile layers. Each chunk tile's visual as one halfword (built at stage load, updated when a script
// changes a tile): bits 0-9 tile index, 10-11 flip, 12 visual plane, 13 "has pixels" (the tile page
// scan in ps1/tileset). Plus, per chunk, which of its 8 tile rows have visible tiles on each plane.

#define PS1_TV_INDEX 0x03FF
#define PS1_TV_DIR_SHIFT 10
#define PS1_TV_PLANE 0x1000
#define PS1_TV_SOLID 0x2000
static uint16_t s_tileVis[CHUNKTILE_COUNT];
static uint16_t s_chunkRowPlanes[CHUNKTILE_COUNT / 64];

static inline uint16_t PS1TileVisOf(int i)
{
    int t = tiles128x128.tileIndex[i] & PS1_TV_INDEX;
    return (uint16_t)(t | ((tiles128x128.direction[i] & 3) << PS1_TV_DIR_SHIFT) | (tiles128x128.visualPlane[i] ? PS1_TV_PLANE : 0)
                      | ((g_ps1TileSolid[t >> 3] >> (t & 7)) & 1 ? PS1_TV_SOLID : 0));
}

static void PS1UpdateChunkRows(int chunk)
{
    uint16_t m = 0;
    for (int t = 0; t < 64; ++t) {
        uint16_t tv = s_tileVis[(chunk << 6) + t];
        if (tv & PS1_TV_SOLID)
            m |= 1 << ((t >> 3) * 2 + ((tv & PS1_TV_PLANE) ? 1 : 0));
    }
    s_chunkRowPlanes[chunk] = m;
}

void PS1BuildTileVisuals()
{
    for (int i = 0; i < CHUNKTILE_COUNT; ++i) s_tileVis[i] = PS1TileVisOf(i);
    for (int c = 0; c < CHUNKTILE_COUNT / 64; ++c) PS1UpdateChunkRows(c);
}

void PS1TileChanged(int chunkTile)
{
    if ((uint)chunkTile >= CHUNKTILE_COUNT)
        return;
    s_tileVis[chunkTile] = PS1TileVisOf(chunkTile);
    PS1UpdateChunkRows(chunkTile >> 6);
}

static psyqo::PrimPieces::TPageAttr s_tileTPage[4];
static uint32_t s_tileClutWord = 0;

static void PS1TileSetup()
{
    if (s_tileClutWord)
        return;
    psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    uint16_t c;
    memcpy(&c, &clut, 2);
    s_tileClutWord = (uint32_t)c << 16;
    for (int p = 0; p < 4; ++p) {
        s_tileTPage[p].set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
        s_tileTPage[p].setPageX((uint8_t)(PS1_TILE_PAGE_X + 2 * (p & 1)));
        s_tileTPage[p].setPageY((uint8_t)(PS1_TILE_PAGE_Y + (p >> 1)));
    }
}

// Textured quad over tile texels, UV edges uL..uR (exclusive, may run backwards) and vT..vB.
static void PS1ChainTileQuad(int x, int y, int w, int h, int pageX, int pageY, int uL, int uR, int vT, int vB)
{
    psyqo::Prim::TexturedQuad quad(psyqo::Color{ { .r = 0x80, .g = 0x80, .b = 0x80 } });
    quad.pointA    = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)y } };
    quad.uvA       = psyqo::PrimPieces::UVCoords{ (uint8_t)uL, (uint8_t)vT };
    quad.clutIndex = psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    quad.pointB    = psyqo::Vertex{ { .x = (int16_t)(x + w), .y = (int16_t)y } };
    quad.uvB       = psyqo::PrimPieces::UVCoords{ (uint8_t)uR, (uint8_t)vT };
    quad.tpage.setPageX((uint8_t)pageX);
    quad.tpage.setPageY((uint8_t)pageY);
    quad.tpage.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    quad.pointC = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)(y + h) } };
    quad.uvC    = psyqo::PrimPieces::UVCoordsPadded{ (uint8_t)uL, (uint8_t)vB, 0 };
    quad.pointD = psyqo::Vertex{ { .x = (int16_t)(x + w), .y = (int16_t)(y + h) } };
    quad.uvD    = psyqo::PrimPieces::UVCoordsPadded{ (uint8_t)uR, (uint8_t)vB, 0 };
    PS1ChainQuad(quad);
}

// Rows [ty0, ty0 + h) of a tile (rows before flipping) at screen (sx, sy): unflipped as a sprite,
// flipped as a quad with backwards UVs (the Nexus port's PS1DrawTilePiece; old GPUs have no
// rectangle flip bits, psx-spx).
static void PS1DrawTilePiece(int sx, int sy, int tileIndex, int ty0, int h, int dir)
{
    int page = tileIndex >> 8, pt = tileIndex & 0xFF;
    int u = (pt & 0xF) << 4, v = (pt >> 4) << 4;
    int pageX = PS1_TILE_PAGE_X + 2 * (page & 1), pageY = PS1_TILE_PAGE_Y + (page >> 1);
    if (dir == FLIP_NONE) {
        PS1ChainTPage(s_tileTPage[page]);
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
        psyqo::Prim::Sprite s(psyqo::Color{ { .r = 0x80, .g = 0x80, .b = 0x80 } });
        s.position = psyqo::Vertex{ { .x = (int16_t)sx, .y = (int16_t)sy } };
        s.texInfo  = psyqo::PrimPieces::TexInfo{ (uint8_t)u, (uint8_t)(v + ty0), clut };
        s.size     = psyqo::Vertex{ { .x = TILE_SIZE, .y = (int16_t)h } };
        PS1ChainSprite(s);
        return;
    }
    int uL = (dir & FLIP_X) ? u + 15 : u;
    int uR = (dir & FLIP_X) ? u - 1 : u + 16;
    int dv = (dir & FLIP_Y) ? -1 : 1;
    int vT = (dir & FLIP_Y) ? v + 15 - ty0 : v + ty0;
    if (uR < 0) { // u -1 / 256 don't fit a UV byte: address the tile from the neighbouring 8-bit page
        pageX -= 1, uL += 128, uR += 128;
    }
    else if (uR > 255) {
        pageX += 1, uL -= 128, uR -= 128;
    }
    int vB = vT + h * dv;
    if (vB < 0 || vB > 255) { // pageY can't shift: the last row gets its own constant-v strip
        int vLast = vT + (h - 1) * dv;
        if (h > 1)
            PS1ChainTileQuad(sx, sy, TILE_SIZE, h - 1, pageX, pageY, uL, uR, vT, vLast);
        PS1ChainTileQuad(sx, sy + h - 1, TILE_SIZE, 1, pageX, pageY, uL, uR, vLast, vLast);
        return;
    }
    PS1ChainTileQuad(sx, sy, TILE_SIZE, h, pageX, pageY, uL, uR, vT, vB);
}

// Screen lines [sy, sy + h) showing layer lines [ly, ly + h) (inside one tile row) scrolled to chunkX.
// Bands touching a screen edge are extended to whole tile rows (the GPU clips).
static void PS1DrawTileBand(const TileLayer *layer, byte above, int sy, int h, int ly, int chunkX, int fullW)
{
    int ty0 = ly & 0xF;
    if (sy == 0) {
        sy -= ty0, h += ty0, ty0 = 0;
    }
    if (sy + h == SCREEN_YSIZE)
        h = TILE_SIZE - ty0;
    const ushort *chunkRow = &layer->tiles[(ly >> 7) * TILELAYER_CHUNK_W];
    int tileRow            = ((ly & 0x7F) >> 4) << 3;
    uint16_t want          = PS1_TV_SOLID | (above ? PS1_TV_PLANE : 0);
    uint32_t yWord         = (uint32_t)(uint16_t)sy << 16;
    bool whole             = h == TILE_SIZE;
    int curPage            = -1;
    int prims              = 0;
    int px                 = chunkX & ~0xF;
    uint16_t rowBit        = 1 << ((tileRow >> 3) * 2 + (above ? 1 : 0));
    for (int sx = -(chunkX & 0xF); sx < SCREEN_XSIZE;) {
        int chunk = chunkRow[px >> 7];
        if (!(s_chunkRowPlanes[chunk] & rowBit)) { // nothing on this plane in this chunk row
            int skip = CHUNK_SIZE - (px & 0x7F);
            sx += skip;
            px += skip;
            if (px >= fullW)
                px -= fullW;
            continue;
        }
        uint16_t tv = s_tileVis[(chunk << 6) + ((px & 0x7F) >> 4) + tileRow];
        sx += TILE_SIZE;
        px += TILE_SIZE;
        if (px >= fullW)
            px = 0;
        if ((tv & (PS1_TV_SOLID | PS1_TV_PLANE)) != want)
            continue;
        ++prims;
        int dir       = (tv >> PS1_TV_DIR_SHIFT) & 3;
        int tileIndex = tv & PS1_TV_INDEX;
        int dx        = sx - TILE_SIZE;
        if (whole && dir == FLIP_NONE) {
            int page = tileIndex >> 8, pt = tileIndex & 0xFF;
            if (page != curPage) {
                PS1ChainTPage(s_tileTPage[page]);
                curPage = page;
            }
            PS1ChainSprite16Raw(yWord | (uint16_t)dx, (uint32_t)(((pt & 0xF) << 4) | (((pt >> 4) << 4) << 8)) | s_tileClutWord);
        }
        else {
            PS1DrawTilePiece(dx, sy, tileIndex, ty0, h, dir);
            curPage = -1;
        }
    }
    g_ps1LayerPrims = g_ps1LayerPrims + prims;
}

// BG line strips (tools/bgstrips/build_bgstrips.py -> Data/Stages/<Stage>/BGStrips[_t].bin, format
// there): BG layer lines pre-rendered at build time as 8-bit rows of one horizontal period, in VRAM
// the stage's atlas leaves free. A band of lines in a strip region costs 1-3 sprites instead of ~21
// tiles (per-line parallax would otherwise make one band per line). From the Nexus port.
#define PS1_STRIP_MAX    64
#define PS1_STRIP_PIECES 9 // tools/bgstrips MAX_PIECES
struct PS1StripRegion {
    uint8_t layer, plane, pieces, pad;
    uint16_t line0, lines, period;
    uint16_t x[PS1_STRIP_PIECES], y[PS1_STRIP_PIECES], w[PS1_STRIP_PIECES]; // piece VRAM origin (hw) + width (texels)
};
static PS1StripRegion s_strips[PS1_STRIP_MAX];
static int s_stripCount = 0;
static uint8_t s_stripRow[LAYER_COUNT][2][TILELAYER_SCROLL_COUNT / TILE_SIZE]; // region + 1 per tile row
volatile uint32_t g_ps1StripRegions = 0; // regions loaded for this stage
volatile uint32_t g_ps1StripBytes   = 0; // strip texel bytes uploaded by the last load

// Loads the stage's strips (player 1 = Tails: BGStrips_t.bin, placed around the Tails atlas) into VRAM.
// Immediate uploads: call during the stage load, after PS1AtlasLoad.
void PS1LoadBGStrips(const char *folder, int player)
{
    s_stripCount = 0;
    g_ps1StripRegions = 0;
    memset(s_stripRow, 0, sizeof(s_stripRow));
    char path[96];
    snprintf(path, sizeof(path), "Data/Stages/%s/BGStrips%s.bin", folder, player == 1 ? "_t" : "");
    FILE *f = fopen(path, "rb");
    if (!f)
        return; // no HScroll BG strips for this stage: tiles only
    uint8_t head[8];
    int count = 0;
    bool ok   = fread(head, 1, 8, f) == 8 && !memcmp(head, "BGS2", 4);
    if (ok) {
        count = head[4] | (head[5] << 8);
        ok    = count <= PS1_STRIP_MAX;
    }
    for (int i = 0; ok && i < count; ++i) {
        uint8_t e[10 + 6 * PS1_STRIP_PIECES];
        ok = fread(e, 1, sizeof(e), f) == sizeof(e);
        PS1StripRegion &r = s_strips[i];
        r.layer  = e[0];
        r.plane  = e[1] & 1;
        r.line0  = e[2] | (e[3] << 8);
        r.lines  = e[4] | (e[5] << 8);
        r.period = e[6] | (e[7] << 8);
        r.pieces = e[8];
        for (int k = 0; k < PS1_STRIP_PIECES; ++k) {
            r.x[k] = e[10 + 6 * k] | (e[11 + 6 * k] << 8);
            r.y[k] = e[12 + 6 * k] | (e[13 + 6 * k] << 8);
            r.w[k] = e[14 + 6 * k] | (e[15 + 6 * k] << 8);
        }
        ok = ok && r.pieces <= PS1_STRIP_PIECES && r.layer < LAYER_COUNT && r.line0 + r.lines <= TILELAYER_SCROLL_COUNT;
    }
    const int stagingBytes = 16 * 1024;
    uint8_t *staging       = ok ? (uint8_t *)psyqo_malloc(stagingBytes) : nullptr;
    uint32_t bytes         = 0;
    if (staging) {
        PS1GpuImmediate();
        for (int i = 0; ok && i < count; ++i) {
            PS1StripRegion &r = s_strips[i];
            for (int k = 0; ok && k < r.pieces; ++k) {
                int rowsPerChunk = stagingBytes / r.w[k];
                for (int row = 0; ok && row < r.lines; row += rowsPerChunk) {
                    int n = r.lines - row < rowsPerChunk ? r.lines - row : rowsPerChunk;
                    ok    = fread(staging, 1, n * r.w[k], f) == (size_t)(n * r.w[k]);
                    if (ok)
                        g_ps1GPU->uploadToVRAM((const uint16_t *)staging,
                                               psyqo::Rect{ { (int16_t)r.x[k], (int16_t)(r.y[k] + row) }, { (int16_t)(r.w[k] / 2), (int16_t)n } });
                    bytes += n * r.w[k];
                }
            }
            for (int row = r.line0 >> 4; row < (r.line0 + r.lines) >> 4; ++row) s_stripRow[r.layer][r.plane][row] = (uint8_t)(i + 1);
        }
        psyqo_free(staging);
    }
    else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        ramsyscall_printf("[strips] bad or unreadable %s: tiles only\n", path);
        memset(s_stripRow, 0, sizeof(s_stripRow));
        return;
    }
    s_stripCount      = count;
    g_ps1StripRegions = count;
    g_ps1StripBytes   = bytes;
}

// A band of lines [ly, ly + h) inside a strip region scrolled to chunkX: walk the period from
// chunkX % period across the screen, one sprite per piece run (<= 256 texels, one page). Only runs of
// piece `piece` are drawn, so all bands of a region are drawn piece by piece (one texpage each).
static int PS1DrawStripBand(const PS1StripRegion &r, int sy, int h, int ly, int chunkX, int piece)
{
    static uint16_t s_clut = 0;
    if (!s_clut) {
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
        memcpy(&s_clut, &clut, 2);
    }
    int row   = ly - r.line0;
    int t     = chunkX % r.period;
    int prims = 0;
    for (int x = 0; x < SCREEN_XSIZE;) {
        int k = 0, s0 = 0;
        while (t >= s0 + r.w[k]) s0 += r.w[k++];
        int u   = t - s0;
        int run = r.w[k] - u;
        if (run > SCREEN_XSIZE - x)
            run = SCREEN_XSIZE - x;
        if (k == piece) {
            psyqo::PrimPieces::TPageAttr attr;
            attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
            attr.setPageX((uint8_t)(r.x[k] >> 6));
            attr.setPageY((uint8_t)(r.y[k] >> 8));
            PS1ChainTPage(attr);
            psyqo::Prim::Sprite s(psyqo::Color{ { .r = 0x80, .g = 0x80, .b = 0x80 } });
            s.position = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)sy } };
            psyqo::PrimPieces::ClutIndex clut;
            memcpy(&clut, &s_clut, 2);
            s.texInfo = psyqo::PrimPieces::TexInfo{ (uint8_t)u, (uint8_t)((r.y[k] & 0xFF) + row), clut };
            s.size    = psyqo::Vertex{ { .x = (int16_t)run, .y = (int16_t)h } };
            PS1ChainSprite(s);
            ++prims;
        }
        x += run;
        t += run;
        if (t >= r.period)
            t -= r.period;
    }
    return prims;
}

struct PS1StripBand {
    short sy, h, ly, chunkX;
    uint8_t region;
};
static PS1StripBand s_stripBands[SCREEN_YSIZE];
static short s_lineX[SCREEN_YSIZE];

// upstream DrawHLineScrollLayer: the same scroll-state update (BG autoscroll, parallax line
// positions: game state) and per-line X (parallax line, deformation above / below the water line,
// wrap), drawn as bands of lines sharing one X (a band never crosses a tile row).
void DrawHLineScrollLayer(int layerID)
{
    uint32_t t0      = PS1Hblanks();
    g_ps1ProfLayer[4] = g_ps1ProfLayer[4] + 1;
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    int layerwidth   = layer->xsize;
    int layerheight  = layer->ysize;
    byte above       = layerID >= tLayerMidPoint;
    int *deformationData, *deformationDataW;

    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int yScroll    = yScrollOffset * layer->parallaxFactor >> 8;
        int fullheight = layerheight << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset    = (yScroll + (layer->scrollPos >> 16)) % fullheight;
        layerheight      = fullheight >> 7;
        deformationData  = &bgDeformationData2[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData3[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }
    else { // FG Layer
        lastXSize     = layer->xsize;
        yscrollOffset = yScrollOffset;
        for (int i = 0; i < PARALLAX_COUNT; ++i) hParallax.linePos[i] = xScrollOffset;
        deformationData  = &bgDeformationData0[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData1[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }

    if (layer->type == LAYER_HSCROLL) {
        if (lastXSize != layerwidth) {
            int fullLayerwidth = layerwidth << 7;
            for (int i = 0; i < hParallax.entryCount; ++i) {
                hParallax.linePos[i] = xScrollOffset * hParallax.parallaxFactor[i] >> 8;
                hParallax.scrollPos[i] += hParallax.scrollSpeed[i];
                if (hParallax.scrollPos[i] > fullLayerwidth << 16)
                    hParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (hParallax.scrollPos[i] < 0)
                    hParallax.scrollPos[i] += fullLayerwidth << 16;
                hParallax.linePos[i] += hParallax.scrollPos[i] >> 16;
                hParallax.linePos[i] %= fullLayerwidth;
            }
        }
        lastXSize = layerwidth;
    }
    // (end of the upstream state update)

    if (!layerwidth || !layerheight || !s_frameOpen)
        return;
    PS1TileSetup();
    int rows  = layerheight < TILELAYER_CHUNK_H ? layerheight : TILELAYER_CHUNK_H; // PS1-sized rows (Scene.hpp)
    int fullW = layerwidth << 7;
    int fullH = rows << 7;
    int tileYPos = yscrollOffset % (layerheight << 7);
    if (tileYPos < 0)
        tileYPos += layerheight << 7;
    tileYPos %= fullH;

    // The FG without deformation scrolls every line by xScrollOffset (upstream sets every
    // hParallax.linePos to it above): one band per tile row, no per-line pass (the Nexus fast path).
    bool uniform = !activeTileLayers[layerID];
    for (int i = 0; i < PARALLAX_COUNT && uniform; ++i) uniform = !hParallax.deform[i];
    int ly = tileYPos;
    if (uniform) {
        int chunkX = xScrollOffset % fullW;
        if (chunkX < 0)
            chunkX += fullW;
        for (int y = 0; y < SCREEN_YSIZE;) {
            int h = TILE_SIZE - (ly & 0xF);
            if (h > SCREEN_YSIZE - y)
                h = SCREEN_YSIZE - y;
            PS1DrawTileBand(layer, above, y, h, ly, chunkX, fullW);
            y += h;
            ly += h;
            if (ly >= fullH)
                ly -= fullH;
        }
        g_ps1LayerHbl     = g_ps1LayerHbl + PS1HblanksSince(t0);
        g_ps1ProfLayer[0] = g_ps1ProfLayer[0] + PS1HblanksSince(t0);
        return;
    }
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        byte idx   = layer->lineScroll[ly];
        int chunkX = hParallax.linePos[idx];
        if (y < waterDrawPos) {
            if (hParallax.deform[idx])
                chunkX += *deformationData;
            ++deformationData;
        }
        else {
            if (hParallax.deform[idx])
                chunkX += *deformationDataW;
            ++deformationDataW;
        }
        if (chunkX < 0)
            chunkX += fullW;
        if (chunkX >= fullW)
            chunkX -= fullW;
        s_lineX[y] = (short)chunkX;
        if (++ly >= fullH)
            ly = 0;
    }

    uint32_t t1             = PS1Hblanks();
    g_ps1ProfLayer[1]       = g_ps1ProfLayer[1] + PS1HblanksSince(t0);
    uint32_t tTiles         = 0;
    int stripBands          = 0;
    int layerIdx            = activeTileLayers[layerID];
    const uint8_t *stripRow = s_stripCount && layerIdx < LAYER_COUNT ? s_stripRow[layerIdx][above] : nullptr;
    ly                      = tileYPos;
    for (int y = 0; y < SCREEN_YSIZE;) {
        int chunkX = s_lineX[y];
        int ri     = stripRow ? stripRow[ly >> 4] : 0;
        int h      = 1;
        if (ri) { // strip region: a band runs until the X scroll changes or the region ends
            const PS1StripRegion &r = s_strips[ri - 1];
            int end                 = r.line0 + r.lines;
            while (y + h < SCREEN_YSIZE && ly + h < end && s_lineX[y + h] == chunkX) ++h;
            PS1StripBand &b = s_stripBands[stripBands++]; // drawn after the loop, piece by piece
            b.sy = (short)y, b.h = (short)h, b.ly = (short)ly, b.chunkX = (short)chunkX, b.region = (uint8_t)(ri - 1);
        }
        else {
            int rowLeft = TILE_SIZE - (ly & 0xF);
            while (h < rowLeft && y + h < SCREEN_YSIZE && s_lineX[y + h] == chunkX) ++h;
            uint32_t tb = PS1Hblanks();
            PS1DrawTileBand(layer, above, y, h, ly, chunkX, fullW);
            tTiles += PS1HblanksSince(tb);
        }
        y += h;
        ly += h;
        if (ly >= fullH)
            ly -= fullH;
    }
    g_ps1ProfLayer[2] = g_ps1ProfLayer[2] + tTiles;
    uint32_t t2       = PS1Hblanks();
    for (int i = 0; i < stripBands;) { // strip bands grouped by region and piece (one texpage each)
        int region = s_stripBands[i].region, j = i;
        while (j < stripBands && s_stripBands[j].region == region) ++j;
        const PS1StripRegion &r = s_strips[region];
        int prims               = 0;
        for (int k = 0; k < r.pieces; ++k)
            for (int b = i; b < j; ++b) prims += PS1DrawStripBand(r, s_stripBands[b].sy, s_stripBands[b].h, s_stripBands[b].ly, s_stripBands[b].chunkX, k);
        g_ps1LayerPrims = g_ps1LayerPrims + prims;
        i               = j;
    }
    g_ps1ProfLayer[3] = g_ps1ProfLayer[3] + PS1HblanksSince(t2);
    (void)t1;
    g_ps1LayerHbl = g_ps1LayerHbl + PS1HblanksSince(t0);
}

// upstream DrawVLineScrollLayer's scroll-state update (incl. its `scrollPos += scrollPos << 16`);
// drawing VScroll layers is step 3.5.
void DrawVLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    int layerwidth   = layer->xsize;
    int layerheight  = layer->ysize;
    if (activeTileLayers[layerID]) { // BG Layer
        int fullLayerwidth = layerwidth << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullLayerwidth << 16)
            layer->scrollPos -= fullLayerwidth << 16;
    }
    else { // FG Layer
        lastYSize            = layer->ysize;
        vParallax.linePos[0] = yScrollOffset;
        vParallax.deform[0]  = true;
    }
    if (layer->type == LAYER_VSCROLL) {
        if (lastYSize != layerheight) {
            int fullLayerheight = layerheight << 7;
            for (int i = 0; i < vParallax.entryCount; ++i) {
                vParallax.linePos[i] = yScrollOffset * vParallax.parallaxFactor[i] >> 8;
                vParallax.scrollPos[i] += vParallax.scrollSpeed[i];
                vParallax.scrollPos[i] += vParallax.scrollPos[i] << 16;
                if (vParallax.scrollPos[i] > fullLayerheight << 16)
                    vParallax.scrollPos[i] -= fullLayerheight << 16;
                vParallax.linePos[i] += vParallax.scrollPos[i] >> 16;
                vParallax.linePos[i] %= fullLayerheight;
            }
        }
        lastYSize = layerheight;
    }
    g_ps1DrawTodo = g_ps1DrawTodo + 1;
}

// ---------------------------------------------------------------------------------------------------
// Shapes

// Rectangle at a PS1 alpha level (see PS1AlphaLevel): 50 % = B/2 + F/2, 75 % = twice.
static void PS1ChainAlphaRect(int XPos, int YPos, int width, int height, int R, int G, int B, int level)
{
    psyqo::Prim::Rectangle r(psyqo::Color{ { .r = (uint8_t)R, .g = (uint8_t)G, .b = (uint8_t)B } });
    r.position = psyqo::Vertex{ { .x = (int16_t)XPos, .y = (int16_t)YPos } };
    r.size     = psyqo::Vertex{ { .w = (int16_t)width, .h = (int16_t)height } };
    if (level >= 3) {
        PS1ChainRect(r);
        return;
    }
    psyqo::PrimPieces::TPageAttr attr; // untextured semi-transparency takes its mode from the texpage
    attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
    PS1ChainTPage(attr);
    r.setSemiTrans();
    PS1ChainRect(r);
    if (level == 2)
        PS1ChainRect(r);
}

// The pending full-screen fade turned out not to end the frame: draw it now (4 levels).
static void PS1FadeHook()
{
    if (!s_fadePending)
        return;
    s_fadePending = false;
    int level     = PS1AlphaLevel(s_fadeA);
    if (level)
        PS1ChainAlphaRect(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, s_fadeR, s_fadeG, s_fadeB, level);
}

void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A)
{
    if (A <= 0 || width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    if (A > 0xFF)
        A = 0xFF;
    PS1FadeHook(); // an earlier pending fade is followed by this: draw it first
    // Only in a frame that started with a clear: a repeated frame's copied pixels aren't CLUT-based
    // any more, so a CLUT fade wouldn't reach them (paused stage mode, PC's persistent framebuffer).
    if (A < 0xFF && s_frameFresh && XPos <= 0 && YPos <= 0 && XPos + width >= SCREEN_XSIZE && YPos + height >= SCREEN_YSIZE) {
        s_fadePending = true; // exact CLUT fade at FlipScreen unless something is drawn after it
        s_fadeR = R, s_fadeG = G, s_fadeB = B, s_fadeA = A;
        PS1ChainSetHook(PS1FadeHook);
        return;
    }
    int level = PS1AlphaLevel(A == 0xFF ? 0xFF : A);
    if (level)
        PS1ChainAlphaRect(XPos, YPos, width, height, R, G, B, level);
}

void DrawTintRectangle(int XPos, int YPos, int width, int height) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID)
{
    g_ps1DrawTodo = g_ps1DrawTodo + 1;
}

// ---------------------------------------------------------------------------------------------------
// Sprites

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) { PS1ChainPieceSprite(p, XPos + p.ox, YPos + p.oy); });
}

void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    bool fx = direction & FLIP_X, fy = direction & FLIP_Y;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        int x = fx ? XPos + width - p.ox - p.w : XPos + p.ox;
        int y = fy ? YPos + height - p.oy - p.h : YPos + p.oy;
        PS1ChainPieceRect(p, x, y, p.w, p.h, fx, fy);
    });
}

// upstream (HW path): scale 0x200 = 1.0; the frame's top-left moves by pivot * scale.
void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID)
{
    if (width <= 0 || height <= 0 || scaleX <= 0 || scaleY <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    int sx = scaleX << 2, sy = scaleY << 2; // 2048 = 1.0
    int x0 = XPos - (pivotX * sx >> 11), y0 = YPos - (pivotY * sy >> 11);
    int W = width * sx >> 11;
    bool fx = direction == FLIP_X;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        int a = p.ox * sx >> 11, b = (p.ox + p.w) * sx >> 11;
        int t = p.oy * sy >> 11, d = (p.oy + p.h) * sy >> 11;
        if (b <= a || d <= t)
            return;
        int x = fx ? x0 + W - b : x0 + a;
        PS1ChainPieceRect(p, x, y0 + t, b - a, d - t, fx, false);
    });
}

void DrawScaledChar(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                    int sheetID)
{
    DrawSpriteScaled(direction, XPos, YPos, pivotX, pivotY, scaleX, scaleY, width, height, sprX, sprY, sheetID);
}

// upstream (HW path) corners: (XPos, YPos) + R * (x, y), x = +-(local x - pivotX), y = local y - pivotY,
// cos/sin from the 512-step tables (scaled by `scale` / 0x200 for rotozoom).
static void PS1DrawSpriteAffine(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                                int scale, int sheetID)
{
    if (width <= 0 || height <= 0 || scale == 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    rotation -= rotation >> 9 << 9;
    if (rotation < 0)
        rotation += 0x200;
    if (rotation != 0)
        rotation = 0x200 - rotation;
    int sn = sin512LookupTable[rotation] * scale >> 9;
    int cs = cos512LookupTable[rotation] * scale >> 9;
    bool fx = direction != FLIP_NONE;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        int16_t xs[4], ys[4];
        const int lx[4] = { p.ox, p.ox + p.w, p.ox, p.ox + p.w };
        const int ly[4] = { p.oy, p.oy, p.oy + p.h, p.oy + p.h };
        for (int k = 0; k < 4; ++k) {
            int x = fx ? pivotX - lx[k] : lx[k] - pivotX;
            int y = ly[k] - pivotY;
            xs[k] = (int16_t)(XPos + ((x * cs + y * sn) >> 9));
            ys[k] = (int16_t)(YPos + ((y * cs - x * sn) >> 9));
        }
        PS1ChainPieceQuad(p, xs, ys, p.u, p.u + p.w, p.v, p.v + p.h);
    });
}

void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID)
{
    if (!(rotation & 0x1FF) && direction == FLIP_NONE) { // no rotation: 1:1 sprite (pixel exact)
        DrawSprite(XPos - pivotX, YPos - pivotY, width, height, sprX, sprY, sheetID); // callers pass -frame pivot
        return;
    }
    PS1DrawSpriteAffine(direction, XPos, YPos, pivotX, pivotY, sprX, sprY, width, height, rotation, 0x200, sheetID);
}

void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID)
{
    PS1DrawSpriteAffine(direction, XPos, YPos, pivotX, pivotY, sprX, sprY, width, height, rotation, scale, sheetID);
}

// Ink effects (user decision 2026-09-24): INK_BLEND = 50 % (exact), INK_ALPHA = 4 levels, INK_ADD /
// INK_SUB = GPU add / subtract with the texels scaled by alpha (exact up to 5-bit colour).
static void PS1DrawInkSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID, uint8_t ink, uint8_t mod, int passes)
{
    s_ink    = ink;
    s_inkMod = mod;
    for (int i = 0; i < passes; ++i) DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
    s_ink    = PS1_INK_OPAQUE;
    s_inkMod = 0x80;
}

void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_HALF, 0x80, 1);
}

void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    int level = PS1AlphaLevel(alpha > 0xFF ? 0xFF : alpha);
    if (level == 3)
        DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
    else if (level)
        PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_HALF, 0x80, level);
}

void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    if (alpha > 0)
        PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_ADD, (uint8_t)((alpha + 1) >> 1), 1);
}

void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    if (alpha > 0)
        PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_SUB, (uint8_t)((alpha + 1) >> 1), 1);
}

// upstream DrawObjectAnimation, verbatim: picks the draw call from the animation's rotation style.
void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos)
{
    ObjectScript *objectScript = (ObjectScript *)objScr;
    Entity *entity             = (Entity *)ent;
    SpriteAnimation *sprAnim   = &animationList[objectScript->animFile->aniListOffset + entity->animation];
    SpriteFrame *frame         = &animFrames[sprAnim->frameListOffset + entity->frame];
    int rotation               = 0;

    switch (sprAnim->rotationStyle) {
        case ROTSTYLE_NONE:
            switch (entity->direction) {
                case FLIP_NONE:
                    DrawSpriteFlipped(frame->pivotX + XPos, frame->pivotY + YPos, frame->width, frame->height, frame->sprX, frame->sprY, FLIP_NONE,
                                      frame->sheetID);
                    break;
                case FLIP_X:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, frame->pivotY + YPos, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_X, frame->sheetID);
                    break;
                case FLIP_Y:
                    DrawSpriteFlipped(frame->pivotX + XPos, YPos - frame->height - frame->pivotY, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_Y, frame->sheetID);
                    break;
                case FLIP_XY:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, YPos - frame->height - frame->pivotY, frame->width, frame->height,
                                      frame->sprX, frame->sprY, FLIP_XY, frame->sheetID);
                    break;
                default: break;
            }
            break;
        case ROTSTYLE_FULL:
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              entity->rotation, frame->sheetID);
            break;
        case ROTSTYLE_45DEG:
            if (entity->rotation >= 0x100)
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, 0x200 - ((532 - entity->rotation) >> 6 << 6), frame->sheetID);
            else
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, (entity->rotation + 20) >> 6 << 6, frame->sheetID);
            break;
        case ROTSTYLE_STATICFRAMES: {
            if (entity->rotation >= 0x100)
                rotation = 8 - ((532 - entity->rotation) >> 6);
            else
                rotation = (entity->rotation + 20) >> 6;
            int frameID = entity->frame;
            switch (rotation) {
                case 0: // 0 deg
                case 8: // 360 deg
                    rotation = 0x00;
                    break;
                case 1: // 45 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0;
                    else
                        rotation = 0x80;
                    break;
                case 2: // 90 deg
                    rotation = 0x80;
                    break;
                case 3: // 135 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x80;
                    else
                        rotation = 0x100;
                    break;
                case 4: // 180 deg
                    rotation = 0x100;
                    break;
                case 5: // 225 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x100;
                    else
                        rotation = 384;
                    break;
                case 6: // 270 deg
                    rotation = 384;
                    break;
                case 7: // 315 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 384;
                    else
                        rotation = 0;
                    break;
                default: break;
            }

            frame = &animFrames[sprAnim->frameListOffset + frameID];
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              rotation, frame->sheetID);
            break;
        }
        default: break;
    }
}

// 3D faces (Sonic CD's title, special stages): step 3.3 / phase 7. Menu text: phase 4.
void DrawFace(void *v, uint colour) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
void DrawTexturedFace(void *v, byte sheetID) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
void DrawBitmapText(void *menu, int XPos, int YPos, int scale, int spacing, int rowStart, int rowCount) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
void DrawTextMenu(void *menu, int XPos, int YPos) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
