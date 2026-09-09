#include <stdio.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include "text_utils.h"
#include "image/image_decoder.h"

#include "sw_renderer_private.h"

void platformSetNextFramebuffer(uintpixel_t* framebuffer, int width, int height);

static void SWRenderer_init(Renderer* renderer, DataWin* dataWin)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    renderer->dataWin = dataWin;
    
    //allocate texture buffer
    swr->textureCount = dataWin->txtr.count;
    swr->surfaceCount = SURFACE_MAX_COUNT;
    swr->totalTextureCount = swr->textureCount + swr->surfaceCount;
    swr->textures = (SWTexture**) safeCalloc(swr->totalTextureCount, sizeof(SWTexture*));
    swr->surfaces = (SWSurface**) safeCalloc(swr->surfaceCount, sizeof(SWSurface*));
    
    //allocate texture LRU cache to allow for dynamic unloading of textures
    swr->textureIndexLRU = (uint32_t*) safeCalloc(TEXTURE_LRU_LENGTH, sizeof(uint32_t));
    swr->textureIndexLRUHead = 0;
    swr->textureIndexLRUTail = 0;
    
    //HACK: this isn't good, really.  This should seriously be refactored.
    //expand datawin's tpag items list to include our surface count.
    swr->originalTPagCount = dataWin->tpag.count;
    dataWin->tpag.items = (TexturePageItem*) safeRealloc(dataWin->tpag.items, sizeof(TexturePageItem) * (dataWin->tpag.count + swr->surfaceCount));
    dataWin->tpag.count += swr->surfaceCount;
    
    swr->originalSpriteCount = dataWin->sprt.count;
    
    for (size_t i = swr->originalTPagCount; i < dataWin->tpag.count; i++)
    {
        memset(&dataWin->tpag.items[i], 0, sizeof(TexturePageItem));
        dataWin->tpag.items[i].texturePageId = -1;
    }
    
    fprintf(stderr, "SWRenderer initialized.\n");
}

static void SWRenderer_destroy(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    // TODO: why didn't I implement this.
    (void) swr;
    
    fprintf(stderr, "SWRenderer destroyed.\n");
}

static void SWRenderer_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->gameW = gameW;
    swr->gameH = gameH;
    swr->drawingToSurface = false;
    swr->blendMode = bm_normal;

    if (swr->width != windowW || swr->height != windowH)
    {
        //allocate frame buffer
        free(swr->fb);
        swr->fb = (uintpixel_t*) safeMalloc(windowW * windowH * sizeof(uintpixel_t));
        swr->fbPitch = windowW;
        swr->width = windowW;
        swr->height = windowH;
    }
}

// This used to be just one, "endFrame". Not sure what the difference is.
static void SWRenderer_endFrameInit(Renderer* renderer)
{
    (void) renderer;
    
    //this is kinda useless to do twice isn't it?
}

static void SWRenderer_endFrameEnd(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    assert(!swr->drawingToSurface);
    platformSetNextFramebuffer(swr->fb, swr->width, swr->height);
}

static void SWRenderer_beginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                                 int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle)
{
    (void)renderer; (void)viewX; (void)viewY; (void)viewW; (void)viewH;
    (void)portX; (void)portY; (void)portW; (void)portH; (void)viewAngle;
    UNIMP2();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    
    float xratio, yratio;
    
    float portviewX = (float) portW / viewW;
    float portviewY = (float) portH / viewH;
    
    int offsetX = 0, offsetY = 0;
    
    if (swr->drawingToSurface) {
        UNIMP();
        xratio = 1.0f;
        yratio = 1.0f;
        portX = (int)(portX * xratio);
        portY = (int)(portY * yratio);
    }
    else {
        float scaleX = (float) swr->width  / swr->gameW;
        float scaleY = (float) swr->height / swr->gameH;
        float scale  = (scaleX < scaleY) ? scaleX : scaleY;

        int32_t scaledW = (int32_t)(swr->gameW * scale);
        int32_t scaledH = (int32_t)(swr->gameH * scale);
        
        offsetX = (swr->width  - scaledW) / 2;
        offsetY = (swr->height - scaledH) / 2;

        xratio = scale;
        yratio = scale;

        portX = (int)(portX * xratio) + offsetX;
        portY = (int)(portY * yratio) + offsetY;
    }
    
    swr->scaleX = xratio * portviewX;
    swr->scaleY = yratio * portviewY;
    swr->offsetX = offsetX;
    swr->offsetY = offsetY;
    swr->defaultScaleX = xratio;
    swr->defaultScaleY = yratio;
    
    portW = (int)(portW * xratio);
    portH = (int)(portH * yratio);
    
    swr->viewActive = true;
    swr->viewX = viewX;
    swr->viewY = viewY;
    swr->viewW = viewW;
    swr->viewH = viewH;
    swr->portX = portX;
    swr->portY = portY;
    swr->portW = portW;
    swr->portH = portH;
    swr->maxX = portX + portW;
    swr->maxY = portY + portH;
}

