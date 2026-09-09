#ifndef _SW_RENDERER_H
#define _SW_RENDERER_H

#include "renderer.h"

Renderer* SWRenderer_create(void);

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color);

#endif//_SW_RENDERER_H
