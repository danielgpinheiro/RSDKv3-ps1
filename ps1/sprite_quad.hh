#pragma once
// Quad math of the sprite paths (ps1/render.cpp): the screen corners of a rotated / zoomed piece and the texel-edge UVs
// of rotated, scaled and mirrored pieces. Pure integer code without psyqo, so the host regression test
// (../tests/host/test_cd_sprite_quads.py) checks it against a model of the GPU's rasterizer.
//
// Conventions (psx-spx GPU "Render Polygon"): corners A B C D = the piece's local top-left, top-right, bottom-left,
// bottom-right; the GPU draws a quad as triangles ABC and BCD, samples each pixel's texel at its top-left point and
// leaves out the right / bottom edges. UVs are texel edges; u0 / v0 belong to corner A's side.

struct PS1QuadCorners {
    int x[4], y[4];
};
struct PS1QuadUV {
    int u0, u1, v0, v1;
};

// Corners of a piece (local rect ox, oy, w, h of the frame) rotated / zoomed about (pivotX, pivotY) and placed at
// (XPos, YPos); sn / cs = sin / cos * scale (512 = 1.0), fx = mirrored. Upstream (HW path) corners.
static inline PS1QuadCorners PS1AffineCorners(int XPos, int YPos, int pivotX, int pivotY, int sn, int cs, bool fx, int ox, int oy,
                                              int w, int h)
{
    PS1QuadCorners q;
    const int lx[4] = { ox, ox + w, ox, ox + w };
    const int ly[4] = { oy, oy, oy + h, oy + h };
    for (int k = 0; k < 4; ++k) {
        int x  = fx ? pivotX - lx[k] : lx[k] - pivotX;
        int y  = ly[k] - pivotY;
        q.x[k] = XPos + ((x * cs + y * sn) >> 9);
        q.y[k] = YPos + ((y * cs - x * sn) >> 9);
    }
    return q;
}

// UV edges of a rotated / zoomed piece (texture at u, v, size w x h) for the corners q; rightAngle = the rotation is a
// multiple of 90 degrees at scale 1.
static inline PS1QuadUV PS1AffineUV(const PS1QuadCorners &q, int u, int v, int w, int h, bool rightAngle)
{
    (void)q, (void)rightAngle;
    return PS1QuadUV{ u, u + w, v, v + h };
}

// UV edges of an axis-aligned piece (texture pu, pv, size pw x ph) drawn at w x h screen pixels, optionally mirrored:
// 1:1 mirrors sample texel u + w - 1 - k at column k (UV edges u + w - 1 .. u - 1), scaled ones the full [u, u + w) range.
static inline PS1QuadUV PS1RectUV(int pu, int pv, int pw, int ph, int w, int h, bool flipX, bool flipY)
{
    bool unit    = w == pw && h == ph;
    PS1QuadUV uv = { pu, pu + pw, pv, pv + ph };
    if (flipX) {
        uv.u0 = unit ? pu + pw - 1 : pu + pw;
        uv.u1 = unit ? pu - 1 : pu;
    }
    if (flipY) {
        uv.v0 = unit ? pv + ph - 1 : pv + ph;
        uv.v1 = unit ? pv - 1 : pv;
    }
    return uv;
}

// Texel edge at screen offset pos of a piece edge spanning len pixels / texels texels from base (mirrored: from the far
// edge), for pieces cut to the screen (PS1ChainPieceRectClipped).
static inline int PS1ClipEdge(int base, int texels, int pos, int len, bool flip)
{
    int t = (int)((long long)pos * texels / len);
    return flip ? base + texels - t : base + t;
}
