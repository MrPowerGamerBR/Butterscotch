#include "noop_renderer.h"

#include "common.h"
#include "renderer.h"
#include "runner.h"
#include "data_win.h"
#include "utils.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

// ===[ NoopRenderer Struct ]===

typedef struct {
    Renderer base; // Must be first field for struct embedding

    // Minimal surface bookkeeping so surface_exists / get_width etc. behave sanely
    int32_t *surfaceWidths;
    int32_t *surfaceHeights;
    bool *surfaceExistsFlag;
    uint32_t surfaceCount;
    uint32_t surfaceCapacity;

    // GPU state shadows (returned by getters, mutated by setters)
    bool blendEnable;
    int32_t blendMode;
    BlendFactors blendFactors;
    bool alphaTestEnable;
    uint8_t alphaTestRef;
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool fogEnable;
    uint32_t fogColor;
} NoopRenderer;

// Helpers
static void noopEnsureSurfaceCapacity(NoopRenderer *noop, uint32_t needed) {
    if (needed <= noop->surfaceCapacity) return;
    uint32_t newCap = noop->surfaceCapacity ? noop->surfaceCapacity * 2 : 16;
    while (newCap < needed) newCap *= 2;
    noop->surfaceWidths = (int32_t *)safeRealloc(noop->surfaceWidths, newCap * sizeof(int32_t));
    noop->surfaceHeights = (int32_t *)safeRealloc(noop->surfaceHeights, newCap * sizeof(int32_t));
    noop->surfaceExistsFlag = (bool *)safeRealloc(noop->surfaceExistsFlag, newCap * sizeof(bool));
    for (uint32_t i = noop->surfaceCapacity; i < newCap; i++) {
        noop->surfaceWidths[i] = 0;
        noop->surfaceHeights[i] = 0;
        noop->surfaceExistsFlag[i] = false;
    }
    noop->surfaceCapacity = newCap;
    if (needed > noop->surfaceCount) noop->surfaceCount = needed;
}

// ===[ Vtable stubs ]===

static void noopInit(Renderer *renderer, DataWin *dataWin) {
    renderer->dataWin = dataWin;
    Matrix4f world;
    Matrix4f_identity(&world);
    renderer->gmlMatrices[MATRIX_WORLD] = world;
    logInfo("No-op renderer initialized\n");
}

static void noopDestroy(Renderer *renderer) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    free(noop->surfaceWidths);
    free(noop->surfaceHeights);
    free(noop->surfaceExistsFlag);
    free(noop);
}

static void noopBeginFrame(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t gameW, MAYBE_UNUSED int32_t gameH, MAYBE_UNUSED int32_t windowW, MAYBE_UNUSED int32_t windowH) {}
static void noopEndFrameInit(MAYBE_UNUSED Renderer *renderer) {}
static void noopEndFrameEnd(MAYBE_UNUSED Renderer *renderer) {}
static void noopBeginView(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t viewX, MAYBE_UNUSED int32_t viewY, MAYBE_UNUSED int32_t viewW, MAYBE_UNUSED int32_t viewH, MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED float viewAngle) {}
static void noopEndView(MAYBE_UNUSED Renderer *renderer) {}
static void noopApplyProjection(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED const Matrix4f *viewMatrix, MAYBE_UNUSED const Matrix4f *projectionMatrix) {}
static void noopBeginGUI(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t guiW, MAYBE_UNUSED int32_t guiH, MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED int32_t targetSurfaceId) {}
static void noopSetGuiProjection(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t guiW, MAYBE_UNUSED int32_t guiH, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED bool renderingToUserSurface) {}
static void noopEndGUI(MAYBE_UNUSED Renderer *renderer) {}

