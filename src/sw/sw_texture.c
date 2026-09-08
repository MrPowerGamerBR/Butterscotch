#include <stdlib.h>
#include "sw_renderer_private.h"

SWTexture* swrCreateTextureEx(const void* srcBuffer, int width, int height, bool convert)
{
    SWTexture* txt = (SWTexture*) safeMalloc(sizeof(SWTexture));
    txt->buffer = (uintpixel_t*) safeMalloc(width * height * sizeof(uintpixel_t));
    
    size_t sz = width * height;
    
    if (srcBuffer)
    {
        if (convert) {
            const uint32_t* rgbaSrc = (const uint32_t*) srcBuffer;
            for (size_t i = 0; i < sz; i++)
                txt->buffer[i] = swrConvertPixelTexture(rgbaSrc[i]);
        }
        else {
            const uintpixel_t* rgbaSrc = (const uintpixel_t*) srcBuffer;
            for (size_t i = 0; i < sz; i++)
                txt->buffer[i] = rgbaSrc[i];
        }
    }
    else
    {
        memset(txt->buffer, 0, width * height * sizeof(uintpixel_t));
    }
    
    txt->width = (uint16_t) width;
    txt->height = (uint16_t) height;
    
    return txt;
}

SWTexture* swrCreateTexture(const uint8_t* srcBuffer, int width, int height)
{
    return swrCreateTextureEx(srcBuffer, width, height, true);
}

SWTexture* swrCopyTexture(SWTexture* texture)
{
    return swrCreateTextureEx(texture->buffer, texture->width, texture->height, false);
}

void swrFreeTexture(SWTexture* texture)
{
    if (UNLIKELY(!texture))
        return;
    
    free(texture->buffer);
    free(texture);
}

SWTexture* swrCropSectionFromTexture(SWTexture* src, int32_t width, int32_t height, int32_t cropLeft, int32_t cropTop, int32_t cropRight, int32_t cropBottom)
{
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "SWR: Cannot resize texture to %dx%d.\n", width, height);
        return NULL;
    }
    
    if (cropLeft < 0) cropLeft = 0;
    if (cropTop < 0) cropTop = 0;
    if (cropRight >= src->width) cropRight = src->width;
    if (cropBottom >= src->height) cropBottom = src->height;
    
    if (cropLeft >= cropRight || cropTop >= cropBottom) {
        fprintf(stderr, "SWR: Invalid crop coordinates for resize.\n");
        return NULL;
    }
    
    int cropWidth = cropRight - cropLeft;
    int cropHeight = cropBottom - cropTop;
    
    SWTexture* dst = swrCreateTexture(NULL, width, height);
    int32_t *mapRows = NULL, *mapCols = NULL;
    
    if (cropWidth != width) {
        mapCols = (int32_t *)safeCalloc(width, sizeof(int32_t));
        for (int32_t i = 0; i < width; i++)
            mapCols[i] = i * (cropRight - cropLeft) / width + cropLeft;
    }
    if (cropHeight != height) {
        mapRows = (int32_t *)safeCalloc(height, sizeof(int32_t));
        for (int32_t i = 0; i < height; i++)
            mapRows[i] = i * (cropBottom - cropTop) / height + cropTop;
    }
    
    for (int32_t y = 0, y1 = cropTop; y < height; y++, y1++)
    {
        uintpixel_t* dstbuf = &dst->buffer[y * width];
        const uintpixel_t* srcbuf = &src->buffer[(mapRows ? mapRows[y] : y1) * src->width];
        
        if (width == cropWidth)
        {
            for (int32_t x = 0, x1 = cropLeft; x < width; x++, x1++) {
                dstbuf[x] = srcbuf[x1];
            }
        }
        else
        {
            for (int32_t x = 0; x < width; x++) {
                dstbuf[x] = srcbuf[mapCols[x]];
            }
        }
    }
    
    if (mapCols) free(mapCols);
    if (mapRows) free(mapRows);
    
    return dst;
}

// Emulates the `removeback` flag from `sprite_create_from_surface`.
void swrRemoveBackgroundFromTexture(SWTexture* texture)
{
    // bottom left pixel
    uintpixel_t background = texture->buffer[texture->width * (texture->height - 1)];
#ifdef TRANSPARENT_MASK
    background &= ~TRANSPARENT_MASK;
#endif
    
    size_t widthheight = texture->width * texture->height;
    for (size_t i = 0; i < widthheight; i++)
    {
#ifdef TRANSPARENT_MASK
        if ((texture->buffer[i] & ~TRANSPARENT_MASK) == background)
#else
        if (texture->buffer[i] == background)
#endif
        {
#ifdef PXL_TRANSPARENT
            texture->buffer[i] = PXL_TRANSPARENT;
#else
            texture->buffer[i] = 0;
#endif
        }
    }
}
