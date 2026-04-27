#include "gl_legacy_renderer.h"
#include "common.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"
#include "image_decoder.h"

// ===[ Vtable Implementations ]===

static void glInit(Renderer* renderer, DataWin* dataWin) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    renderer->dataWin = dataWin;

    // Load textures from TXTR pages
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    gl->textureCount = dataWin->txtr.count;
    gl->glTextures = safeMalloc(gl->textureCount * sizeof(GLuint));
    gl->textureWidths = safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureHeights = safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = safeMalloc(gl->textureCount * sizeof(bool));

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

    fprintf(stderr, "GL: Renderer initialized (%u texture pages)\n", gl->textureCount);
}

static void glDestroy(Renderer* renderer) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glDeleteTextures(1, &gl->whiteTexture);

    glDeleteTextures((GLsizei) gl->textureCount, gl->glTextures);

    free(gl->glTextures);
    free(gl->textureWidths);
    free(gl->textureHeights);
    free(gl->textureLoaded);
    free(gl);
}

static void glBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glBindTexture(GL_TEXTURE_2D, 0);
    gl->windowW = windowW;
    gl->windowH = windowH;
    gl->gameW = gameW;
    gl->gameH = gameH;

}

static void glBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glBindTexture(GL_TEXTURE_2D, 0);

    // Set viewport and scissor to the port rectangle within the FBO
    // FBO uses game resolution, port coordinates are in game space
    // OpenGL viewport Y is bottom-up, game Y is top-down
    int32_t glPortY = gl->gameH - portY - portH;
    glViewport(portX, glPortY, gl->windowW, gl->windowH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(portX, glPortY, gl->windowW, gl->windowH);

    // Build orthographic projection (Y-down for GML coordinate system)
    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, (float) viewX, (float) (viewX + viewW), (float) (viewY + viewH), (float) viewY, -1.0f, 1.0f);

    if (viewAngle != 0.0f) {
        // GML view_angle: rotate camera by this angle (degrees, counter-clockwise)
        // To rotate the camera, we rotate the world in the opposite direction around the view center
        float cx = (float) viewX + (float) viewW / 2.0f;
        float cy = (float) viewY + (float) viewH / 2.0f;
        Matrix4f rot;
        Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.0f);
        float angleRad = viewAngle * (float) M_PI / 180.0f;
        Matrix4f_rotateZ(&rot, -angleRad);
        Matrix4f_translate(&rot, -cx, -cy, 0.0f);
        Matrix4f result;
        Matrix4f_multiply(&result, &projection, &rot);
        projection = result;
    }

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glActiveTexture(GL_TEXTURE0);
}

static void glEndView(MAYBE_UNUSED Renderer* renderer) {
    glDisable(GL_SCISSOR_TEST);
}

static void glBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;

    glBindTexture(GL_TEXTURE_2D, 0);

    int32_t glPortY = gl->gameH - portY - portH;
    glViewport(portX, glPortY, portW, portH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(portX, glPortY, portW, portH);

    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, 0.0f, (float) guiW, (float) guiH, 0.0f, -1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glActiveTexture(GL_TEXTURE0);
}

static void glEndGUI(MAYBE_UNUSED Renderer* renderer) {
    glDisable(GL_SCISSOR_TEST);
}

static void glEndFrame(MAYBE_UNUSED Renderer* renderer) {}

static void glRendererFlush(MAYBE_UNUSED Renderer* renderer) {}

// Lazily decodes and uploads a TXTR page on first access.
// Returns true if the texture is ready, false if it failed to decode.
static bool ensureTextureLoaded(GLLegacyRenderer* gl, uint32_t pageId) {
    if (gl->textureLoaded[pageId]) return (gl->textureWidths[pageId] != 0);

    gl->textureLoaded[pageId] = true;

    DataWin* dw = gl->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    int w, h;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t) txtr->blobSize, gm2022_5, &w, &h);
    if (pixels == nullptr) {
        fprintf(stderr, "GL: Failed to decode TXTR page %u\n", pageId);
        return false;
    }

    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;

    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    free(pixels);
    fprintf(stderr, "GL: Loaded TXTR page %u (%dx%d)\n", pageId, w, h);
    return true;
}

static void glDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];
    glBindTexture(GL_TEXTURE_2D, texId);

    // Compute normalized UVs from TPAG source rect
    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Compute local quad corners (relative to origin, with target offset)
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

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
}

