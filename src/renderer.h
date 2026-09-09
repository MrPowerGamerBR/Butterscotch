#ifndef _BS_RENDERER_H_
#define _BS_RENDERER_H_

#include "common.h"
#include <stdint.h>
#include "stdio_compat.h"
#include "math_compat.h"
#include "matrix_math.h"
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

#define	MATRIX_VIEW 					0
#define	MATRIX_PROJECTION 				1
#define	MATRIX_WORLD 					2
#define	MATRIX_WORLD_VIEW 				3
#define	MATRIX_WORLD_VIEW_PROJECTION 	4
#define	MATRICES_MAX					5

#define MAX_VS_LIGHTS	8

#define MAX_TEXTURE_STAGES 8
//these 2 IDs are just IDs I simply made up, replace them with proper ones eventually oki?
#define GUI_CAMERA 4096
#define SURFACE_CAMERA 8192

// Sentinel returned by ensureApplicationSurface on platforms that don't back the application_surface with a real entry in the renderer's surface table.
//
// Also used as the initial value of Runner.applicationSurfaceId before the first ensure call.
#define APPLICATION_SURFACE_ID (-1)

// Sentinel used when beginGUI should render to the host framebuffer.
#define RENDER_TARGET_HOST_FRAMEBUFFER (-1)

// Nine-slice tile mode constants
#define NS_STRETCH    0
#define NS_REPEAT     1
#define NS_MIRROR     2
#define NS_BLANKREPEAT 3
#define NS_HIDE       4

// ===[ Renderer Vtable ]===

typedef struct Renderer Renderer;
#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif

typedef struct {
    int32_t src;
    int32_t dst;
    int32_t srcAlpha;
    int32_t dstAlpha;
} BlendFactors;

// Vertex    

typedef enum {
    PRIMITIVE_NONE = -1,
    PRIMITIVE_POINTS = 0,
    PRIMITIVE_LINES = 1,
    PRIMITIVE_LINE_STRIP = 2,
    PRIMITIVE_TRIANGLES = 3,
    PRIMITIVE_TRIANGLE_STRIP = 4,
    PRIMITIVE_TRIANGLE_FAN = 5,
} PrimitiveType;

typedef enum {
    VERTEX_USAGE_POSITION = 1,
    VERTEX_USAGE_COLOR   = 2,
    VERTEX_USAGE_NORMAL   = 3,
    VERTEX_USAGE_TEXCOORD = 4,
} VertexUsage;

typedef enum {
    VERTEX_TYPE_FLOAT1,
    VERTEX_TYPE_FLOAT2,
    VERTEX_TYPE_FLOAT3,
    VERTEX_TYPE_FLOAT4,
    VERTEX_TYPE_UBYTE4,
    VERTEX_TYPE_COLOR,
} VertexType;

typedef struct {
    VertexUsage usage;
    VertexType type;
    uint32_t offset;
    uint32_t size;
} VertexElement;

typedef struct {
    VertexElement elements[16];
    int numElements;
    uint32_t stride;
} VertexFormat;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;

    VertexFormat *format;
    uint32_t vertexSize;

    uint8_t currentVertex[256];
    uint32_t currentOffset;
    uint32_t currentElementMask;

    bool isFrozen;
    bool vertexStarted; 
    void* rendererData; // Backend-specific data (e.g., OpenGL buffer ID)
} VertexBuffer;