static void SWRenderer_endView(Renderer* renderer)
{
    (void)renderer;
    UNIMP2();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->viewActive = false;
    
    swr->viewX = 0;
    swr->viewY = 0;
    swr->portX = swr->offsetX;
    swr->portY = swr->offsetY;
    swr->portW = swr->viewW = swr->width;
    swr->portH = swr->viewH = swr->height;
    swr->maxX = swr->portX + swr->portW;
    swr->maxY = swr->portY + swr->portH;
    swr->scaleX = swr->defaultScaleX;
    swr->scaleY = swr->defaultScaleY;
}

static void SWRenderer_beginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                                int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId)
{
    swrSwitchToSurface(renderer, targetSurfaceId, false);
    
    (void)guiW; (void)guiH;
    (void)portX; (void)portY; (void)portW; (void)portH;
    (void)targetSurfaceId;
    UNIMP2();
}

static void SWRenderer_setGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface)
{
    (void) renderer;
    (void) guiW; (void) guiH;
    (void) portW; (void) portH;
    (void) renderingToUserSurface;
    UNIMP();
}

static void SWRenderer_endGUI(Renderer* renderer)
{
    (void)renderer;
    UNIMP2();
}

static void SWRenderer_drawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                                  float originX, float originY, float xscale, float yscale,
                                  float angleDeg, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) {
        fprintf(stderr, "%s: tpagIndex of %d is invalid\n", __func__, tpagIndex);
        return;
    }

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) {
        fprintf(stderr, "%s: tpagIndex of %d is invalid, as pageId of %d is invalid\n", __func__, tpagIndex, pageId);
        return;
    }
    if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) {
        fprintf(stderr, "%s: could not ensure texture is loaded, tpagIndex: %d, pageId: %d\n", __func__, tpagIndex, pageId);
        return;
    }
    
    int sx = tpag->sourceX;
    int sy = tpag->sourceY;
    int sw = tpag->sourceWidth;
    int sh = tpag->sourceHeight;
    
    float dx = (float)(tpag->targetX - originX);
    float dy = (float)(tpag->targetY - originY);
    int dw = (int)(xscale * tpag->targetWidth);
    int dh = (int)(yscale * tpag->targetHeight);
    dx *= xscale;
    dy *= yscale;
    dx += x;
    dy += y;

    SWTexture* texture = swr->textures[pageId];
    
    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        float pivotX = (x - dx) * swrSgn(xscale);
        float pivotY = (y - dy) * swrSgn(yscale);
        
        if (tpag->targetWidth != tpag->sourceWidth)
            pivotX *= (float)tpag->targetWidth / tpag->sourceWidth;
        if (tpag->targetHeight != tpag->sourceHeight)
            pivotY *= (float)tpag->targetHeight/ tpag->sourceHeight;
        
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, pivotX, pivotY);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
    }
}

static void SWRenderer_drawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                      int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                                      float x, float y, float xscale, float yscale, float angleDeg,
                                      float pivotX, float pivotY, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;
    
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) return;
    
    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;
    if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) return;
    
    int sx = tpag->sourceX + srcOffX;
    int sy = tpag->sourceY + srcOffY;
    int sw = srcW;
    int sh = srcH;
    
    float dx = x;
    float dy = y;
    int dw = swrCeiling(xscale * sw);
    int dh = swrCeiling(yscale * sh);
    
    if (tpag->sourceWidth != tpag->targetWidth) {
        sx = sx * tpag->sourceWidth / tpag->targetWidth;
        sw = sw * tpag->sourceWidth / tpag->targetWidth;
    }
    if (tpag->sourceHeight != tpag->targetHeight) {
        sy = sy * tpag->sourceHeight / tpag->targetHeight;
        sh = sh * tpag->sourceHeight / tpag->targetHeight;
    }
    
    SWTexture* texture = swr->textures[pageId];
    
    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, pivotX * dw, pivotY * dh);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
    }
}

static void SWRenderer_drawSpritePos(Renderer* renderer, int32_t tpagIndex,
                                     float x1, float y1, float x2, float y2,
                                     float x3, float y3, float x4, float y4, float alpha)
{
    (void)renderer; (void)tpagIndex;
    (void)x1; (void)y1; (void)x2; (void)y2;
    (void)x3; (void)y3; (void)x4; (void)y4; (void)alpha;
    UNIMP();
}

static void SWRenderer_drawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                                     uint32_t color, float alpha, bool outline)
{
    uintpixel_t pxcolor = swrConvertPixel(color);
    
    if (outline)
        swrDrawRectangle(renderer, x1, y1, x2, y2, pxcolor, alpha);
    else
        swrFillRectangle(renderer, x1, y1, x2, y2, pxcolor, alpha);
}

static void SWRenderer_drawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                          uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
                                          float alpha, bool outline)
{
    uintpixel_t pxcolor1 = swrConvertPixel(color1);
    uintpixel_t pxcolor2 = swrConvertPixel(color2);
    uintpixel_t pxcolor3 = swrConvertPixel(color3);
    uintpixel_t pxcolor4 = swrConvertPixel(color4);
    
    if (outline)
        swrDrawRectangleColor(renderer, x1, y1, x2, y2, pxcolor1, pxcolor2, pxcolor3, pxcolor4, alpha);
    else
        swrFillRectangleColor(renderer, x1, y1, x2, y2, pxcolor1, pxcolor2, pxcolor3, pxcolor4, alpha);
}