static void noopDrawSprite(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float originX, MAYBE_UNUSED float originY, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void noopDrawSpritePart(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex, MAYBE_UNUSED int32_t srcOffX, MAYBE_UNUSED int32_t srcOffY, MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED float pivotX, MAYBE_UNUSED float pivotY, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void noopDrawSpritePartColor(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex, MAYBE_UNUSED int32_t srcOffX, MAYBE_UNUSED int32_t srcOffY, MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED float pivotX, MAYBE_UNUSED float pivotY, MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED uint32_t color4, MAYBE_UNUSED float alpha) {}
static void noopDrawSpritePos(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex, MAYBE_UNUSED float x1, MAYBE_UNUSED float y1, MAYBE_UNUSED float x2, MAYBE_UNUSED float y2, MAYBE_UNUSED float x3, MAYBE_UNUSED float y3, MAYBE_UNUSED float x4, MAYBE_UNUSED float y4, MAYBE_UNUSED float alpha) {}
static void noopDrawRectangle(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED float x1, MAYBE_UNUSED float y1, MAYBE_UNUSED float x2, MAYBE_UNUSED float y2, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {}
static void noopDrawRectangleColor(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED float x1, MAYBE_UNUSED float y1, MAYBE_UNUSED float x2, MAYBE_UNUSED float y2, MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED uint32_t color4, MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {}
static void noopDrawLine(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED float x1, MAYBE_UNUSED float y1, MAYBE_UNUSED float x2, MAYBE_UNUSED float y2, MAYBE_UNUSED float width, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void noopDrawLineColor(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED float x1, MAYBE_UNUSED float y1, MAYBE_UNUSED float x2, MAYBE_UNUSED float y2, MAYBE_UNUSED float width, MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED float alpha) {}
static void noopDrawTriangle(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED float x1, MAYBE_UNUSED float y1, MAYBE_UNUSED float x2, MAYBE_UNUSED float y2, MAYBE_UNUSED float x3, MAYBE_UNUSED float y3, MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {}
static void noopDrawText(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED const char *text, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED float lineSeparation) {}
static void noopDrawTextColor(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED const char *text, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED int32_t c1, MAYBE_UNUSED int32_t c2, MAYBE_UNUSED int32_t c3, MAYBE_UNUSED int32_t c4, MAYBE_UNUSED float alpha, MAYBE_UNUSED float lineSeparation) {}
static void noopFlush(MAYBE_UNUSED Renderer *renderer) {}
static void noopClearScreen(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}

static int32_t noopCreateSpriteFromSurface(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y, MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h, MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth, MAYBE_UNUSED int32_t xorig, MAYBE_UNUSED int32_t yorig) {
    return -1;
}
static void noopDeleteSprite(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t spriteIndex) {}

static BlendFactors noopGpuGetBlendFactors(Renderer *renderer) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    return noop->blendFactors;
}
static int32_t noopGpuGetBlendMode(Renderer *renderer) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    return noop->blendMode;
}
static void noopGpuSetBlendMode(Renderer *renderer, int32_t mode) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    noop->blendMode = mode;
}
static void noopGpuSetBlendModeExt(Renderer *renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    noop->blendFactors.src = sfactor;
    noop->blendFactors.dst = dfactor;
    noop->blendFactors.srcAlpha = sfactor_alpha;
    noop->blendFactors.dstAlpha = dfactor_alpha;
}
static void noopGpuSetBlendEnable(Renderer *renderer, bool enable) {
    ((NoopRenderer *)renderer)->blendEnable = enable;
}
static void noopGpuSetAlphaTestEnable(Renderer *renderer, bool enable) {
    ((NoopRenderer *)renderer)->alphaTestEnable = enable;
}
static bool noopGpuGetAlphaTestEnable(Renderer *renderer) {
    return ((NoopRenderer *)renderer)->alphaTestEnable;
}
static void noopGpuSetAlphaTestRef(Renderer *renderer, uint8_t ref) {
    ((NoopRenderer *)renderer)->alphaTestRef = ref;
}
static void noopGpuSetColorWriteEnable(Renderer *renderer, bool red, bool green, bool blue, bool alpha) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    noop->colorWriteR = red;
    noop->colorWriteG = green;
    noop->colorWriteB = blue;
    noop->colorWriteA = alpha;
}
static void noopGpuGetColorWriteEnable(Renderer *renderer, bool *red, bool *green, bool *blue, bool *alpha) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    if (red) *red = noop->colorWriteR;
    if (green) *green = noop->colorWriteG;
    if (blue) *blue = noop->colorWriteB;
    if (alpha) *alpha = noop->colorWriteA;
}
static bool noopGpuGetBlendEnable(Renderer *renderer) {
    return ((NoopRenderer *)renderer)->blendEnable;
}
static void noopGpuSetFog(Renderer *renderer, bool enable, uint32_t color) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    noop->fogEnable = enable;
    noop->fogColor = color;
}
static void noopDrawTile(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED RoomTile *tile, MAYBE_UNUSED float offsetX, MAYBE_UNUSED float offsetY) {}
static void noopDrawSpriteTiled(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex, MAYBE_UNUSED float originX, MAYBE_UNUSED float originY, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED bool tileX, MAYBE_UNUSED bool tileY, MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}