typedef struct {
    void (*init)(Renderer* renderer, DataWin* dataWin);
    void (*destroy)(Renderer* renderer);
    void (*beginFrame)(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH);
    void (*endFrameInit)(Renderer* renderer);
    void (*endFrameEnd)(Renderer* renderer);
    void (*beginView)(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle);
    void (*endView)(Renderer* renderer);
    void (*applyProjection)(Renderer* renderer, const Matrix4f* viewMatrix,const Matrix4f* projectionMatrix);
    // GUI pass: coordinates are (0,0)..(guiW,guiH) mapped to the current view's port rect. Called after endView.
    // targetSurfaceId is the surface the pass renders into, or RENDER_TARGET_HOST_FRAMEBUFFER.
    void (*beginGUI)(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId);
    // Sets the GUI-space projection matrix without rebinding the target/viewport/scissor for the current GUI pass.
    void (*setGuiProjection)(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface);
    void (*endGUI)(Renderer* renderer);
    void (*drawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
    void (*drawSpritePart)(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha);
    // Per-corner-coloured variant of drawSpritePart. Colors map to TL, TR, BR, BL (GM convention).
    void (*drawSpritePartColor)(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha);
    void (*drawSpritePos)(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha);
    void (*drawRectangle)(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline);
    void (*drawRectangleColor)(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha, bool outline);
    void (*drawLine)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha);
    void (*drawTriangle)(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline);
    void (*drawLineColor)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha);
    void (*drawText)(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation);
    void (*drawTextColor)(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation);
    void (*primitiveBegin)(Renderer* renderer, int32_t primitiveType);
    void (*primitiveBeginTexture)(Renderer* renderer, int32_t primitiveType, int32_t texture);
    void (*primitiveEnd)(Renderer* renderer);
    void (*drawVertex)(Renderer* renderer, float x, float y, float z, uint32_t color, float alpha, float u, float v);
    void (*drawVertexBuffer)(Renderer* renderer, VertexBuffer* buffer, int32_t primitive, int32_t texture, int32_t offset, int32_t count);
    void (*flush)(Renderer* renderer);
    void (*clearScreen)(Renderer* renderer, uint32_t color, float alpha);
    int32_t (*createSpriteFromSurface)(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig);
    void (*deleteSprite)(Renderer* renderer, int32_t spriteIndex);
    BlendFactors (*gpuGetBlendFactors)(Renderer* renderer);
    int32_t (*gpuGetBlendMode)(Renderer* renderer);
    void (*gpuSetBlendMode)(Renderer* renderer, int32_t mode);
    void (*gpuSetBlendModeExt)(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha);
    void (*gpuSetBlendEnable)(Renderer* renderer, bool enable);
    void (*gpuSetAlphaTestEnable)(Renderer* renderer, bool enable);
    bool (*gpuGetAlphaTestEnable)(Renderer* renderer);
    void (*gpuSetAlphaTestRef)(Renderer* renderer, uint8_t ref);
    void (*gpuSetColorWriteEnable)(Renderer* renderer, bool red, bool green, bool blue, bool alpha);
    void (*gpuGetColorWriteEnable)(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha);
    bool (*gpuGetBlendEnable)(Renderer* renderer);
    // Optional: when enabled, replaces output RGB with the fog color (preserving alpha)
    void (*gpuSetFog)(Renderer* renderer, bool enable, uint32_t color);
    // Optional: platform-specific tile rendering (nullptr = use default drawSpritePart path)
    void (*drawTile)(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY);
    void (*drawSpriteTiled)(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha);
    // Surface Functions
    int32_t (*createSurface)(Renderer* renderer, int32_t width, int32_t height);
    bool (*surfaceExists)(Renderer* renderer, int32_t surfaceID);
    // Bind the given surface as the active render target. Pass renderer->runner->applicationSurfaceId to bind the application surface.
    // implicitApplicationSurface is only valid if the surfaceID is the runner->applicationSurfaceId, it means that it was implicitly set (example: stack was empty) instead of explicitly (example: GML code using surface_set_target(application_surface))
    // The idea is that when implicitApplicationSurface is set AND the surfaceId is runner->applicationSurfaceId, you MUST restore the previousViewMatrix
    bool (*setRenderTarget)(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface);
    // Lazy allocation hook called every frame by Runner_beginFrame.
    // Creates the application_surface on the first frame (and after application_surface_enable(false) -> true cycles).
    // Resizes in place if the requested dimensions changed.
    // Returns the surface ID to use.
    int32_t (*ensureApplicationSurface)(Renderer* renderer, int32_t width, int32_t height);
    float (*getSurfaceWidth)(Renderer* renderer, int32_t surfaceID);
    float (*getSurfaceHeight)(Renderer* renderer, int32_t surfaceID);
    void (*drawSurface)(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
    void (*drawSurfaceColor)(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha);
    // Tiles the whole surface across the room (always tileX=tileY=true). Only the modern GL renderer draws; legacy/console renderers stub it.
    void (*drawSurfaceTiled)(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha);
    void (*surfaceResize)(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height);
    void (*surfaceFree)(Renderer* renderer, int32_t surfaceID);
    void (*surfaceCopy)(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part);
    bool (*surfaceGetPixels)(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA);
    // Optional: tile a source sub-rect (in tpag source-page space) across a dest rect, for nine-slice Repeat/BlankRepeat at angle 0.
    // srcX/srcY are post tpag->targetX/Y. nullptr = per-tile drawSpritePart fallback (also used for Mirror and non-zero angle).
    void (*drawTiledPart)(Renderer* renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha);
    // Shader Functions
    void (*gpuSetShader)(Renderer* renderer, int32_t shaderIndex);
    void (*gpuResetShader)(Renderer* renderer);
    int32_t (*shaderGetUniform)(Renderer* renderer, int32_t shaderIndex, char* uniform);
    int32_t (*shaderGetSamplerIndex)(Renderer* renderer, int32_t shaderIndex, char* uniform);
    void (*shaderSetUniformF)(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4);
    void (*shaderSetUniformFArray)(Renderer* renderer, int32_t handle, float* values, uint32_t count);
    void (*shaderSetUniformI)(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4);
    // Returns a texture pointer for a specific sprite, where 0 = "no texture".
    uint32_t (*spriteGetTexture)(Renderer* renderer, int32_t tpagIndex);
    // Returns a texture handle for a surface (surface_get_texture), where 0 = "no texture".
    uint32_t (*surfaceGetTexture)(Renderer* renderer, int32_t surfaceID);
    float (*textureGetTexelWidth)(Renderer* renderer, uint32_t texID);
    float (*textureGetTexelHeight)(Renderer* renderer, uint32_t texID);
    // Fills outUVs[0..3] with the texture sub-region on its page (left, top, right, bottom). Returns false if the handle can't be resolved.
    bool (*textureGetUVs)(Renderer* renderer, uint32_t texID, float* outUVs);
    void (*textureSetStage)(Renderer* renderer, int32_t slot, uint32_t texID);
    bool (*shaderIsCompiled)(Renderer* renderer, int32_t shader);
    bool (*shadersSupported)(void);
    void (*setMatrix)(Renderer* renderer, int32_t matrixType, Matrix4f matrix);
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
    int32_t circlePrecision; // segments used by draw_circle/draw_ellipse, clamped to [4, 64] and rounded down to multiple of 4. Default 24.
    //It's The Simplest Way I Found To Restore Previous Thingies For Rendering SORRY
    Matrix4f previousViewMatrix; //when you go fix the Legacy OpenGL renderer, please remove this, as we don't need this anymore I hope
    int32_t CPortX;
    int32_t CPortY;
    int32_t CPortW;
    int32_t CPortH;
    Runner* runner;
    Matrix4f gmlMatrices[MATRICES_MAX];
    int32_t currentShader;
    BlendFactors blendFactors;
    int32_t cameraCurrent;
};

