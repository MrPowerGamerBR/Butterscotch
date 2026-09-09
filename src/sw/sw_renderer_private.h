#ifndef _SW_RENDERER_PRIVATE_H
#define _SW_RENDERER_PRIVATE_H

#include "sw_renderer.h"
#include "defines.h"
#include "pixel_convert.h"

// Unimplemented Functions
#define UNIMP() do { fprintf(stderr, "NYI %s\n", __func__); } while (0)
//#define UNIMP() do { } while (0)
#define UNIMP2() do { } while (0)

// Provide PI if not specified
#ifndef M_PI
#define M_PI 3.1415926535897932384626
#endif

// Configurable Properties
#define TEXTURE_LRU_LENGTH 64
#define SURFACE_MAX_COUNT 64
// (NOTE: See PIXEL_SIZE in defines.h)


// Struct Definitions
typedef struct
{
    uintpixel_t* buffer;
    uint16_t width, height;
}
SWTexture;

typedef struct
{
    // used for almost all intents and purposes.
    SWTexture* texture;
    // upon a gpuSetColorWriteEnable change, the shadow texture is used for writing instead.
    SWTexture* shadowTexture;
}
SWSurface;

#define WRITE_MASK_ALL   (15)
#define WRITE_MASK_RED   (1)
#define WRITE_MASK_GREEN (2)
#define WRITE_MASK_BLUE  (4)
#define WRITE_MASK_ALPHA (8)

typedef struct
{
    Renderer base;
    
    // Window Properties
    uint16_t width;
    uint16_t height;
    // Framebuffer
    uintpixel_t* fb;
    uint16_t fbPitch; // in sizeof(uintpixel_t) units, NOT in bytes!
    
    bool drawingToSurface;
    uintpixel_t* mainFb;
    uint16_t mainWidth;
    uint16_t mainHeight;
    uint16_t mainPitch;
    int lastViewX, lastViewY, lastViewW, lastViewH;
    int lastPortX, lastPortY, lastPortW, lastPortH;
    int lastGameW, lastGameH, lastMaxX, lastMaxY;
    float lastScaleX, lastScaleY;
    
    SWTexture** textures;
    SWSurface** surfaces;
    uint32_t* textureIndexLRU;
    uint32_t textureIndexLRUHead;
    uint32_t textureIndexLRUTail;
    size_t textureCount;
    size_t surfaceCount;
    size_t totalTextureCount;
    size_t originalTPagCount;
    size_t originalSpriteCount;
    
    bool viewActive;
    int viewX, viewY, viewW, viewH;
    int portX, portY, portW, portH;
    int gameW, gameH, maxX, maxY;

    int offsetX, offsetY;
    float scaleX, scaleY;
    float defaultScaleX, defaultScaleY;
    
    int blendMode;
    
    // only used for surfaces.  The application surface doesn't support these at the moment.
    int currentSurfaceIndex;
    int writeMask;
}
SWRenderer;

// Inlined function definitions included below
#include "sw_pixel_calc.h"
#include "sw_inlined.h"
#include "sw_transform.h"

#include "sw_texture.h"
#include "sw_surface.h"
#include "sw_drawing.h"
#include "sw_texture_lru.h"

#endif//_SW_RENDERER_PRIVATE_H
