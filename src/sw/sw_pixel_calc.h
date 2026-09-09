#ifndef _SW_PIXEL_CALC_H
#define _SW_PIXEL_CALC_H

#include "defines.h"
#include "pixel_convert.h"

// Check if a pixel is opaque.
//
// Later, this should be changed to perform full alpha-blending
// (at least in 32-bit pixel mode)
FORCE_INLINE bool opaque(uintpixel_t color)
{
#if PIXEL_SIZE == 8
    return (color != PXL_TRANSPARENT);
#else
    return (color & TRANSPARENT_MASK) != 0;
#endif
}

// Multiplies a color value (`color`) by another color value (`tintColor`).
FORCE_INLINE uintpixel_t tint(uintpixel_t tintColor, uintpixel_t color)
{
#if PIXEL_SIZE == 32
    Pixel32ARGB x, y;
    
    if ((tintColor & 0xFFFFFF) == 0xFFFFFF)
        return color;
    
    x.l = color;
    y.l = tintColor;
    
    x.p.b = (int)x.p.b * y.p.b / 255;
    x.p.g = (int)x.p.g * y.p.g / 255;
    x.p.r = (int)x.p.r * y.p.r / 255;
    return x.l;
#elif PIXEL_SIZE == 16
    if ((tintColor & 0x7FFF) == 0x7FFF)
        return color;
    
    int tcb = tintColor & 0x1F;
    int tcg = (tintColor >> 5) & 0x1F;
    int tcr = (tintColor >> 10) & 0x1F;
    
    int cb = color & 0x1F;
    int cg = (color >> 5) & 0x1F;
    int cr = (color >> 10) & 0x1F;
    int ca = color & 0x8000;
    
    cb = (cb * tcb) / 32;
    cg = (cg * tcg) / 32;
    cr = (cr * tcr) / 32;
    return ca | cb | (cg << 5) | (cr << 10);
#elif PIXEL_SIZE == 8
    // fast but hacky
    if (tintColor == 0xFF || tintColor == PXL_TRANSPARENT)
        return color;
    
    return color & tintColor;
#endif
}

// Performs alpha blending on a pixel, with another pixel.
//
// NOTE: alpha is between 0 and 256, NOT between 0 and 255!
//
// TODO: This routine could use some optimization.  Obviously I tried my best, but clearly
// it's still true that too many calculations are being performed.
//
// NOTE: Obviously I could use SIMD here, but old computers didn't have SIMD, and the code
// runs fast enough on modern computers to not need to do SIMD.
FORCE_INLINE void alphaBlend(uintpixel_t* dcolor, uintpixel_t scolor, int srcalpha, int dstalpha)
{
#if PIXEL_SIZE == 32 || PIXEL_SIZE == 16
    // it's so significant here we might as well fill in the whole color
    if (LIKELY(dstalpha < 3 && srcalpha > 253)) {
        *dcolor = scolor;
        return;
    }
    
    // it's so insignificant here nobody will notice if we just don't...
    if (UNLIKELY(srcalpha == 0))
        return;
#endif

#if PIXEL_SIZE == 32
    Pixel32ARGB dc, sc;
    dc.l = *dcolor;
    sc.l = scolor;
    
    int dcr = (dc.p.r * dstalpha + sc.p.r * srcalpha) >> 8;
    int dcg = (dc.p.g * dstalpha + sc.p.g * srcalpha) >> 8;
    int dcb = (dc.p.b * dstalpha + sc.p.b * srcalpha) >> 8;
    
    //clamp to 0
    dcr &= ((-dcr) >> 31);
    dcg &= ((-dcg) >> 31);
    dcb &= ((-dcb) >> 31);
    //clamp to 255
    dcr |= ((signed char)(dcr >> 1) >> 7);
    dcg |= ((signed char)(dcg >> 1) >> 7);
    dcb |= ((signed char)(dcb >> 1) >> 7);
    
    dc.p.r = dcr;
    dc.p.g = dcg;
    dc.p.b = dcb;
    dc.p.a = 0xFF;
    
    *dcolor = dc.l;
#elif PIXEL_SIZE == 16
    int scb = scolor & 0x1F;
    int scg = (scolor >> 5) & 0x1F;
    int scr = (scolor >> 10) & 0x1F;

    uintpixel_t _dcolor = *dcolor;
    int dcb = _dcolor & 0x1F;
    int dcg = (_dcolor >> 5) & 0x1F;
    int dcr = (_dcolor >> 10) & 0x1F;
    
    dcr = (dcr * dstalpha + scr * srcalpha) >> 8;
    dcg = (dcg * dstalpha + scg * srcalpha) >> 8;
    dcb = (dcb * dstalpha + scb * srcalpha) >> 8;
    
    //clamp to 0
    dcr &= ((-dcr) >> 31);
    dcg &= ((-dcg) >> 31);
    dcb &= ((-dcb) >> 31);
    //clamp to 255
    dcr |= ((signed char)(dcr >> 1) >> 7);
    dcg |= ((signed char)(dcg >> 1) >> 7);
    dcb |= ((signed char)(dcb >> 1) >> 7);
    
    *dcolor = 0x8000 | dcb | (dcg << 5) | (dcr << 10);
#else
    if (srcalpha < 240) {
        static int alphaApproximationThingy = 0;
        alphaApproximationThingy += 1339;
        if (alphaApproximationThingy > 601000)
            alphaApproximationThingy = 0;
        
        //gotta love that RNG
        if ((alphaApproximationThingy & 0xFF) >= srcalpha)
            return;
    }
    
    *dcolor = scolor;
#endif
}

// Calculates an internal "alpha" value from GML-provided "alpha" values.
FORCE_INLINE int swrIntAlpha(float alphaf)
{
    return (int)(alphaf * 256);
}

// Calculates the source alpha for a pixel based on the current blend mode.
FORCE_INLINE int swrCalcSrcAlpha(SWRenderer* swr, int alpha)
{
    switch (swr->blendMode)
    {
        default:
            return alpha;
        case bm_add:
            return alpha;
        case bm_subtract:
            return -alpha;
    }
}

// Calculates the destination alpha for a pixel based on the current blend mode.
FORCE_INLINE int swrCalcDstAlpha(SWRenderer* swr, int alpha)
{
    switch (swr->blendMode)
    {
        default:
            return 256 - alpha;
        case bm_add:
            return 256;
        case bm_subtract:
            return 256;
    }
}

#endif//_SW_PIXEL_CALC_H