// ===[ Shared Helpers (platform-agnostic) ]===

// Resolves a sprite + subimage to a TPAG index, with frame wrapping
static inline int32_t Renderer_resolveTPAGIndex(DataWin* dataWin, int32_t spriteIndex, int32_t subimg) {
    if (0 > spriteIndex || dataWin->sprt.count <= (uint32_t) spriteIndex) return -1;

    Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return -1;

    // Wrap subimage index
    int32_t frameIndex = subimg % (int32_t) sprite->textureCount;
    if (0 > frameIndex) frameIndex += (int32_t) sprite->textureCount;

    return sprite->tpagIndices[frameIndex];
}

// Forward declaration: defined further down once drawSpritePartExt is available.
static inline void Renderer_drawSpriteNineSlice(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, bool flipX, bool flipY, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha);

// Stretched: draw_sprite_stretched(sprite, subimg, x, y, w, h)
static inline void Renderer_drawSpriteStretched(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (spriteIndex >= 0 && (uint32_t) spriteIndex < dw->sprt.count && dw->sprt.sprites[spriteIndex].nineSliceEnabled) {
        Renderer_drawSpriteNineSlice(renderer, spriteIndex, subimg, x, y, w, h, false, false, 0.0f, x, y, color, alpha);
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
static inline void Renderer_drawSpriteExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float xscale, float yscale, float rot, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];

    // Nine-slice activates only when the draw scales the sprite away from its native size. At scale 1 there is nothing to slice.
    if (sprite->nineSliceEnabled && (xscale != 1.0f || yscale != 1.0f)) {
        bool flipX = 0.0f > xscale;
        bool flipY = 0.0f > yscale;
        float absX = fabsf(xscale);
        float absY = fabsf(yscale);
        float w = (float) sprite->width * absX;
        float h = (float) sprite->height * absY;
        float tlX = x - (float) sprite->originX * xscale; // signed: negative xscale shifts tlX right
        float tlY = y - (float) sprite->originY * yscale;
        Renderer_drawSpriteNineSlice(renderer, spriteIndex, subimg, tlX, tlY, w, h, flipX, flipY, rot, x, y, color, alpha);
        return;
    }

    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, (float) sprite->originX, (float) sprite->originY, xscale, yscale, rot, color, alpha);
}

// Convenience: draw_sprite(sprite, subimg, x, y)
static inline void Renderer_drawSprite(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y) {
    Renderer_drawSpriteExt(renderer, spriteIndex, subimg, x, y, 1.0f, 1.0f, 0.0f, 0xFFFFFF, renderer->drawAlpha);
}

static inline void Renderer_drawSpritePos(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    renderer->vtable->drawSpritePos(renderer, tpagIndex, x1, y1, x2, y2, x3, y3, x4, y4, alpha);
}

static inline int32_t Renderer_createSurface(Renderer* renderer, int32_t width, int32_t height) {
    //if (0 > width)  (0 > height) return;
    return renderer->vtable->createSurface(renderer, width, height);
}

static inline bool Renderer_surfaceExists(Renderer* renderer, int32_t surfaceIndex) {
    return renderer->vtable->surfaceExists(renderer, surfaceIndex);
}

static inline float Renderer_getSurfaceWidth(Renderer* renderer, int32_t surfaceIndex) {
    return renderer->vtable->getSurfaceWidth(renderer, surfaceIndex);
}

