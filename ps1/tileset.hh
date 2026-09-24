/*
 * Stage tile set (phase 3.1): Data/Stages/<folder>/16x16Tiles.vram, built by
 * tools/tiles/convert_tiles.py from 16x16Tiles.gif (format there): 4 texture pages of 256 tiles,
 * 8-bit master-palette indices, streamed straight into VRAM; tiles never live in main RAM.
 */
#pragma once

#include <stdint.h>

// VRAM home of the tile pages (as the Nexus port): 8-bit pages at pageX 9 and 11 (X 576-831), pageY
// 0 and 1; page p (tiles 256p..256p+255) at pageX 9 + 2 * (p & 1), pageY p >> 1.
#define PS1_TILE_PAGE_X 9
#define PS1_TILE_PAGE_Y 0

// Loads the folder's tile set: pages into VRAM (immediate uploads: call outside the chain, during a
// load) and the GIF's 256 RGB colour table into pal[768] (the caller applies entries 0x80-0xFF, as
// upstream LoadStageGIFFile). Returns false if the file is missing or short.
bool PS1TileSetLoad(const char *folder, uint8_t *pal768);

// Bit t set = tile t has at least one non-transparent texel (filled by PS1TileSetLoad from the pages
// it streams; the renderer skips empty tiles, as PC's per-pixel `> 0` test draws nothing for them).
extern uint8_t g_ps1TileSolid[1024 / 8];

extern volatile uint32_t g_ps1TileSetLoads; // tile sets loaded (GDB)
extern volatile uint32_t g_ps1TileSetBytes; // texel bytes uploaded by the last load
