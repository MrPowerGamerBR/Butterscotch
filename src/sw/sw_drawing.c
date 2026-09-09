#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include "text_utils.h"
#include "sw_renderer_private.h"

// ==== Internal structures ====

typedef struct
{
    Font* font;
    TexturePageItem* fontTpag; // single TPAG for regular fonts (NULL for sprite fonts)
    int fontTpagIndex;
    int fontPageId;
    Sprite* spriteFontSprite; // source sprite for sprite fonts (NULL for regular fonts)
}
SwrFontState;

// ==== Internal functions ====

FORCE_INLINE void swrPlotPixel(Renderer* renderer, int x, int y, uintpixel_t color, int srcalpha, int dstalpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (x < swr->portX || y < swr->portY) return;
    if (x >= swr->maxX || y >= swr->maxY) return;
    
    alphaBlend(&swr->fb[y * swr->fbPitch + x], color, srcalpha, dstalpha);
}

static void swrDrawHLineInt(Renderer* renderer, int dx, int dy, int dw, uintpixel_t color, UNUSED uintpixel_t color2, int alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    if (dy < swr->portY) return;
    if (dy >= swr->maxY) return;
    if (dx < swr->portX) { dw += dx - swr->portX; dx = swr->portX; }
    if (dx + dw >= swr->maxX) dw = swr->maxX - dx;
    if (dw <= 0) return;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
#if PIXEL_SIZE == 32
    if (color == color2)
#endif
    {
        uintpixel_t *line = &swr->fb[dy * swr->fbPitch + dx];
        for (int i = 0; i < dw; i++)
            alphaBlend(&line[i], color, srcalpha, invalpha);
    }
#if PIXEL_SIZE == 32
    else
    {
        Pixel32ARGB clr1, clr2;
        clr1.l = color;
        clr2.l = color2;
        
        uint32_t rinit = clr1.p.r << 20;
        uint32_t ginit = clr1.p.g << 20;
        uint32_t binit = clr1.p.b << 20;
        int32_t rstep = ((int)clr2.p.r - clr1.p.r) << 20;
        int32_t gstep = ((int)clr2.p.g - clr1.p.g) << 20;
        int32_t bstep = ((int)clr2.p.b - clr1.p.b) << 20;
        rstep /= dw;
        gstep /= dw;
        bstep /= dw;
        
        uintpixel_t *line = &swr->fb[dy * swr->fbPitch + dx];
        for (int i = 0; i < dw; i++)
        {
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            alphaBlend(&line[i], resultPixel.l, srcalpha, invalpha);
        }
    }
#endif
}

static void swrDrawVLineInt(Renderer* renderer, int dx, int dy, int dh, uintpixel_t color, UNUSED uintpixel_t color2, int alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    if (dx < swr->portX) return;
    if (dx >= swr->maxX) return;
    if (dy < swr->portY) { dh += dy - swr->portY; dy = swr->portY; }
    if (dy + dh >= swr->maxY) dh = swr->maxY - dy;
    if (dh <= 0) return;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
#if PIXEL_SIZE == 32
    if (color == color2)
#endif
    {
        for (int i = 0; i < dh; i++)
        {
            uintpixel_t *line = &swr->fb[(dy + i) * swr->fbPitch + dx];
            alphaBlend(&line[0], color, srcalpha, invalpha);
        }
    }
#if PIXEL_SIZE == 32
    else
    {
        Pixel32ARGB clr1, clr2;
        clr1.l = color;
        clr2.l = color2;
        
        uint32_t rinit = clr1.p.r << 20;
        uint32_t ginit = clr1.p.g << 20;
        uint32_t binit = clr1.p.b << 20;
        int32_t rstep = ((int)clr2.p.r - clr1.p.r) << 20;
        int32_t gstep = ((int)clr2.p.g - clr1.p.g) << 20;
        int32_t bstep = ((int)clr2.p.b - clr1.p.b) << 20;
        rstep /= dh;
        gstep /= dh;
        bstep /= dh;
        
        for (int i = 0; i < dh; i++)
        {
            uintpixel_t *line = &swr->fb[(dy + i) * swr->fbPitch + dx];
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            alphaBlend(&line[0], resultPixel.l, srcalpha, invalpha);
        }
    }
#endif
}