static inline float Renderer_getSurfaceHeight(Renderer* renderer, int32_t surfaceIndex) {
    return renderer->vtable->getSurfaceHeight(renderer, surfaceIndex);
}


// Draws part of a sprite with extended parameters (scale, rotation, color, alpha)
static inline void Renderer_drawSpritePartExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
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

    renderer->vtable->drawSpritePart(renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
}

// Partial draw: draw_sprite_part(sprite, subimg, left, top, width, height, x, y)
static inline void Renderer_drawSpritePart(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y) {
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, left, top, width, height, x, y, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFF, renderer->drawAlpha);
}

static inline void Renderer_primitiveBegin(Renderer* renderer, int32_t primitiveType) {
    if (renderer != nullptr && renderer->vtable != nullptr && renderer->vtable->primitiveBegin != nullptr) {
        renderer->vtable->primitiveBegin(renderer, primitiveType);
    }
}

static inline void Renderer_primitiveBeginTexture(Renderer* renderer, int32_t primitiveType, int32_t texture) {
    if (renderer != nullptr && renderer->vtable != nullptr && renderer->vtable->primitiveBeginTexture != nullptr) {
        renderer->vtable->primitiveBeginTexture(renderer, primitiveType, texture);
    }
}

static inline void Renderer_primitiveEnd(Renderer* renderer) {
    if (renderer != nullptr && renderer->vtable != nullptr && renderer->vtable->primitiveEnd != nullptr) {
        renderer->vtable->primitiveEnd(renderer);
    }
}

// Full draw: draw_sprite_general(sprite, subimg, left, top, width, height, x, y, xscale, yscale, rot, c1, c2, c3, c4, alpha).
static inline void Renderer_drawSpriteGeneral(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Clip region to TPAG bounds (same as Renderer_drawSpritePart(Ext))
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

    if (color1 == color2 && color2 == color3 && color3 == color4) {
        renderer->vtable->drawSpritePart(renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, angleDeg, x, y, color1, alpha);
        return;
    }

    renderer->vtable->drawSpritePartColor(renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, angleDeg, x, y, color1, color2, color3, color4, alpha);
}

static inline void Renderer_drawVertex(Renderer* renderer, float x, float y, float z, uint32_t color, float alpha, float u, float v) {
    if (renderer != nullptr && renderer->vtable != nullptr && renderer->vtable->drawVertex != nullptr) {
        renderer->vtable->drawVertex(renderer, x, y, z, color, alpha, u, v);
    }
}

// Resolves tpag and converts nine-slice bounding-box coords to tpag source-page space for drawTiledPart.
// Returns false if the resulting region is empty. Adjusts all in/out parameters in place.
// aX/aY/aW/aH: source coords (in, out). adX/adY/adW/adH: dest coords (in, out; pass nullptr for axes that don't change).
static inline bool Renderer_nineSliceAdjustForTiledPart(DataWin* dw, int32_t spriteIndex, int32_t subimg, int32_t* aX, int32_t* aY, int32_t* aW, int32_t* aH, float* adX, float* adY, float* adW, float* adH, int32_t* tpagIndexOut) {
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return false;
    *tpagIndexOut = tpagIndex;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    if (tpag->targetX > *aX) { int32_t off = tpag->targetX - *aX; if (adX) *adX += (float) off; if (adW) *adW -= (float) off; *aW -= off; *aX = 0; } else { *aX -= tpag->targetX; }
    if (tpag->targetY > *aY) { int32_t off = tpag->targetY - *aY; if (adY) *adY += (float) off; if (adH) *adH -= (float) off; *aH -= off; *aY = 0; } else { *aY -= tpag->targetY; }
    if (*aW > tpag->sourceWidth  - *aX) *aW = tpag->sourceWidth  - *aX;
    if (*aH > tpag->sourceHeight - *aY) *aH = tpag->sourceHeight - *aY;
    return 0 < *aW && 0 < *aH;
}

