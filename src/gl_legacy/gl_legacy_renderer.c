#include "gl_legacy_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "gl_wrappers.h"


#ifdef PLATFORM_PS3
#include "ps3gl.h"
#include "rsxutil.h"
#include "ps3_textures.h"
extern GLuint gPalettedProgram;
extern GLint  gPalettedUPaletteVLoc;
// Activate the paletted shader for a sprite draw. The caller has already bound the index texture (via glBindTexture on TEXUNIT0).
// Sets unit 1 to the CLUT atlas and pushes uPaletteV for the TPAG's row.
#define PS3_PALETTED_BEGIN(tpagIndex) do {                                                  \
    float _v = PS3Textures_getTpagPaletteV(tpagIndex);                                      \
    if (0.0f > _v) break;                                                                   \
    glActiveTexture(GL_TEXTURE1);                                                           \
    glBindTexture(GL_TEXTURE_2D, PS3Textures_getClutTexture());                             \
    glEnable(GL_TEXTURE_2D);                                                                \
    glActiveTexture(GL_TEXTURE0);                                                           \
    glUseProgram(gPalettedProgram);                                                        \
    if (gPalettedUPaletteVLoc >= 0) glUniform1f(gPalettedUPaletteVLoc, _v);               \
} while (0)
#define PS3_PALETTED_END() do {                                                             \
    glUseProgram(0);                                                                        \
    glActiveTexture(GL_TEXTURE1);                                                           \
    glDisable(GL_TEXTURE_2D);                                                               \
    glActiveTexture(GL_TEXTURE0);                                                           \
} while (0)
#elif PLATFORM_VITA
#include <vitaGL.h>
#include "vita_textures.h"
#define PS3_PALETTED_BEGIN(tpagIndex) ((void)0)
#define PS3_PALETTED_END()            ((void)0)
#else
#include <glad/glad.h>
#define PS3_PALETTED_BEGIN(tpagIndex) ((void)0)
#define PS3_PALETTED_END()            ((void)0)
#endif
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "math_compat.h"

// Next power-of-two, used for FBO texture dimensions on older GPUs (Intel 82865G etc.)
// that cannot attach NPOT textures to framebuffer objects.
static inline int32_t nextPow2(int32_t v) {
    int32_t r = 1;
    while (r < v) r <<= 1;
    return r;
}

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"
#include "image_decoder.h"
#include "gl_common.h"

// ===[ Runtime OpenGL extension checks ]===

// Checks whether an OpenGL extension is available. Uses the modern
// (glGetStringi + GL_NUM_EXTENSIONS) path when glGetStringi is non-null
// (GL 3.0+), otherwise falls back to the legacy glGetString(GL_EXTENSIONS)
// approach so the code works with any GL loader (glad, PS3, etc.).
#if !defined(PLATFORM_PS3) && !defined(PLATFORM_VITA)
static bool hasGLExtension(const char* name) {
    if (glGetStringi) {
        GLint numExts = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExts);
        for (GLint i = 0; i < numExts; i++) {
            const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (ext && strcmp(ext, name) == 0)
                return true;
        }
        return false;
    }
    const char* extStr = (const char*)glGetString(GL_EXTENSIONS);
    if (!extStr) return false;
    size_t len = strlen(name);
    for (const char* p = extStr; (p = strstr(p, name)) != NULL; p++) {
        if ((p == extStr || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0'))
            return true;
    }
    return false;
}
#endif

static bool hasFBO() {
#if defined(PLATFORM_PS3) || defined(PLATFORM_VITA)
    return true;
#else
    return (glGenFramebuffers && glBlitFramebuffer);
#endif
}

// ===[ Helpers ]===

static void glApplyViewport(GLLegacyRenderer* gl, int32_t x, int32_t y, int32_t w, int32_t h) {
    glViewport(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);

    gl->base.CPortX = x;
    gl->base.CPortY = y;
    gl->base.CPortW = w;
    gl->base.CPortH = h;
}

// camera_apply: swap the active world->clip projection on the current target without touching its viewport.
static void glApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix) {

    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = *viewMatrix;
    Matrix4f projection = *projectionMatrix;

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);

    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_VIEW] = view;
    renderer->gmlMatrices[MATRIX_PROJECTION] = projection;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;

#ifndef PLATFORM_PS3
    Matrix4f_flipClipY(&projection);
#endif

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(worldView.m);
}

// ===[ Vtable Implementations ]===

static void glInit(Renderer* renderer, DataWin* dataWin) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    renderer->dataWin = dataWin;

    Matrix4f world;
    Matrix4f_identity(&world);
    renderer->gmlMatrices[MATRIX_WORLD] = world;

#if !defined(PLATFORM_PS3) && !defined(PLATFORM_VITA)
    gl_init_wrappers();
#endif

    if (!hasFBO()) {
        logError("GL: The legacy-gl renderer requires FBO support!\n");
        abort();
    }

    // GL 2.0+ has NPOT textures as core; older GL (1.x) may or may not have
    // GL_ARB_texture_non_power_of_two. Only round up to power-of-two on GPUs
    // that actually need it (Intel 82865G etc.).
    {
#if defined(PLATFORM_PS3) || defined(PLATFORM_VITA)
        gl->needsPOT = false;
#else
        GLVer ver = GLCommon_getGLVersion();
        gl->needsPOT = (ver.major < 2) && !hasGLExtension("GL_ARB_texture_non_power_of_two");
#endif
    }

    // Prepare texture slots for lazy loading (PNG decode deferred to first use)
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

#ifdef PLATFORM_PS3
    // TXTR is empty on PS3; page count comes from TEXTURES.BIN.
    gl->textureCount = PS3Textures_getPageCount();
#elif defined(PLATFORM_VITA)
    if (VitaTextures_Active())
        gl->textureCount = VitaTextures_GetPageCount();
    else
        gl->textureCount = dataWin->txtr.count;
#else
    gl->textureCount = dataWin->txtr.count;
#endif
    gl->glTextures = (GLuint *)safeMalloc(gl->textureCount * sizeof(GLuint));
    gl->textureWidths = (int32_t *)safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureHeights = (int32_t *)safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = (bool *)safeMalloc(gl->textureCount * sizeof(bool));

    glGenTextures((GLsizei) gl->textureCount, gl->glTextures);

    for (uint32_t i = 0; gl->textureCount > i; i++) {
        gl->textureWidths[i] = 0;
        gl->textureHeights[i] = 0;
        gl->textureLoaded[i] = false;
    }

    // Create 1x1 white pixel texture for primitive drawing (rectangles, lines, etc.)
    glGenTextures(1, &gl->whiteTexture);
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D, 0);

    // Save original counts so we know which slots are from data.win vs dynamic
    gl->originalTexturePageCount = gl->textureCount;
    gl->originalTpagCount = dataWin->tpag.count;
    gl->originalSpriteCount = dataWin->sprt.count;

    // application_surface is allocated lazily by glLegacyEnsureApplicationSurface as a normal entry in the surface table.
    gl->surfaces = nullptr;
    gl->surfaceTexture = nullptr;
    gl->surfaceWidth = nullptr;
    gl->surfaceHeight = nullptr;
    gl->surfaceCount = 0;

    logInfo("GL: Renderer initialized (%u texture pages)\n", gl->textureCount);
}