static void swrDrawLineInt(Renderer* renderer, int x1, int y1, int x2, int y2, MAYBE_UNUSED int width, uintpixel_t color1, uintpixel_t color2, int alpha)
{
    if (x1 == x2)
    {
        swrDrawVLineInt(renderer, x1, swrMin(y1, y2), swrAbs(y1 - y2), color1, color2, alpha);
        return;
    }
    if (y1 == y2)
    {
        swrDrawHLineInt(renderer, swrMin(x1, x2), y1, swrAbs(x1 - x2), color1, color2, alpha);
        return;
    }
    
    int dx = x2 - x1, dy = y2 - y1;
    int dx1 = swrAbs(dx), dy1 = swrAbs(dy), xe, ye, x, y;
    int px = 2 * dy1 - dx1, py = 2 * dx1 - dy1;
    int srcalpha = swrCalcSrcAlpha((SWRenderer*) renderer, alpha);
    int invalpha = swrCalcDstAlpha((SWRenderer*) renderer, alpha);
    
    uintpixel_t color = color1;
#if PIXEL_SIZE == 32
    Pixel32ARGB clr1, clr2;
    clr1.l = color1;
    clr2.l = color2;
    
    uint32_t rinit = clr1.p.r << 20;
    uint32_t ginit = clr1.p.g << 20;
    uint32_t binit = clr1.p.b << 20;
    int32_t rstep = ((int)clr2.p.r - clr1.p.r) << 20;
    int32_t gstep = ((int)clr2.p.g - clr1.p.g) << 20;
    int32_t bstep = ((int)clr2.p.b - clr1.p.b) << 20;
#endif
    
    if (dy1 <= dx1)
    {
        if (dx >= 0)
        {
            x = x1, y = y1, xe = x2;
        }
        else
        {
            x = x2, y = y2, xe = x1;
        }
        
#if PIXEL_SIZE == 32
        if (dx1 > 0) {
            rstep /= dx1;
            gstep /= dx1;
            bstep /= dx1;
        } else {
            rstep = gstep = bstep = 0;
        }
#endif
        
        swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        
        while (x < xe)
        {
            x++;
            if (px < 0)
            {
                px += 2 * dy1;
            }
            else
            {
                if ((dx < 0 && dy < 0) || (dx > 0 && dy > 0)) y++; else y--;
                px += 2 * (dy1 - dx1);
            }
            
#if PIXEL_SIZE == 32
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            color = resultPixel.l;
#endif
            
            swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        }
    }
    else
    {
        if (dy >= 0)
        {
            x = x1, y = y1, ye = y2;
        }
        else
        {
            x = x2, y = y2, ye = y1;
        }
        
#if PIXEL_SIZE == 32
        if (dy1 > 0) {
            rstep /= dy1;
            gstep /= dy1;
            bstep /= dy1;
        } else {
            rstep = gstep = bstep = 0;
        }
#endif
        
        swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        
        while (y < ye)
        {
            y++;
            if (py <= 0)
            {
                py += 2 * dx1;
            }
            else
            {
                if ((dx < 0 && dy < 0) || (dx > 0 && dy > 0)) x++; else x--;
                py += 2 * (dx1 - dy1);
            }
            
#if PIXEL_SIZE == 32
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            color = resultPixel.l;
#endif
            
            swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        }
    }
}

