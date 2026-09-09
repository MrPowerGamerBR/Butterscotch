#ifndef _SW_SURFACE_H
#define _SW_SURFACE_H

SWSurface* swrCreateSurface(int width, int height);
void swrFreeSurface(SWSurface* surface);
SWTexture* swrWritableSurfaceTexture(SWRenderer* swr, int surfaceID);
void swrCommitShadowWritesToSurfaceIfNeeded(SWRenderer* swr, SWSurface* surface);
int32_t swrFindSurfaceTextureSlot(SWRenderer* swr);
int32_t swrFindSurfaceTPagSlot(SWRenderer* swr);

#endif//_SW_SURFACE_H
