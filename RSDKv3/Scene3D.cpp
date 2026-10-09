#include "RetroEngine.hpp"

int vertexCount = 0;
int faceCount   = 0;

Matrix matFinal = {};
Matrix matWorld = {};
Matrix matView  = {};
Matrix matTemp  = {};

Face faceBuffer[FACEBUFFER_SIZE];
Vertex vertexBuffer[VERTEXBUFFER_SIZE];
Vertex vertexBufferT[VERTEXBUFFER_SIZE];

DrawListEntry3D drawList3D[FACEBUFFER_SIZE];

int projectionX = 136;
int projectionY = 160;

int faceLineStart[SCREEN_YSIZE];
int faceLineEnd[SCREEN_YSIZE];
int faceLineStartU[SCREEN_YSIZE];
int faceLineEndU[SCREEN_YSIZE];
int faceLineStartV[SCREEN_YSIZE];
int faceLineEndV[SCREEN_YSIZE];

void SetIdentityMatrix(Matrix *matrix)
{
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixMultiply(Matrix *matrixA, Matrix *matrixB)
{
    int output[16];

    for (int i = 0; i < 0x10; ++i) {
        uint RowB = i & 3;
        uint RowA = (i & 0xC) / 4;
        output[i] = (matrixA->values[RowA][3] * matrixB->values[3][RowB] >> 8) + (matrixA->values[RowA][2] * matrixB->values[2][RowB] >> 8)
                    + (matrixA->values[RowA][1] * matrixB->values[1][RowB] >> 8) + (matrixA->values[RowA][0] * matrixB->values[0][RowB] >> 8);
    }

    for (int i = 0; i < 0x10; ++i) matrixA->values[i / 4][i % 4] = output[i];
}
void MatrixTranslateXYZ(Matrix *matrix, int x, int y, int z)
{
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = x;
    matrix->values[3][1] = y;
    matrix->values[3][2] = z;
    matrix->values[3][3] = 0x100;
}
void MatrixScaleXYZ(Matrix *matrix, int scaleX, int scaleY, int scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = 0;
    matrix->values[1][1] = scaleY;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = scaleZ;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateX(Matrix *matrix, int rotationX)
{
    if (rotationX < 0)
        rotationX = 0x200 - rotationX;
    rotationX &= 0x1FF;
    int sine             = sin512LookupTable[rotationX] >> 1;
    int cosine           = cos512LookupTable[rotationX] >> 1;
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = 0;
    matrix->values[1][1] = cosine;
    matrix->values[1][2] = sine;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = 0;
    matrix->values[2][1] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateY(Matrix *matrix, int rotationY)
{
    if (rotationY < 0)
        rotationY = 0x200 - rotationY;
    rotationY &= 0x1FF;
    int sine             = sin512LookupTable[rotationY] >> 1;
    int cosine           = cos512LookupTable[rotationY] >> 1;
    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateZ(Matrix *matrix, int rotationZ)
{
    if (rotationZ < 0)
        rotationZ = 0x200 - rotationZ;
    rotationZ &= 0x1FF;
    int sine             = sin512LookupTable[rotationZ] >> 1;
    int cosine           = cos512LookupTable[rotationZ] >> 1;
    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateXYZ(Matrix *matrix, int rotationX, int rotationY, int rotationZ)
{
    if (rotationX < 0)
        rotationX = 0x200 - rotationX;
    rotationX &= 0x1FF;
    if (rotationY < 0)
        rotationY = 0x200 - rotationY;
    rotationY &= 0x1FF;
    if (rotationZ < 0)
        rotationZ = 0x200 - rotationZ;
    rotationZ &= 0x1FF;
    int sineX   = sin512LookupTable[rotationX] >> 1;
    int cosineX = cos512LookupTable[rotationX] >> 1;
    int sineY   = sin512LookupTable[rotationY] >> 1;
    int cosineY = cos512LookupTable[rotationY] >> 1;
    int sineZ   = sin512LookupTable[rotationZ] >> 1;
    int cosineZ = cos512LookupTable[rotationZ] >> 1;

    matrix->values[0][0] = (sineZ * (sineY * sineX >> 8) >> 8) + (cosineZ * cosineY >> 8);
    matrix->values[0][1] = (sineZ * cosineY >> 8) - (cosineZ * (sineY * sineX >> 8) >> 8);
    matrix->values[0][2] = sineY * cosineX >> 8;
    matrix->values[0][3] = 0;
    matrix->values[1][0] = sineZ * -cosineX >> 8;
    matrix->values[1][1] = cosineZ * cosineX >> 8;
    matrix->values[1][2] = sineX;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = (sineZ * (cosineY * sineX >> 8) >> 8) - (cosineZ * sineY >> 8);
    matrix->values[2][1] = (sineZ * -sineY >> 8) - (cosineZ * (cosineY * sineX >> 8) >> 8);
    matrix->values[2][2] = cosineY * cosineX >> 8;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void TransformVertexBuffer()
{
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            matFinal.values[y][x] = matWorld.values[y][x];
        }
    }
    MatrixMultiply(&matFinal, &matView);

    if (vertexCount <= 0)
        return;

#if RETRO_PLATFORM == RETRO_PS1
    // A rotation about the X axis (the Title's clouds, docs/28 speed pass 2) has four zero factors: their terms,
    // (v * 0) >> 8, are 0 and are left out (the same sums, 5 multiplies a vertex instead of 9).
    const int(*m)[4] = matFinal.values;
    if (!m[0][1] && !m[0][2] && !m[1][0] && !m[2][0]) {
        int m00 = m[0][0], m11 = m[1][1], m12 = m[1][2], m21 = m[2][1], m22 = m[2][2], m30 = m[3][0], m31 = m[3][1], m32 = m[3][2];
        for (int v = 0; v < vertexCount; ++v) {
            int vx = vertexBuffer[v].x, vy = vertexBuffer[v].y, vz = vertexBuffer[v].z;
            Vertex *vert = &vertexBufferT[v];
            vert->x      = (vx * m00 >> 8) + m30;
            vert->y      = (vy * m11 >> 8) + (vz * m21 >> 8) + m31;
            vert->z      = (vy * m12 >> 8) + (vz * m22 >> 8) + m32;
        }
        return;
    }
#endif
    int inVertexID  = 0;
    int outVertexID = 0;
    do {
        int vx       = vertexBuffer[inVertexID].x;
        int vy       = vertexBuffer[inVertexID].y;
        int vz       = vertexBuffer[inVertexID].z;
        Vertex *vert = &vertexBufferT[inVertexID++];

        vert->x = (vx * matFinal.values[0][0] >> 8) + (vy * matFinal.values[1][0] >> 8) + (vz * matFinal.values[2][0] >> 8) + matFinal.values[3][0];
        vert->y = (vx * matFinal.values[0][1] >> 8) + (vy * matFinal.values[1][1] >> 8) + (vz * matFinal.values[2][1] >> 8) + matFinal.values[3][1];
        vert->z = (vx * matFinal.values[0][2] >> 8) + (vy * matFinal.values[1][2] >> 8) + (vz * matFinal.values[2][2] >> 8) + matFinal.values[3][2];
    } while (++outVertexID != vertexCount);
}
void TransformVerticies(Matrix *matrix, int startIndex, int endIndex)
{
    if (startIndex > endIndex)
        return;

    do {
        int vx       = vertexBuffer[startIndex].x;
        int vy       = vertexBuffer[startIndex].y;
        int vz       = vertexBuffer[startIndex].z;
        Vertex *vert = &vertexBuffer[startIndex];
        vert->x      = (vx * matrix->values[0][0] >> 8) + (vy * matrix->values[1][0] >> 8) + (vz * matrix->values[2][0] >> 8) + matrix->values[3][0];
        vert->y      = (vx * matrix->values[0][1] >> 8) + (vy * matrix->values[1][1] >> 8) + (vz * matrix->values[2][1] >> 8) + matrix->values[3][1];
        vert->z      = (vx * matrix->values[0][2] >> 8) + (vy * matrix->values[1][2] >> 8) + (vz * matrix->values[2][2] >> 8) + matrix->values[3][2];
    } while (++startIndex < endIndex);
}
#if RETRO_PLATFORM == RETRO_PS1 || defined(PS1_SORT3D_CHECK)
// Stable descending sort by depth (ties keep their order, like upstream's bubble sort): under 64 entries a merge sort,
// else an LSD radix sort, 8 bits a pass, over the key ~(depth ^ 0x80000000) (ascending key = descending depth); a pass whose byte is the same for every
// entry is skipped (docs/28 speed pass 2: the Title's 160 clouds take 1-2 passes instead of a merge sort's ~8).
void PS1SortDrawList3D(DrawListEntry3D *list, int count)
{
    static DrawListEntry3D scratch[FACEBUFFER_SIZE];
    DrawListEntry3D *src = list, *dst = scratch;
    if (count < 64) { // a few faces (special-stage backgrounds: 5): a bottom-up merge sort costs less than 256 buckets
        for (int width = 1; width < count; width <<= 1) {
            for (int lo = 0; lo < count; lo += width << 1) {
                int mid = lo + width < count ? lo + width : count;
                int hi  = lo + (width << 1) < count ? lo + (width << 1) : count;
                int a = lo, b = mid, o = lo;
                while (a < mid && b < hi) dst[o++] = src[b].depth > src[a].depth ? src[b++] : src[a++];
                while (a < mid) dst[o++] = src[a++];
                while (b < hi) dst[o++] = src[b++];
            }
            DrawListEntry3D *t = src;
            src                = dst;
            dst                = t;
        }
        if (src != list)
            for (int i = 0; i < count; ++i) list[i] = src[i];
        return;
    }
    for (int shift = 0; shift < 32; shift += 8) {
        int counts[256];
        for (int b = 0; b < 256; ++b) counts[b] = 0;
        for (int i = 0; i < count; ++i) ++counts[(~((unsigned)src[i].depth ^ 0x80000000u) >> shift) & 0xFF];
        if (count && counts[(~((unsigned)src[0].depth ^ 0x80000000u) >> shift) & 0xFF] == count)
            continue; // every key has this byte: the order stays
        for (int b = 0, at = 0; b < 256; ++b) {
            int n     = counts[b];
            counts[b] = at;
            at += n;
        }
        for (int i = 0; i < count; ++i) dst[counts[(~((unsigned)src[i].depth ^ 0x80000000u) >> shift) & 0xFF]++] = src[i];
        DrawListEntry3D *t = src;
        src                = dst;
        dst                = t;
    }
    if (src != list)
        for (int i = 0; i < count; ++i) list[i] = src[i];
}
#endif

void Sort3DDrawList()
{
    for (int i = 0; i < faceCount; ++i) {
        drawList3D[i].depth = (vertexBufferT[faceBuffer[i].d].z + vertexBufferT[faceBuffer[i].c].z + vertexBufferT[faceBuffer[i].b].z
                               + vertexBufferT[faceBuffer[i].a].z)
                              >> 2;
        drawList3D[i].faceID = i;
    }

#if RETRO_PLATFORM == RETRO_PS1
    // PS1: the bubble sort below is stable and descending, so any stable descending sort gives the
    // same order: a radix sort (the bubble sort took 235 hblanks = 0.9 frame of the title's 160 faces, docs/28
    // phase 2.3; a merge sort then ~32, speed pass 2). Identical order checked on the host:
    // tools/scene3d/sort3d_check.sh.
    PS1SortDrawList3D(drawList3D, faceCount);
    return;
#endif
    for (int i = 0; i < faceCount; ++i) {
        for (int j = faceCount - 1; j > i; --j) {
            if (drawList3D[j].depth > drawList3D[j - 1].depth) {
                int faceID               = drawList3D[j].faceID;
                int depth                = drawList3D[j].depth;
                drawList3D[j].faceID     = drawList3D[j - 1].faceID;
                drawList3D[j].depth      = drawList3D[j - 1].depth;
                drawList3D[j - 1].faceID = faceID;
                drawList3D[j - 1].depth  = depth;
            }
        }
    }
}
void Draw3DScene(int spriteSheetID)
{
    Vertex quad[4];
    for (int i = 0; i < faceCount; ++i) {
        Face *face = &faceBuffer[drawList3D[i].faceID];
#if RETRO_PLATFORM != RETRO_PS1 // PS1: DrawFace / DrawTexturedFace (ps1/render.cpp) read only the fields set below
        memset(quad, 0, 4 * sizeof(Vertex));
#endif
        switch (face->flags) {
            default: break;
            case FACE_FLAG_TEXTURED_3D:
                if (vertexBufferT[face->a].z > 0x100 && vertexBufferT[face->b].z > 0x100 && vertexBufferT[face->c].z > 0x100
                    && vertexBufferT[face->d].z > 0x100) {
                    quad[0].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->b].x / vertexBufferT[face->b].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->b].y / vertexBufferT[face->b].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->c].x / vertexBufferT[face->c].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->c].y / vertexBufferT[face->c].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->d].x / vertexBufferT[face->d].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->d].y / vertexBufferT[face->d].z;
                    quad[0].u = vertexBuffer[face->a].u;
                    quad[0].v = vertexBuffer[face->a].v;
                    quad[1].u = vertexBuffer[face->b].u;
                    quad[1].v = vertexBuffer[face->b].v;
                    quad[2].u = vertexBuffer[face->c].u;
                    quad[2].v = vertexBuffer[face->c].v;
                    quad[3].u = vertexBuffer[face->d].u;
                    quad[3].v = vertexBuffer[face->d].v;
                    DrawTexturedFace(quad, spriteSheetID);
                }
                break;
            case FACE_FLAG_TEXTURED_2D:
                quad[0].x = vertexBuffer[face->a].x;
                quad[0].y = vertexBuffer[face->a].y;
                quad[1].x = vertexBuffer[face->b].x;
                quad[1].y = vertexBuffer[face->b].y;
                quad[2].x = vertexBuffer[face->c].x;
                quad[2].y = vertexBuffer[face->c].y;
                quad[3].x = vertexBuffer[face->d].x;
                quad[3].y = vertexBuffer[face->d].y;
                quad[0].u = vertexBuffer[face->a].u;
                quad[0].v = vertexBuffer[face->a].v;
                quad[1].u = vertexBuffer[face->b].u;
                quad[1].v = vertexBuffer[face->b].v;
                quad[2].u = vertexBuffer[face->c].u;
                quad[2].v = vertexBuffer[face->c].v;
                quad[3].u = vertexBuffer[face->d].u;
                quad[3].v = vertexBuffer[face->d].v;
                DrawTexturedFace(quad, spriteSheetID);
                break;
            case FACE_FLAG_COLOURED_3D:
                if (vertexBufferT[face->a].z > 0x100 && vertexBufferT[face->b].z > 0x100 && vertexBufferT[face->c].z > 0x100
                    && vertexBufferT[face->d].z > 0x100) {
                    quad[0].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->b].x / vertexBufferT[face->b].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->b].y / vertexBufferT[face->b].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->c].x / vertexBufferT[face->c].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->c].y / vertexBufferT[face->c].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->d].x / vertexBufferT[face->d].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->d].y / vertexBufferT[face->d].z;
                    DrawFace(quad, face->colour);
                }
                break;
            case FACE_FLAG_COLOURED_2D:
                quad[0].x = vertexBuffer[face->a].x;
                quad[0].y = vertexBuffer[face->a].y;
                quad[1].x = vertexBuffer[face->b].x;
                quad[1].y = vertexBuffer[face->b].y;
                quad[2].x = vertexBuffer[face->c].x;
                quad[2].y = vertexBuffer[face->c].y;
                quad[3].x = vertexBuffer[face->d].x;
                quad[3].y = vertexBuffer[face->d].y;
                DrawFace(quad, face->colour);
                break;
        }
    }
}