static void SWRenderer_drawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                                float width, uint32_t color, float alpha)
{
    (void)renderer; (void)x1; (void)y1; (void)x2; (void)y2;
    (void)width; (void)color; (void)alpha;
    
    uintpixel_t colorCvt = swrConvertPixel(color);
#ifdef TRANSPARENT_MASK
    colorCvt |= TRANSPARENT_MASK;
#endif
    swrDrawLine(renderer, x1, y1, x2, y2, width, colorCvt, colorCvt, alpha);
}

static void SWRenderer_drawTriangle(Renderer* renderer,
                                    float x1, float y1, float x2, float y2, float x3, float y3,
                                    uint32_t color1, uint32_t color2, uint32_t color3,
                                    float alpha, bool outline)
{
    if (outline)
    {
        uintpixel_t color1cvt = swrConvertPixel(color1);
        uintpixel_t color2cvt = swrConvertPixel(color2);
        uintpixel_t color3cvt = swrConvertPixel(color3);
        swrDrawLine(renderer, x1, y1, x2, y2, 1, color1cvt, color2cvt, renderer->drawAlpha);
        swrDrawLine(renderer, x1, y1, x3, y3, 1, color1cvt, color3cvt, renderer->drawAlpha);
        swrDrawLine(renderer, x2, y2, x3, y3, 1, color3cvt, color3cvt, renderer->drawAlpha);
    }
    else
    {
        swrDrawTriangle(renderer, x1, y1, x2, y2, x3, y3, color1, color2, color3, alpha);
    }
}

static void SWRenderer_drawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                     float width, uint32_t color1, uint32_t color2, float alpha)
{
    swrDrawLine(renderer, x1, y1, x2, y2, width, swrConvertPixel(color1), swrConvertPixel(color2), alpha);
}

static void SWRenderer_drawText(Renderer* renderer, const char* text, float x, float y,
                                float xscale, float yscale, float angleDeg, float lineSeparation)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrDrawText(swr, text, x, y, xscale, yscale, angleDeg, renderer->drawColor, renderer->drawAlpha, lineSeparation);
}

static void SWRenderer_drawTextColor(Renderer* renderer, const char* text, float x, float y,
                                     float xscale, float yscale, float angleDeg,
                                     int32_t c1, int32_t c2, int32_t c3, int32_t c4, MAYBE_UNUSED float alpha,
                                     float lineSeparation)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    // TODO: allow c2, c3, c4
    (void) c2;
    (void) c3;
    (void) c4;
    
    swrDrawText(swr, text, x, y, xscale, yscale, angleDeg, c1, renderer->drawAlpha, lineSeparation);
}

static void SWRenderer_drawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
                                       float originX, float originY, float x, float y,
                                       float xscale, float yscale, bool tileX, bool tileY,
                                       float roomW, float roomH, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (0 > tpagIndex || dwin->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;
    if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) return;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;
    if (0 >= tileW || 0 >= tileH) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(x - originX * axScale, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = x - originX * axScale;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(y - originY * ayScale, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = y - originY * ayScale;
        endY = startY + tileH;
    }
    
    int sx = tpag->sourceX;
    int sy = tpag->sourceY;
    int sw = tpag->sourceWidth;
    int sh = tpag->sourceHeight;

    int localX0 = tpag->targetX - originX;
    int localY0 = tpag->targetY - originY;
    int localX1 = localX0 + tpag->sourceWidth;
    int localY1 = localY0 + tpag->sourceHeight;
    int sx0 = xscale * localX0;
    int sy0 = yscale * localY0;
    int sx1 = xscale * localX1;
    int sy1 = yscale * localY1;

    for (int dy = startY; endY > dy; dy += tileH) {
        int cy = dy + (int)(originY * ayScale);
        int vy0 = cy + sy0;
        int vy1 = cy + sy1;
        int dh = vy1 - vy0;

        for (int dx = startX; endX > dx; dx += tileW) {
            int cx = dx + (int)(originX * axScale);
            int vx0 = cx + sx0;
            int vx1 = cx + sx1;
            int dw = vx1 - vx0;

            swrDrawSprite(renderer, vx0, vy0, dw, dh, swr->textures[pageId], sx, sy, sw, sh, color, alpha);
        }
    }
}

