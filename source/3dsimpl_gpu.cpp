#include "3dsimpl_gpu.h"

void gpu3dsAddQuadRect(float x0, float y0, float x1, float y1, u16 wx, u16 wy, int z, u32 fillColor, u32 borderColor, u8 borderSize)
{
    if (borderSize > 0) {
        float cx0 = x0 + borderSize;
        float cy0 = y0 + borderSize;
        float cx1 = x1 - borderSize;
        float cy1 = y1 - borderSize;

        // top, bottom, left, right
        gpu3dsAddSimpleQuadVertexes(x0, y0, x1, cy0, wx, wy, wx, wy, z, borderColor);
        gpu3dsAddSimpleQuadVertexes(x0, cy1, x1, y1, wx, wy, wx, wy, z, borderColor);
        gpu3dsAddSimpleQuadVertexes(x0, cy0, cx0, cy1, wx, wy, wx, wy, z, borderColor);
        gpu3dsAddSimpleQuadVertexes(cx1, cy0, x1, cy1, wx, wy, wx, wy, z, borderColor);

        gpu3dsAddSimpleQuadVertexes(cx0, cy0, cx1, cy1, wx, wy, wx, wy, z, fillColor);
    } else {
        gpu3dsAddSimpleQuadVertexes(x0, y0, x1, y1, wx, wy, wx, wy, z, fillColor);
    }
}