static int32_t noopCreateSurface(Renderer *renderer, int32_t width, int32_t height) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    // Find free slot
    for (uint32_t i = 0; i < noop->surfaceCount; i++) {
        if (!noop->surfaceExistsFlag[i]) {
            noop->surfaceWidths[i] = width;
            noop->surfaceHeights[i] = height;
            noop->surfaceExistsFlag[i] = true;
            return (int32_t)i;
        }
    }
    uint32_t id = noop->surfaceCount;
    noopEnsureSurfaceCapacity(noop, id + 1);
    noop->surfaceWidths[id] = width;
    noop->surfaceHeights[id] = height;
    noop->surfaceExistsFlag[id] = true;
    return (int32_t)id;
}
static bool noopSurfaceExists(Renderer *renderer, int32_t surfaceID) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= noop->surfaceCount) return false;
    return noop->surfaceExistsFlag[surfaceID];
}
static bool noopSetRenderTarget(MAYBE_UNUSED Renderer *renderer, int32_t surfaceID, MAYBE_UNUSED bool implicitApplicationSurface) {
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) return true;
    return noopSurfaceExists(renderer, surfaceID);
}
static int32_t noopEnsureApplicationSurface(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {
    return APPLICATION_SURFACE_ID;
}
static float noopGetSurfaceWidth(Renderer *renderer, int32_t surfaceID) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= noop->surfaceCount) return 0.0f;
    if (!noop->surfaceExistsFlag[surfaceID]) return 0.0f;
    return (float)noop->surfaceWidths[surfaceID];
}
static float noopGetSurfaceHeight(Renderer *renderer, int32_t surfaceID) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= noop->surfaceCount) return 0.0f;
    if (!noop->surfaceExistsFlag[surfaceID]) return 0.0f;
    return (float)noop->surfaceHeights[surfaceID];
}
static void noopDrawSurface(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED int32_t srcLeft, MAYBE_UNUSED int32_t srcTop, MAYBE_UNUSED int32_t srcWidth, MAYBE_UNUSED int32_t srcHeight, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void noopDrawSurfaceColor(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED int32_t srcLeft, MAYBE_UNUSED int32_t srcTop, MAYBE_UNUSED int32_t srcWidth, MAYBE_UNUSED int32_t srcHeight, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED uint32_t color4, MAYBE_UNUSED float alpha) {}
static void noopDrawSurfaceTiled(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void noopSurfaceResize(Renderer *renderer, int32_t surfaceID, int32_t width, int32_t height) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= noop->surfaceCount) return;
    if (!noop->surfaceExistsFlag[surfaceID]) return;
    noop->surfaceWidths[surfaceID] = width;
    noop->surfaceHeights[surfaceID] = height;
}
static void noopSurfaceFree(Renderer *renderer, int32_t surfaceID) {
    NoopRenderer *noop = (NoopRenderer *)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= noop->surfaceCount) return;
    noop->surfaceExistsFlag[surfaceID] = false;
    noop->surfaceWidths[surfaceID] = 0;
    noop->surfaceHeights[surfaceID] = 0;
}
static void noopSurfaceCopy(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t destSurfaceID, MAYBE_UNUSED int32_t destX, MAYBE_UNUSED int32_t destY, MAYBE_UNUSED int32_t srcSurfaceID, MAYBE_UNUSED int32_t srcX, MAYBE_UNUSED int32_t srcY, MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH, MAYBE_UNUSED bool part) {}
static bool noopSurfaceGetPixels(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED uint8_t *outRGBA) {
    return false;
}
static void noopDrawTiledPart(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex, MAYBE_UNUSED int32_t srcX, MAYBE_UNUSED int32_t srcY, MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH, MAYBE_UNUSED float dstX, MAYBE_UNUSED float dstY, MAYBE_UNUSED float dstW, MAYBE_UNUSED float dstH, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}