static void swrDrawSpriteInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha
)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    bool flipX = false, flipY = false;
    if (dw < 0) { dx += dw; dw = -dw; flipX = true; }
    if (dh < 0) { dy += dh; dh = -dh; flipY = true; }
    
    //basic out of bound checks
    if (dw == 0 || dh == 0) return;
    if (sw == 0) sw = 1;
    if (sh == 0) sh = 1;
    if (dx + dw <= swr->portX) return;
    if (dy + dh <= swr->portY) return;
    if (dx >= swr->maxX) return;
    if (dy >= swr->maxY) return;
    
    int odw = dw, odh = dh;
    int osw = sw, osh = sh;
    
    int minx = swr->portX, miny = swr->portY, maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    
    //out of bounds adjustment checks
    int diffxl = 0, diffyl = 0, diffxu = 0, diffyu = 0;
    if (dx < minx) { diffxl = minx - dx; dx = minx; dw -= diffxl; }
    if (dy < miny) { diffyl = miny - dy; dy = miny; dh -= diffyl; }
    if (dx + dw > maxx) { diffxu = dx + dw - maxx; dw -= diffxu; }
    if (dy + dh > maxy) { diffyu = dy + dh - maxy; dh -= diffyu; }
    
    if (diffxl != 0 || diffyl != 0 || diffxu != 0 || diffyu != 0)
    {
        //adjust source coordinates too
        diffxl = (int)((long)diffxl * osw / odw);
        diffyl = (int)((long)diffyl * osh / odh);
        diffxu = (int)((long)(diffxu + 1) * osw / odw);
        diffyu = (int)((long)(diffyu + 1) * osh / odh);
        sx += flipX ? diffxu : diffxl;
        sy += flipY ? diffyu : diffyl;
        sw -= diffxl + diffxu;
        sh -= diffyl + diffyu;
        if (sw <= 0 || sh <= 0) return;
    }
    
    //clip the source coords into bounds too
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw >= texture->width)  { sw = texture->width  - sx; }
    if (sy + sh >= texture->height) { sh = texture->height - sy; }
    if (sw <= 0 || sh <= 0) return;
    
    //okay, now we can finally get on with rendering
    
    int ixs = 0, oxs = 1, iys = 0, oys = 1;
    if (flipX) ixs = dw - 1, oxs = -1;
    if (flipY) iys = dh - 1, oys = -1;
    
    // tweak these if stuff doesn't look right
    typedef int32_t fixedp_t;
    const int fp_prec = 14;

    fixedp_t ystep = (sh == dh) ? (1 << fp_prec) : ((fixedp_t) osh << fp_prec) / odh;
    fixedp_t xstep = (sw == dw) ? (1 << fp_prec) : ((fixedp_t) osw << fp_prec) / odw;
    fixedp_t oxs2 = oxs * xstep;
    fixedp_t oys2 = oys * ystep;
    fixedp_t ixs2 = ixs * xstep;
    fixedp_t iys2 = iys * ystep;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
    if (sw == dw)
    {
        fixedp_t ys2 = (fixedp_t) iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline;
            const uintpixel_t* srcline;
            dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];
            
            for (int x = 0, xs = ixs; x < dw; x++, xs += oxs)
            {
                uintpixel_t pixel = srcline[xs];
                if (opaque(pixel))
                    alphaBlend(&dstline[x], tint(tintColor, pixel), srcalpha, invalpha);
            }
        }
    }
    else
    {
        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline;
            const uintpixel_t* srcline;
            dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];
            
            fixedp_t xs2 = ixs2;
            for (int x = 0, xs = ixs; x < dw; x++, xs += oxs, xs2 += oxs2)
            {
                uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                if (opaque(pixel))
                    alphaBlend(&dstline[x], tint(tintColor, pixel), srcalpha, invalpha);
            }
        }
    }
}

static void swrDrawSpriteRotatedInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha,
    float angleDeg,
    float pivotX,
    float pivotY
)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    float angleRad = -angleDeg * M_PI / 180.0f;
    
    bool flipX = false, flipY = false;
    if (dw < 0) { dw = -dw; dx -= dw; pivotX = dw - pivotX; flipX = true; }
    if (dh < 0) { dh = -dh; dy -= dh; pivotY = dh - pivotY; flipY = true; }
    
    float cosA = cosf(angleRad);
    float sinA = sinf(angleRad);
    
    float cnrx[4], cnry[4];
    cnrx[0] = cnrx[3] = dx;
    cnry[0] = cnry[1] = dy;
    cnrx[1] = cnrx[2] = dx + dw;
    cnry[2] = cnry[3] = dy + dh;
    
    float pxa = pivotX + dx;
    float pya = pivotY + dy;
    
    float minXf = FLT_MAX, minYf = FLT_MAX, maxXf = -FLT_MAX, maxYf = -FLT_MAX;
    for (int i = 0; i < 4; i++)
    {
        float cxi = cnrx[i] - pxa;
        float cyi = cnry[i] - pya;
        float rx = cosA * cxi - sinA * cyi + pxa;
        float ry = sinA * cxi + cosA * cyi + pya;
        if (minXf > rx) minXf = rx;
        if (maxXf < rx) maxXf = rx;
        if (minYf > ry) minYf = ry;
        if (maxYf < ry) maxYf = ry;
    }

    // minX, minY, maxX, maxY now represent an AABB of pixels we should loop over
    int minX = swrFloor(minXf);
    int minY = swrFloor(minYf);
    int maxX = swrCeiling(maxXf);
    int maxY = swrCeiling(maxYf);
    
    // basic out-of-bound checks
    if (maxX < swr->portX) return;
    if (maxY < swr->portY) return;
    if (minX >= swr->maxX) return;
    if (minY >= swr->maxY) return;
    
    // however, we'll need to clip it against out of bounds first
    int minXc = minX, minYc = minY, maxXc = maxX, maxYc = maxY;
    int minx = swr->portX, miny = swr->portY, maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    
    if (minXc < minx) minXc = minx;
    if (minYc < miny) minYc = miny;
    if (maxXc >= maxx) maxXc = maxx;
    if (maxYc >= maxy) maxYc = maxy;
    
    // some final clip checks
    if (minXc >= maxXc || minYc >= maxYc) return;
    
    int sox = flipX ? sw - 1 : 0;
    int soy = flipY ? sh - 1 : 0;
    int six = flipX ? -1 : 1;
    int siy = flipY ? -1 : 1;
    
    float sw_dw = (float) sw / dw;
    float sh_dh = (float) sh / dh;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
    for (int cy = minYc; cy < maxYc; cy++)
    {
        uintpixel_t *dstline = &swr->fb[cy * swr->fbPitch];
        for (int cx = minXc; cx < maxXc; cx++)
        {
            // we need to determine the texture-space coordinate of cx/cy
            float ox = (float) cx + 0.5f - pxa;
            float oy = (float) cy + 0.5f - pya;
            
            // "undo" the rotation
            float lx =  cosA * ox + sinA * oy;
            float ly = -sinA * ox + cosA * oy;
            
            // turn it into a texture-local coordinate
            lx += pxa - dx;
            ly += pya - dy;
            
            if (lx < 0 || ly < 0 || lx >= (float) dw || ly >= (float) dh) continue;
            
            lx = lx * sw_dw;
            ly = ly * sh_dh;
            
            int tx = (int)(sox + lx * six);
            int ty = (int)(soy + ly * siy);
            
            if (tx < 0) tx = 0;
            if (ty < 0) ty = 0;
            if (tx >= sw) tx = sw - 1;
            if (ty >= sh) ty = sh - 1;
            
            tx += sx;
            ty += sy;
            
            uintpixel_t src = texture->buffer[ty * texture->width + tx];
            
            if (opaque(src))
                alphaBlend(&dstline[cx], tint(tintColor, src), srcalpha, invalpha);
        }
    }
}