static void glDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

    // Flush if texture changed or batch full
    glBindTexture(GL_TEXTURE_2D, texId);

    // Compute UVs for the sub-region within the atlas
    float u0 = (float) (tpag->sourceX + srcOffX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / (float) texH;

    // Quad corners (no origin offset, no transform - draw_sprite_part ignores sprite origin)
    float x0 = x;
    float y0 = y;
    float x1 = x + (float) srcW * xscale;
    float y1 = y + (float) srcH * yscale;

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
        glVertex2f(x1, y0);

        // Vertex 2: bottom-right
        glColor4f(r, g, b, alpha);
        glTexCoord2f(u1, v1);
        glVertex2f(x1, y1);

        // Vertex 3: bottom-left
        glColor4f(r, g, b, alpha);
        glTexCoord2f(u0, v1);
        glVertex2f(x0, y1);
    glEnd();
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

static void glDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline)
{
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    if(outline)
    {
        glDrawLine(renderer, x1, y1, x2, y2, 1, renderer->drawColor, 1.0);
        glDrawLine(renderer, x2, y2, x3, y3, 1, renderer->drawColor, 1.0);
        glDrawLine(renderer, x3, y3, x1, y1, 1, renderer->drawColor, 1.0);
    } else {
        float r = (float) BGR_R(renderer->drawColor) / 255.0f;
        float g = (float) BGR_G(renderer->drawColor) / 255.0f;
        float b = (float) BGR_B(renderer->drawColor) / 255.0f;
        
        glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

        glBegin(GL_TRIANGLES);
            glColor4f(r, g, b, renderer->drawAlpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x1 , y1); 

            glColor4f(r, g, b, renderer->drawAlpha);
            glTexCoord2f(0.5f, 0.5f);
            glVertex2f(x2, y2); 

            glColor4f(r, g, b, renderer->drawAlpha);
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
    GLuint texId;
    int32_t texW, texH;
    Sprite* spriteFontSprite; // source sprite for sprite fonts (nullptr for regular fonts)
} GlFontState;

// Resolves font texture state
// Returns false if the font can't be drawn
static bool glResolveFontState(GLLegacyRenderer* gl, DataWin* dw, Font* font, GlFontState* state) {
    state->font = font;
    state->fontTpag = nullptr;
    state->texId = 0;
    state->texW = 0;
    state->texH = 0;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
        if (0 > fontTpagIndex) return false;

        state->fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= gl->textureCount) return false;
        if (!ensureTextureLoaded(gl, (uint32_t) pageId)) return false;

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
static bool glResolveGlyph(GLLegacyRenderer* gl, DataWin* dw, GlFontState* state, FontGlyph* glyph, float cursorX, float cursorY, GLuint* outTexId, float* outU0, float* outV0, float* outU1, float* outV1, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        uint32_t tpagOffset = sprite->textureOffsets[glyphIndex];
        int32_t tpagIdx = DataWin_resolveTPAG(dw, tpagOffset);
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= gl->textureCount) return false;
        if (!ensureTextureLoaded(gl, (uint32_t) pid)) return false;

        *outTexId = gl->glTextures[pid];
        int32_t tw = gl->textureWidths[pid];
        int32_t th = gl->textureHeights[pid];

        *outU0 = (float) glyphTpag->sourceX / (float) tw;
        *outV0 = (float) glyphTpag->sourceY / (float) th;
        *outU1 = (float) (glyphTpag->sourceX + glyphTpag->sourceWidth) / (float) tw;
        *outV1 = (float) (glyphTpag->sourceY + glyphTpag->sourceHeight) / (float) th;

        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) ((int32_t) glyphTpag->targetY - sprite->originY);
    } else {
        *outTexId = state->texId;
        *outU0 = (float) (state->fontTpag->sourceX + glyph->sourceX) / (float) state->texW;
        *outV0 = (float) (state->fontTpag->sourceY + glyph->sourceY) / (float) state->texH;
        *outU1 = (float) (state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) state->texW;
        *outV1 = (float) (state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) state->texH;

        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void glDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
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

    float lineStride = TextUtils_lineStride(font);

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

        // Render each glyph in the line
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float u0, v0, u1, v1;
            float localX0, localY0;
            GLuint glyphTexId;

            if (!glResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &glyphTexId, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                cursorX += glyph->shift;
                continue;
            }

            // Flush if texture changed or batch full
            glBindTexture(GL_TEXTURE_2D, glyphTexId);

            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            // Transform corners
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

            // Write 4 vertices
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

            // Advance cursor by glyph shift + kerning
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
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

static void glDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha) {
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

    float lineStride = TextUtils_lineStride(font);

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

    // get delta's  (16.16 format)
	int32_t left_r_dx = ((_c2 & 0xff0000) - (_c1 & 0xff0000)) / textLen;
	int32_t left_g_dx = ((((_c2 & 0xff00) << 8) - ((_c1 & 0xff00) << 8))) / textLen;
	int32_t left_b_dx = ((((_c2 & 0xff) << 16) - ((_c1 & 0xff) << 16))) / textLen;

	int32_t right_r_dx = ((_c3 & 0xff0000) - (_c4 & 0xff0000)) / textLen;
	int32_t right_g_dx = ((((_c3 & 0xff00) << 8) - ((_c4 & 0xff00) << 8))) / textLen;
	int32_t right_b_dx = ((((_c3 & 0xff) << 16) - ((_c4 & 0xff) << 16))) / textLen;

    int32_t left_delta_r = left_r_dx;
	int32_t left_delta_g = left_g_dx;
	int32_t left_delta_b = left_b_dx;
	int32_t right_delta_r = right_r_dx;
	int32_t right_delta_g = right_g_dx;
	int32_t right_delta_b = right_b_dx;

    int32_t c1 = _c1;
    int32_t c4 = _c4;

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

        // Render each glyph in the line
        int32_t pos = 0;
        while (lineLen > pos) {
            // do 16.16 maths
            int32_t c2 = ((c1 & 0xff0000) + (left_delta_r & 0xff0000)) & 0xff0000;
                c2 |= ((c1 & 0xff00) + (left_delta_g >> 8) & 0xff00) & 0xff00;
                c2 |= ((c1 & 0xff) + (left_delta_b >> 16)) & 0xff;
            int32_t c3 = ((c4 & 0xff0000) + (right_delta_r & 0xff0000)) & 0xff0000;
                c3 |= ((c4 & 0xff00) + (right_delta_g >> 8) & 0xff00) & 0xff00;
                c3 |= ((c4 & 0xff) + (right_delta_b >> 16)) & 0xff;

            left_delta_r += left_r_dx;
            left_delta_g += left_g_dx;
            left_delta_b += left_b_dx;
            right_delta_r += right_r_dx;
            right_delta_g += right_g_dx;
            right_delta_b += right_b_dx;

            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float u0, v0, u1, v1;
            float localX0, localY0;
            GLuint glyphTexId;

            if (!glResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &glyphTexId, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                cursorX += glyph->shift;
                continue;
            }

            // Flush if texture changed or batch full
            glBindTexture(GL_TEXTURE_2D, glyphTexId);

            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            // Transform corners
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

            // Write 4 vertices
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

            // Advance cursor by glyph shift + kerning
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
            c4 = c3;    // set left edge to be what the last right edge was....
		    c1 = c2;    //
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

// Sentinel base for fake TPAG offsets used by dynamic sprites
#define DYNAMIC_TPAG_OFFSET_BASE 0xD0000000u

// Finds a free dynamic texture page slot (glTextures[i] == 0), or appends a new one.
static uint32_t findOrAllocTexturePageSlot(GLLegacyRenderer* gl) {
    // Scan dynamic range for a reusable slot
    for (uint32_t i = gl->originalTexturePageCount; gl->textureCount > i; i++) {
        if (gl->glTextures[i] == 0) return i;
    }
    // No free slot found, grow the arrays
    uint32_t newPageId = gl->textureCount;
    gl->textureCount++;
    gl->glTextures = safeRealloc(gl->glTextures, gl->textureCount * sizeof(GLuint));
    gl->textureWidths = safeRealloc(gl->textureWidths, gl->textureCount * sizeof(int32_t));
    gl->textureHeights = safeRealloc(gl->textureHeights, gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = safeRealloc(gl->textureLoaded, gl->textureCount * sizeof(bool));
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
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t glCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 >= w || 0 >= h) return -1;


    uint8_t* pixels = safeMalloc((size_t) w * (size_t) h * 4);
    if (pixels == nullptr) return -1;

    // OpenGL Y is bottom-up, GML Y is top-down, so flip the Y coordinate
    int32_t glY = gl->gameH - y - h;
    glReadPixels(x, glY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Flip vertically (OpenGL reads bottom-to-top)
    size_t rowBytes = (size_t) w * 4;
    uint8_t* rowTemp = safeMalloc(rowBytes);
    repeat(h / 2, row) {
        uint8_t* top = pixels + row * rowBytes;
        uint8_t* bot = pixels + (h - 1 - row) * rowBytes;
        memcpy(rowTemp, top, rowBytes);
        memcpy(top, bot, rowBytes);
        memcpy(bot, rowTemp, rowBytes);
    }
    free(rowTemp);

    // Create a new GL texture from the captured pixels
    GLuint newTexId;
    glGenTextures(1, &newTexId);
    glBindTexture(GL_TEXTURE_2D, newTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    free(pixels);

    // Find or allocate slots for texture page, TPAG, and sprite
    uint32_t pageId = findOrAllocTexturePageSlot(gl);
    gl->glTextures[pageId] = newTexId;
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;

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

    // Register a fake offset in the tpagOffsetMap so DataWin_resolveTPAG works
    uint32_t fakeOffset = DYNAMIC_TPAG_OFFSET_BASE + tpagIndex;
    hmput(dw->tpagOffsetMap, fakeOffset, (int32_t) tpagIndex);

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, gl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->textureOffsets = safeMalloc(sizeof(uint32_t));
    sprite->textureOffsets[0] = fakeOffset;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    fprintf(stderr, "GL: Created dynamic sprite %u (%dx%d) from surface at (%d,%d)\n", spriteIndex, w, h, x, y);
    return (int32_t) spriteIndex;
}

static void glDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    GLLegacyRenderer* gl = (GLLegacyRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (gl->originalSpriteCount > (uint32_t) spriteIndex) {
        fprintf(stderr, "GL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Clean up GL texture, TPAG entries, and tpagOffsetMap entries
    repeat(sprite->textureCount, i) {
        uint32_t offset = sprite->textureOffsets[i];
        if (offset >= DYNAMIC_TPAG_OFFSET_BASE) {
            int32_t tpagIdx = DataWin_resolveTPAG(dw, offset);
            if (tpagIdx >= 0) {
                TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
                int16_t pageId = tpag->texturePageId;
                if (pageId >= 0 && gl->textureCount > (uint32_t) pageId) {
                    glDeleteTextures(1, &gl->glTextures[pageId]);
                    gl->glTextures[pageId] = 0;
                }
                // Mark TPAG slot as free for reuse
                tpag->texturePageId = -1;
            }
            // Remove the fake offset from the lookup map
            hmdel(dw->tpagOffsetMap, offset);
        }
    }

    // Clear the sprite entry so it won't be drawn and can be reused. Preserve `name` across the memset: the slot is still in sprt.count and must keep a valid string for asset_get_index / name lookups.
    free(sprite->textureOffsets);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    fprintf(stderr, "GL: Deleted sprite %d\n", spriteIndex);
}

// ===[ Vtable ]===

static RendererVtable glVtable = {
    .init = glInit,
    .destroy = glDestroy,
    .beginFrame = glBeginFrame,
    .endFrame = glEndFrame,
    .beginView = glBeginView,
    .endView = glEndView,
    .beginGUI = glBeginGUI,
    .endGUI = glEndGUI,
    .drawSprite = glDrawSprite,
    .drawSpritePart = glDrawSpritePart,
    .drawRectangle = glDrawRectangle,
    .drawLine = glDrawLine,
    .drawLineColor = glDrawLineColor,
    .drawTriangle = glDrawTriangle,
    .drawText = glDrawText,
    .drawTextColor = glDrawTextColor,
    .flush = glRendererFlush,
    .createSpriteFromSurface = glCreateSpriteFromSurface,
    .deleteSprite = glDeleteSprite,
    .drawTile = nullptr,
};

// ===[ Public API ]===

Renderer* GLLegacyRenderer_create(void) {
    GLLegacyRenderer* gl = safeCalloc(1, sizeof(GLLegacyRenderer));
    gl->base.vtable = &glVtable;
    gl->base.drawColor = 0xFFFFFF; // white (BGR)
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    return (Renderer*) gl;
}
