#pragma once

#include "common.h"
#include "renderer.h"
#include <gsKit.h>

// ===[ GsRendererFlat Struct ]===
// Simple PS2 renderer using gsKit ONE SHOT mode.
// Renders all sprites/text as colored rectangles (no textures).
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GSGLOBAL* gsGlobal;

    // View transform state (set each view in beginView)
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    int32_t viewX;
    int32_t viewY;
} GsRendererFlat;

Renderer* GsRendererFlat_create(GSGLOBAL* gsGlobal);