static void swrDrawTriangleInternal(SWRenderer* swr, int xup, int yup, int xleft, int yleft, int xright, int yright, uint32_t color1, uint32_t color2, uint32_t color3, int alpha)
{
    // TODO: update this
    (void) color2;
    (void) color3;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
    // Figure out the maximum Y extent of the triangle.
    // (Note that we know yup is the minimum.)
    int xmid, ymid, xmid2 = xup, xmax, ymax;
    if (yleft < yright) {
        xmax = xright, ymax = yright;
        xmid = xleft, ymid = yleft;
        if (yright != yup)
            xmid2 = xup + (xright - xup) * (ymid - yup) / (yright - yup);
    } else {
        xmax = xleft, ymax = yleft;
        xmid = xright, ymid = yright;
        if (yleft != yup)
            xmid2 = xup + (xleft - xup) * (ymid - yup) / (yleft - yup);
    }
    
    for (int y = yup; y < ymax; y++)
    {
        if (y < 0) continue;
        if (y >= swr->height) break;
        
        int x1 = xup, x2 = xup;
        if (y <= ymid)
        {
            // Lines: between up and mid, and between up and max
            if (ymid != yup)
                x1 = xup + (xmid - xup) * (y - yup) / (ymid - yup);
            
            if (ymid != yup)
                x2 = xup + (xmid2 - xup) * (y - yup) / (ymid - yup);
        }
        else
        {
            // Lines: between mid and max, and between up and max
            if (ymax != yup)
                x1 = xup + (xmax - xup) * (y - yup) / (ymax - yup);
            
            if (ymax != ymid)
                x2 = xmid + (xmax - xmid) * (y - ymid) / (ymax - ymid);
        }
        
        if (x1 >= x2) {
            int tmp = x1;
            x1 = x2;
            x2 = tmp;
        }
        
        if (x1 < swr->portX) x1 = swr->portX;
        if (x1 >= swr->maxX) continue;
        if (x2 < swr->portX) continue;
        if (x2 >= swr->maxX) x2 = swr->maxX - 1;
        if (x1 > x2) continue;
        
        uintpixel_t* line = &swr->fb[y * swr->width];
        for (int x = x1; x < x2; x++) {
            alphaBlend(&line[x], color1, srcalpha, invalpha);
        }
    }
}

static bool swrResolveFontState(SWRenderer* swr, DataWin* dw, Font* font, SwrFontState* state)
{
    state->font = font;
    state->fontTpag = NULL;
    state->fontTpagIndex = 0;
    state->spriteFontSprite = NULL;
    
    if (font->isSpriteFont)
    {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    else
    {
        state->fontTpagIndex = font->tpagIndex;
        if (state->fontTpagIndex < 0) return false;
        
        state->fontTpag = &dw->tpag.items[state->fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= swr->totalTextureCount) return false;
        if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) return false;
        
        state->fontPageId = pageId;
    }
    
    return true;
}

static bool swrResolveGlyph(
    SWRenderer* swr, DataWin* dw, SwrFontState* state, FontGlyph* glyph, float cursorX, float cursorY,
    int* tpagIndex, int* pageId, int* sx, int* sy, int* sw, int* sh, float* dx, float* dy
)
{
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != NULL)
    {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= swr->totalTextureCount) return false;
        if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pid)) return false;

        *tpagIndex = tpagIdx;
        *pageId = glyphTpag->texturePageId;
        
        *sx = glyphTpag->sourceX;
        *sy = glyphTpag->sourceY;
        *sw = glyphTpag->sourceWidth;
        *sh = glyphTpag->sourceHeight;
        
        *dx = cursorX + glyph->offset;
        *dy = cursorY + glyphTpag->targetY - sprite->originY;
    }
    else
    {
        *tpagIndex = state->fontTpagIndex;
        *pageId = state->fontPageId;
        
        *sx = state->fontTpag->sourceX + glyph->sourceX;
        *sy = state->fontTpag->sourceY + glyph->sourceY;
        *sw = glyph->sourceWidth;
        *sh = glyph->sourceHeight;
        
        *dx = cursorX + glyph->offset;
        *dy = cursorY;
    }
    
    return true;
}