static void glDestroy(Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glDeleteTextures(1, &gl->whiteTexture);

    glDeleteTextures((GLsizei) gl->textureCount, gl->glTextures);

    for (uint32_t i = 0; gl->surfaceCount > i; i++) {
        if (gl->surfaceTexture[i] != 0) glDeleteTextures(1, &gl->surfaceTexture[i]);
        if (gl->surfaces[i] != 0) glDeleteFramebuffers(1, &gl->surfaces[i]);
    }
    free(gl->surfaces);
    free(gl->surfaceTexture);
    free(gl->surfaceWidth);
    free(gl->surfaceHeight);

    free(gl->glTextures);
    free(gl->textureWidths);
    free(gl->textureHeights);
    free(gl);
}

static void glBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    gl->windowW = windowW;
    gl->windowH = windowH;
    gl->gameW = gameW;
    gl->gameH = gameH;

    // Bind the application_surface (sized/created by Runner_beginFrame's ensureApplicationSurface call right before this).
    int32_t appId = gl->base.runner->applicationSurfaceId;
    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[appId]);
    glViewport(0, 0, gameW, gameH);
    gl->base.CPortX = 0;
    gl->base.CPortY = 0;
    gl->base.CPortW = gameW;
    gl->base.CPortH = gameH;
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void glBeginView(Renderer* renderer, MAYBE_UNUSED int32_t viewX, MAYBE_UNUSED int32_t viewY, MAYBE_UNUSED int32_t viewW, MAYBE_UNUSED int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, MAYBE_UNUSED float viewAngle) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glBindTexture(GL_TEXTURE_2D, 0);

    // Set viewport and scissor to the port rectangle within the FBO
    // FBO uses game resolution, port coordinates are in game space
    // OpenGL viewport Y is bottom-up, game Y is top-down
    glApplyViewport(gl, portX, portY, portW, portH);

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
    viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = &renderer->runner->views[viewCurrent];
    gl->base.cameraCurrent = view->cameraId;
    GMLCamera* camera = Runner_getCameraById(renderer->runner, gl->base.cameraCurrent);
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);

    glActiveTexture(GL_TEXTURE0);

}

static void glEndView(MAYBE_UNUSED Renderer* renderer) {
    glDisable(GL_SCISSOR_TEST);
}

static void glBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glBindTexture(GL_TEXTURE_2D, 0);

    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, portW, portH);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, portW, portH);
    } else {
        require(targetSurfaceId >= 0 && (uint32_t) targetSurfaceId < gl->surfaceCount);
        require(gl->surfaces[targetSurfaceId] != 0);
        glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[targetSurfaceId]);
        glApplyViewport(gl, portX, portY, portW, portH);
    }

    //I dunno hopefully this is at least somewhat correct...
    gl->base.cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    Matrix4f projectionMatrix;
    Matrix4f_Orthographic(&projectionMatrix, (float) guiW, (float) guiH, 32000.0, 0.0);

    Matrix4f viewMatrix;
    float x = (float) guiW * 0.5f;
    float y = (float) guiH * 0.5f;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_LookAt(&viewMatrix, x, y, -16000.0, x, y, 16000.0, 0.0, 1.0, 0.0);
    camera->viewMatrix = viewMatrix;
    camera->projectionMatrix = projectionMatrix;
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);

    glActiveTexture(GL_TEXTURE0);
}

static void glSetGuiProjection(MAYBE_UNUSED Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface) {
    Matrix4f projection;
    Matrix4f_guiProjection(&projection, (float) guiW, (float) guiH, (float) portW, (float) portH);
    // GL surfaces are stored bottom-up and draw_surface samples them with vertical flip.

    renderer->cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    //yeah no I have no idea how to do the GUI
    Matrix4f projectionMatrix;
    Matrix4f_Orthographic(&projectionMatrix, (float) guiW, (float) guiH, 32000.0, 0.0);
    // Flip the projection when we are rendering to a user surface so it comes back upright.
    if (renderingToUserSurface) Matrix4f_flipClipY(&projectionMatrix);
    Matrix4f viewMatrix;
    float x = (float) guiW * 0.5f;
    float y = (float) guiH * 0.5f;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_LookAt(&viewMatrix, x, y, -16000.0, x, y, 16000.0, 0.0, 1.0, 0.0);
    camera->viewMatrix = viewMatrix;
    camera->projectionMatrix = projectionMatrix;
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
}

static void glEndGUI(MAYBE_UNUSED Renderer* renderer) {
    glDisable(GL_SCISSOR_TEST);
}

static void glEndFrameInit(Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    int32_t appId = gl->base.runner->applicationSurfaceId;
    GLCommon_beginLetterboxBlit(gl->surfaces[appId], 0);
}

static void glEndFrameEnd(Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        return;
    }
    int32_t appId = gl->base.runner->applicationSurfaceId;
    GLCommon_beginLetterboxBlit(gl->surfaces[appId], 0);
    GLCommon_endLetterboxBlit(gl->surfaceWidth[appId], gl->surfaceHeight[appId], gl->gameW, gl->gameH, gl->windowW, gl->windowH, 0);
}

static void glRendererFlush(MAYBE_UNUSED Renderer* renderer) {}

static void glClearScreen(MAYBE_UNUSED Renderer* renderer, uint32_t color, float alpha) {
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // GML draw_clear ignores the active scissor and clears the whole target. Disable scissor for the clear and restore it after.

    glClearColor(r, g, b, alpha);
    glClear(GL_COLOR_BUFFER_BIT);

}

// Lazily decodes and uploads a TXTR page on first access.
// Returns true if the texture is ready, false if it failed to decode.
bool GLLegacyRenderer_ensureTextureLoaded(GLLegacyRenderer* gl, uint32_t pageId) {
    if (gl->textureLoaded[pageId]) return (gl->textureWidths[pageId] != 0);

    gl->textureLoaded[pageId] = true;

    int w, h;
    uint8_t* pixels = nullptr;
#ifdef PLATFORM_PS3
    // We'll load the textures on demand.
    if (!PS3Textures_loadPage(pageId, &w, &h, &pixels)) {
        logWarn("GL: PS3 page %u has no pixels\n", pageId);
        return false;
    }
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    // Nearest is mandatory for index textures, bilinear would interpolate palette indices into nonsense colors.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    free(pixels);
#elif defined(PLATFORM_VITA)
    if (VitaTextures_Active()) {
        glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
        if (!VitaTextures_LoadPage(pageId, &gl->textureWidths[pageId], &gl->textureHeights[pageId])) {
            logError("GL: Failed to load Vita TXTR page %u", pageId);
            return false;
        }
        logInfo("GL: Loaded TXTR page %u (%dx%d)\n", pageId, gl->textureWidths[pageId], gl->textureHeights[pageId]);
        return true;
    }
#endif
    DataWin* dw = gl->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    DataWin_loadTxtrIfNeeded(dw, pageId);

    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t) txtr->blobSize, gm2022_5, &w, &h);
    if (pixels == nullptr) {
        logWarn("GL: Failed to decode TXTR page %u\n", pageId);
        return false;
    }
    if (!txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }

    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;

    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    free(pixels);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    logInfo("GL: Loaded TXTR page %u (%dx%d)\n", pageId, w, h);
    return true;
}

