#ifndef _BS_GL_RENDERER_H_
#define _BS_GL_RENDERER_H_

#include "common.h"
#include "renderer.h"
#include "runner.h"
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__SWITCH__)
#include <GLES3/gl3.h>
#elif PLATFORM_VITA
#include <vitaGL.h>
#define GL_BOOL 0x8B56
#else
#include <glad/glad.h>
#endif

typedef enum {
    BATCHTYPE_QUAD,
    BATCHTYPE_TRIANGLE
} BatchType;

// ===[ GLRenderer Struct ]===
typedef struct {
    char* name; // owned
    int32_t location;
    GLenum type;
    uint32_t samplerSlot;
} GLShaderUniform;

typedef struct {
    GLuint shaderId;
    bool compiled;
    uint32_t uniformCount;
    GLShaderUniform* uniforms;

    // cached uniforms
    GLShaderUniform* gmBaseTexture;
    GLShaderUniform* gmMatrices;
    GLShaderUniform* gmFogColour;
    GLShaderUniform* gmAlphaTestEnabled;
    GLShaderUniform* gmAlphaRefValue;
} GMLShader;

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} Vertex;

// Exposed in the header so platform-specific code (main.c) can access FBO fields for screenshots.
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GMLShader* defaultShaderProgram;
    GMLShader* gmlShaders;
    uint32_t gmlShaderCount;

    bool alphaTestEnable;
    float alphaTestRef;
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool fogEnable;
    uint32_t fogColor; // BGR

    GLuint vao, vbo, ebo;
    Vertex* vertexData; // MAX_QUADS * VERTICES_PER_QUAD vertices

    BatchType batchType;
    int32_t primitiveType;
    int32_t primitiveVertexCount;
    GLuint primitiveTextureId;
    bool primitiveHasTexture;
    int32_t batchCount;
    GLuint currentTextureId;

    GLuint* glTextures;       // one GL texture per TXTR page
    int32_t* textureWidths;   // needed for UV normalization
    int32_t* textureHeights;
    bool* textureLoaded;      // lazy loading: true once PNG decoded and uploaded
    uint32_t textureCount;

    GLuint whiteTexture; // 1x1 white pixel for drawing primitives (rectangles, lines, etc.)

    int32_t windowW; // stored from beginFrame for endFrame blit
    int32_t windowH;
    int32_t gameW; // game width (matches the application_surface size)
    int32_t gameH; // game height (matches the application_surface size)

    GLuint hostFramebuffer; // present target for the composited frame, where 0 == the window

    // Original counts from data.win (dynamic slots start at these indices)
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
    GLuint* surfaces;
    GLuint* surfaceTexture;
    int32_t* surfaceWidth;
    int32_t* surfaceHeight;
    uint32_t surfaceCount;

    // Blending mode + factors
    bool blendEnable;
    int32_t currentBlendMode;
    int32_t currentSFactor;
    int32_t currentDFactor;
    int32_t currentSFactorAlpha;
    int32_t currentDFactorAlpha;

    bool isGL3; // TRUE if running on OpenGL (ES) 3.x+
    bool isGLES;  // TRUE if running on OpenGL ES (GLES)

    // Cached default shader uniforms
    GLShaderUniform* uWorldViewProjection;
    GLShaderUniform* uFogColor;
    GLShaderUniform* uAlphaTestRef;
    GLShaderUniform* uAlphaTestEnabled;
    GLShaderUniform* uTexture;
} GLRenderer;

bool GLRenderer_ensureTextureLoaded(GLRenderer* gl, uint32_t pageId);
Renderer* GLRenderer_create(void);

#endif /* _BS_GL_RENDERER_H_ */