// ==== Exposed interface ====

bool swrSwitchToSurface(Renderer* renderer, int32_t targetSurfaceId, bool restoreOldView)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (swr->drawingToSurface) {
        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[swr->currentSurfaceIndex]);
    }
    
    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER)
    {
        if (!swr->drawingToSurface)
            return true;
        
        // restore the original framebuffer
        fprintf(stderr, "back to original framebuffer\n");
        swr->drawingToSurface = false;
        swr->fb = swr->mainFb;
        swr->width = swr->mainWidth;
        swr->height = swr->mainHeight;
        swr->fbPitch = swr->mainPitch;
        swr->blendMode = bm_normal;
        swr->currentSurfaceIndex = -1;
        swr->writeMask = WRITE_MASK_ALL;
        
        if (restoreOldView) {
            // restore the old transform, if needed
            swr->viewX = swr->lastViewX;
            swr->viewY = swr->lastViewY;
            swr->viewW = swr->lastViewW;
            swr->viewH = swr->lastViewH;
            swr->portX = swr->lastPortX;
            swr->portY = swr->lastPortY;
            swr->portW = swr->lastPortW;
            swr->portH = swr->lastPortH;
            swr->gameW = swr->lastGameW;
            swr->gameH = swr->lastGameH;
            swr->maxX = swr->lastMaxX;
            swr->maxY = swr->lastMaxY;
            swr->scaleX = swr->lastScaleX;
            swr->scaleY = swr->lastScaleY;
        }
        return true;
    }
    
    if (targetSurfaceId < 0 || (size_t) targetSurfaceId >= swr->surfaceCount || swr->surfaces[targetSurfaceId] == NULL) {
        fprintf(stderr, "swr: Invalid surface id %d\n", targetSurfaceId);
        return false;
    }
    
    if (!swr->drawingToSurface)
    {
        // back up the original framebuffer
        swr->drawingToSurface = true;
        swr->mainFb = swr->fb;
        swr->mainWidth = swr->width;
        swr->mainHeight = swr->height;
        swr->mainPitch = swr->fbPitch;
        swr->blendMode = bm_normal;
        
        // and the old transform
        swr->lastViewX = swr->viewX;
        swr->lastViewY = swr->viewY;
        swr->lastViewW = swr->viewW;
        swr->lastViewH = swr->viewH;
        swr->lastPortX = swr->portX;
        swr->lastPortY = swr->portY;
        swr->lastPortW = swr->portW;
        swr->lastPortH = swr->portH;
        swr->lastGameW = swr->gameW;
        swr->lastGameH = swr->gameH;
        swr->lastMaxX = swr->maxX;
        swr->lastMaxY = swr->maxY;
        swr->lastScaleX = swr->scaleX;
        swr->lastScaleY = swr->scaleY;
    }
    
    SWTexture* surface = swr->surfaces[targetSurfaceId]->texture;
    swr->fb = surface->buffer;
    swr->width = surface->width;
    swr->height = surface->height;
    swr->fbPitch = surface->width;
    swr->drawingToSurface = true;
    swr->blendMode = bm_normal;
    swr->currentSurfaceIndex = targetSurfaceId;
    swr->writeMask = WRITE_MASK_ALL;
    
    swr->viewX = swr->portX = 0;
    swr->viewY = swr->portY = 0;
    swr->maxX = swr->viewW = swr->portW = surface->width;
    swr->maxY = swr->viewH = swr->portH = surface->height;
    swr->scaleX = swr->scaleY = 1.0f;
    
    fprintf(stderr, "switching to surface %p, fb %p, %dx%d\n", surface, swr->fb, swr->width, swr->height);
    
    return true;
}

