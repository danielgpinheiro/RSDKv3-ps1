/*
 * Build stamp: the git commit of the build (VERSION.TXT at the disc root, written by tools/disc/build_disc.py with
 * tools/gen_version.py) drawn in the bottom-left corner of the title screen, e.g. "v a1b2c3d4" / "v a1b2c3d4-dirty",
 * "v unknown" when the disc has no VERSION.TXT.
 *
 * PS1VersionLoad (boot, before the first load) reads the file with one sector read into a stack buffer (no heap,
 * no read-ahead window) and uploads the text once as a 4-bit texture: rows 504-511 of VRAM, x 0-31, its CLUT at
 * (32, 504); nothing else uses that corner (the atlas / BG strip CLUT slots end at row 503). The renderer
 * (FlipScreen) then chains it once per title frame (PS1VersionDraw: a dark shadow + the light text, one 9-word
 * fragment); PS1VersionUpload puts the texture back when the title comes again.
 */
#pragma once

#include <stdint.h>

void PS1VersionLoad();   // boot: VERSION.TXT -> the stamp text (32-byte static buffer) + texture upload
void PS1VersionUpload(); // the texture + CLUT again (immediate upload: waits for the in-flight chain)
const char *PS1VersionText(); // "v a1b2c3d4" (GDB / tests)

// Chains the stamp with its text's top-left at x, y: texpage + two GP0(64h) sprites of the same texels, a dark grey
// shadow one pixel down-right, then the light grey text. Their colours apply the frame's pending screen fade (fade
// colour r, g, b at alpha a, 0-255; a = 0: none) as FlipScreen applies it to direct colours: each sprite is one
// colour, so modulating it gives the exact blend (the Title fades from black and flashes to white).
void PS1VersionDraw(int x, int y, int r, int g, int b, int a);

#define PS1_VERSION_TEXT_H 7

extern volatile uint32_t g_ps1VersionFound;   // 1 = VERSION.TXT was read
extern volatile uint32_t g_ps1VersionUploads; // texture uploads (boot + each return to the title)
extern volatile uint32_t g_ps1VersionDraws;   // frames the stamp was drawn