// Tiles srcW x srcH pixels from (srcX, srcY) horizontally across dstW pixels starting at (dstX, dstY).
// Mirror mode flips alternate tiles on the horizontal axis.
static inline void Renderer_nineSliceTileH(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, uint8_t mode, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    if (mode != NS_MIRROR && angleDeg == 0.0f && renderer->vtable->drawTiledPart != nullptr) {
        int32_t tpagIndex, aX = srcX, aY = srcY, aW = srcW, aH = srcH;
        float adX = dstX, adY = dstY, adW = dstW;
        if (!Renderer_nineSliceAdjustForTiledPart(renderer->dataWin, spriteIndex, subimg, &aX, &aY, &aW, &aH, &adX, &adY, &adW, nullptr, &tpagIndex) || 0.0f >= adW) return;
        renderer->vtable->drawTiledPart(renderer, tpagIndex, aX, aY, aW, aH, adX, adY, adW, (float) aH, color, alpha);
        return;
    }
    float cursor = dstX;
    float remaining = dstW;
    int32_t tileIndex = 0;
    while (remaining > 0.0f) {
        bool flipped = (mode == NS_MIRROR) && (tileIndex % 2 == 1);
        int32_t drawW = ((float) srcW > remaining) ? (int32_t) remaining : srcW;
        int32_t srcLeft = flipped ? (srcX + srcW - drawW) : srcX;
        float xs = flipped ? -1.0f : 1.0f;
        float drawX = flipped ? (cursor + (float) drawW) : cursor;
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcLeft, srcY, drawW, srcH, drawX, dstY, xs, 1.0f, angleDeg, pivotX, pivotY, color, alpha);
        cursor += (float) drawW;
        remaining -= (float) drawW;
        tileIndex++;
    }
}

// Tiles srcW x srcH pixels from (srcX, srcY) vertically across dstH pixels starting at (dstX, dstY).
// Mirror mode flips alternate tiles on the vertical axis.
static inline void Renderer_nineSliceTileV(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstH, uint8_t mode, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    if (mode != NS_MIRROR && angleDeg == 0.0f && renderer->vtable->drawTiledPart != nullptr) {
        int32_t tpagIndex, aX = srcX, aY = srcY, aW = srcW, aH = srcH;
        float adX = dstX, adY = dstY, adH = dstH;
        if (!Renderer_nineSliceAdjustForTiledPart(renderer->dataWin, spriteIndex, subimg, &aX, &aY, &aW, &aH, &adX, &adY, nullptr, &adH, &tpagIndex) || 0.0f >= adH) return;
        renderer->vtable->drawTiledPart(renderer, tpagIndex, aX, aY, aW, aH, adX, adY, (float) aW, adH, color, alpha);
        return;
    }
    float cursor = dstY;
    float remaining = dstH;
    int32_t tileIndex = 0;
    while (remaining > 0.0f) {
        bool flipped = (mode == NS_MIRROR) && (tileIndex % 2 == 1);
        int32_t drawH = ((float) srcH > remaining) ? (int32_t) remaining : srcH;
        int32_t srcTop = flipped ? (srcY + srcH - drawH) : srcY;
        float ys = flipped ? -1.0f : 1.0f;
        float drawY = flipped ? (cursor + (float) drawH) : cursor;
        Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcX, srcTop, srcW, drawH, dstX, drawY, 1.0f, ys, angleDeg, pivotX, pivotY, color, alpha);
        cursor += (float) drawH;
        remaining -= (float) drawH;
        tileIndex++;
    }
}

// Tiles a 2D region across dstW x dstH. Mirror flips alternate tiles on each axis independently.
static inline void Renderer_nineSliceTile2D(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint8_t mode, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    if (mode != NS_MIRROR && angleDeg == 0.0f && renderer->vtable->drawTiledPart != nullptr) {
        int32_t tpagIndex, aX = srcX, aY = srcY, aW = srcW, aH = srcH;
        float adX = dstX, adY = dstY, adW = dstW, adH = dstH;
        if (!Renderer_nineSliceAdjustForTiledPart(renderer->dataWin, spriteIndex, subimg, &aX, &aY, &aW, &aH, &adX, &adY, &adW, &adH, &tpagIndex) || 0.0f >= adW || 0.0f >= adH) return;
        renderer->vtable->drawTiledPart(renderer, tpagIndex, aX, aY, aW, aH, adX, adY, adW, adH, color, alpha);
        return;
    }
    float cursorY = dstY;
    float remH = dstH;
    int32_t tileRow = 0;
    while (remH > 0.0f) {
        bool flipY = (mode == NS_MIRROR) && (tileRow % 2 == 1);
        int32_t drawH = ((float) srcH > remH) ? (int32_t) remH : srcH;
        int32_t srcTop = flipY ? (srcY + srcH - drawH) : srcY;
        float ys = flipY ? -1.0f : 1.0f;
        float drawY = flipY ? (cursorY + (float) drawH) : cursorY;

        float cursorX = dstX;
        float remW = dstW;
        int32_t tileCol = 0;
        while (remW > 0.0f) {
            bool flipX = (mode == NS_MIRROR) && (tileCol % 2 == 1);
            int32_t drawW = (remW < (float) srcW) ? (int32_t) remW : srcW;
            int32_t srcLeft = flipX ? (srcX + srcW - drawW) : srcX;
            float xs = flipX ? -1.0f : 1.0f;
            float drawX = flipX ? (cursorX + (float) drawW) : cursorX;
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcLeft, srcTop, drawW, drawH, drawX, drawY, xs, ys, angleDeg, pivotX, pivotY, color, alpha);
            cursorX += (float) drawW;
            remW -= (float) drawW;
            tileCol++;
        }

        cursorY += (float) drawH;
        remH -= (float) drawH;
        tileRow++;
    }
}

