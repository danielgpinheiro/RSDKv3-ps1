// RSDKv3 PS1 renderer — phase 2 (docs/28): headless stand-in for RSDKv3/Drawing.cpp.
//
// Defines exactly what the rest of the engine uses from Drawing.cpp (the link's undefined list),
// with the state kept and every draw call a no-op. Phase 3 replaces this with the GPU renderer
// (per-stage VRAM atlas from tools/atlas, tile strips, DMA-chained primitives like RSDKv2-ps1's
// ps1 renderer). Sprite pixels are never in RAM on the PS1 (GFXDATA_SIZE, Sprite.cpp).
#include "RetroEngine.hpp"

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

int InitRenderDevice()
{
    SCREEN_XSIZE   = DEFAULT_SCREEN_XSIZE;
    SCREEN_CENTERX = SCREEN_XSIZE / 2;
    return 1;
}
void FlipScreen() {}
void GenerateBlendLookupTable() {}
void UpdateHardwareTextures() {}
void ClearScreen(byte index) {}

void DrawObjectList(int layer) {}
void DrawStageGFX() {}
void DrawDebugOverlays() {}

void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A) {}
void DrawTintRectangle(int XPos, int YPos, int width, int height) {}
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID)
{
}

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {}
void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID) {}
void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID)
{
}
void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID)
{
}
void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID)
{
}
void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {}
void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}

void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos) {}
void DrawFace(void *v, uint colour) {}
void DrawTexturedFace(void *v, byte sheetID) {}
void DrawBitmapText(void *menu, int XPos, int YPos, int scale, int spacing, int rowStart, int rowCount) {}
void DrawTextMenu(void *menu, int XPos, int YPos) {}