static void SWRenderer_drawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;

    if (0 > surfaceID || swr->surfaceCount <= (size_t) surfaceID) return;

    SWSurface* surfaceP = swr->surfaces[surfaceID];
    if (!surfaceP) return;

    swrCommitShadowWritesToSurfaceIfNeeded(swr, surfaceP);
    SWTexture* surface = surfaceP->texture;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) surface->width * axScale;
    float tileH = (float) surface->height * ayScale;
    if (0 >= tileW || 0 >= tileH) return;
    
    float originX = 0, originY = 0;

    float startX, endX, startY, endY;
    startX = fmodf(x - originX * axScale, tileW);
    if (startX > 0) startX -= tileW;
    endX = roomW;
    startY = fmodf(y - originY * ayScale, tileH);
    if (startY > 0) startY -= tileH;
    endY = roomH;
    
    int sx = 0, sy = 0;
    int sw = surface->width;
    int sh = surface->height;

    int localX0 = -originX;
    int localY0 = -originY;
    int localX1 = localX0 + surface->width;
    int localY1 = localY0 + surface->height;
    int sx0 = xscale * localX0;
    int sy0 = yscale * localY0;
    int sx1 = xscale * localX1;
    int sy1 = yscale * localY1;

    for (int dy = startY; endY > dy; dy += tileH) {
        int cy = dy + (int)(originY * ayScale);
        int vy0 = cy + sy0;
        int vy1 = cy + sy1;
        int dh = vy1 - vy0;

        for (int dx = startX; endX > dx; dx += tileW) {
            int cx = dx + (int)(originX * axScale);
            int vx0 = cx + sx0;
            int vx1 = cx + sx1;
            int dw = vx1 - vx0;

            swrDrawSprite(renderer, vx0, vy0, dw, dh, surface, sx, sy, sw, sh, color, alpha);
        }
    }
}

static void SWRenderer_flush(Renderer* renderer)
{
    (void)renderer;
    UNIMP();
}

static void SWRenderer_clearScreen(Renderer* renderer, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    color = swrConvertPixel(color);
#ifdef TRANSPARENT_MASK
    if (alpha >= 0.95f) {
        color |= TRANSPARENT_MASK;
    }
    else if (alpha < 0.01f) {
        color &= ~TRANSPARENT_MASK;
    }
    else {
    #if PIXEL_SIZE == 32
        int alphai = (int)(255.0f * alpha);
        if (alphai < 0) alphai = 0;
        if (alphai > 255) alphai = 255;
        color |= (alphai << 24);
    #elif PIXEL_SIZE == 16
        color |= (alpha > 0.5f);
    #endif
    }
#endif
    
    for (int y = 0; y < swr->height; y++) {
        uintpixel_t* line = &swr->fb[y * swr->fbPitch];
        for (int x = 0; x < swr->width; x++) {
            line[x] = color;
        }
    }
}

static void SWRenderer_gpuSetBlendMode(Renderer* renderer, int32_t mode)
{
    //UNIMP();
    //(void)renderer; (void)mode;
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->blendMode = mode;
    
    //if (mode != bm_normal && mode != bm_add && mode != bm_subtract)
    {
        fprintf(stderr, "swr: unsupported blend mode: %d\n", mode);
    }
}

static void SWRenderer_gpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha)
{
    UNIMP();
    (void)renderer; (void)sfactor; (void)dfactor; (void)sfactor_alpha; (void)dfactor_alpha;
}

static void SWRenderer_gpuSetBlendEnable(Renderer* renderer, bool enable)
{
    UNIMP();
    (void)renderer; (void)enable;
}

static void SWRenderer_gpuSetAlphaTestEnable(Renderer* renderer, bool enable)
{
    UNIMP();
    (void)renderer; (void)enable;
}

static void SWRenderer_gpuSetAlphaTestRef(Renderer* renderer, uint8_t ref)
{
    UNIMP();
    (void)renderer; (void)ref;
}

static void SWRenderer_gpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (!swr->drawingToSurface) {
        fprintf(stderr, "swr: gpuSetColorWriteEnable not supported for main framebuffer");
        return;
    }
    
    SWSurface* currSurf = swr->surfaces[swr->currentSurfaceIndex];
    
    swrCommitShadowWritesToSurfaceIfNeeded(swr, currSurf);
    
    swr->writeMask =
        (red ? WRITE_MASK_RED : 0) |
        (green ? WRITE_MASK_GREEN : 0) |
        (blue ? WRITE_MASK_BLUE : 0) |
        (alpha ? WRITE_MASK_ALPHA : 0);
    
    // no need to change other properties, because the width and height are the same.
    // but we ALWAYS need to re-fetch the writable surface texture since the old one
    // may have been freed.
    swr->fb = swrWritableSurfaceTexture(swr, swr->currentSurfaceIndex)->buffer;
    
    if (currSurf->shadowTexture) {
        assert(currSurf->texture->width == currSurf->shadowTexture->width);
        assert(currSurf->texture->height == currSurf->shadowTexture->height);
        assert(currSurf->texture->buffer != currSurf->shadowTexture->buffer);
    }
}

static void SWRenderer_gpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha)
{
    *red = false;
    *green = false;
    *blue = false;
    *alpha = false;
    
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (!swr->drawingToSurface) {
        fprintf(stderr, "swr: gpuGetColorWriteEnable not supported for main framebuffer");
        return;
    }
    
    *red = (swr->writeMask & WRITE_MASK_RED) != 0;
    *green = (swr->writeMask & WRITE_MASK_GREEN) != 0;
    *blue = (swr->writeMask & WRITE_MASK_BLUE) != 0;
    *alpha = (swr->writeMask & WRITE_MASK_ALPHA) != 0;
}