static inline void Renderer_drawSpriteNineSlice(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, bool flipX, bool flipY, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
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

    uint8_t modeTop    = sprite->nsTileModes[1]; // top edge
    uint8_t modeBottom = sprite->nsTileModes[3]; // bottom edge
    uint8_t modeLeft   = sprite->nsTileModes[0]; // left edge
    uint8_t modeRight  = sprite->nsTileModes[2]; // right edge
    uint8_t modeCenter = sprite->nsTileModes[4]; // center

    // Flip remaps which source corner/edge content appears at which destination position.
    // flipX swaps left <-> right; flipY swaps top <-> bottom.
    // dstL/dstR are the dest margin widths; srcXLeft/srcXRight are the source x-offsets.
    int32_t dstL = flipX ? R : L;
    int32_t dstR = flipX ? L : R;
    int32_t dstT = flipY ? B : T;
    int32_t dstB = flipY ? T : B;
    int32_t srcXLeft  = flipX ? (sw - R) : 0;
    int32_t srcXRight = flipX ? 0 : (sw - R);
    int32_t srcYTop   = flipY ? (sh - B) : 0;
    int32_t srcYBot   = flipY ? 0 : (sh - B);

    float dstCW = w - (float) (L + R);
    float dstCH = h - (float) (T + B);
    float xsCenter = (dstCW > 0) ? dstCW / (float) srcCW : 0.0f;
    float ysCenter = (dstCH > 0) ? dstCH / (float) srcCH : 0.0f;

    // Corners: always drawn at native pixel size regardless of tile mode.
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcXLeft,  srcYTop, dstL, dstT, x,           y,           1.0f, 1.0f, angleDeg, pivotX, pivotY, color, alpha);
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcXRight, srcYTop, dstR, dstT, x + w - dstR, y,          1.0f, 1.0f, angleDeg, pivotX, pivotY, color, alpha);
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcXLeft,  srcYBot, dstL, dstB, x,           y + h - dstB, 1.0f, 1.0f, angleDeg, pivotX, pivotY, color, alpha);
    Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcXRight, srcYBot, dstR, dstB, x + w - dstR, y + h - dstB, 1.0f, 1.0f, angleDeg, pivotX, pivotY, color, alpha);

    // Top and bottom edges (horizontal variable axis). Source x is always the center strip (L..sw-R).
    if (dstCW > 0) {
        if (modeTop == NS_STRETCH) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, L, srcYTop, srcCW, dstT, x + dstL, y,           xsCenter, 1.0f, angleDeg, pivotX, pivotY, color, alpha);
        } else if (modeTop == NS_REPEAT || modeTop == NS_MIRROR || modeTop == NS_BLANKREPEAT) {
            Renderer_nineSliceTileH(renderer, spriteIndex, subimg, L, srcYTop, srcCW, dstT, x + dstL, y,           dstCW, modeTop, angleDeg, pivotX, pivotY, color, alpha);
        } // NS_HIDE: draw nothing

        if (modeBottom == NS_STRETCH) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, L, srcYBot, srcCW, dstB, x + dstL, y + h - dstB, xsCenter, 1.0f, angleDeg, pivotX, pivotY, color, alpha);
        } else if (modeBottom == NS_REPEAT || modeBottom == NS_MIRROR || modeBottom == NS_BLANKREPEAT) {
            Renderer_nineSliceTileH(renderer, spriteIndex, subimg, L, srcYBot, srcCW, dstB, x + dstL, y + h - dstB, dstCW, modeBottom, angleDeg, pivotX, pivotY, color, alpha);
        } // NS_HIDE: draw nothing
    }

    // Left and right edges (vertical variable axis). Source y is always the center strip (T..sh-B).
    if (dstCH > 0) {
        if (modeLeft == NS_STRETCH) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcXLeft,  T, dstL, srcCH, x,           y + dstT, 1.0f, ysCenter, angleDeg, pivotX, pivotY, color, alpha);
        } else if (modeLeft == NS_REPEAT || modeLeft == NS_MIRROR || modeLeft == NS_BLANKREPEAT) {
            Renderer_nineSliceTileV(renderer, spriteIndex, subimg, srcXLeft,  T, dstL, srcCH, x,           y + dstT, dstCH, modeLeft, angleDeg, pivotX, pivotY, color, alpha);
        } // NS_HIDE: draw nothing

        if (modeRight == NS_STRETCH) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, srcXRight, T, dstR, srcCH, x + w - dstR, y + dstT, 1.0f, ysCenter, angleDeg, pivotX, pivotY, color, alpha);
        } else if (modeRight == NS_REPEAT || modeRight == NS_MIRROR || modeRight == NS_BLANKREPEAT) {
            Renderer_nineSliceTileV(renderer, spriteIndex, subimg, srcXRight, T, dstR, srcCH, x + w - dstR, y + dstT, dstCH, modeRight, angleDeg, pivotX, pivotY, color, alpha);
        } // NS_HIDE: draw nothing
    }

    // Center.
    if (dstCW > 0 && dstCH > 0) {
        if (modeCenter == NS_STRETCH) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, L, T, srcCW, srcCH, x + dstL, y + dstT, xsCenter, ysCenter, angleDeg, pivotX, pivotY, color, alpha);
        } else if (modeCenter == NS_REPEAT || modeCenter == NS_MIRROR) {
            Renderer_nineSliceTile2D(renderer, spriteIndex, subimg, L, T, srcCW, srcCH, x + dstL, y + dstT, dstCW, dstCH, modeCenter, angleDeg, pivotX, pivotY, color, alpha);
        } // NS_BLANKREPEAT and NS_HIDE: draw nothing
    }
}