static void glDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

    glBindTexture(GL_TEXTURE_2D, texId);
    PS3_PALETTED_BEGIN(tpagIndex);

    // Compute normalized UVs from TPAG source rect
    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->targetWidth;
    float localY1 = localY0 + (float) tpag->targetHeight;

    // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
    // GML rotation is counter-clockwise, OpenGL rotation is counter-clockwise, but
    // since we have Y-down, we negate the angle to get the correct visual rotation
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    // Transform 4 corners
    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0, &y0); // top-left
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1, &y1); // top-right
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2, &y2); // bottom-right
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3, &y3); // bottom-left

    // Convert BGR color to RGB floats
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    glBegin(GL_QUADS);
        // Vertex 0: top-left
        glColor4f(r, g, b, alpha);
        glTexCoord2f(u0, v0);
        glVertex2f(x0, y0);

        // Vertex 1: top-right
        glColor4f(r, g, b, alpha);
        glTexCoord2f(u1, v0);
        glVertex2f(x1, y1);

        // Vertex 2: bottom-right
        glColor4f(r, g, b, alpha);
        glTexCoord2f(u1, v1);
        glVertex2f(x2, y2);

        // Vertex 3: bottom-left
        glColor4f(r, g, b, alpha);
        glTexCoord2f(u0, v1);
        glVertex2f(x3, y3);
    glEnd();
    PS3_PALETTED_END();
}

static void glDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

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

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->targetWidth;
    float localY1 = localY0 + (float) tpag->targetHeight;
    float sx0 = xscale * localX0;
    float sy0 = yscale * localY0;
    float sx1 = xscale * localX1;
    float sy1 = yscale * localY1;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Emit the entire tile grid in a single glBegin -> glEnd
    glBindTexture(GL_TEXTURE_2D, texId);
    PS3_PALETTED_BEGIN(tpagIndex);
    glBegin(GL_QUADS);
    glColor4f(r, g, b, alpha);
    for (float dy = startY; endY > dy; dy += tileH) {
        float cy = dy + originY * ayScale;
        float vy0 = cy + sy0;
        float vy1 = cy + sy1;
        for (float dx = startX; endX > dx; dx += tileW) {
            float cx = dx + originX * axScale;
            float vx0 = cx + sx0;
            float vx1 = cx + sx1;

            glTexCoord2f(u0, v0); glVertex2f(vx0, vy0);
            glTexCoord2f(u1, v0); glVertex2f(vx1, vy0);
            glTexCoord2f(u1, v1); glVertex2f(vx1, vy1);
            glTexCoord2f(u0, v1); glVertex2f(vx0, vy1);
        }
    }
    glEnd();
    PS3_PALETTED_END();
}

static void glDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];
    glBindTexture(GL_TEXTURE_2D, texId);
    PS3_PALETTED_BEGIN(tpagIndex);

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    glBegin(GL_QUADS);
        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glTexCoord2f(u0, v0);
        glVertex2f(x1, y1);

        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glTexCoord2f(u1, v0);
        glVertex2f(x2, y2);

        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glTexCoord2f(u1, v1);
        glVertex2f(x3, y3);

        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glTexCoord2f(u0, v1);
        glVertex2f(x4, y4);
    glEnd();
    PS3_PALETTED_END();
}

static void glDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

    glBindTexture(GL_TEXTURE_2D, texId);

    // Compute UVs for the sub-region within the atlas
    float u0 = (float) (tpag->sourceX + srcOffX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / (float) texH;

    // Convert BGR colors to RGB floats
    float r1 = (float) BGR_R(color1) / 255.0f, g1 = (float) BGR_G(color1) / 255.0f, b1 = (float) BGR_B(color1) / 255.0f;
    float r2 = (float) BGR_R(color2) / 255.0f, g2 = (float) BGR_G(color2) / 255.0f, b2 = (float) BGR_B(color2) / 255.0f;
    float r3 = (float) BGR_R(color3) / 255.0f, g3 = (float) BGR_G(color3) / 255.0f, b3 = (float) BGR_B(color3) / 255.0f;
    float r4 = (float) BGR_R(color4) / 255.0f, g4 = (float) BGR_G(color4) / 255.0f, b4 = (float) BGR_B(color4) / 255.0f;

    // Quad corners (no origin offset - draw_sprite_part ignores sprite origin)
    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    if (angleDeg == 0.0f) {
        cx0 = x;                         cy0 = y;
        cx1 = x + (float) srcW * xscale; cy1 = y;
        cx2 = x + (float) srcW * xscale; cy2 = y + (float) srcH * yscale;
        cx3 = x;                         cy3 = y + (float) srcH * yscale;
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float qx0 = x,                         qy0 = y;
        float qx1 = x + (float) srcW * xscale, qy1 = y;
        float qx2 = x + (float) srcW * xscale, qy2 = y + (float) srcH * yscale;
        float qx3 = x,                         qy3 = y + (float) srcH * yscale;
        float dx, dy;
        dx = qx0 - pivotX; dy = qy0 - pivotY; cx0 = cosA * dx - sinA * dy + pivotX; cy0 = sinA * dx + cosA * dy + pivotY;
        dx = qx1 - pivotX; dy = qy1 - pivotY; cx1 = cosA * dx - sinA * dy + pivotX; cy1 = sinA * dx + cosA * dy + pivotY;
        dx = qx2 - pivotX; dy = qy2 - pivotY; cx2 = cosA * dx - sinA * dy + pivotX; cy2 = sinA * dx + cosA * dy + pivotY;
        dx = qx3 - pivotX; dy = qy3 - pivotY; cx3 = cosA * dx - sinA * dy + pivotX; cy3 = sinA * dx + cosA * dy + pivotY;
    }

    PS3_PALETTED_BEGIN(tpagIndex);
    glBegin(GL_QUADS);
        glColor4f(r1, g1, b1, alpha);
        glTexCoord2f(u0, v0); glVertex2f(cx0, cy0);

        glColor4f(r2, g2, b2, alpha);
        glTexCoord2f(u1, v0); glVertex2f(cx1, cy1);

        glColor4f(r3, g3, b3, alpha);
        glTexCoord2f(u1, v1); glVertex2f(cx2, cy2);

        glColor4f(r4, g4, b4, alpha);
        glTexCoord2f(u0, v1); glVertex2f(cx3, cy3);
    glEnd();
    PS3_PALETTED_END();
}

static void glDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    glDrawSpritePartColor(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, color, color, color, alpha);
}

// Emits a single colored quad into the batch using the white pixel texture
static void emitColoredQuad(GLLegacyRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

    glBegin(GL_QUADS);
        // All UVs point to (0.5, 0.5) center of the 1x1 white texture
        // Vertex 0: top-left
        glColor4f(r, g, b, a);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x0, y0);

        // Vertex 1: top-right
        glColor4f(r, g, b, a);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x1, y0);

        // Vertex 2: bottom-right
        glColor4f(r, g, b, a);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x1, y1);

        // Vertex 3: bottom-left
        glColor4f(r, g, b, a);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x0, y1);
    glEnd();
}

static void glDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        emitColoredQuad(gl, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha); // top
        emitColoredQuad(gl, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha); // bottom
        emitColoredQuad(gl, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha); // left
        emitColoredQuad(gl, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha); // right
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitColoredQuad(gl, x1, y1, x2 + 1, y2 + 1, r, g, b, alpha);
    }
}

