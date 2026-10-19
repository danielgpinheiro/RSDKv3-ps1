// Build stamp on the title screen (see version_stamp.hh).
#include "version_stamp.hh"

#include "gpu_chain.hh"

#include <string.h>

#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>
#include <psyqo/primitives/control.hh>

extern psyqo::GPU *g_ps1GPU;
extern psyqo::CDRomDevice *g_ps1CDRom;
bool PS1FsFind(const char *path, uint32_t *lba, uint32_t *size); // cdrom_fs.cpp

volatile uint32_t g_ps1VersionFound   = 0;
volatile uint32_t g_ps1VersionUploads = 0;
volatile uint32_t g_ps1VersionDraws   = 0;

namespace {

constexpr int16_t TEX_X = 0, TEX_Y = 504; // 4-bit texture: 128 x 8 texels = 32 x 8 halfwords (page (0, 256), v 248)
constexpr int16_t CLUT_X = 32, CLUT_Y = 504;
constexpr int TEX_W = 128, TEX_HW = TEX_W / 4, TEX_H = 8;
constexpr int CELL = 6; // glyph advance (5 x 7 glyph + 1 column)
constexpr int MAX_CHARS = TEX_W / CELL; // 21: "v " + 16 version characters fit
constexpr int GREY = 22;   // text colour (5-bit grey, 176 in 8 bits): light, quieter than the title's white
constexpr int SHADOW = 16; // shadow colour (8 bits): the texels modulated down to dark grey

char s_text[32] = "v unknown";
int s_len       = 9;

// 5 x 7 glyphs, row-major, bit 4 = leftmost: what a version string uses (git hashes are lowercase hex).
const char s_chars[] = "0123456789abcdefiknortuvwy-";
const uint8_t s_font[sizeof(s_chars) - 1][7] = {
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
    {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}, // 'a'
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1e}, // 'b'
    {0x00, 0x00, 0x0e, 0x10, 0x10, 0x11, 0x0e}, // 'c'
    {0x01, 0x01, 0x0d, 0x13, 0x11, 0x11, 0x0f}, // 'd'
    {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e}, // 'e'
    {0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08}, // 'f'
    {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e}, // 'i'
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, // 'k'
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}, // 'n'
    {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e}, // 'o'
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, // 'r'
    {0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06}, // 't'
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d}, // 'u'
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04}, // 'v'
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a}, // 'w'
    {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e}, // 'y'
    {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}, // '-'
};

const uint8_t *glyph(char c) {
    for (int i = 0; s_chars[i]; ++i)
        if (s_chars[i] == c)
            return s_font[i];
    return nullptr; // blank cell
}

} // namespace

void PS1VersionLoad() {
    uint32_t lba = 0, size = 0;
    uint32_t sec[512]; // one sector, word aligned for the CD DMA
    if (PS1FsFind("VERSION.TXT", &lba, &size) && size && g_ps1CDRom->readSectorsBlocking(lba, 1, sec, *g_ps1GPU)) {
        const char *p = (const char *)sec;
        int n         = 2; // after "v "
        for (uint32_t i = 0; i < size && i < 2048 && p[i] != '\n' && p[i] != '\r' && p[i] && n < 2 + 16; ++i) {
            char c      = p[i];
            s_text[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        if (n > 2) {
            s_text[n] = 0;
            s_len     = n;
            g_ps1VersionFound = 1;
        }
    }
    PS1VersionUpload();
}

void PS1VersionUpload() {
    uint16_t tex[TEX_HW * TEX_H] = {};
    for (int i = 0; i < s_len && i < MAX_CHARS; ++i) {
        const uint8_t *g = glyph(s_text[i]);
        for (int y = 0; g && y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (g[y] & (0x10 >> x)) {
                    int u = i * CELL + x;
                    tex[y * TEX_HW + u / 4] |= (uint16_t)(1u << ((u & 3) * 4)); // CLUT index 1; texel 0 = low nibble
                }
    }
    uint16_t clut[16] = {}; // 0 transparent; 1 the text colour, STP clear (a fade to black leaves 0x0000)
    clut[1] = GREY | GREY << 5 | GREY << 10;
    PS1GpuImmediate();
    g_ps1GPU->uploadToVRAM(tex, psyqo::Rect{{TEX_X, TEX_Y}, {TEX_HW, TEX_H}});
    g_ps1GPU->uploadToVRAM(clut, psyqo::Rect{{CLUT_X, CLUT_Y}, {16, 1}});
    g_ps1VersionUploads = g_ps1VersionUploads + 1;
}

const char *PS1VersionText() { return s_text; }

// One raw fragment: texpage (4-bit page (0, 256)), the shadow sprite one pixel down-right, the text sprite. The words
// change only with the fade, so they are rebuilt only then.
void PS1VersionDraw(int x, int y, int r, int g, int b, int a) {
    static uint32_t s_words[9];
    static uint32_t s_key = 0xFFFFFFFFu;
    uint32_t key = a ? (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r : 0;
    if (key != s_key || s_words[2] != ((uint32_t)(uint16_t)(x + 1) | (uint32_t)(uint16_t)(y + 1) << 16)) {
        s_key = key;
        psyqo::Prim::TPage tp;
        tp.attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex4Bits);
        tp.attr.setPageX(TEX_X / 64);
        tp.attr.setPageY(TEX_Y / 256);
        __builtin_memcpy(&s_words[0], &tp, sizeof(uint32_t));
        // texel x colour / 128 per channel (psx-spx "Texture Color"): colour = 128 * blend / texel colour; the texel
        // is 176 (>= 128), so the blend reaches white too
        constexpr int T = GREY * 8;
        int width = (s_len < MAX_CHARS ? s_len : MAX_CHARS) * CELL - 1;
        for (int k = 0; k < 2; ++k) { // k 0: shadow, 1: text
            int want = k ? T : SHADOW, c[3] = { r, g, b };
            uint32_t mod = 0;
            for (int ch = 0; ch < 3; ++ch) {
                int m = ((want * (255 - a) + c[ch] * a) / 255) * 128 / T;
                mod |= (uint32_t)(m > 255 ? 255 : m) << (8 * ch);
            }
            uint32_t *w = &s_words[1 + 4 * k];
            w[0] = 0x64000000u | mod; // textured rectangle, variable size, opaque, modulated
            w[1] = (uint32_t)(uint16_t)(x + 1 - k) | (uint32_t)(uint16_t)(y + 1 - k) << 16;
            w[2] = (uint32_t)(TEX_X % 64) * 4 | (uint32_t)(TEX_Y % 256) << 8 | (uint32_t)((CLUT_Y << 6) | (CLUT_X >> 4)) << 16;
            w[3] = (uint32_t)width | (uint32_t)PS1_VERSION_TEXT_H << 16;
        }
    }
    uint32_t *d = PS1ChainRawBegin(9);
    if (!d)
        return;
    for (int i = 0; i < 9; ++i) d[i] = s_words[i];
    PS1ChainRawEnd();
    PS1ChainInvalidateTPage(); // the texpage went in raw: the next PS1ChainTPage sends its own
    g_ps1VersionDraws = g_ps1VersionDraws + 1;
}