// Resolves a BGND index to its TPAG index.
static inline int32_t Renderer_resolveBackgroundTPAGIndex(DataWin* dataWin, int32_t bgndIndex) {
    if (0 > bgndIndex || (uint32_t) bgndIndex >= dataWin->bgnd.count) return -1;
    return dataWin->bgnd.backgrounds[bgndIndex].tpagIndex;
}

// Resolves a SPRT index to the TPAG index of its first frame.
static inline int32_t Renderer_resolveSpriteTPAGIndex(DataWin* dataWin, int32_t sprtIndex) {
    if (0 > sprtIndex || (uint32_t) sprtIndex >= dataWin->sprt.count) return -1;
    Sprite* spr = &dataWin->sprt.sprites[sprtIndex];
    if (spr->textureCount == 0) return -1;
    return spr->tpagIndices[0];
}

// Resolves a SPRT or BGND index to a TPAG index
static inline int32_t Renderer_resolveObjectTPAGIndex(DataWin* dataWin, RoomTile *tile) {
    if (!tile->useSpriteDefinition)
        return Renderer_resolveBackgroundTPAGIndex(dataWin, tile->backgroundDefinition);
    else
        return Renderer_resolveSpriteTPAGIndex(dataWin, tile->backgroundDefinition);
}

// Draws a tiled background
static inline void Renderer_drawBackgroundTiled(Renderer* renderer, int32_t tpagIndex, float bgX, float bgY, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t blend, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    renderer->vtable->drawSpriteTiled(renderer, tpagIndex, 0.0f, 0.0f, bgX, bgY, xscale, yscale, tileX, tileY, roomW, roomH, blend, alpha);
}

// Draws a tiled sprite across the room
static inline void Renderer_drawSpriteTiled(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    float originX = (float) sprite->originX;
    float originY = (float) sprite->originY;

    renderer->vtable->drawSpriteTiled(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, true, true, roomW, roomH, color, alpha);
}

// Default draw: draws instance's sprite using its image_* properties
static inline void Renderer_drawSelf(Renderer* renderer, Instance* instance) {
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
static inline void Renderer_drawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
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

    uint32_t bgr = tile->color & 0x00FFFFFF;

    renderer->vtable->drawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH, drawX, drawY, tile->scaleX, tile->scaleY, 0.0f, 0.0f, 0.0f, bgr, tile->alpha);
}

// Native runner clamps to [4, 64] and rounds down to the nearest multiple of 4.
static inline int32_t Renderer_normalizeCirclePrecision(int32_t precision) {
    if (4 > precision) precision = 4;
    if (precision > 64) precision = 64;
    return precision & 0x7C;
}

// draw_circle/draw_ellipse helper: approximates the shape as a polygon with "circlePrecision" segments.
// Filled: triangle fan from center (center = col1, perimeter = col2). Outline: line strip around the perimeter.
static inline void Renderer_drawEllipseColor(Renderer* renderer, float cx, float cy, float rx, float ry, uint32_t col1, uint32_t col2, bool outline) {
    int32_t segments = Renderer_normalizeCirclePrecision(renderer->circlePrecision);
    if (4 > segments) segments = 4;

    float step = 6.2831853f / (float) segments;
    float prevX = cx + rx;
    float prevY = cy;

    for (int32_t i = 1; segments >= i; i++) {
        float angle = step * (float) i;
        float curX = cx + rx * cosf(angle);
        float curY = cy + ry * sinf(angle);

        if (outline) {
            renderer->vtable->drawLine(renderer, prevX, prevY, curX, curY, 1.0f, col2, renderer->drawAlpha);
        } else {
            renderer->vtable->drawTriangle(renderer, cx, cy, prevX, prevY, curX, curY, col1, col2, col2, renderer->drawAlpha, false);
        }

        prevX = curX;
        prevY = curY;
    }
}

static inline void Renderer_drawCircleColor(Renderer* renderer, float cx, float cy, float radius, uint32_t col1, uint32_t col2, bool outline) {
    Renderer_drawEllipseColor(renderer, cx, cy, radius, radius, col1, col2, outline);
}

