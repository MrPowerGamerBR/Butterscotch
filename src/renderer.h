#pragma once

#include "common.h"
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "data_win.h"
#include "instance.h"

// GameMaker Blend Modes
#define bm_complex -1

#define bm_normal 0
#define bm_add 1
#define bm_max 2
#define bm_subtract 3
#define bm_min 4
#define bm_reverse_subtract 5

#define bm_zero 1
#define bm_one 2
#define bm_src_color 3
#define bm_inv_src_color 4
#define bm_src_alpha 5
#define bm_inv_src_alpha 6
#define bm_dest_alpha 7
#define bm_inv_dest_alpha 8
#define bm_dest_color 9
#define bm_inv_dest_color 10
#define bm_src_alpha_sat 11

// ===[ Renderer Vtable ]===

typedef struct Renderer Renderer;

typedef struct {
    void (*init)(Renderer* renderer, DataWin* dataWin);
    void (*destroy)(Renderer* renderer);
    void (*beginFrame)(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH);
    void (*endFrame)(Renderer* renderer);
    void (*beginView)(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle);
    void (*endView)(Renderer* renderer);
    // GUI pass: coordinates are (0,0)..(guiW,guiH) mapped to the current view's port rect. Called after endView.
    void (*beginGUI)(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH);
    void (*endGUI)(Renderer* renderer);
    void (*drawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
    void (*drawSpritePart)(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha);
    void (*drawSpritePos)(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha);
    void (*drawRectangle)(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline);
    void (*drawLine)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha);
    void (*drawTriangle)(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline);
    void (*drawLineColor)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha);
    void (*drawText)(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg);
    void (*drawTextColor)(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha);
    void (*flush)(Renderer* renderer);
    int32_t (*createSpriteFromSurface)(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig);
    void (*deleteSprite)(Renderer* renderer, int32_t spriteIndex);
    void (*gpuSetBlendMode)(Renderer* renderer, int32_t mode);
    void (*gpuSetBlendModeExt)(Renderer* renderer, int32_t sfactor, int32_t dfactor);
    void (*gpuSetBlendEnable)(Renderer* renderer, bool enable);
    void (*gpuSetAlphaTestEnable)(Renderer* renderer, bool enable);
    void (*gpuSetAlphaTestRef)(Renderer* renderer, uint8_t ref);
    void (*gpuSetColorWriteEnable)(Renderer* renderer, bool red, bool green, bool blue, bool alpha);
    // Optional: platform-specific tile rendering (nullptr = use default drawSpritePart path)
    void (*drawTile)(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY);
    // Optional: platform-specific tiled draw (nullptr = use default per-tile drawSprite loop).
    void (*drawTiled)(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha);
} RendererVtable;

// ===[ Renderer Base Struct ]===

struct Renderer {
    RendererVtable* vtable;
    DataWin* dataWin;
    uint32_t drawColor;  // BGR format, default 0xFFFFFF (white)
    float drawAlpha;     // default 1.0
    int32_t drawFont;    // default -1 (no font)
    int32_t drawHalign;  // 0=left, 1=center, 2=right
    int32_t drawValign;  // 0=top, 1=middle, 2=bottom
};

// ===[ Shared Helpers (platform-agnostic) ]===

// Resolves a sprite + subimage to a TPAG index, with frame wrapping
static int32_t Renderer_resolveTPAGIndex(DataWin* dataWin, int32_t spriteIndex, int32_t subimg) {
    if (0 > spriteIndex || dataWin->sprt.count <= (uint32_t) spriteIndex) return -1;

    Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return -1;

    // Wrap subimage index
    int32_t frameIndex = subimg % (int32_t) sprite->textureCount;
    if (0 > frameIndex) frameIndex += (int32_t) sprite->textureCount;

    return sprite->tpagIndices[frameIndex];
}

// Forward declaration: defined further down once drawSpritePartExt is available.
static void Renderer_drawSpriteNineSlice(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, uint32_t color, float alpha);

// Stretched: draw_sprite_stretched(sprite, subimg, x, y, w, h)
static void Renderer_drawSpriteStretched(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (spriteIndex >= 0 && (uint32_t) spriteIndex < dw->sprt.count && dw->sprt.sprites[spriteIndex].nineSliceEnabled) {
        Renderer_drawSpriteNineSlice(renderer, spriteIndex, subimg, x, y, w, h, color, alpha);
        return;
    }

    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float xscale = w / (float) tpag->boundingWidth;
    float yscale = h / (float) tpag->boundingHeight;
    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, 0.0f, 0.0f, xscale, yscale, 0.0f, color, alpha);
}