void swrDrawHLine(Renderer* renderer, float dx, float dy, float dw, uintpixel_t color, uintpixel_t color2, float alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    float thickness = 1;

    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &dw, &thickness);

    // TODO: use thickness
    swrDrawHLineInt(renderer, swrFloor(dx), swrFloor(dy), swrCeiling(dw), color, color2, swrIntAlpha(alpha));
}

void swrDrawVLine(Renderer* renderer, float dx, float dy, float dh, uintpixel_t color, uintpixel_t color2, float alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    float thickness = 1;

    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &thickness, &dh);
    
    // TODO: use thickness
    swrDrawVLineInt(renderer, swrFloor(dx), swrFloor(dy), swrCeiling(dh), color, color2, swrIntAlpha(alpha));
}

void swrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uintpixel_t color, uintpixel_t color2, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformSizeIfNeeded(swr, &width, NULL);
    swrDrawLineInt(renderer, swrFloor(x1), swrFloor(y1), swrCeiling(x2), swrCeiling(y2), swrCeiling(width), color, color2, swrIntAlpha(alpha));
}

void swrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha)
{
    swrDrawHLine(renderer, x1, y1, (x2 - x1) + 1, color, color, alpha);
    swrDrawHLine(renderer, x1, y2, (x2 - x1) + 1, color, color, alpha);
    swrDrawVLine(renderer, x1, y1, (y2 - y1) + 1, color, color, alpha);
    swrDrawVLine(renderer, x2, y1, (y2 - y1) + 1, color, color, alpha);
}

void swrDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha)
{
    swrDrawHLine(renderer, x1, y1, (x2 - x1) + 1, color1, color2, alpha);
    swrDrawHLine(renderer, x1, y2, (x2 - x1) + 1, color3, color4, alpha);
    swrDrawVLine(renderer, x1, y1, (y2 - y1) + 1, color1, color3, alpha);
    swrDrawVLine(renderer, x2, y1, (y2 - y1) + 1, color2, color4, alpha);
}

void swrFillRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);

    int alphaInt = swrIntAlpha(alpha);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return;
    
    for (int y = 0; y <= yd; y++) {
        swrDrawHLineInt(renderer, x1i, y1i + y, xd, pxcolor, pxcolor, alphaInt);
    }
}

void swrFillRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor1, uintpixel_t pxcolor2, uintpixel_t pxcolor3, uintpixel_t pxcolor4, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);

    int alphaInt = swrIntAlpha(alpha);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return;
    
    // TODO: blending vertically
    (void) pxcolor3;
    (void) pxcolor4;
    
    for (int y = 0; y <= yd; y++) {
        swrDrawHLineInt(renderer, x1i, y1i + y, xd, pxcolor1, pxcolor2, alphaInt);
    }
}

void swrDrawSprite(
    Renderer* renderer, float dx, float dy, float dw, float dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uint32_t tintColor, float alpha
)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &dw, &dh);
    
    swrDrawSpriteInternal(
        renderer,
        swrFloor(dx),
        swrFloor(dy),
        swrCeiling(dw),
        swrCeiling(dh),
        texture,
        sx, sy,
        sw, sh,
        swrConvertPixel(tintColor),
        swrIntAlpha(alpha)
    );
}

void swrDrawSpriteRotated(
    Renderer* renderer, float dx, float dy, float dw, float dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uint32_t tintColor, float alpha,
    float angleDeg,
    float pivotX,
    float pivotY
)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &pivotX, &pivotY);
    swrTransformSizeIfNeeded(swr, &dw, &dh);
    
    swrDrawSpriteRotatedInternal(
        renderer,
        swrFloor(dx),
        swrFloor(dy),
        swrCeiling(dw),
        swrCeiling(dh),
        texture,
        sx, sy,
        sw, sh,
        swrConvertPixel(tintColor),
        swrIntAlpha(alpha),
        angleDeg,
        pivotX,
        pivotY
    );
}

void swrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha)
{
    float xup, yup, xleft, yleft, xright, yright;
    uint32_t colorup, colorleft, colorright;
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformPosIfNeeded(swr, &x3, &y3);
    
    //which vertex is higher?
    xup = x1, yup = y1; colorup = color1;
    xleft = x2, yleft = y2; colorleft = color2;
    xright = x3, yright = y3; colorright = color3;
    if (yup > y2) {
        xup = x2, yup = y2, colorup = color2;
        xleft = x1, yleft = y1, colorleft = color1;
        //xright = x3, yright = y3;
    }
    if (yup > y3) {
        xup = x3, yup = y3, colorup = color3;
        xleft = x1, yleft = y1, colorleft = color1;
        xright = x2, yright = y2, colorright = color2;
    }
    
    if (xleft > xright) {
        float tmp = xleft;
        xleft = xright;
        xright = tmp;
        tmp = yleft;
        yleft = yright;
        yright = tmp;
        uint32_t tmp2 = colorleft;
        colorleft = colorright;
        colorright = tmp2;
    }
    
    swrDrawTriangleInternal(
        swr,
        swrFloor(xup), swrFloor(yup),
        swrFloor(xleft), swrCeiling(yleft),
        swrFloor(xright), swrCeiling(yright),
        swrConvertPixel(colorup),
        swrConvertPixel(colorleft),
        swrConvertPixel(colorright),
        swrIntAlpha(alpha)
    );
}

void swrDrawText(SWRenderer* swr, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t color, float alpha, float lineSeparation)
{
    Renderer* renderer = &swr->base;
    DataWin* dwin = renderer->dataWin;
    
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dwin->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dwin->font.fonts[fontIndex];
    
    SwrFontState fontState;
    memset(&fontState, 0, sizeof fontState); // silence warning treated as error
    
    if (!swrResolveFontState(swr, dwin, font, &fontState)) return;
    
    // TODO: do we need to mirror the way the text scrolls too?!
    float cosA = 1.0f, sinA = 0.0f, angleRad = 0.0f;
    bool mustRotate = swrMustRotateSensitive(angleDeg);
    if (UNLIKELY(mustRotate))
    {
        angleRad = -angleDeg * M_PI / 180.0f;
        cosA = cosf(angleRad);
        sinA = sinf(angleRad);
    }
    
    int textLen = (int) strlen(text);
    int lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;
    
    xscale *= font->scaleX;
    yscale *= font->scaleY;

    // Iterate through lines. HTML5 subtracts ascenderOffset from the per-line y offset
    // (see yyFont.GR_Text_Draw), shifting glyphs up so the baseline aligns with the drawn y.
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

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    int fontTpagIndex = 0, pageId = 0;
                    int sx, sy, sw, sh, dw, dh;
                    float dx, dy;
                    if (swrResolveGlyph(swr, dwin, &fontState, glyph, cursorX, cursorY,
                            &fontTpagIndex, &pageId, &sx, &sy, &sw, &sh, &dx, &dy))
                    {
                        dx *= xscale; dx += x;
                        dy *= xscale; dy += y;
                        dw = swrCeiling(xscale * glyph->sourceWidth);
                        dh = swrCeiling(yscale * glyph->sourceHeight);
                        
                        // TODO: at 640x480, for some reason, without this fixup the
                        // letters in the "Name the fallen human." screen don't shake
                        dx = roundf(dx * 2) / 2;
                        dy = roundf(dy * 2) / 2;
                        
                        SWTexture* texture = swr->textures[pageId];
                        
                        if (UNLIKELY(mustRotate))
                        {
                            dx -= x;
                            dy -= y;
                            float ndx = cosA * dx - sinA * dy;
                            float ndy = sinA * dx + cosA * dy;
                            ndx += x;
                            ndy += y;
                            swrDrawSpriteRotated(renderer, ndx, ndy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, 0.0f, 0.0f);
                        }
                        else
                        {
                            swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
                        }
                        
                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
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