void ProcessScanEdge(Vertex *vertA, Vertex *vertB)
{
    int bottom, top;
    int fullX, fullY;
    int trueX, yDifference;

    if (vertA->y == vertB->y)
        return;
    if (vertA->y >= vertB->y) {
        top    = vertB->y;
        bottom = vertA->y + 1;
    }
    else {
        top    = vertA->y;
        bottom = vertB->y + 1;
    }
    if (top > SCREEN_YSIZE - 1 || bottom < 0)
        return;
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;
    fullX       = vertA->x << 16;
    yDifference = vertB->y - vertA->y;
    fullY       = ((vertB->x - vertA->x) << 16) / yDifference;
    if (top < 0) {
        fullX -= top * fullY;
        top = 0;
    }
    for (int i = top; i < bottom; ++i) {
        trueX = fullX >> 16;
        if (fullX >> 16 < faceLineStart[i])
            faceLineStart[i] = trueX;
        if (trueX > faceLineEnd[i])
            faceLineEnd[i] = trueX;
        fullX += fullY;
    }
}
void ProcessScanEdgeUV(Vertex *vertA, Vertex *vertB)
{
    int bottom, top;
    int fullX, fullU, fullV;
    int trueX, trueU, trueV, yDifference;

    if (vertA->y == vertB->y)
        return;
    if (vertA->y >= vertB->y) {
        top    = vertB->y;
        bottom = vertA->y + 1;
    }
    else {
        top    = vertA->y;
        bottom = vertB->y + 1;
    }
    if (top > SCREEN_YSIZE - 1 || bottom < 0)
        return;
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;
    fullX      = vertA->x << 16;
    fullU      = vertA->u << 16;
    fullV      = vertA->v << 16;
    int finalX = ((vertB->x - vertA->x) << 16) / (vertB->y - vertA->y);
    if (vertA->u == vertB->u)
        trueU = 0;
    else
        trueU = ((vertB->u - vertA->u) << 16) / (vertB->y - vertA->y);

    if (vertA->v == vertB->v) {
        trueV = 0;
    }
    else {
        yDifference = vertB->y - vertA->y;
        trueV       = ((vertB->v - vertA->v) << 16) / yDifference;
    }
    if (top < 0) {
        fullX -= top * finalX;
        fullU -= top * trueU;
        fullV -= top * trueV;
        top = 0;
    }
    for (int i = top; i < bottom; ++i) {
        trueX = fullX >> 16;
        if (fullX >> 16 < faceLineStart[i]) {
            faceLineStart[i]  = trueX;
            faceLineStartU[i] = fullU;
            faceLineStartV[i] = fullV;
        }
        if (trueX > faceLineEnd[i]) {
            faceLineEnd[i]  = trueX;
            faceLineEndU[i] = fullU;
            faceLineEndV[i] = fullV;
        }
        fullX += finalX;
        fullU += trueU;
        fullV += trueV;
    }
}