static void noopGpuSetShader(Renderer *renderer, int32_t shaderIndex) {
    renderer->currentShader = shaderIndex;
}
static void noopGpuResetShader(Renderer *renderer) {
    renderer->currentShader = -1;
}
static int32_t noopShaderGetUniform(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char *uniform) {
    return -1;
}
static int32_t noopShaderGetSamplerIndex(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char *uniform) {
    return -1;
}
static void noopShaderSetUniformF(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count, MAYBE_UNUSED float v1, MAYBE_UNUSED float v2, MAYBE_UNUSED float v3, MAYBE_UNUSED float v4) {}
static void noopShaderSetUniformFArray(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED float *values, MAYBE_UNUSED uint32_t count) {}
static void noopShaderSetUniformI(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count, MAYBE_UNUSED int32_t v1, MAYBE_UNUSED int32_t v2, MAYBE_UNUSED int32_t v3, MAYBE_UNUSED int32_t v4) {}
static uint32_t noopSpriteGetTexture(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t tpagIndex) {
    return 0;
}
static uint32_t noopSurfaceGetTexture(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID) {
    return 0;
}
static float noopTextureGetTexelWidth(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t texID) {
    return 1.0f;
}
static float noopTextureGetTexelHeight(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t texID) {
    return 1.0f;
}
static bool noopTextureGetUVs(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t texID, MAYBE_UNUSED float *outUVs) {
    return false;
}
static void noopTextureSetStage(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t slot, MAYBE_UNUSED uint32_t texID) {}
static bool noopShaderIsCompiled(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shader) {
    return false;
}
static bool noopShadersSupported(void) {
    return false;
}
static void noopSetMatrix(Renderer *renderer, int32_t matrixType, Matrix4f matrix) {
    if (matrixType >= 0 && matrixType < MATRICES_MAX) renderer->gmlMatrices[matrixType] = matrix;
}

static RendererVtable noopVtable;