static void glDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha);
static void glDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha, bool outline) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;

    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    float r3 = (float) BGR_R(color3) / 255.0f;
    float g3 = (float) BGR_G(color3) / 255.0f;
    float b3 = (float) BGR_B(color3) / 255.0f;

    float r4 = (float) BGR_R(color4) / 255.0f;
    float g4 = (float) BGR_G(color4) / 255.0f;
    float b4 = (float) BGR_B(color4) / 255.0f;

    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        glDrawLineColor(renderer, x1, y1, x2, y1, 1.0, color1, color2, alpha);
        glDrawLineColor(renderer, x2, y1, x2, y2, 1.0, color2, color3, alpha);
        glDrawLineColor(renderer, x2, y2, x1, y2, 1.0, color3, color4, alpha);
        glDrawLineColor(renderer, x1, y2, x1, y1, 1.0, color4, color1, alpha);
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects

        // All UVs point to (0.5, 0.5) center of the 1x1 white texture
        glBegin(GL_QUADS);
            // Vertex 0: top-left
            glColor4f(r1, g1, b1, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x1, y1);

            // Vertex 1: top-right
            glColor4f(r2, g2, b2, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x2+1, y1);

            // Vertex 2: bottom-right
            glColor4f(r3, g3, b3, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x2+1, y2+1);

            // Vertex 3: bottom-left
            glColor4f(r4, g4, b4, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x1, y2+1);

        glEnd();
    }
}

// ===[ Line Drawing ]===

static void glDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

    // Vertex 0: start + perpendicular
    glBegin(GL_QUADS);
        glColor4f(r, g, b, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x1 + px, y1 + py);

        // Vertex 1: start - perpendicular
        glColor4f(r, g, b, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x1 - px, y1 - py);

        // Vertex 2: end - perpendicular
        glColor4f(r, g, b, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x2 - px, y2 - py);

        // Vertex 3: end + perpendicular
        glColor4f(r, g, b, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x2 + px, y2 + py);
    glEnd();
}

static void glDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;

    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    // Emit quad with per-vertex colors (color1 at start, color2 at end)
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

    glBegin(GL_QUADS);
        // Vertex 0: start + perpendicular (color1)
        glColor4f(r1, g1, b1, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x1 + px, y1 + py);

        // Vertex 1: start - perpendicular (color1)
        glColor4f(r1, g1, b1, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x1 - px, y1 - py);

        // Vertex 2: end - perpendicular (color2)
        glColor4f(r2, g2, b2, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x2 - px, y2 - py);

        // Vertex 3: end + perpendicular (color2)
        glColor4f(r2, g2, b2, alpha);
        glTexCoord2f(0.5f, 0.5f);
        glVertex2f(x2 + px, y2 + py);
    glEnd();
}

static void glDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline)
{
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if(outline)
    {
        glDrawLineColor(renderer, x1, y1, x2, y2, 1, color1, color2, alpha);
        glDrawLineColor(renderer, x2, y2, x3, y3, 1, color2, color3, alpha);
        glDrawLineColor(renderer, x3, y3, x1, y1, 1, color3, color1, alpha);
    } else {
        glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

        glBegin(GL_TRIANGLES);
            glColor4f((float) BGR_R(color1) / 255.0f, (float) BGR_G(color1) / 255.0f, (float) BGR_B(color1) / 255.0f, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x1 , y1);

            glColor4f((float) BGR_R(color2) / 255.0f, (float) BGR_G(color2) / 255.0f, (float) BGR_B(color2) / 255.0f, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x2, y2);

            glColor4f((float) BGR_R(color3) / 255.0f, (float) BGR_G(color3) / 255.0f, (float) BGR_B(color3) / 255.0f, alpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x3, y3);
        glEnd();
    }
}

// ===[ Text Drawing ]===

// Resolved font state shared between glDrawText and glDrawTextColor
typedef struct {
    Font* font;
    TexturePageItem* fontTpag; // single TPAG for regular fonts (nullptr for sprite fonts)
    int32_t fontTpagIndex;     // TPAG index for regular fonts (-1 for sprite fonts)
    GLuint texId;
    int32_t texW, texH;
    Sprite* spriteFontSprite; // source sprite for sprite fonts (nullptr for regular fonts)
} GlFontState;

// Resolves font texture state
// Returns false if the font can't be drawn
static bool glResolveFontState(GLLegacyRenderer* gl, DataWin* dw, Font* font, GlFontState* state) {
    state->font = font;
    state->fontTpag = nullptr;
    state->fontTpagIndex = -1;
    state->texId = 0;
    state->texW = 0;
    state->texH = 0;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (0 > fontTpagIndex) return false;

        state->fontTpagIndex = fontTpagIndex;
        state->fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= gl->textureCount) return false;
        if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return false;

        state->texId = gl->glTextures[pageId];
        state->texW = gl->textureWidths[pageId];
        state->texH = gl->textureHeights[pageId];
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    return true;
}