// Full version: draw_sprite_ext(sprite, subimg, x, y, xscale, yscale, rot, color, alpha)
static void Renderer_drawSpriteExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float xscale, float yscale, float rot, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];

    // Nine-slice activates only when the draw scales the sprite away from its native size. At scale 1 there is nothing to slice.
    if (sprite->nineSliceEnabled && (xscale != 1.0f || yscale != 1.0f)) {
        // Rotated or flipped (negative-scale) draws would need rotation-aware sub-quads, which the current drawSpritePart API can't express. Fall back to a plain scaled draw and warn once.
        if (rot != 0.0f || 0.0f > xscale || 0.0f > yscale) {
            if (!sprite->nsWarnedUnsupportedTransform) {
                fprintf(stderr, "[nine-slice] sprite '%s': nine-slice with rotation or negative scale is not implemented (rot=%g, xscale=%g, yscale=%g), falling back to a plain scaled draw\n", sprite->name ? sprite->name : "<unnamed>", (double) rot, (double) xscale, (double) yscale);
                sprite->nsWarnedUnsupportedTransform = true;
            }
        } else {
            float w = (float) sprite->width * xscale;
            float h = (float) sprite->height * yscale;
            float tlX = x - (float) sprite->originX * xscale;
            float tlY = y - (float) sprite->originY * yscale;
            Renderer_drawSpriteNineSlice(renderer, spriteIndex, subimg, tlX, tlY, w, h, color, alpha);
            return;
        }
    }

    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, (float) sprite->originX, (float) sprite->originY, xscale, yscale, rot, color, alpha);
}

// Convenience: draw_sprite(sprite, subimg, x, y)
static void Renderer_drawSprite(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y) {
    Renderer_drawSpriteExt(renderer, spriteIndex, subimg, x, y, 1.0f, 1.0f, 0.0f, 0xFFFFFF, renderer->drawAlpha);
}

static void Renderer_drawSpritePos(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    renderer->vtable->drawSpritePos(renderer, tpagIndex, x1, y1, x2, y2, x3, y3, x4, y4, alpha);
}

// Draws part of a sprite with extended parameters (scale, color, alpha)
static void Renderer_drawSpritePartExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Clip region to TPAG bounds (same as Renderer_drawSpritePart)
    if (tpag->targetX > left) {
        int32_t off = tpag->targetX - left;
        x += (float) off * xscale;
        width -= off;
        left = 0;
    } else {
        left -= tpag->targetX;
    }

    if (tpag->targetY > top) {
        int32_t off = tpag->targetY - top;
        y += (float) off * yscale;
        height -= off;
        top = 0;
    } else {
        top -= tpag->targetY;
    }

    if (width > tpag->sourceWidth - left) width = tpag->sourceWidth - left;
    if (height > tpag->sourceHeight - top) height = tpag->sourceHeight - top;
    if (0 >= width || 0 >= height) return;

    renderer->vtable->drawSpritePart(renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, color, alpha);
}

// Partial draw: draw_sprite_part(sprite, subimg, left, top, width, height, x, y)
static void Renderer_drawSpritePart(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y) {
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, left, top, width, height, x, y, 1.0f, 1.0f, 0xFFFFFF, renderer->drawAlpha);
}

// Nine-slice draw into a target rect (x, y, w, h). Stretch tile mode is implemented; Repeat/Mirror/BlankRepeat/Hide
// fall back to Stretch and log a warning the first time each sprite hits an unsupported mode.
static const char* Renderer_nineSliceTileModeName(uint8_t mode) {
    switch (mode) {
        case 0: return "Stretch";
        case 1: return "Repeat";
        case 2: return "Mirror";
        case 3: return "BlankRepeat";
        case 4: return "Hide";
        default: return "Unknown";
    }
}