static inline void Renderer_drawCircle(Renderer* renderer, float cx, float cy, float radius, bool outline) {
    Renderer_drawCircleColor(renderer, cx, cy, radius, renderer->drawColor, renderer->drawColor, outline);
}

#define DRAW_ROUNDRECT_FIXED_RADIUS 8.0f

#define BS_RR_EMIT(prevX, prevY, curX, curY) \
    do { \
        if (outline) { \
            renderer->vtable->drawLine(renderer, prevX, prevY, curX, curY, 1.0f, col2, renderer->drawAlpha); \
        } else { \
            renderer->vtable->drawTriangle(renderer, cx, cy, prevX, prevY, curX, curY, col1, col2, col2, renderer->drawAlpha, false); \
        } \
    } while (0)

static inline void Renderer_drawRoundRectColor(Renderer* renderer, float x1, float y1, float x2, float y2, float xrad, float yrad, uint32_t col1, uint32_t col2, bool outline) {
    float halfW = (x2 - x1) * 0.5f;
    float halfH = (y2 - y1) * 0.5f;
    if (0.0f >= halfW || 0.0f >= halfH) return;

    if (xrad < 0.0f) xrad = 0.0f;
    if (yrad < 0.0f) yrad = 0.0f;
    if (xrad > halfW) xrad = halfW;
    if (yrad > halfH) yrad = halfH;

    if (!outline) {
        x2 += 1.0f;
        y2 += 1.0f;
    }

    float cx = (x1 + x2) * 0.5f;
    float cy = (y1 + y2) * 0.5f;

    float ccxTL = x1 + xrad, ccyTL = y1 + yrad;
    float ccxTR = x2 - xrad, ccyTR = y1 + yrad;
    float ccxBR = x2 - xrad, ccyBR = y2 - yrad;
    float ccxBL = x1 + xrad, ccyBL = y2 - yrad;

    int32_t quarterSeg = Renderer_normalizeCirclePrecision(renderer->circlePrecision) / 4;
    if (1 > quarterSeg) quarterSeg = 1;

    const float PI = 3.14159265358979323846f;
    const float halfPi = PI * 0.5f;

    float px = x1;
    float py = y1 + yrad;

    {
        float step = halfPi / (float) quarterSeg;
        for (int32_t i = 1; quarterSeg >= i; i++) {
            float ang = PI + step * (float) i;
            float curX = ccxTL + xrad * cosf(ang);
            float curY = ccyTL + yrad * sinf(ang);
            BS_RR_EMIT(px, py, curX, curY);
            px = curX;
            py = curY;
        }
    }
    {
        float curX = x2 - xrad, curY = y1;
        BS_RR_EMIT(px, py, curX, curY);
        px = curX;
        py = curY;
    }
    {
        float step = halfPi / (float) quarterSeg;
        for (int32_t i = 1; quarterSeg >= i; i++) {
            float ang = 3.0f * halfPi + step * (float) i;
            float curX = ccxTR + xrad * cosf(ang);
            float curY = ccyTR + yrad * sinf(ang);
            BS_RR_EMIT(px, py, curX, curY);
            px = curX;
            py = curY;
        }
    }
    {
        float curX = x2, curY = y2 - yrad;
        BS_RR_EMIT(px, py, curX, curY);
        px = curX;
        py = curY;
    }
    {
        float step = halfPi / (float) quarterSeg;
        for (int32_t i = 1; quarterSeg >= i; i++) {
            float ang = step * (float) i;
            float curX = ccxBR + xrad * cosf(ang);
            float curY = ccyBR + yrad * sinf(ang);
            BS_RR_EMIT(px, py, curX, curY);
            px = curX;
            py = curY;
        }
    }
    {
        float curX = x1 + xrad, curY = y2;
        BS_RR_EMIT(px, py, curX, curY);
        px = curX;
        py = curY;
    }
    {
        float step = halfPi / (float) quarterSeg;
        for (int32_t i = 1; quarterSeg >= i; i++) {
            float ang = halfPi + step * (float) i;
            float curX = ccxBL + xrad * cosf(ang);
            float curY = ccyBL + yrad * sinf(ang);
            BS_RR_EMIT(px, py, curX, curY);
            px = curX;
            py = curY;
        }
    }
    {
        float curX = x1, curY = y1 + yrad;
        if (px != curX || py != curY) {
            BS_RR_EMIT(px, py, curX, curY);
        }
    }
}

#undef BS_RR_EMIT

static inline void Renderer_drawRoundRect(Renderer* renderer, float x1, float y1, float x2, float y2, float xrad, float yrad, bool outline) {
    Renderer_drawRoundRectColor(renderer, x1, y1, x2, y2, xrad, yrad, renderer->drawColor, renderer->drawColor, outline);
}

#endif /* _BS_RENDERER_H_ */