// Resolves UV coordinates, texture ID, and local position for a single glyph
// Returns false if the glyph can't be drawn
static bool glResolveGlyph(GLLegacyRenderer* gl, DataWin* dw, GlFontState* state, FontGlyph* glyph, float cursorX, float cursorY, GLuint* outTexId, int32_t* outTpagIdx, float* outU0, float* outV0, float* outU1, float* outV1, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= gl->textureCount) return false;
        if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pid)) return false;

        *outTexId = gl->glTextures[pid];
        *outTpagIdx = tpagIdx;
        int32_t tw = gl->textureWidths[pid];
        int32_t th = gl->textureHeights[pid];

        *outU0 = (float) glyphTpag->sourceX / (float) tw;
        *outV0 = (float) glyphTpag->sourceY / (float) th;
        *outU1 = (float) (glyphTpag->sourceX + glyphTpag->sourceWidth) / (float) tw;
        *outV1 = (float) (glyphTpag->sourceY + glyphTpag->sourceHeight) / (float) th;

        // Sprite-font glyphs sit at the cell offset. GM 2023.2+ subtracts the sprite origin, pre-2023.2 it cancels.
        // (See GameMaker-HTML5's commit a7c5b909209d5a28602fedfe2031965386a99921)
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) (int32_t) glyphTpag->targetY - (float) font->spriteOriginYAdjust;
    } else {
        *outTexId = state->texId;
        *outTpagIdx = state->fontTpagIndex;
        *outU0 = (float) (state->fontTpag->sourceX + glyph->sourceX) / (float) state->texW;
        *outV0 = (float) (state->fontTpag->sourceY + glyph->sourceY) / (float) state->texH;
        *outU1 = (float) (state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) state->texW;
        *outV1 = (float) (state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) state->texH;

        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void glDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    GlFontState fontState;
    if (!glResolveFontState(gl, dw, font, &fontState)) return;

    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    int32_t textLen = (int32_t) strlen(text);

    // Count lines, treating \r\n and \n\r as single breaks
    int32_t lineCount = TextUtils_countLines(text, textLen);

    // Per-line vertical stride. HTML5 runner's default `linesep` is `max_glyph_height * scaleY`.
    // We apply scaleY via the transform matrix below, so keep the stride in pre-scale (local) coords.
    // Caller-supplied separation is in world pre-scale pixels; divide by font->scaleY so the transform restores it.
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    // Build transform matrix
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    // Iterate through lines. HTML5 subtracts ascenderOffset from the per-line y offset
    // (see yyFont.GR_Text_Draw), shifting glyphs up so the baseline aligns with the drawn y.
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                bool drewSuccessfully = false;
				if (ch == ' ') {
					drewSuccessfully = true;
                } else if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1;
                    float localX0, localY0;
                    GLuint glyphTexId;
                    int32_t glyphTpagIdx;

                    if (glResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &glyphTexId, &glyphTpagIdx, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        glBindTexture(GL_TEXTURE_2D, glyphTexId);
                        PS3_PALETTED_BEGIN(glyphTpagIdx);

                        float localX1 = localX0 + (float) glyph->sourceWidth;
                        float localY1 = localY0 + (float) glyph->sourceHeight;

                        // Transform corners
                        float px0, py0, px1, py1, px2, py2, px3, py3;
                        Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
                        Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
                        Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
                        Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

                        glBegin(GL_QUADS);
                            glColor4f(r, g, b, alpha);
                            glTexCoord2f(u0, v0);
                            glVertex2f(px0, py0);

                            glColor4f(r, g, b, alpha);
                            glTexCoord2f(u1, v0);
                            glVertex2f(px1, py1);

                            glColor4f(r, g, b, alpha);
                            glTexCoord2f(u1, v1);
                            glVertex2f(px2, py2);

                            glColor4f(r, g, b, alpha);
                            glTexCoord2f(u0, v1);
                            glVertex2f(px3, py3);
                        glEnd();
                        PS3_PALETTED_END();

                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static void glDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha, float lineSeparation) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    GlFontState fontState;
    if (!glResolveFontState(gl, dw, font, &fontState)) return;

    int32_t textLen = (int32_t) strlen(text);
    if(textLen == 0) return;

    // Count lines, treating \r\n and \n\r as single breaks
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    // Build transform matrix
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    // Iterate through lines. HTML5 subtracts ascenderOffset from per-line y offset.
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        // Pixel-position cursor for the gradient
        float gradientX = 0.0f;

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                float advance = (float) glyph->shift;
                float leftFrac  = (lineWidth > 0.0f) ? (gradientX           / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                int32_t c1 = Color_lerp(_c1, _c2, leftFrac);
                int32_t c2 = Color_lerp(_c1, _c2, rightFrac);
                int32_t c3 = Color_lerp(_c4, _c3, rightFrac);
                int32_t c4 = Color_lerp(_c4, _c3, leftFrac);

                bool drewSuccessfully = false;
				if (ch == ' ') {
					drewSuccessfully = true;
                } else if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1;
                    float localX0, localY0;
                    GLuint glyphTexId;
                    int32_t glyphTpagIdx;

                    if (glResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &glyphTexId, &glyphTpagIdx, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        glBindTexture(GL_TEXTURE_2D, glyphTexId);
                        PS3_PALETTED_BEGIN(glyphTpagIdx);

                        float localX1 = localX0 + (float) glyph->sourceWidth;
                        float localY1 = localY0 + (float) glyph->sourceHeight;

                        // Transform corners
                        float px0, py0, px1, py1, px2, py2, px3, py3;
                        Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
                        Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
                        Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
                        Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

                        glBegin(GL_QUADS);
                            glColor4ub(BGR_R(c1), BGR_G(c1), BGR_B(c1), alpha * 255);
                            glTexCoord2f(u0, v0);
                            glVertex2f(px0, py0);

                            glColor4ub(BGR_R(c2), BGR_G(c2), BGR_B(c2), alpha * 255);
                            glTexCoord2f(u1, v0);
                            glVertex2f(px1, py1);

                            glColor4ub(BGR_R(c3), BGR_G(c3), BGR_B(c3), alpha * 255);
                            glTexCoord2f(u1, v1);
                            glVertex2f(px2, py2);

                            glColor4ub(BGR_R(c4), BGR_G(c4), BGR_B(c4), alpha * 255);
                            glTexCoord2f(u0, v1);
                            glVertex2f(px3, py3);
                        glEnd();
                        PS3_PALETTED_END();

                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                gradientX   += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX   += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

// ===[ Dynamic Sprite Creation/Deletion ]===

// Finds a free dynamic texture page slot (glTextures[i] == 0), or appends a new one.
static uint32_t findOrAllocTexturePageSlot(GLLegacyRenderer* gl) {
    // Scan dynamic range for a reusable slot
    for (uint32_t i = gl->originalTexturePageCount; gl->textureCount > i; i++) {
        if (gl->glTextures[i] == 0) return i;
    }
    // No free slot found, grow the arrays
    uint32_t newPageId = gl->textureCount;
    gl->textureCount++;
    gl->glTextures = (GLuint *)safeRealloc(gl->glTextures, gl->textureCount * sizeof(GLuint));
    gl->textureWidths = (int32_t *)safeRealloc(gl->textureWidths, gl->textureCount * sizeof(int32_t));
    gl->textureHeights = (int32_t *)safeRealloc(gl->textureHeights, gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = (bool *)safeRealloc(gl->textureLoaded, gl->textureCount * sizeof(bool));
    gl->glTextures[newPageId] = 0;
    gl->textureWidths[newPageId] = 0;
    gl->textureHeights[newPageId] = 0;
    gl->textureLoaded[newPageId] = false;
    return newPageId;
}

// Finds a free dynamic TPAG slot (texturePageId == -1), or appends a new one.
static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = (TexturePageItem *)safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t glCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    // TODO: implement these
    (void)smooth;
    (void)removeback;
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 >= w || 0 >= h) return -1;
    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return -1;
    if (gl->surfaces[surfaceID] == 0) return -1;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->surfaces[surfaceID]);

    uint8_t* pixels = (uint8_t *)safeMalloc((size_t) w * (size_t) h * 4);
    if (pixels == nullptr)
        return -1;

    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Create a new GL texture from the captured pixels
    GLuint newTexId;
    glGenTextures(1, &newTexId);
    glBindTexture(GL_TEXTURE_2D, newTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    free(pixels);

    // Find or allocate slots for texture page, TPAG, and sprite
    uint32_t pageId = findOrAllocTexturePageSlot(gl);
    gl->glTextures[pageId] = newTexId;
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;
    gl->textureLoaded[pageId] = true;

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, gl->originalTpagCount);
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
    tpag->texturePageId = (int16_t) pageId;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, gl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t *)safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    logInfo("GL: Created dynamic sprite %u (%dx%d) from surface at (%d,%d)\n", spriteIndex, w, h, x, y);
    return (int32_t) spriteIndex;
}

static void glDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (gl->originalSpriteCount > (uint32_t) spriteIndex) {
        logWarn("GL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Clean up GL texture and TPAG entries owned by this sprite.
    // Slots with index >= originalTpagCount are dynamically allocated and ours to free.
    repeat(sprite->textureCount, i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= gl->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && gl->textureCount > (uint32_t) pageId) {
                glDeleteTextures(1, &gl->glTextures[pageId]);
                gl->glTextures[pageId] = 0;
            }
            // Mark TPAG slot as free for reuse
            tpag->texturePageId = -1;
        }
    }

    // Clear the sprite entry so it won't be drawn and can be reused. Preserve `name` across the memset: the slot is still in sprt.count and must keep a valid string for asset_get_index / name lookups.
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    logInfo("GL: Deleted sprite %d\n", spriteIndex);
}

static BlendFactors glGpuGetBlendFactors(Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*)renderer;
    BlendFactors ret;
    ret.src = gl->currentSFactor;
    ret.dst = gl->currentDFactor;
    ret.srcAlpha = gl->currentSFactorAlpha;
    ret.dstAlpha = gl->currentDFactorAlpha;
    return ret;
}

static int32_t glGpuGetBlendMode(Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    return gl->currentBlendMode;
}

static void glGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (gl->currentBlendMode == mode) return;

    gl->currentBlendMode = mode;
    gl->currentSFactor = GLCommon_blendModeToSFactor(mode);
    gl->currentDFactor = GLCommon_blendModeToDFactor(mode);
    gl->currentSFactorAlpha = gl->currentSFactor;
    gl->currentDFactorAlpha = gl->currentDFactor;
    glBlendEquation(GLCommon_blendModeToEquation(mode));
    glBlendFunc(gl->currentSFactor, gl->currentDFactor);
}