static void Renderer_drawSpriteNineSlice(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];

    int32_t L = sprite->nsLeft;
    int32_t T = sprite->nsTop;
    int32_t R = sprite->nsRight;
    int32_t B = sprite->nsBottom;
    int32_t sw = (int32_t) sprite->width;
    int32_t sh = (int32_t) sprite->height;
    int32_t srcCW = sw - L - R;
    int32_t srcCH = sh - T - B;

    // Degenerate slice (insets meet or overlap, or zero-size sprite): fall through to a plain stretch.
    if (0 >= srcCW || 0 >= srcCH || 0 >= sw || 0 >= sh) {
        int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
        if (0 > tpagIndex) return;
        TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
        renderer->vtable->drawSprite(renderer, tpagIndex, x, y, 0.0f, 0.0f, w / (float) tpag->boundingWidth, h / (float) tpag->boundingHeight, 0.0f, color, alpha);
        return;
    }

    // Warn (once per sprite) if any tile mode is something we don't yet implement.
    if (!sprite->nsWarnedUnsupportedMode) {
        static const char* SLOT_NAMES[5] = { "Left", "Top", "Right", "Bottom", "Center" };
        repeat(5, i) {
            if (sprite->nsTileModes[i] != 0) {
                fprintf(stderr, "[nine-slice] sprite '%s': %s tile mode '%s' is not implemented, falling back to Stretch\n", sprite->name ? sprite->name : "<unnamed>", SLOT_NAMES[i], Renderer_nineSliceTileModeName(sprite->nsTileModes[i]));
                sprite->nsWarnedUnsupportedMode = true;
            }
        }
    }

    float dstCW = w - (float) (L + R);
    float dstCH = h - (float) (T + B);
    float xsCenter = (dstCW > 0) ? dstCW / (float) srcCW : 0.0f;
    float ysCenter = (dstCH > 0) ? dstCH / (float) srcCH : 0.0f;

    // Corners: drawn at native pixel size, anchored to the four corners of the target rect.
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, 0,      0,      L, T, x,         y,         1.0f,     1.0f,     color, alpha);
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, sw - R, 0,      R, T, x + w - R, y,         1.0f,     1.0f,     color, alpha);
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, 0,      sh - B, L, B, x,         y + h - B, 1.0f,     1.0f,     color, alpha);
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, sw - R, sh - B, R, B, x + w - R, y + h - B, 1.0f,     1.0f,     color, alpha);

    // Edges: stretch only along the variable axis.
    if (dstCW > 0) {
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, L, 0,      srcCW, T, x + L, y,         xsCenter, 1.0f,     color, alpha);
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, L, sh - B, srcCW, B, x + L, y + h - B, xsCenter, 1.0f,     color, alpha);
    }
    if (dstCH > 0) {
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, 0,      T, L, srcCH, x,         y + T, 1.0f,     ysCenter, color, alpha);
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, sw - R, T, R, srcCH, x + w - R, y + T, 1.0f,     ysCenter, color, alpha);
    }

    // Center: stretches on both axes.
    if (dstCW > 0 && dstCH > 0) {
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, L, T, srcCW, srcCH, x + L, y + T, xsCenter, ysCenter, color, alpha);
    }
}

// Resolves a BGND index to its TPAG index.
static int32_t Renderer_resolveBackgroundTPAGIndex(DataWin* dataWin, int32_t bgndIndex) {
    if (0 > bgndIndex || (uint32_t) bgndIndex >= dataWin->bgnd.count) return -1;
    return dataWin->bgnd.backgrounds[bgndIndex].tpagIndex;
}

// Resolves a SPRT index to the TPAG index of its first frame.
static int32_t Renderer_resolveSpriteTPAGIndex(DataWin* dataWin, int32_t sprtIndex) {
    if (0 > sprtIndex || (uint32_t) sprtIndex >= dataWin->sprt.count) return -1;
    Sprite* spr = &dataWin->sprt.sprites[sprtIndex];
    if (spr->textureCount == 0) return -1;
    return spr->tpagIndices[0];
}

// Resolves a SPRT or BGND index to a TPAG index
static int32_t Renderer_resolveObjectTPAGIndex(DataWin* dataWin, RoomTile *tile) {
    if (!tile->useSpriteDefinition)
        return Renderer_resolveBackgroundTPAGIndex(dataWin, tile->backgroundDefinition);
    else
        return Renderer_resolveSpriteTPAGIndex(dataWin, tile->backgroundDefinition);
}

// Tiled draws.
// This will use a specialized vtable->drawTiled implementation, but if it doesn't, it will fall back to "manual" tiled rendering.
static void Renderer_drawTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    // Use the renderer's fast drawTiled path if it has one
    if (renderer->vtable->drawTiled != nullptr) {
        renderer->vtable->drawTiled(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha);
        return;
    }

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

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

    for (float dy = startY; endY > dy; dy += tileH) {
        for (float dx = startX; endX > dx; dx += tileW) {
            renderer->vtable->drawSprite(renderer, tpagIndex, dx + originX * axScale, dy + originY * ayScale, originX, originY, xscale, yscale, 0.0f, color, alpha);
        }
    }
}