Renderer* NoopRenderer_create(void) {
    NoopRenderer *noop = (NoopRenderer *)safeCalloc(1, sizeof(NoopRenderer));
    noop->base.vtable = &noopVtable;
    noopVtable.init = noopInit;
    noopVtable.destroy = noopDestroy;
    noopVtable.beginFrame = noopBeginFrame;
    noopVtable.endFrameInit = noopEndFrameInit;
    noopVtable.endFrameEnd = noopEndFrameEnd;
    noopVtable.beginView = noopBeginView;
    noopVtable.endView = noopEndView;
    noopVtable.applyProjection = noopApplyProjection;
    noopVtable.beginGUI = noopBeginGUI;
    noopVtable.setGuiProjection = noopSetGuiProjection;
    noopVtable.endGUI = noopEndGUI;
    noopVtable.drawSprite = noopDrawSprite;
    noopVtable.drawSpritePart = noopDrawSpritePart;
    noopVtable.drawSpritePartColor = noopDrawSpritePartColor;
    noopVtable.drawSpritePos = noopDrawSpritePos;
    noopVtable.drawRectangle = noopDrawRectangle;
    noopVtable.drawRectangleColor = noopDrawRectangleColor;
    noopVtable.drawLine = noopDrawLine;
    noopVtable.drawTriangle = noopDrawTriangle;
    noopVtable.drawLineColor = noopDrawLineColor;
    noopVtable.drawText = noopDrawText;
    noopVtable.drawTextColor = noopDrawTextColor;
    noopVtable.flush = noopFlush;
    noopVtable.clearScreen = noopClearScreen;
    noopVtable.createSpriteFromSurface = noopCreateSpriteFromSurface;
    noopVtable.deleteSprite = noopDeleteSprite;
    noopVtable.gpuGetBlendFactors = noopGpuGetBlendFactors;
    noopVtable.gpuGetBlendMode = noopGpuGetBlendMode;
    noopVtable.gpuSetBlendMode = noopGpuSetBlendMode;
    noopVtable.gpuSetBlendModeExt = noopGpuSetBlendModeExt;
    noopVtable.gpuSetBlendEnable = noopGpuSetBlendEnable;
    noopVtable.gpuSetAlphaTestEnable = noopGpuSetAlphaTestEnable;
    noopVtable.gpuGetAlphaTestEnable = noopGpuGetAlphaTestEnable;
    noopVtable.gpuSetAlphaTestRef = noopGpuSetAlphaTestRef;
    noopVtable.gpuSetColorWriteEnable = noopGpuSetColorWriteEnable;
    noopVtable.gpuGetColorWriteEnable = noopGpuGetColorWriteEnable;
    noopVtable.gpuGetBlendEnable = noopGpuGetBlendEnable;
    noopVtable.gpuSetFog = noopGpuSetFog;
    noopVtable.drawTile = noopDrawTile;
    noopVtable.drawSpriteTiled = noopDrawSpriteTiled;
    noopVtable.createSurface = noopCreateSurface;
    noopVtable.surfaceExists = noopSurfaceExists;
    noopVtable.setRenderTarget = noopSetRenderTarget;
    noopVtable.ensureApplicationSurface = noopEnsureApplicationSurface;
    noopVtable.getSurfaceWidth = noopGetSurfaceWidth;
    noopVtable.getSurfaceHeight = noopGetSurfaceHeight;
    noopVtable.drawSurface = noopDrawSurface;
    noopVtable.drawSurfaceColor = noopDrawSurfaceColor;
    noopVtable.drawSurfaceTiled = noopDrawSurfaceTiled;
    noopVtable.surfaceResize = noopSurfaceResize;
    noopVtable.surfaceFree = noopSurfaceFree;
    noopVtable.surfaceCopy = noopSurfaceCopy;
    noopVtable.surfaceGetPixels = noopSurfaceGetPixels;
    noopVtable.drawTiledPart = noopDrawTiledPart;
    noopVtable.gpuSetShader = noopGpuSetShader;
    noopVtable.gpuResetShader = noopGpuResetShader;
    noopVtable.shaderGetUniform = noopShaderGetUniform;
    noopVtable.shaderGetSamplerIndex = noopShaderGetSamplerIndex;
    noopVtable.shaderSetUniformF = noopShaderSetUniformF;
    noopVtable.shaderSetUniformFArray = noopShaderSetUniformFArray;
    noopVtable.shaderSetUniformI = noopShaderSetUniformI;
    noopVtable.spriteGetTexture = noopSpriteGetTexture;
    noopVtable.surfaceGetTexture = noopSurfaceGetTexture;
    noopVtable.textureGetTexelWidth = noopTextureGetTexelWidth;
    noopVtable.textureGetTexelHeight = noopTextureGetTexelHeight;
    noopVtable.textureGetUVs = noopTextureGetUVs;
    noopVtable.textureSetStage = noopTextureSetStage;
    noopVtable.shaderIsCompiled = noopShaderIsCompiled;
    noopVtable.shadersSupported = noopShadersSupported;
    noopVtable.setMatrix = noopSetMatrix;
    noop->base.drawColor = 0xFFFFFF;
    noop->base.drawAlpha = 1.0f;
    noop->base.drawFont = -1;
    noop->base.drawHalign = 0;
    noop->base.drawValign = 0;
    noop->base.circlePrecision = 24;
    noop->base.currentShader = -1;
    Matrix4f_identity(&noop->base.gmlMatrices[MATRIX_WORLD]);
    noop->blendEnable = true;
    noop->blendMode = bm_normal;
    noop->blendFactors.src = bm_src_alpha;
    noop->blendFactors.dst = bm_inv_src_alpha;
    noop->blendFactors.srcAlpha = bm_src_alpha;
    noop->blendFactors.dstAlpha = bm_inv_src_alpha;
    noop->alphaTestEnable = false;
    noop->alphaTestRef = 0;
    noop->colorWriteR = noop->colorWriteG = noop->colorWriteB = noop->colorWriteA = true;
    noop->fogEnable = false;
    noop->fogColor = 0;
    noop->base.currentShader = -1;
    return (Renderer *)noop;
}