static void glGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (gl->currentBlendMode == bm_complex &&
        gl->currentSFactor == sfactor &&
        gl->currentDFactor == dfactor &&
        gl->currentSFactorAlpha == sfactor_alpha &&
        gl->currentDFactorAlpha == dfactor_alpha) return;

    gl->currentBlendMode = bm_complex;
    gl->currentSFactor = sfactor;
    gl->currentDFactor = dfactor;
    gl->currentSFactorAlpha = sfactor_alpha;
    gl->currentDFactorAlpha = dfactor_alpha;

    glBlendFuncSeparate(
        GLCommon_blendFactorToGL(sfactor),
        GLCommon_blendFactorToGL(dfactor),
        GLCommon_blendFactorToGL(sfactor_alpha),
        GLCommon_blendFactorToGL(dfactor_alpha)
    );
}

static void glGpuSetBlendEnable(Renderer* renderer, bool enable) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (gl->blendEnable == enable) return;
    enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    gl->blendEnable = enable;
}

static bool glGpuGetBlendEnable(MAYBE_UNUSED Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    return gl->blendEnable;
}

static void glGpuSetAlphaTestEnable(MAYBE_UNUSED Renderer* renderer, bool enable) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (gl->alphaTestEnable == enable) return;
    enable ? glEnable(GL_ALPHA_TEST) : glDisable(GL_ALPHA_TEST);
    gl->alphaTestEnable = enable;
}

static bool glGpuGetAlphaTestEnable(MAYBE_UNUSED Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    return gl->alphaTestEnable;
}

static void glGpuSetAlphaTestRef(MAYBE_UNUSED Renderer* renderer, uint8_t ref) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (gl->alphaTestRef == ref) return;
    gl->alphaTestRef = ref;
    glAlphaFunc(GL_GREATER, ref/255.0f);
}

static void glGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (gl->colorWriteR == red && gl->colorWriteG == green && gl->colorWriteB == blue && gl->colorWriteA == alpha) return;
    gl->colorWriteR = red;
    gl->colorWriteG = green;
    gl->colorWriteB = blue;
    gl->colorWriteA = alpha;
    glColorMask(red, green, blue, alpha);
}

static void glGpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    *red = gl->colorWriteR;
    *green = gl->colorWriteG;
    *blue = gl->colorWriteB;
    *alpha = gl->colorWriteA;
}

// ===[ Surfaces ]===

static int32_t glLegacyCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    // Save the current FBO binding so creating a surface doesn't change the active render target.
    GLint prevBinding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);

    uint32_t surfaceIndex = GLCommon_findOrAllocateSurfaceSlot(&gl->surfaces, &gl->surfaceTexture, &gl->surfaceWidth, &gl->surfaceHeight, &gl->surfaceCount);

    int32_t texW = gl->needsPOT ? nextPow2(width)  : width;
    int32_t texH = gl->needsPOT ? nextPow2(height) : height;

    glGenFramebuffers(1, &gl->surfaces[surfaceIndex]);
    glGenTextures(1, &gl->surfaceTexture[surfaceIndex]);
    glBindTexture(GL_TEXTURE_2D, gl->surfaceTexture[surfaceIndex]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceIndex]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->surfaceTexture[surfaceIndex], 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        logWarn("GL: Surface FBO incomplete (status=0x%X)\n", status);
    }

    gl->surfaceWidth[surfaceIndex] = width;
    gl->surfaceHeight[surfaceIndex] = height;

    logInfo("GL: Created surface %u with size (%dx%d)\n", surfaceIndex, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);
    return (int32_t) surfaceIndex;
}

static int32_t glLegacyEnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    int32_t id = renderer->runner->applicationSurfaceId;

    bool needsCreate = (id < 0) || ((uint32_t) id >= gl->surfaceCount) || (gl->surfaces[id] == 0);
    if (needsCreate) {
        id = glLegacyCreateSurface(renderer, width, height);
        // Publish immediately so anything that re-queries the runner during this frame sees the new ID.
        renderer->runner->applicationSurfaceId = id;
        return id;
    }

    if (gl->surfaceWidth[id] != width || gl->surfaceHeight[id] != height) {
        renderer->vtable->surfaceResize(renderer, id, width, height);
    }
    return id;
}

static bool glLegacySurfaceExists(Renderer* renderer, int32_t surfaceId) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    return gl->surfaces[surfaceId] != 0;
}

static float glLegacyGetSurfaceWidth(Renderer* renderer, int32_t surfaceId) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return 0.0f;
    if (gl->surfaces[surfaceId] == 0) return 0.0f;
    return (float) gl->surfaceWidth[surfaceId];
}

static float glLegacyGetSurfaceHeight(Renderer* renderer, int32_t surfaceId) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return 0.0f;
    if (gl->surfaces[surfaceId] == 0) return 0.0f;
    return (float) gl->surfaceHeight[surfaceId];
}

static void glLegacySurfaceResize(Renderer* renderer, int32_t surfaceId, int32_t width, int32_t height) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return;
    if (gl->surfaces[surfaceId] == 0) return;
    if (gl->surfaceWidth[surfaceId] == width && gl->surfaceHeight[surfaceId] == height) return;

    GLint prevBinding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);

    if (gl->surfaceTexture[surfaceId] != 0) glDeleteTextures(1, &gl->surfaceTexture[surfaceId]);

    int32_t texW = gl->needsPOT ? nextPow2(width)  : width;
    int32_t texH = gl->needsPOT ? nextPow2(height) : height;

    glGenTextures(1, &gl->surfaceTexture[surfaceId]);
    glBindTexture(GL_TEXTURE_2D, gl->surfaceTexture[surfaceId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceId]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->surfaceTexture[surfaceId], 0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);

    gl->surfaceWidth[surfaceId] = width;
    gl->surfaceHeight[surfaceId] = height;
    logInfo("GL: Resized Surface %u to (%dx%d)\n", surfaceId, width, height);
}

static void glLegacySurfaceFree(Renderer* renderer, int32_t surfaceId) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return;
    // Freeing the application_surface is a no-op from GML; the runner manages its lifecycle via application_surface_enable.
    if (surfaceId == renderer->runner->applicationSurfaceId) return;
    if (gl->surfaceTexture[surfaceId] != 0) glDeleteTextures(1, &gl->surfaceTexture[surfaceId]);
    if (gl->surfaces[surfaceId] != 0) glDeleteFramebuffers(1, &gl->surfaces[surfaceId]);
    gl->surfaces[surfaceId] = 0;
    gl->surfaceTexture[surfaceId] = 0;
    gl->surfaceWidth[surfaceId] = 0;
    gl->surfaceHeight[surfaceId] = 0;
    logInfo("GL: Freed Surface %d\n", surfaceId);
}

