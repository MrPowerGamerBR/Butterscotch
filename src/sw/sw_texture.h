#ifndef _SW_TEXTURE_H
#define _SW_TEXTURE_H

SWTexture* swrCreateTextureEx(const void* srcBuffer, int width, int height, bool convert);
SWTexture* swrCreateTexture(const uint8_t* srcBuffer, int width, int height);
SWTexture* swrCopyTexture(SWTexture* texture);
void swrFreeTexture(SWTexture* texture);
SWTexture* swrCropSectionFromTexture(SWTexture* src, int32_t width, int32_t height, int32_t cropLeft, int32_t cropTop, int32_t cropRight, int32_t cropBottom);
void swrRemoveBackgroundFromTexture(SWTexture* texture);

#endif//_SW_TEXTURE_H