static bool SWRenderer_gpuGetBlendEnable(Renderer* renderer)
{
    UNIMP();
    (void)renderer;
    return false;
}

static void SWRenderer_gpuSetFog(Renderer* renderer, bool enable, uint32_t color)
{
    UNIMP();
    (void)renderer; (void)enable; (void)color;
}

static int32_t SWRenderer_createSurface(Renderer* renderer, int32_t width, int32_t height)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    int32_t slot = -1;
    for (size_t i = 0; i < swr->surfaceCount; i++)
    {
        if (swr->surfaces[i] == NULL) {
            slot = (int32_t) i;
            break;
        }
    }
    
    if (slot < 0) {
        fprintf(stderr, "swr: Could not create surface, too many exist at once.\n");
        return slot;
    }
    
    swr->surfaces[slot] = swrCreateSurface(width, height);
    return slot;
}

static bool SWRenderer_surfaceExists(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount)
        return false;
    
    return swr->surfaces[surfaceID] != NULL;
}

static bool SWRenderer_setRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface)
{
    return swrSwitchToSurface(renderer, surfaceID, implicitApplicationSurface);
}

static float SWRenderer_getSurfaceWidth(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID)
        return (float)(int)((swr->drawingToSurface ? swr->mainWidth : swr->width) / swr->scaleX);
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return 0.0f;
    
    return (float) swr->surfaces[surfaceID]->texture->width;
}

static float SWRenderer_getSurfaceHeight(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) 
        return (float)(int)((swr->drawingToSurface ? swr->mainHeight : swr->height) / swr->scaleY);
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return 0.0f;
    
    return (float) swr->surfaces[surfaceID]->texture->height;
}

static void SWRenderer_drawSurface(Renderer* renderer, int32_t surfaceID,
                                   int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                   float x, float y, float xscale, float yscale, float angleDeg,
                                   uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    SWTexture* surface, localSurface;
    if (surfaceID == APPLICATION_SURFACE_ID) {
        localSurface.buffer = swr->drawingToSurface ? swr->mainFb : swr->fb;
        localSurface.width = swr->drawingToSurface ? swr->mainWidth : swr->width;
        localSurface.height = swr->drawingToSurface ? swr->mainHeight : swr->height;
        surface = &localSurface;
    } else {
        if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
            fprintf(stderr, "swr: Invalid surface id %d for drawSurface\n", surfaceID);
            return;
        }

        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[surfaceID]);
        surface = swr->surfaces[surfaceID]->texture;
    }
    
    if (srcWidth < 0) {
        srcWidth = surface->width;
        swrReverseTransformSizeIfNeeded(swr, &xscale, NULL);
    }
    if (srcHeight < 0) {
        srcHeight = surface->height;
        swrReverseTransformSizeIfNeeded(swr, NULL, &yscale);
    }
    
    int sx = srcLeft;
    int sy = srcTop;
    int sw = srcWidth;
    int sh = srcHeight;
    
    int tw = (int)(srcWidth * swr->scaleX);
    int th = (int)(srcHeight * swr->scaleY);
    
    float dx = x;
    float dy = y;
    int dw = (int)(xscale * tw);
    int dh = (int)(yscale * th);

    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        float pivotX = (x - dx) * swrSgn(xscale);
        float pivotY = (y - dy) * swrSgn(yscale);
        
        if (tw != sw)
            pivotX *= (float)tw / sw;
        if (th != sh)
            pivotY *= (float)th / sh;
        
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, surface, sx, sy, sw, sh, color, alpha, angleDeg, pivotX, pivotY);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, surface, sx, sy, sw, sh, color, alpha);
    }
}

static void SWRenderer_surfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        fprintf(stderr, "swr: Don't support resizing the application window with this.  There must be another way! (need to set to %dx%d)\n", width, height);
        return;
    }
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid\n", surfaceID);
        return;
    }
    
    swrFreeSurface(swr->surfaces[surfaceID]);
    swr->surfaces[surfaceID] = swrCreateSurface(width, height);
}

static void SWRenderer_surfaceFree(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        fprintf(stderr, "swr: Don't support SWRenderer_surfaceCopy the application window with this.  There must be another way!\n");
        return;
    }
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid\n", surfaceID);
        return;
    }
    
    swrFreeSurface(swr->surfaces[surfaceID]);
    swr->surfaces[surfaceID] = NULL;
}