static bool glLegacySetRenderTarget(Renderer* renderer, int32_t surfaceId, bool implicitApplicationSurface) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
    viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = &renderer->runner->views[viewCurrent];
    gl->base.cameraCurrent = view->cameraId;
    GMLCamera* camera = Runner_getCameraById(renderer->runner, gl->base.cameraCurrent);

    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    if (gl->surfaces[surfaceId] == 0) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceId]);

    if (surfaceId == renderer->runner->applicationSurfaceId && implicitApplicationSurface) {
        gl->base.CPortX = 0;
        gl->base.CPortY = 0;
        gl->base.CPortW = gl->gameW;
        gl->base.CPortH = gl->gameH;

        glViewport(gl->base.CPortX, gl->base.CPortY, gl->base.CPortW, gl->base.CPortH);
        glEnable(GL_SCISSOR_TEST);
        glScissor(gl->base.CPortX, gl->base.CPortY, gl->base.CPortW, gl->base.CPortH);
        glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
        return true;
    }

    if (surfaceId == view->surfaceId) {
    //the surface belongs to the view we are rending, we use the view's camera.
    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
    return true;
    } else {
    //camera will use full surface.
    gl->base.cameraCurrent = SURFACE_CAMERA;
    GMLCamera* camera =  &renderer->runner->surfaceCamera;

    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = gl->surfaceWidth[surfaceId];
    camera->viewHeight = gl->surfaceHeight[surfaceId];
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;
    Runner_updateCameraViewSimple(camera);

    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);
    glApplyProjection(renderer, &camera->viewMatrix,&camera->projectionMatrix);
    return true;
    }


    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);

    return true;
}

// Resolves a surfaceID to a GL texture and its actual texture size
// (POT dimensions if needsPOT, logical dimensions otherwise).
static bool resolveSurfaceTexture(GLLegacyRenderer* gl, int32_t surfaceId, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH) {
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    if (gl->surfaces[surfaceId] == 0) return false;
    *outTexId = gl->surfaceTexture[surfaceId];
    if (gl->needsPOT) {
        *outTexW = nextPow2(gl->surfaceWidth[surfaceId]);
        *outTexH = nextPow2(gl->surfaceHeight[surfaceId]);
    } else {
        *outTexW = gl->surfaceWidth[surfaceId];
        *outTexH = gl->surfaceHeight[surfaceId];
    }
    return true;
}

static void glLegacyDrawSurfaceTiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
    // No-op
}

static void glLegacyDrawSurface(Renderer* renderer, int32_t surfaceId, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSurfaceTexture(gl, surfaceId, &texId, &texW, &texH)) return;

    // Use the logical surface size for the default "draw everything" case,
    // not the POT texture dimensions (texW/texH may be rounded up).
    if (0 > srcWidth) {
        srcLeft = 0;
        srcTop = 0;
        srcWidth = gl->surfaceWidth[surfaceId];
        srcHeight = gl->surfaceHeight[surfaceId];
    }

    // top-down GML coords -> flipped V for our bottom-up texture
    float u0 = (float) srcLeft / (float) texW;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, 0.0f,             0.0f,             &x0, &y0);
    Matrix4f_transformPoint(&transform, (float) srcWidth, 0.0f,             &x1, &y1);
    Matrix4f_transformPoint(&transform, (float) srcWidth, (float) srcHeight, &x2, &y2);
    Matrix4f_transformPoint(&transform, 0.0f,             (float) srcHeight, &x3, &y3);

    glBindTexture(GL_TEXTURE_2D, texId);
    glBegin(GL_QUADS);
        glColor4f(r, g, b, alpha); glTexCoord2f(u0, v0); glVertex2f(x0, y0);
        glColor4f(r, g, b, alpha); glTexCoord2f(u1, v0); glVertex2f(x1, y1);
        glColor4f(r, g, b, alpha); glTexCoord2f(u1, v1); glVertex2f(x2, y2);
        glColor4f(r, g, b, alpha); glTexCoord2f(u0, v1); glVertex2f(x3, y3);
    glEnd();
}

static void glLegacyDrawSurfaceColor(Renderer* renderer, int32_t surfaceId, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSurfaceTexture(gl, surfaceId, &texId, &texW, &texH)) return;

    // Use the logical surface size for the default "draw everything" case,
    // not the POT texture dimensions (texW/texH may be rounded up).
    if (0 > srcWidth) {
        srcLeft = 0;
        srcTop = 0;
        srcWidth = gl->surfaceWidth[surfaceId];
        srcHeight = gl->surfaceHeight[surfaceId];
    }

    // top-down GML coords -> flipped V for our bottom-up texture
    float u0 = (float) srcLeft / (float) texW;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;
    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;
    float r3 = (float) BGR_R(color3) / 255.0f;
    float g3 = (float) BGR_G(color3) / 255.0f;
    float b3 = (float) BGR_B(color3) / 255.0f;
    float r4 = (float) BGR_R(color4) / 255.0f;
    float g4 = (float) BGR_G(color4) / 255.0f;
    float b4 = (float) BGR_B(color4) / 255.0f;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, 0.0f,             0.0f,             &x0, &y0);
    Matrix4f_transformPoint(&transform, (float) srcWidth, 0.0f,             &x1, &y1);
    Matrix4f_transformPoint(&transform, (float) srcWidth, (float) srcHeight, &x2, &y2);
    Matrix4f_transformPoint(&transform, 0.0f,             (float) srcHeight, &x3, &y3);

    glBindTexture(GL_TEXTURE_2D, texId);
    glBegin(GL_QUADS);
        glColor4f(r1, g1, b1, alpha); glTexCoord2f(u0, v0); glVertex2f(x0, y0);
        glColor4f(r2, g2, b2, alpha); glTexCoord2f(u1, v0); glVertex2f(x1, y1);
        glColor4f(r3, g3, b3, alpha); glTexCoord2f(u1, v1); glVertex2f(x2, y2);
        glColor4f(r4, g4, b4, alpha); glTexCoord2f(u0, v1); glVertex2f(x3, y3);
    glEnd();
}


static void glLegacySurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    GLCommon_surfaceBlit(gl->surfaces, gl->surfaceWidth, gl->surfaceHeight, gl->surfaceCount, destSurfaceID, destX, destY, srcSurfaceID, srcX, srcY, srcW, srcH, part);
}

static bool glLegacySurfaceGetPixels(Renderer* renderer, int32_t surfaceId, uint8_t* outRGBA) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    return GLCommon_surfaceGetPixels(gl->surfaces, gl->surfaceWidth, gl->surfaceHeight, gl->surfaceCount, surfaceId, outRGBA);
}


// ===[ Vtable ]===

// Decode a texture handle produced by glSpriteGetTexture back into its tpag and page dimensions.
// Returns false for the 0 ("no texture") handle or an unresolvable one.
static bool glLegacyResolveTextureHandle(GLLegacyRenderer* gl, uint32_t texHandle, TexturePageItem** outTpag, int32_t* outW, int32_t* outH) {
    if (texHandle == 0) return false;
    if (texHandle & GL_SURFACE_TEXTURE_FLAG) {
        uint32_t sid = texHandle & ~GL_SURFACE_TEXTURE_FLAG;
        if (sid >= gl->surfaceCount || gl->surfaceTexture[sid] == 0) return false;
        if (outTpag) *outTpag = nullptr;
        *outW = gl->surfaceWidth[sid];
        *outH = gl->surfaceHeight[sid];
        return true;
    }
    DataWin* dw = gl->base.dataWin;
    int32_t tpagIndex = (int32_t) texHandle - 1;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return false;
    if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return false;
    *outTpag = tpag;
    *outW = gl->textureWidths[pageId];
    *outH = gl->textureHeights[pageId];
    return true;
}

static uint32_t glSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return 0;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return 0;
    if (!GLLegacyRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return 0;
    return (uint32_t) (tpagIndex + 1);
}

static uint32_t glSurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if (surfaceID < 0 || (uint32_t) surfaceID >= gl->surfaceCount) return 0;
    if (gl->surfaceTexture[surfaceID] == 0) return 0;
    return GL_SURFACE_TEXTURE_FLAG | (uint32_t) surfaceID;
}

