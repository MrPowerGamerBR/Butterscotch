#include <stdlib.h>
#include "sw_renderer_private.h"

SWSurface* swrCreateSurface(int width, int height)
{
    SWSurface* surf = (SWSurface*) safeMalloc(sizeof(SWSurface));
    surf->texture = swrCreateTexture(NULL, width, height);
    surf->shadowTexture = NULL;
    return surf;
}

void swrFreeSurface(SWSurface* surface)
{
    if (UNLIKELY(!surface))
        return;
    
    swrFreeTexture(surface->texture);
    swrFreeTexture(surface->shadowTexture);
    free(surface);
}

SWTexture* swrWritableSurfaceTexture(SWRenderer* swr, int surfaceID)
{
    if (UNLIKELY(swr->surfaces[surfaceID]->shadowTexture)) {
        return swr->surfaces[surfaceID]->shadowTexture;
    }
    
    if (LIKELY(swr->writeMask == WRITE_MASK_ALL)) {
        return swr->surfaces[surfaceID]->texture;
    }
    
    if (UNLIKELY(!swr->surfaces[surfaceID]->shadowTexture)) {
        swr->surfaces[surfaceID]->shadowTexture = swrCopyTexture(swr->surfaces[surfaceID]->texture);
    }
    
    return swr->surfaces[surfaceID]->shadowTexture;
}

void swrCommitShadowWritesToSurfaceIfNeeded(SWRenderer* swr, SWSurface* surface)
{
    if (LIKELY(!swr->drawingToSurface))
        return;
    
    if (UNLIKELY(!surface))
        return;
    
    if (UNLIKELY(!surface->shadowTexture))
        return;
    
    if (LIKELY(swr->writeMask == WRITE_MASK_ALL)) {
        swrFreeTexture(surface->texture);
        surface->texture = surface->shadowTexture;
        surface->shadowTexture = NULL;
        return;
    }
    
    if (UNLIKELY(swr->writeMask == 0)) {
        swrFreeTexture(surface->shadowTexture);
        surface->shadowTexture = NULL;
        return;
    }
    
    uintpixel_t mask = 0;
#if PIXEL_SIZE == 32
    Pixel32ARGB x;
    x.l = 0;
    if (swr->writeMask & WRITE_MASK_RED)   x.p.r = 255;
    if (swr->writeMask & WRITE_MASK_GREEN) x.p.g = 255;
    if (swr->writeMask & WRITE_MASK_BLUE)  x.p.b = 255;
    if (swr->writeMask & WRITE_MASK_ALPHA) x.p.a = 255;
    mask = x.l;
#elif PIXEL_SIZE == 16
    if (swr->writeMask & WRITE_MASK_RED)   l |= 0x7C00;
    if (swr->writeMask & WRITE_MASK_GREEN) l |= 0x03E0;
    if (swr->writeMask & WRITE_MASK_BLUE)  l |= 0x001F;
    if (swr->writeMask & WRITE_MASK_ALPHA) l |= 0x8000;
#else
    //although it DOES ues rgb332, needs special handling for ALPHA
    fprintf(stderr, "swr: Unimplemented color masking for 8-bit mode TODO\n");
    swrFreeTexture(surface->texture);
    surface->texture = surface->shadowTexture;
    surface->shadowTexture = NULL;
    return;
#endif

    uintpixel_t invmask = ~mask;
    size_t max = surface->texture->width * surface->texture->height;
    for (size_t i = 0; i < max; i++)
    {
        surface->shadowTexture->buffer[i] =
        surface->texture->buffer[i] = (surface->texture->buffer[i] & invmask) | (surface->shadowTexture->buffer[i] & mask);
    }
}

// TODO[MrPowerGamerBR]: This is supposed to be refactored, not to modify data.win structs directly.
int32_t swrFindSurfaceTextureSlot(SWRenderer* swr)
{
    // NOTE: dynamic textures are not enrolled into the eviction cache for
    // hopefully obvious reasons ...
    for (size_t i = swr->textureCount; i != swr->totalTextureCount; i++)
    {
        if (swr->textures[i] == NULL) {
            return (int32_t) i;
        }
    }
    
    return -1;
}

int32_t swrFindSurfaceTPagSlot(SWRenderer* swr)
{
    DataWin* dw = swr->base.dataWin;
    for (size_t i = swr->originalTPagCount; i != dw->tpag.count; i++)
    {
        if (dw->tpag.items[i].texturePageId == -1) {
            return (int32_t) i;
        }
    }
    
    return -1;
}