static void SWRenderer_surfaceCopy(Renderer* renderer,
                                   int32_t DestSurfaceID, int32_t DestX, int32_t DestY,
                                   int32_t SrcSurfaceID, int32_t SrcX, int32_t SrcY,
                                   int32_t SrcW, int32_t SrcH, bool part)
{
    (void) part; // TODO: figure out the meaning of this parameter

    SWRenderer* swr = (SWRenderer*) renderer;
    
    SWTexture temp1, temp2;
    SWTexture *dstSurf, *srcSurf;
    
    if (DestSurfaceID == APPLICATION_SURFACE_ID) {
        dstSurf = &temp1;
        temp1.width = swr->mainWidth;
        temp1.height = swr->mainHeight;
        temp1.buffer = swr->mainFb;
    }
    else if (DestSurfaceID < 0 || (size_t) DestSurfaceID >= swr->surfaceCount || swr->surfaces[DestSurfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid (dest in surfaceCopy)\n", DestSurfaceID);
        return;
    }
    else {
        dstSurf = swrWritableSurfaceTexture(swr, DestSurfaceID);
    }
    
    if (SrcSurfaceID == APPLICATION_SURFACE_ID) {
        srcSurf = &temp2;
        temp2.width = swr->mainWidth;
        temp2.height = swr->mainHeight;
        temp2.buffer = swr->mainFb;
    }
    else if (SrcSurfaceID < 0 || (size_t) SrcSurfaceID >= swr->surfaceCount || swr->surfaces[SrcSurfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid (src in surfaceCopy)\n", SrcSurfaceID);
        return;
    }
    else {
        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[SrcSurfaceID]);
        srcSurf = swr->surfaces[SrcSurfaceID]->texture;
    }
    
    if (SrcX + SrcW < 0) return;
    if (SrcY + SrcH < 0) return;
    if (SrcX >= srcSurf->width) return;
    if (SrcY >= srcSurf->height) return;
    if (DestX + SrcW < 0) return;
    if (DestY + SrcH < 0) return;
    if (DestX >= dstSurf->width) return;
    if (DestY >= dstSurf->height) return;
    
    if (SrcY < 0) {
        SrcH += SrcY;
        DestY -= SrcY;
        SrcY = 0;
    }
    if (SrcX < 0) {
        SrcW += SrcX;
        DestX -= SrcX;
        SrcX = 0;
    }
    if (SrcX + SrcW >= srcSurf->width)
        SrcW = srcSurf->width - SrcX;
    if (SrcY + SrcH >= srcSurf->height)
        SrcH = srcSurf->height - SrcY;
    
    if (DestX + SrcW >= dstSurf->width)
        SrcW = dstSurf->width - DestX;
    if (DestY + SrcH >= dstSurf->height)
        SrcH = dstSurf->height - DestY;
    
    for (int dy = 0; dy < SrcH; dy++) {
        /***/ uintpixel_t* dstLine = &dstSurf->buffer[(dy + DestY) * dstSurf->width];
        const uintpixel_t* srcLine = &srcSurf->buffer[(dy + SrcY)  * srcSurf->width];
        for (int dx = 0; dx < SrcW; dx++) {
            dstLine[dx + DestX] = srcLine[dx + SrcX];
        }
    }
    
    UNIMP();
}

static bool SWRenderer_surfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA)
{
    UNIMP();
    (void)renderer; (void)surfaceID; (void)outRGBA;
    return false;
}

static int32_t SWRenderer_gpuGetBlendMode(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    return swr->blendMode;
}

static int32_t SWRenderer_createSpriteFromSurface(Renderer* renderer, int32_t surfaceID,
                                                   int32_t x, int32_t y, int32_t w, int32_t h,
                                                   bool removeback, bool smooth,
                                                   int32_t xorig, int32_t yorig)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    (void) smooth;
    
    int cropLeft = x;
    int cropTop = y;
    int cropRight = x + w;
    int cropBottom = y + h;
    
    SWTexture *srcTex, temp1;
    if (surfaceID == APPLICATION_SURFACE_ID)
    {
        srcTex = &temp1;
        temp1.width = swr->width;
        temp1.height = swr->height;
        temp1.buffer = swr->fb;
        
        swrTransformPosIntIfNeeded(swr, &cropLeft, &cropTop);
        swrTransformPosIntIfNeeded(swr, &cropRight, &cropBottom);
    }
    else
    {
        if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL){
            fprintf(stderr, "%s: Invalid surface ID %d\n", __func__, surfaceID);
            return -1;
        }
        SWSurface* surf = swr->surfaces[surfaceID];
        swrCommitShadowWritesToSurfaceIfNeeded(swr, surf);
        srcTex = surf->texture;
    }
    
    int32_t texturePageId = swrFindSurfaceTextureSlot(swr);
    int32_t tpagIndex = swrFindSurfaceTPagSlot(swr);
    if (texturePageId == -1 || tpagIndex == -1) {
        fprintf(stderr, "%s: Sprite overflow!!\n", __func__);
        return 0;
    }
    
    SWTexture* tex = swrCropSectionFromTexture(srcTex, w, h, cropLeft, cropTop, cropRight, cropBottom);
    if (removeback)
        swrRemoveBackgroundFromTexture(tex);
    
    swr->textures[texturePageId] = tex;

    // TODO[MrPowerGamerBR]: This is supposed to be refactored, not to modify data.win structs directly.
    DataWin* dw = swr->base.dataWin;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = texturePageId;
    
    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, swr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) tpag->targetWidth;
    sprite->height = (uint32_t) tpag->targetHeight;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*) safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    fprintf(stderr, "%s: Allocated surface sprite with ID %d\n", __func__, spriteIndex);
    return spriteIndex;
}