static float glTextureGetTexelWidth(Renderer* renderer, uint32_t texHandle) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    TexturePageItem* tpag;
    int32_t w = 0, h = 0;
    if (!glLegacyResolveTextureHandle(gl, texHandle, &tpag, &w, &h) || 0 >= w) return 1.0f;
    return 1.0f / (float) w;
}

static float glTextureGetTexelHeight(Renderer* renderer, uint32_t texHandle) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    TexturePageItem* tpag;
    int32_t w = 0, h = 0;
    if (!glLegacyResolveTextureHandle(gl, texHandle, &tpag, &w, &h) || 0 >= h) return 1.0f;
    return 1.0f / (float) h;
}

static bool glTextureGetUVs(Renderer* renderer, uint32_t texHandle, float* outUVs) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    TexturePageItem* tpag;
    int32_t w = 0, h = 0;
    if (!glLegacyResolveTextureHandle(gl, texHandle, &tpag, &w, &h) || 0 >= w || 0 >= h) return false;
    // Surface handles cover the whole texture (no tpag sub-region).
    if (tpag == nullptr) {
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    float divW = 1.0f / (float) w;
    float divH = 1.0f / (float) h;
    outUVs[0] = (float) tpag->sourceX * divW;                       // left
    outUVs[1] = (float) tpag->sourceY * divH;                       // top
    outUVs[2] = outUVs[0] + (float) tpag->sourceWidth * divW;       // right
    outUVs[3] = outUVs[1] + (float) tpag->sourceHeight * divH;      // bottom
    return true;
}

static void glTextureSetStage(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t slot, MAYBE_UNUSED uint32_t texHandle) {
}

static void glGpuSetShader(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex) {}
static void glGpuResetShader(MAYBE_UNUSED Renderer* renderer) {}
static int32_t glShaderGetUniform(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char* uniform) { return -1; }
static int32_t glShaderGetSamplerIndex(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char* uniform) { return -1; }
static void glShaderSetUniformF(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count, MAYBE_UNUSED float value1, MAYBE_UNUSED float value2, MAYBE_UNUSED float value3, MAYBE_UNUSED float value4) {}
static void glShaderSetUniformFArray(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED float* values, MAYBE_UNUSED uint32_t count) {}
static void glShaderSetUniformI(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count, MAYBE_UNUSED int32_t value1, MAYBE_UNUSED int32_t value2, MAYBE_UNUSED int32_t value3, MAYBE_UNUSED int32_t value4) {}
static bool glShaderIsCompiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shader) { return false; }
static bool glShadersSupported(void) { return false; }

static RendererVtable glVtable;

// ===[ Public API ]===

Renderer* GLLegacyRenderer_create(void) {
    GLLegacyRenderer* gl = (GLLegacyRenderer *)safeCalloc(1, sizeof(GLLegacyRenderer));
    gl->base.vtable = &glVtable;
    glVtable.init = glInit;
    glVtable.destroy = glDestroy;
    glVtable.beginFrame = glBeginFrame;
    glVtable.endFrameInit = glEndFrameInit;
    glVtable.endFrameEnd = glEndFrameEnd;
    glVtable.beginView = glBeginView;
    glVtable.endView = glEndView;
    glVtable.applyProjection = glApplyProjection;
    glVtable.beginGUI = glBeginGUI;
    glVtable.setGuiProjection = glSetGuiProjection;
    glVtable.endGUI = glEndGUI;
    glVtable.drawSprite = glDrawSprite;
    glVtable.drawSpritePos = glDrawSpritePos;
    glVtable.drawSpritePart = glDrawSpritePart;
    glVtable.drawSpritePartColor = glDrawSpritePartColor;
    glVtable.drawRectangle = glDrawRectangle;
    glVtable.drawRectangleColor = glDrawRectangleColor;
    glVtable.drawLine = glDrawLine;
    glVtable.drawLineColor = glDrawLineColor;
    glVtable.drawTriangle = glDrawTriangle;
    glVtable.drawText = glDrawText;
    glVtable.drawTextColor = glDrawTextColor;
    glVtable.flush = glRendererFlush;
    glVtable.clearScreen = glClearScreen;
    glVtable.createSpriteFromSurface = glCreateSpriteFromSurface;
    glVtable.deleteSprite = glDeleteSprite;
    glVtable.gpuGetBlendFactors = glGpuGetBlendFactors;
    glVtable.gpuGetBlendMode = glGpuGetBlendMode;
    glVtable.gpuSetBlendMode = glGpuSetBlendMode;
    glVtable.gpuSetBlendModeExt = glGpuSetBlendModeExt;
    glVtable.gpuSetBlendEnable = glGpuSetBlendEnable;
    glVtable.gpuSetAlphaTestEnable = glGpuSetAlphaTestEnable;
    glVtable.gpuGetAlphaTestEnable = glGpuGetAlphaTestEnable;
    glVtable.gpuSetAlphaTestRef = glGpuSetAlphaTestRef;
    glVtable.gpuSetColorWriteEnable = glGpuSetColorWriteEnable;
    glVtable.gpuGetColorWriteEnable = glGpuGetColorWriteEnable;
    glVtable.gpuGetBlendEnable = glGpuGetBlendEnable;
    glVtable.drawTile = nullptr;
    glVtable.drawSpriteTiled = glDrawSpriteTiled;
    glVtable.createSurface = glLegacyCreateSurface;
    glVtable.surfaceExists = glLegacySurfaceExists;
    glVtable.setRenderTarget = glLegacySetRenderTarget;
    glVtable.ensureApplicationSurface = glLegacyEnsureApplicationSurface;
    glVtable.getSurfaceWidth = glLegacyGetSurfaceWidth;
    glVtable.getSurfaceHeight = glLegacyGetSurfaceHeight;
    glVtable.drawSurface = glLegacyDrawSurface;
    glVtable.drawSurfaceColor = glLegacyDrawSurfaceColor;
    glVtable.drawSurfaceTiled = glLegacyDrawSurfaceTiled;
    glVtable.surfaceResize = glLegacySurfaceResize;
    glVtable.surfaceFree = glLegacySurfaceFree;
    glVtable.surfaceCopy = glLegacySurfaceCopy;
    glVtable.surfaceGetPixels = glLegacySurfaceGetPixels;
    glVtable.spriteGetTexture = glSpriteGetTexture;
    glVtable.surfaceGetTexture = glSurfaceGetTexture;
    glVtable.textureGetTexelWidth = glTextureGetTexelWidth;
    glVtable.textureGetTexelHeight = glTextureGetTexelHeight;
    glVtable.textureGetUVs = glTextureGetUVs;
    glVtable.textureSetStage = glTextureSetStage;
    glVtable.gpuSetShader = glGpuSetShader;
    glVtable.gpuResetShader = glGpuResetShader;
    glVtable.shaderGetUniform = glShaderGetUniform;
    glVtable.shaderGetSamplerIndex = glShaderGetSamplerIndex;
    glVtable.shaderSetUniformF = glShaderSetUniformF;
    glVtable.shaderSetUniformFArray = glShaderSetUniformFArray;
    glVtable.shaderSetUniformI = glShaderSetUniformI;
    glVtable.shaderIsCompiled = glShaderIsCompiled;
    glVtable.shadersSupported = glShadersSupported;
    gl->base.drawColor = 0xFFFFFF; // white (BGR)
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    gl->base.circlePrecision = 24;
    gl->colorWriteR = true;
    gl->colorWriteG = true;
    gl->colorWriteB = true;
    gl->colorWriteA = true;

    return (Renderer*) gl;
}
