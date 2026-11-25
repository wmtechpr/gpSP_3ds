#ifndef _3DSIMPL_GPU_H_
#define _3DSIMPL_GPU_H_

#include "3dsgpu.h"

#define MAX_VERTICES_QUAD 256

typedef struct {
    float u, v;
} STexCoord2f;

typedef struct {
    float x, y, z, w;
} SVector4f;

typedef struct {
    SVector4f    Position;
    STexCoord2f  TexCoord;
    u32          Color;
    u32          Flags;
} SQuadVertex;

void gpu3dsAddQuadRect(float x0, float y0, float x1, float y1, u16 wx, u16 wy, int z, u32 fillColor, u32 borderColor = 0, u8 borderSize = 0);

inline void __attribute__((always_inline)) gpu3dsAddQuadVertexes(
    float x0, float y0, float x1, float y1,
    STexCoord2f tl, STexCoord2f tr, STexCoord2f bl, STexCoord2f br,
    float z, int color = 0)
{
    SVertexList *list = &GPU3DS.vertices[VBO_SCREEN];
    SQuadVertex *vertices = &((SQuadVertex *) list->data)[list->from + list->count];

    vertices[0].Position = {x0, y0, z, 1};
    vertices[1].Position = {x1, y0, z, 1};
    vertices[2].Position = {x0, y1, z, 1};

    vertices[3].Position = {x1, y1, z, 1};
    vertices[4].Position = {x0, y1, z, 1};
    vertices[5].Position = {x1, y0, z, 1};

    vertices[0].TexCoord = tl;
    vertices[1].TexCoord = tr;
    vertices[2].TexCoord = bl;

    vertices[3].TexCoord = br;
    vertices[4].TexCoord = bl;
    vertices[5].TexCoord = tr;

    u32 colorSwapped = __builtin_bswap32(color);

    vertices[0].Color = colorSwapped;
    vertices[1].Color = colorSwapped;
    vertices[2].Color = colorSwapped;

    vertices[3].Color = colorSwapped;
    vertices[4].Color = colorSwapped;
    vertices[5].Color = colorSwapped;

    list->count += 6;
}

inline void __attribute__((always_inline)) gpu3dAddSubTextureQuadVertexes(
    float x0, float y0, float x1, float y1,
    const Tex3DS_SubTexture* subTex, int origWidth, int origHeight, int textureWidth, int textureHeight,
    float z, u32 color)
{
    if (!Tex3DS_SubTextureRotated(subTex)) {
        float top_u, top_v, bot_u, bot_v;
        Tex3DS_SubTextureTopLeft(subTex, &top_u, &top_v);
        Tex3DS_SubTextureBottomLeft(subTex, &bot_u, &bot_v);

        float tx0 = top_u * textureWidth;
        float ty0 = top_v * textureHeight;

        // If Top > Bottom, we are traversing memory backwards (common in 3DS)
        float dirY = (bot_v * textureHeight > ty0) ? 1.0f : -1.0f;
        float tx1 = tx0 + origWidth;
        float ty1 = ty0 + (origHeight * dirY);

        STexCoord2f tl = { tx0, ty0 };
        STexCoord2f tr = { tx1, ty0 };
        STexCoord2f bl = { tx0, ty1 };
        STexCoord2f br = { tx1, ty1 };

        gpu3dsAddQuadVertexes(x0, y0, x1, y1, tl, tr, bl, br, z, color);
    }
    else {
        float tl_u, tl_v, tr_u, tr_v, bl_u, bl_v, br_u, br_v;
        Tex3DS_SubTextureTopLeft(subTex, &tl_u, &tl_v);
        Tex3DS_SubTextureTopRight(subTex, &tr_u, &tr_v);
        Tex3DS_SubTextureBottomLeft(subTex, &bl_u, &bl_v);
        Tex3DS_SubTextureBottomRight(subTex, &br_u, &br_v);

        STexCoord2f tl = { tl_u * textureWidth, tl_v * textureHeight };
        STexCoord2f tr = { tr_u * textureWidth, tr_v * textureHeight };
        STexCoord2f bl = { bl_u * textureWidth, bl_v * textureHeight };
        STexCoord2f br = { br_u * textureWidth, br_v * textureHeight };

        gpu3dsAddQuadVertexes(x0, y0, x1, y1, tl, tr, bl, br, z, color);
    }
}

inline void __attribute__((always_inline)) gpu3dsAddSimpleQuadVertexes(
    float x0, float y0, float x1, float y1,
    float tx0, float ty0, float tx1, float ty1,
    float z, int color = 0)
{
    STexCoord2f tl = {tx0, ty0};
    STexCoord2f tr = {tx1, ty0};
    STexCoord2f bl = {tx0, ty1};
    STexCoord2f br = {tx1, ty1};

    gpu3dsAddQuadVertexes(x0, y0, x1, y1, tl, tr, bl, br, z, color);
}

#endif