static void SWRenderer_deleteSprite(Renderer* renderer, int32_t spriteIndex)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    DataWin* dw = renderer->dataWin;
    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (swr->originalSpriteCount > (uint32_t) spriteIndex) {
        fprintf(stderr, "%s: Cannot delete sprite with index %d, it's invalid.\n", __func__, spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    for (uint32_t i = 0; i < sprite->textureCount; i++)
    {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= swr->originalTPagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && swr->totalTextureCount > (uint32_t) pageId) {
                swrFreeTexture(swr->textures[pageId]);
                swr->textures[pageId] = NULL;
            }
            // Mark TPAG slot as free for reuse
            tpag->texturePageId = -1;
        }
    }
    
    free(sprite->tpagIndices);
    sprite->tpagIndices = NULL;
    
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    fprintf(stderr, "SWR: Deleted sprite %d\n", spriteIndex);
}

static void SWRenderer_drawTiledPart(Renderer* renderer, int32_t tpagIndex,
                                     int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                     float dstX, float dstY, float dstW, float dstH,
                                     uint32_t color, float alpha)
{
    UNIMP();
    (void)renderer; (void)tpagIndex;
    (void)srcX; (void)srcY; (void)srcW; (void)srcH;
    (void)dstX; (void)dstY; (void)dstW; (void)dstH;
    (void)color; (void)alpha;
}

static int32_t SWRenderer_ensureApplicationSurface(Renderer* renderer, int32_t width, int32_t height)
{
    // We don't evict surfaces, and especially not the primary framebuffer,
    // but if we did, this is where we would restore it.
    (void) renderer;
    (void) width;
    (void) height;
    
    return APPLICATION_SURFACE_ID;
}

static RendererVtable swrVtable;

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    uintpixel_t pxcolor = swrConvertPixel(color);
    
    size_t fbSize = swr->fbPitch;
    fbSize *= swr->height;
    for (size_t i = 0; i < fbSize; i++)
    {
        swr->fb[i] = pxcolor;
    }
}

static uint32_t SWRenderer_spriteGetTexture(Renderer* renderer, int32_t tpagIndex)
{
    (void) renderer;

    return (uint32_t) tpagIndex + 1;
}

static uint32_t SWRenderer_surfaceGetTexture(Renderer* renderer, int32_t surfaceID)
{
    (void) renderer;
    (void) surfaceID;

    return (uint32_t) -1;
}

static float SWRenderer_textureGetTexelWidth(Renderer* renderer, uint32_t texID)
{
    (void) renderer;
    (void) texID;
    
    return 1.0f;
}

static float SWRenderer_textureGetTexelHeight(Renderer* renderer, uint32_t texID)
{
    (void) renderer;
    (void) texID;
    
    return 1.0f;
}

static bool SWRenderer_textureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs)
{
    (void) renderer;
    (void) texID;
    (void) outUVs;
    
    return false;
}

static void SWRenderer_textureSetStage(Renderer* renderer, int32_t slot, uint32_t texID)
{
    (void) renderer;
    (void) slot;
    (void) texID;
}

static bool SWRenderer_shaderIsCompiled(Renderer* renderer, int32_t shader)
{
    (void) renderer;
    (void) shader;
    
    return false;
}

static bool SWRenderer_shadersSupported(void)
{
    return false;
}

static void SWRenderer_gpuSetShader(Renderer* renderer, int32_t shaderIndex)
{
    (void) renderer;
    (void) shaderIndex;
}

static void SWRenderer_gpuResetShader(Renderer* renderer)
{
    (void) renderer;
}

static int32_t SWRenderer_shaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform)
{
    (void) renderer;
    (void) shaderIndex;
    (void) uniform;
    
    return 0;
}

static int32_t SWRenderer_shaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform)
{
    (void) renderer;
    (void) shaderIndex;
    (void) uniform;
    
    return 0;
}

static void SWRenderer_shaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4)
{
    (void) renderer;
    (void) handle;
    (void) count;
    (void) value1;
    (void) value2;
    (void) value3;
    (void) value4;
}

static void SWRenderer_shaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4)
{
    (void) renderer;
    (void) handle;
    (void) count;
    (void) value1;
    (void) value2;
    (void) value3;
    (void) value4;
}

static void SWRenderer_applyProjection(Renderer* renderer, const Matrix4f* worldToClip, const Matrix4f* idk)
{
    (void) renderer;
    (void) worldToClip;
    (void) idk;
    UNIMP();
}

