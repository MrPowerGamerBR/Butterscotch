#ifndef _SW_DRAWING_H
#define _SW_DRAWING_H

bool swrSwitchToSurface(Renderer* renderer, int32_t targetSurfaceId, bool restoreOldView);
void swrDrawHLine(Renderer* renderer, float dx, float dy, float dw, uintpixel_t color, uintpixel_t color2, float alpha);
void swrDrawVLine(Renderer* renderer, float dx, float dy, float dh, uintpixel_t color, uintpixel_t color2, float alpha);
void swrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uintpixel_t color, uintpixel_t color2, float alpha);
void swrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha);
void swrDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha);
void swrFillRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha);
void swrFillRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha);
void swrDrawSprite(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture, int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha);
void swrDrawSpriteRotated(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture, int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha, float angleDeg, float pivotX, float pivotY);
void swrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha);
void swrDrawText(SWRenderer* swr, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t color, float alpha, float lineSeparation);

#endif//_SW_DRAWING_H