// Draws a tiled background
static void Renderer_drawBackgroundTiled(Renderer* renderer, int32_t tpagIndex, float bgX, float bgY, bool tileX, bool tileY, float roomW, float roomH, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    Renderer_drawTiled(renderer, tpagIndex, 0.0f, 0.0f, bgX, bgY, 1.0f, 1.0f, tileX, tileY, roomW, roomH, 0xFFFFFFu, alpha);
}

// Draws a tiled sprite across the room
static void Renderer_drawSpriteTiled(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    float originX = (float) sprite->originX;
    float originY = (float) sprite->originY;

    Renderer_drawTiled(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, true, true, roomW, roomH, color, alpha);
}

// Default draw: draws instance's sprite using its image_* properties
static void Renderer_drawSelf(Renderer* renderer, Instance* instance) {
    if (0 > instance->spriteIndex) return;

    int32_t subimg = (int32_t) instance->imageIndex;
    Renderer_drawSpriteExt(
        renderer,
        instance->spriteIndex,
        subimg,
        (float) instance->x,
        (float) instance->y,
        (float) instance->imageXscale,
        (float) instance->imageYscale,
        (float) instance->imageAngle,
        instance->imageBlend,
        (float) instance->imageAlpha
    );
}

// Draws a room tile with layer shift offset applied
static void Renderer_drawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    // If the platform has a dedicated tile renderer, use it (PS2 has separate tile atlas entries)
    if (renderer->vtable->drawTile != nullptr) {
        renderer->vtable->drawTile(renderer, tile, offsetX, offsetY);
        return;
    }

    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tile);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

    // The tile's sourceX/Y are in the background image's coordinate space (bounding rect).
    // The TPAG may have been trimmed: actual content starts at (targetX, targetY) within the
    // bounding rect and has size sourceWidth x sourceHeight. We must clamp the tile's source
    // rect to the TPAG's content area to avoid sampling adjacent atlas textures.
    int32_t srcX = tile->sourceX;
    int32_t srcY = tile->sourceY;
    int32_t srcW = (int32_t) tile->width;
    int32_t srcH = (int32_t) tile->height;
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;

    // Clip left/top: if tile starts before the content region
    int32_t contentLeft = tpag->targetX;
    int32_t contentTop = tpag->targetY;
    if (contentLeft > srcX) {
        int32_t clip = contentLeft - srcX;
        drawX += (float) clip * tile->scaleX;
        srcW -= clip;
        srcX = contentLeft;
    }
    if (contentTop > srcY) {
        int32_t clip = contentTop - srcY;
        drawY += (float) clip * tile->scaleY;
        srcH -= clip;
        srcY = contentTop;
    }

    // Clip right/bottom: if tile extends past the content region
    int32_t contentRight = tpag->targetX + tpag->sourceWidth;
    int32_t contentBottom = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > contentRight) {
        srcW = contentRight - srcX;
    }
    if (srcY + srcH > contentBottom) {
        srcH = contentBottom - srcY;
    }

    if (0 >= srcW || 0 >= srcH) return;

    // Convert from bounding-rect coords to atlas-relative coords (subtract targetX/Y)
    int32_t atlasOffX = srcX - tpag->targetX;
    int32_t atlasOffY = srcY - tpag->targetY;

    // Extract alpha from high byte, default to 1.0 if alpha byte is 0
    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : (float) alphaByte / 255.0f;
    uint32_t bgr = tile->color & 0x00FFFFFF;

    renderer->vtable->drawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH, drawX, drawY, tile->scaleX, tile->scaleY, bgr, alpha);
}

// Mixes 2 colors with a blend factor
static uint32_t Renderer_mixColors(uint32_t color1, uint32_t color2, float blending) {
    // Extracts the color values out of each color
    uint8_t r1 = BGR_R(color1), g1 = BGR_G(color1), b1 = BGR_B(color1);
    uint8_t r2 = BGR_R(color2), g2 = BGR_G(color2), b2 = BGR_B(color2);

    // mixes each color together using linear interpolation
    uint8_t mixr = (uint8_t)(r1 * (1 - blending) + r2 * blending);
    uint8_t mixg = (uint8_t)(g1 * (1 - blending) + g2 * blending);
    uint8_t mixb = (uint8_t)(b1 * (1 - blending) + b2 * blending);

    uint32_t resultColor = ((mixr << 0) | (mixg << 8) | (mixb << 16)) & 0x00FFFFFF;
    return resultColor;
}