Renderer* SWRenderer_create(void)
{
    SWRenderer* swr = (SWRenderer*) safeCalloc(1, sizeof(SWRenderer));
    swr->base.vtable = &swrVtable;
    swrVtable.init                     = SWRenderer_init;
    swrVtable.destroy                  = SWRenderer_destroy;
    swrVtable.beginFrame               = SWRenderer_beginFrame;
    swrVtable.endFrameInit             = SWRenderer_endFrameInit;
    swrVtable.endFrameEnd              = SWRenderer_endFrameEnd;
    swrVtable.beginView                = SWRenderer_beginView;
    swrVtable.endView                  = SWRenderer_endView;
    swrVtable.beginGUI                 = SWRenderer_beginGUI;
    swrVtable.setGuiProjection         = SWRenderer_setGuiProjection;
    swrVtable.endGUI                   = SWRenderer_endGUI;
    swrVtable.drawSprite               = SWRenderer_drawSprite;
    swrVtable.drawSpritePart           = SWRenderer_drawSpritePart;
    swrVtable.drawSpritePos            = SWRenderer_drawSpritePos;
    swrVtable.drawRectangle            = SWRenderer_drawRectangle;
    swrVtable.drawRectangleColor       = SWRenderer_drawRectangleColor;
    swrVtable.drawLine                 = SWRenderer_drawLine;
    swrVtable.drawTriangle             = SWRenderer_drawTriangle;
    swrVtable.drawLineColor            = SWRenderer_drawLineColor;
    swrVtable.drawText                 = SWRenderer_drawText;
    swrVtable.drawTextColor            = SWRenderer_drawTextColor;
    swrVtable.flush                    = SWRenderer_flush;
    swrVtable.clearScreen              = SWRenderer_clearScreen;
    swrVtable.createSpriteFromSurface  = SWRenderer_createSpriteFromSurface;
    swrVtable.deleteSprite             = SWRenderer_deleteSprite;
    swrVtable.gpuSetBlendMode          = SWRenderer_gpuSetBlendMode;
    swrVtable.gpuSetBlendModeExt       = SWRenderer_gpuSetBlendModeExt;
    swrVtable.gpuSetBlendEnable        = SWRenderer_gpuSetBlendEnable;
    swrVtable.gpuSetAlphaTestEnable    = SWRenderer_gpuSetAlphaTestEnable;
    swrVtable.gpuSetAlphaTestRef       = SWRenderer_gpuSetAlphaTestRef;
    swrVtable.gpuSetColorWriteEnable   = SWRenderer_gpuSetColorWriteEnable;
    swrVtable.gpuGetColorWriteEnable   = SWRenderer_gpuGetColorWriteEnable;
    swrVtable.gpuGetBlendEnable        = SWRenderer_gpuGetBlendEnable;
    swrVtable.gpuGetBlendMode          = SWRenderer_gpuGetBlendMode;
    swrVtable.gpuSetFog                = SWRenderer_gpuSetFog;
    swrVtable.drawSpriteTiled          = SWRenderer_drawSpriteTiled;
    swrVtable.drawSurfaceTiled         = SWRenderer_drawSurfaceTiled;
    swrVtable.createSurface            = SWRenderer_createSurface;
    swrVtable.surfaceExists            = SWRenderer_surfaceExists;
    swrVtable.setRenderTarget          = SWRenderer_setRenderTarget;
    swrVtable.ensureApplicationSurface = SWRenderer_ensureApplicationSurface;
    swrVtable.getSurfaceWidth          = SWRenderer_getSurfaceWidth;
    swrVtable.getSurfaceHeight         = SWRenderer_getSurfaceHeight;
    swrVtable.drawSurface              = SWRenderer_drawSurface;
    swrVtable.surfaceResize            = SWRenderer_surfaceResize;
    swrVtable.surfaceFree              = SWRenderer_surfaceFree;
    swrVtable.surfaceCopy              = SWRenderer_surfaceCopy;
    swrVtable.surfaceGetPixels         = SWRenderer_surfaceGetPixels;
    swrVtable.drawTiledPart            = SWRenderer_drawTiledPart;
    swrVtable.spriteGetTexture         = SWRenderer_spriteGetTexture;
    swrVtable.surfaceGetTexture        = SWRenderer_surfaceGetTexture;
    swrVtable.textureGetTexelWidth     = SWRenderer_textureGetTexelWidth;
    swrVtable.textureGetTexelHeight    = SWRenderer_textureGetTexelHeight;
    swrVtable.textureGetUVs            = SWRenderer_textureGetUVs;
    swrVtable.textureSetStage          = SWRenderer_textureSetStage;
    swrVtable.gpuSetShader             = SWRenderer_gpuSetShader;
    swrVtable.gpuResetShader           = SWRenderer_gpuResetShader;
    swrVtable.shaderGetUniform         = SWRenderer_shaderGetUniform;
    swrVtable.shaderGetSamplerIndex    = SWRenderer_shaderGetSamplerIndex;
    swrVtable.shaderSetUniformF        = SWRenderer_shaderSetUniformF;
    swrVtable.shaderSetUniformI        = SWRenderer_shaderSetUniformI;
    swrVtable.shaderIsCompiled         = SWRenderer_shaderIsCompiled;
    swrVtable.shadersSupported         = SWRenderer_shadersSupported;
    swrVtable.applyProjection          = SWRenderer_applyProjection;
    
    swrVtable.drawTile                 = NULL;
    
    swr->base.drawColor = 0xFFFFFF;
    swr->base.drawAlpha = 1.0f;
    swr->base.drawFont = -1;
    swr->base.drawHalign = 0;
    swr->base.drawValign = 0;
    swr->base.circlePrecision = 24;

    return (Renderer*) swr;
}
