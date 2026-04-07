#include "sdl_renderer.h"
#include "data_win.h"
#include "text_utils.h"
#include "matrix_math.h"
#include "utils.h"
#include "renderer.h"
#include "debug_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <time.h>

// High-resolution timing helpers (microsecond resolution)
static inline uint64_t tickUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

#include "stb_image.h"

// ===[ Debug bounding box storage ]===
#define MAX_DEBUG_BBOXES 512
#define MAX_BBOX_LABEL 16

typedef struct {
    int16_t x, y, w, h;
    int spriteNum;
    char label[MAX_BBOX_LABEL];
} DebugBBox;

typedef struct {
    DebugBBox boxes[MAX_DEBUG_BBOXES];
    size_t count;
} DebugBBoxArray;

// ===[ Константы кэша — оптимизировано для 32 МБ ОЗУ ]===
#define TEXTURE_CACHE_CAPACITY  64
#define TEXTURE_CACHE_THRESHOLD 48

// Формат текстур: 16-бит RGB565 (binary alpha: пиксель 0 = прозрачный)
// Глобальная прозрачность: 4 бита на спрайт (16 уровней)
// Формат экрана: 16-бит RGB565

// Forward declaration for loading screen
static void drawLoadingText(const char* text, SDL_Surface* screen);

// Global TTF font for loading screen
static TTF_Font* g_loadingFont = NULL;

// Debug overlay — similar to PS2 debug overlay
static void drawDebugOverlay(SDL_Surface* screen, const SDLDebugInfo* info) {
    if (!screen || !g_loadingFont || !info) return;

    // Формируем строки отдельно — TTF_RenderText_Blended не поддерживает \n
    char lines[8][128];
    int lineCount = 0;

    snprintf(lines[lineCount++], sizeof(lines[0]),
             "Frame: %.2fms", info->frameTimeMs);
    snprintf(lines[lineCount++], sizeof(lines[1]),
             "Instances: %d", info->instanceCount);
    snprintf(lines[lineCount++], sizeof(lines[2]),
             "Sprites: %d", info->spritesDrawn);
    snprintf(lines[lineCount++], sizeof(lines[3]),
             "Free mem: %d KB", info->freeMemoryBytes / 1024);
    snprintf(lines[lineCount++], sizeof(lines[4]),
             "Room: %s (fps: %u)",
             info->roomName ? info->roomName : "unknown", info->roomSpeed);

    // Рендерим каждую строку отдельно
    SDL_Color white = { 255, 255, 255, 255 };
    int y = 4;
    int lineH = TTF_FontLineSkip(g_loadingFont);
    if (lineH == 0) lineH = 14;

    for (int i = 0; i < lineCount; i++) {
        SDL_Surface* textSurf = TTF_RenderText_Blended(g_loadingFont, lines[i], white);
        if (textSurf) {
            SDL_Rect dst = { 4, y, 0, 0 };
            SDL_BlitSurface(textSurf, NULL, screen, &dst);
            SDL_FreeSurface(textSurf);
        }
        y += lineH;
    }
}


// ===[ Быстрый доступ к пиксельному формату ]===
typedef struct {
    uint32_t rMask, gMask, bMask, aMask;
    uint8_t  rShift, gShift, bShift, aShift;
    int      bpp;
} FastFmt;

static inline void FastFmt_init(FastFmt* f, const SDL_PixelFormat* fmt) {
    f->rMask  = fmt->Rmask;  f->rShift = fmt->Rshift;
    f->gMask  = fmt->Gmask;  f->gShift = fmt->Gshift;
    f->bMask  = fmt->Bmask;  f->bShift = fmt->Bshift;
    f->aMask  = fmt->Amask;  f->aShift = fmt->Ashift;
    f->bpp    = fmt->BytesPerPixel;
}

static inline uint32_t FastFmt_read(const uint8_t* p, int bpp) {
    if (bpp == 4) return *(const uint32_t*)p;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline void FastFmt_write(uint8_t* p, uint32_t v, int bpp) {
    if (bpp == 4) { *(uint32_t*)p = v; return; }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

static inline uint8_t FastFmt_A(uint32_t px, const FastFmt* f) {
    return f->aMask ? (uint8_t)((px & f->aMask) >> f->aShift) : 255u;
}
static inline uint8_t FastFmt_R(uint32_t px, const FastFmt* f) {
    return (uint8_t)((px & f->rMask) >> f->rShift);
}
static inline uint8_t FastFmt_G(uint32_t px, const FastFmt* f) {
    return (uint8_t)((px & f->gMask) >> f->gShift);
}
static inline uint8_t FastFmt_B(uint32_t px, const FastFmt* f) {
    return (uint8_t)((px & f->bMask) >> f->bShift);
}

static inline uint32_t FastFmt_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                    const FastFmt* f) {
    return ((uint32_t)r << f->rShift)
         | ((uint32_t)g << f->gShift)
         | ((uint32_t)b << f->bShift)
         | (f->aMask ? ((uint32_t)a << f->aShift) : 0u);
}

// ===[ Fast RGB565 unpack/pack — оптимизировано для ARM ]===
// Unpack: 3 ops на канал вместо 6
// Pack: 3 ops вместо 6
// ВАЖНО: 0x0001 = чёрный непрозрачный (sentinel), распаковываем как (0,0,0)
static inline uint8_t R565_R(uint16_t p) { uint8_t r = (p >> 8) & 0xF8; return r | (r >> 5); }
static inline uint8_t R565_G(uint16_t p) { uint8_t g = (p >> 3) & 0xFC; return g | (g >> 6); }
static inline uint8_t R565_B(uint16_t p) { uint8_t b = (p << 3) & 0xF8; return b | (b >> 5); }
static inline uint16_t R565_PACK(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// Проверка прозрачности: 0 = прозрачный, 0x0001 = чёрный непрозрачный
static inline bool R565_IS_TRANSPARENT(uint16_t p) { return p == 0; }

// Safe unpack: 0x0001 → чёрный (0,0,0) вместо тёмно-синего
static inline uint8_t R565_SR(uint16_t p) { return (p == 0x0001) ? 0 : R565_R(p); }
static inline uint8_t R565_SG(uint16_t p) { return (p == 0x0001) ? 0 : R565_G(p); }
static inline uint8_t R565_SB(uint16_t p) { return (p == 0x0001) ? 0 : R565_B(p); }

// Fast blend: (a * alpha + b * invAlpha) >> 8 вместо div255fast
// Погрешность ~0.4%, но в 3 раза быстрее
static inline uint8_t blend8(uint8_t a, uint8_t b, uint8_t alpha, uint8_t invAlpha) {
    return (uint8_t)(((uint32_t)a * alpha + (uint32_t)b * invAlpha) >> 8);
}

// Fast div255 — используется для 32-бит blending (точнее чем >> 8)
static inline uint32_t div255fast(uint32_t x) {
    return (x + (x >> 8u) + 1u) >> 8u;
}

// ===[ Sin/Cos LUT — fixed-point 16.16 для affine rotation ]===
#define SIN_LUT_SIZE 360
static int32_t s_sinLUT[SIN_LUT_SIZE];  // sin(deg) * 65536
static int32_t s_cosLUT[SIN_LUT_SIZE];  // cos(deg) * 65536

static void initTrigLUT(void) {
    static int initialized = 0;
    if (initialized) return;
    for (int i = 0; i < SIN_LUT_SIZE; i++) {
        float rad = (float)i * 3.14159265358979f / 180.0f;
        s_sinLUT[i] = (int32_t)(sinf(rad) * 65536.0f);
        s_cosLUT[i] = (int32_t)(cosf(rad) * 65536.0f);
    }
    initialized = 1;
}

static inline int32_t fastCos32(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    return s_cosLUT[deg];
}

static inline int32_t fastSin32(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    return s_sinLUT[deg];
}

// ===[ LUT координат источника — fixed-point 16.16 ]===
#define SPX_LUT_MAX 2048
#define SPY_LUT_MAX 2048
static int s_spxLUT[SPX_LUT_MAX];
static int s_spyLUT[SPY_LUT_MAX];

// Fixed-point 16.16: заменяем float умножение/деление на integer
// step = (sw << 16) / dstW, затем accum += step каждый шаг
static inline void buildSpxLUT(int count, int sx0, int startDx, int sw, int dstW) {
    if (dstW <= 0) return;
    int32_t step = (sw << 16) / dstW;
    int32_t accum = startDx * step;
    for (int i = 0; i < count; i++) {
        s_spxLUT[i] = sx0 + (accum >> 16);
        accum += step;
    }
}

static inline void buildSpyLUT(int count, int sy0, int startDy, int sh, int dstH) {
    if (dstH <= 0) return;
    int32_t step = (sh << 16) / dstH;
    int32_t accum = startDy * step;
    for (int j = 0; j < count; j++) {
        s_spyLUT[j] = sy0 + (accum >> 16);
        accum += step;
    }
}

// Integer square root (Newton's method)
static inline int isqrt(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    int x = n;
    int y = (x + n / x) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

// Flip-версия: идёт справа налево (обратное направление для зеркалирования)
// startDx = 0 → первый пиксель = sx0 + sw - 1 (правый край)
// startDx = dstW - 1 → первый пиксель = sx0 (левый край)
static inline void buildSpxLUTFlip(int count, int sx0, int sw, int startDx, int dstW) {
    if (dstW <= 0) return;
    int32_t step = (sw << 16) / dstW;
    // Начальная точка: sx0 + sw - 1 (правый край), минус startDx шагов
    int32_t accum = (sw << 16) - step - startDx * step;
    for (int i = 0; i < count; i++) {
        s_spxLUT[i] = sx0 + (accum >> 16);
        accum -= step;
    }
}

static inline void buildSpyLUTFlip(int count, int sy0, int sh, int startDy, int dstH) {
    if (dstH <= 0) return;
    int32_t step = (sh << 16) / dstH;
    int32_t accum = (sh << 16) - step - startDy * step;
    for (int j = 0; j < count; j++) {
        s_spyLUT[j] = sy0 + (accum >> 16);
        accum -= step;
    }
}

// ===[ Texture Cache ]===
typedef struct {
    uint32_t txtrId;
    uint32_t lastUsed;
    uint16_t width;
    uint16_t height;
    uint8_t*     pixels;
    SDL_Surface* surface;
    FastFmt fmt;
    bool    fmtReady;
} TextureCacheEntry;

#define CACHE_HASH_SIZE 128u
#define CACHE_HASH_MASK (CACHE_HASH_SIZE - 1u)
#define CACHE_EMPTY_SLOT 0xFFFFFFFFu

typedef struct {
    TextureCacheEntry entries[TEXTURE_CACHE_CAPACITY];
    uint32_t hashSlots[CACHE_HASH_SIZE];
    size_t   count;
    uint32_t frameCounter;
} TextureCache;

static inline uint32_t cacheHash(uint32_t id) {
    id = (id ^ 61u) ^ (id >> 16u);
    id *= 9u;
    id ^= id >> 4u;
    id *= 0x27d4eb2du;
    id ^= id >> 15u;
    return id & CACHE_HASH_MASK;
}

static void TextureCache_init(TextureCache* cache) {
    memset(cache, 0, sizeof(TextureCache));
    memset(cache->hashSlots, 0xFF, sizeof(cache->hashSlots));
}

static TextureCacheEntry* TextureCache_find(TextureCache* cache, uint32_t txtrId) {
    uint32_t h = cacheHash(txtrId);
    for (uint32_t probe = 0; probe < CACHE_HASH_SIZE; probe++) {
        uint32_t idx = cache->hashSlots[(h + probe) & CACHE_HASH_MASK];
        if (idx == CACHE_EMPTY_SLOT) return NULL;
        if (cache->entries[idx].txtrId == txtrId) return &cache->entries[idx];
    }
    return NULL;
}

static void TextureCache_hashInsert(TextureCache* cache, uint32_t txtrId, uint32_t entryIdx) {
    uint32_t h = cacheHash(txtrId);
    for (uint32_t probe = 0; probe < CACHE_HASH_SIZE; probe++) {
        uint32_t slot = (h + probe) & CACHE_HASH_MASK;
        if (cache->hashSlots[slot] == CACHE_EMPTY_SLOT) {
            cache->hashSlots[slot] = entryIdx;
            return;
        }
    }
}

static void TextureCache_hashRemove(TextureCache* cache, uint32_t txtrId) {
    uint32_t h = cacheHash(txtrId);
    uint32_t removeSlot = CACHE_HASH_SIZE;
    for (uint32_t probe = 0; probe < CACHE_HASH_SIZE; probe++) {
        uint32_t slot = (h + probe) & CACHE_HASH_MASK;
        uint32_t idx = cache->hashSlots[slot];
        if (idx == CACHE_EMPTY_SLOT) break;
        if (cache->entries[idx].txtrId == txtrId) { removeSlot = slot; break; }
    }
    if (removeSlot == CACHE_HASH_SIZE) return;
    uint32_t cur = removeSlot;
    for (;;) {
        uint32_t next = (cur + 1u) & CACHE_HASH_MASK;
        if (cache->hashSlots[next] == CACHE_EMPTY_SLOT) break;
        uint32_t nextIdx = cache->hashSlots[next];
        uint32_t natural = cacheHash(cache->entries[nextIdx].txtrId);
        bool move = (natural <= cur)
                 || ((next < cur) && (natural > cur || natural <= next));
        if (!move) break;
        cache->hashSlots[cur]  = cache->hashSlots[next];
        cache->hashSlots[next] = CACHE_EMPTY_SLOT;
        cur = next;
    }
    cache->hashSlots[cur] = CACHE_EMPTY_SLOT;
}

static void TextureCache_evictLRU(TextureCache* cache) {
    size_t lruIdx = 0;
    uint32_t lruFrame = cache->entries[0].lastUsed;
    for (size_t i = 1; i < cache->count; i++) {
        if (cache->entries[i].lastUsed < lruFrame) {
            lruFrame = cache->entries[i].lastUsed;
            lruIdx = i;
        }
    }
    TextureCacheEntry* e = &cache->entries[lruIdx];
    TextureCache_hashRemove(cache, e->txtrId);
    if (e->pixels)  { free(e->pixels);            e->pixels  = NULL; }
    if (e->surface) { SDL_FreeSurface(e->surface); e->surface = NULL; }
    e->txtrId = CACHE_EMPTY_SLOT;
    e->lastUsed = 0;
    e->width = e->height = 0;
    e->fmtReady = false;
}

static TextureCacheEntry* TextureCache_getOrLoad(TextureCache* cache, SDLRenderer* sdl,
                                                  DataWin* dataWin, uint32_t txtrId) {
    cache->frameCounter++;
    TextureCacheEntry* entry = TextureCache_find(cache, txtrId);
    if (entry) { entry->lastUsed = cache->frameCounter; return entry; }

    if (cache->count >= TEXTURE_CACHE_THRESHOLD) TextureCache_evictLRU(cache);

    entry = NULL;
    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].txtrId == CACHE_EMPTY_SLOT) { entry = &cache->entries[i]; break; }
    }
    if (!entry) {
        if (cache->count >= TEXTURE_CACHE_CAPACITY) {
            TextureCache_evictLRU(cache);
            entry = &cache->entries[cache->count - 1];
        } else {
            entry = &cache->entries[cache->count++];
        }
    }

    memset(entry, 0, sizeof(TextureCacheEntry));
    entry->txtrId   = txtrId;
    entry->lastUsed = cache->frameCounter;

    uint32_t entryIdx = (uint32_t)(entry - cache->entries);
    TextureCache_hashInsert(cache, txtrId, entryIdx);

    if (txtrId >= dataWin->txtr.count) {
        fprintf(stderr, "SDL-OPT: Texture ID %u out of range (max %u)\n",
                txtrId, dataWin->txtr.count);
        return entry;
    }

    Texture* txtr = &dataWin->txtr.textures[txtrId];
    if (!txtr->blobData || txtr->blobSize == 0) {
        fprintf(stderr, "SDL-OPT: Texture %u has no data\n", txtrId);
        return entry;
    }

    if (sdl && sdl->screen) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Loading spritesheet %u", txtrId);
        drawLoadingText(msg, sdl->screen);
    }

    int w, h, ch;
    uint8_t* px = stbi_load_from_memory(txtr->blobData, (int)txtr->blobSize,
                                         &w, &h, &ch, 4);
    if (!px) {
        fprintf(stderr, "SDL-OPT: stbi failed for TXTR %u: %s\n",
                txtrId, stbi_failure_reason());
        return entry;
    }

    entry->width  = (uint16_t)w;
    entry->height = (uint16_t)h;
    entry->pixels = px;
    fprintf(stderr, "SDL-OPT: Loaded TXTR %u (%dx%d)\n", txtrId, w, h);
    (void)sdl;
    return entry;
}

static SDL_Surface* TextureCache_getSurface(TextureCacheEntry* entry, SDLRenderer* sdl) {
    (void)sdl;
    if (!entry->pixels && !entry->surface) return NULL;
    if (!entry->surface && entry->pixels) {
        // Конвертируем RGBA → RGB565 вручную
        // Binary alpha: alpha < 128 → пиксель = 0 (прозрачный)
        int w = entry->width;
        int h = entry->height;
        uint8_t* srcPixels = entry->pixels;

        entry->surface = SDL_CreateRGBSurface(
            SDL_SWSURFACE, w, h, 16,
            0xF800, 0x07E0, 0x001F, 0);
        if (!entry->surface) {
            fprintf(stderr, "SDL-OPT: Failed to create 16-bit surface for TXTR\n");
            return NULL;
        }

        SDL_LockSurface(entry->surface);

        uint16_t* dstRow = (uint16_t*)entry->surface->pixels;
        int dstPitch = entry->surface->pitch / 2;
        for (int y = 0; y < h; y++) {
            uint8_t* srcRow = srcPixels + y * w * 4;
            for (int x = 0; x < w; x++) {
                uint8_t r = srcRow[x * 4 + 0];
                uint8_t g = srcRow[x * 4 + 1];
                uint8_t b = srcRow[x * 4 + 2];
                uint8_t a = srcRow[x * 4 + 3];

                if (a < 128) {
                    dstRow[x] = 0;
                } else {
                    // Чёрный непрозрачный → минимальная яркость (0x0001 вместо 0x0000)
                    // чтобы не совпадать с "прозрачным" значением 0
                    if (r == 0 && g == 0 && b == 0) {
                        dstRow[x] = 0x0001;
                    } else {
                        dstRow[x] = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
                    }
                }
            }
            dstRow += dstPitch;
        }

        SDL_UnlockSurface(entry->surface);

        free(entry->pixels);
        entry->pixels = NULL;

        FastFmt_init(&entry->fmt, entry->surface->format);
        entry->fmtReady = true;
    }
    return entry->surface;
}

static void TextureCache_cleanup(TextureCache* cache) {
    for (size_t i = 0; i < cache->count; i++) {
        TextureCacheEntry* e = &cache->entries[i];
        if (e->pixels)  free(e->pixels);
        if (e->surface) SDL_FreeSurface(e->surface);
    }
}

// ===[ Coordinate Transform — оптимизировано: только умножения ]===
static void viewToScreen(const SDLRenderer* sdl,
                         float gx, float gy, float* bx, float* by)
{
    float px, py;
    if (sdl->viewW > 0 && sdl->viewH > 0) {
        px = (gx - sdl->viewX) * sdl->viewToPortX;
        py = (gy - sdl->viewY) * sdl->viewToPortY;
    } else {
        px = gx; py = gy;
    }
    *bx = (sdl->portX + px) * sdl->gameToWindowX;
    *by = (sdl->portY + py) * sdl->gameToWindowY;
}

static float scaleW(const SDLRenderer* sdl, float w) {
    if (sdl->viewW > 0) {
        return w * sdl->viewToPortX * sdl->gameToWindowX;
    }
    return w * sdl->gameToWindowX;
}
static float scaleH(const SDLRenderer* sdl, float h) {
    if (sdl->viewH > 0) {
        return h * sdl->viewToPortY * sdl->gameToWindowY;
    }
    return h * sdl->gameToWindowY;
}

// ===[ Clip helper ]===
static bool computeClip(SDL_Surface* dst,
                         int dstX, int dstY, int dstW, int dstH,
                         int* outStartX, int* outStartY,
                         int* outEndX,   int* outEndY,
                         int* outSpanW,  int* outSpanH)
{
    int cx1 = dst->clip_rect.x;
    int cy1 = dst->clip_rect.y;
    int cx2 = cx1 + (int)dst->clip_rect.w; if (cx2 > dst->w) cx2 = dst->w;
    int cy2 = cy1 + (int)dst->clip_rect.h; if (cy2 > dst->h) cy2 = dst->h;

    *outStartX = dstX > cx1 ? dstX : cx1;
    *outStartY = dstY > cy1 ? dstY : cy1;
    *outEndX   = (dstX + dstW) < cx2 ? (dstX + dstW) : cx2;
    *outEndY   = (dstY + dstH) < cy2 ? (dstY + dstH) : cy2;

    if (*outStartX >= *outEndX || *outStartY >= *outEndY) return false;

    *outSpanW = *outEndX - *outStartX;
    *outSpanH = *outEndY - *outStartY;
    if (*outSpanW > SPX_LUT_MAX) *outSpanW = SPX_LUT_MAX;
    if (*outSpanH > SPY_LUT_MAX) *outSpanH = SPY_LUT_MAX;
    return true;
}

// ===[ Affine rotation blit — произвольный угол ]===
// pivotDstX/Y — позиция origin/pivot в screen coords
// pivotSrcX/Y — позиция origin внутри srcRect (смещение от srcRect->x/y)
// scaleX/Y    — screen pixels per src pixel (для обратного маппинга)
static void blitScaledAlphaAffine(SDL_Surface* src, const SDL_Rect* srcRect,
                                   SDL_Surface* dst,
                                   int pivotDstX, int pivotDstY,
                                   int pivotSrcX, int pivotSrcY,
                                   float scaleX,  float scaleY,
                                   uint8_t globalAlpha, int angleDeg)
{
    if (!src || !dst || globalAlpha == 0) return;
    if (scaleX == 0.0f || scaleY == 0.0f) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    // Четыре угла спрайта относительно pivot в screen coords
    float cx[4] = {
        (float)(-pivotSrcX)      * scaleX,
        (float)(sw - pivotSrcX)  * scaleX,
        (float)(-pivotSrcX)      * scaleX,
        (float)(sw - pivotSrcX)  * scaleX
    };
    float cy[4] = {
        (float)(-pivotSrcY)      * scaleY,
        (float)(-pivotSrcY)      * scaleY,
        (float)(sh - pivotSrcY)  * scaleY,
        (float)(sh - pivotSrcY)  * scaleY
    };
    float maxR2 = 0.0f;
    for (int k = 0; k < 4; k++) {
        float r2 = cx[k] * cx[k] + cy[k] * cy[k];
        if (r2 > maxR2) maxR2 = r2;
    }
    int radius = (int)sqrtf(maxR2) + 2;


    int dstX = pivotDstX - radius;
    int dstY = pivotDstY - radius;
    int dstW = radius * 2 + 1;
    int dstH = radius * 2 + 1;

    int cx1 = dst->clip_rect.x;
    int cy1 = dst->clip_rect.y;
    int cx2 = cx1 + (int)dst->clip_rect.w; if (cx2 > dst->w) cx2 = dst->w;
    int cy2 = cy1 + (int)dst->clip_rect.h; if (cy2 > dst->h) cy2 = dst->h;

    int startX = dstX > cx1 ? dstX : cx1;
    int startY = dstY > cy1 ? dstY : cy1;
    int endX   = (dstX + dstW) < cx2 ? (dstX + dstW) : cx2;
    int endY   = (dstY + dstH) < cy2 ? (dstY + dstH) : cy2;
    if (startX >= endX || startY >= endY) return;

    int spanW = endX - startX;
    int spanH = endY - startY;

    // cos/sin угла поворота спрайта (прямой, не обратный)
    // Обратное преобразование: rotate(-angle) → cos(-a)=cos(a), sin(-a)=-sin(a)
    int32_t cos16 = fastCos32(angleDeg);
    int32_t sin16 = fastSin32(angleDeg);

    // Обратный масштаб: src pixels per screen pixel, fixed-point 16.16
    // rx_src = rx_screen * invScale
    // invScale_fp16 = round(65536 / scale)
    float invSX = 1.0f / scaleX;
    float invSY = 1.0f / scaleY;
    int32_t invSX_fp16 = (int32_t)(invSX * 65536.0f);
    int32_t invSY_fp16 = (int32_t)(invSY * 65536.0f);

    int sxMin = sx0,       syMin = sy0;
    int sxMax = sx0 + sw,  syMax = sy0 + sh;

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    // Только 16-бит RGB565 fast path
    if (src->format->BytesPerPixel == 2 && dst->format->BytesPerPixel == 2) {
        uint8_t ga    = globalAlpha;
        uint8_t invGa = 255u - ga;

        for (int j = 0; j < spanH; j++) {
            int py = startY + j;
            int dy = py - pivotDstY;

            // Вклад строки в обратное вращение (вычисляем один раз на строку)
            int32_t dy_cos16 =  (int32_t)dy * cos16;  // dy*cos * 65536
            int32_t dy_sin16 =  (int32_t)dy * sin16;  // dy*sin * 65536

            uint16_t* dstRow = (uint16_t*)((uint8_t*)dst->pixels + py * dst->pitch);

            for (int i = 0; i < spanW; i++) {
                int px = startX + i;
                int dx = px - pivotDstX;

                // Обратное вращение на -angleDeg:
                //   rx_fp = dx*cos + dy*sin   (смещение в screen пикселях, * 65536)
                //   ry_fp = -dx*sin + dy*cos
                int32_t rx_fp = (int32_t)dx * cos16 - dy_sin16;
                int32_t ry_fp = (int32_t)dx * sin16 + dy_cos16;

                // screen pixels → src pixels (fixed-point 16.16)
                // rx_screen = rx_fp >> 16
                // rx_src = rx_screen * invSX = (rx_fp >> 16) * invSX_fp16 >> 16
                // Упрощаем через 64-bit, чтобы не терять точность:
                int rx_screen = rx_fp >> 16;
                int ry_screen = ry_fp >> 16;
                int sx_rel = (int)((int64_t)rx_screen * invSX_fp16 >> 16);
                int sy_rel = (int)((int64_t)ry_screen * invSY_fp16 >> 16);

                int sx = sx0 + pivotSrcX + sx_rel;
                int sy = sy0 + pivotSrcY + sy_rel;

                if (sx < sxMin || sx >= sxMax || sy < syMin || sy >= syMax) continue;

                uint16_t sc = ((const uint16_t*)((const uint8_t*)src->pixels
                                                 + sy * src->pitch))[sx];
                if (sc == 0) continue;  // binary alpha

                if (ga == 255) {
                    dstRow[px] = sc;
                } else {
                    uint16_t dc = dstRow[px];
                    dstRow[px] = R565_PACK(
                        blend8(R565_SR(sc), R565_R(dc), ga, invGa),
                        blend8(R565_SG(sc), R565_G(dc), ga, invGa),
                        blend8(R565_SB(sc), R565_B(dc), ga, invGa));
                }
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Blit со спрайтовым alpha-blend + globalAlpha + rotation ]===
// rotation: 0=0°, 1=90° CW, 2=180°, 3=270° CW, 4=произвольный угол (через angleDeg)
//
// globalAlpha == 255 → оригинальный fast path без лишних операций
// globalAlpha == 0   → ранний выход
// иначе              → sa = div255fast(sa * globalAlpha), затем обычный blend
// dstW/dstH должны быть положительными — нормализуйте ДО вызова
// flipX/flipY: 1 = отзеркалить по оси (LUT строится справа налево)
static void blitScaledAlphaOptimized(SDL_Surface* src, const SDL_Rect* srcRect,
                                      SDL_Surface* dst, int dstX, int dstY,
                                      int dstW, int dstH,
                                      uint8_t globalAlpha, int rotation,
                                      int flipX, int flipY)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;
    if (globalAlpha == 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    int startX, startY, endX, endY, spanW, spanH;
    if (!computeClip(dst, dstX, dstY, dstW, dstH,
                     &startX, &startY, &endX, &endY, &spanW, &spanH)) return;
    (void)endX; (void)endY;

    int startDx = startX - dstX;
    int startDy = startY - dstY;

    // flipX — ЗЕРКАЛИРОВАНИЕ по экранной оси X (отрицательный xscale)
    // flipY — ЗЕРКАЛИРОВАНИЕ по экранной оси Y (отрицательный yscale)
    // Для rotation & 1 (90°/270°) оси перепутаны: spxLUT из Y источника, spyLUT из X.
    // Но flipX всегда инвертирует spxLUT, flipY — spyLUT.
    if (rotation & 1) {
        if (flipX) {
            buildSpxLUTFlip(spanW, sy0, sh, startDx, dstW);
        } else {
            buildSpxLUT(spanW, sy0, startDx, sh, dstW);
        }
        if (flipY) {
            buildSpyLUTFlip(spanH, sx0, sw, startDy, dstH);
        } else {
            buildSpyLUT(spanH, sx0, startDy, sw, dstH);
        }
    } else {
        if (flipX) {
            buildSpxLUTFlip(spanW, sx0, sw, startDx, dstW);
        } else {
            buildSpxLUT(spanW, sx0, startDx, sw, dstW);
        }
        if (flipY) {
            buildSpyLUTFlip(spanH, sy0, sh, startDy, dstH);
        } else {
            buildSpyLUT(spanH, sy0, startDy, sh, dstH);
        }
    }

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    FastFmt sf, df;
    FastFmt_init(&sf, src->format);
    FastFmt_init(&df, dst->format);

    // ===[ 16-бит RGB565 → RGB565 с alpha blending ]===
    if (sf.bpp == 2 && df.bpp == 2) {
        if (globalAlpha == 255) {
            // Fast path: полная непрозрачность, просто копируем
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint16_t* srcRow =
                    (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint16_t* dstRow =
                    (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    // При rotation & 1: spx=srcY, spy=srcX (LUT уже повёрнут)
                    // 90° CW: srcX инвертирован, 180°: оба инвертированы, 270° CW: srcY инвертирован
                    int rx, ry;
                    if (rotation == 1) {
                        rx = sx0 + sw - 1 - (spy - sx0);
                        ry = spx;
                    } else if (rotation == 2) {
                        rx = sx0 + sw - 1 - (spx - sx0);
                        ry = sy0 + sh - 1 - (spy - sy0);
                    } else if (rotation == 3) {
                        rx = spy;
                        ry = sy0 + sh - 1 - (spx - sy0);
                    } else {
                        rx = spx;
                        ry = spy;
                    }

                    // Check bounds within the sprite rect
                    if (rx < sx0 || rx >= sx0 + sw || ry < sy0 || ry >= sy0 + sh) continue;

                    uint16_t sc = ((const uint16_t*)((const uint8_t*)src->pixels + ry * src->pitch))[rx];
                    if (sc == 0) continue; // binary alpha

                    dstRow[startX + i] = sc;
                }
            }
        } else {
            // Alpha blending
            uint8_t ga = globalAlpha;
            uint8_t invGa = 255u - ga;
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                uint16_t* dstRow =
                    (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    // При rotation & 1: spx=srcY, spy=srcX (LUT уже повёрнут)
                    // 90° CW: srcX инвертирован, 180°: оба инвертированы, 270° CW: srcY инвертирован
                    int rx, ry;
                    if (rotation == 1) {
                        rx = sx0 + sw - 1 - (spy - sx0);
                        ry = spx;
                    } else if (rotation == 2) {
                        rx = sx0 + sw - 1 - (spx - sx0);
                        ry = sy0 + sh - 1 - (spy - sy0);
                    } else if (rotation == 3) {
                        rx = spy;
                        ry = sy0 + sh - 1 - (spx - sy0);
                    } else {
                        rx = spx;
                        ry = spy;
                    }

                    // Check bounds within the sprite rect
                    if (rx < sx0 || rx >= sx0 + sw || ry < sy0 || ry >= sy0 + sh) continue;

                    uint16_t sc = ((const uint16_t*)((const uint8_t*)src->pixels + ry * src->pitch))[rx];
                    if (sc == 0) continue;

                    uint8_t sr = R565_SR(sc);
                    uint8_t sg = R565_SG(sc);
                    uint8_t sb = R565_SB(sc);

                    uint16_t dc = dstRow[startX + i];
                    uint8_t dr = R565_R(dc);
                    uint8_t dg = R565_G(dc);
                    uint8_t db = R565_B(dc);

                    dstRow[startX + i] = R565_PACK(
                        blend8(sr, dr, ga, invGa),
                        blend8(sg, dg, ga, invGa),
                        blend8(sb, db, ga, invGa));
                }
            }
        }

        SDL_UnlockSurface(dst);
        SDL_UnlockSurface(src);
        return;
    }

    if (sf.bpp == 4 && df.bpp == 4) {

        if (globalAlpha == 255) {
            // ===[ Fast path: globalAlpha полный — ноль лишних операций ]===
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint32_t* srcRow =
                    (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint32_t* dstRow =
                    (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    uint32_t sc = srcRow[spx];
                    uint8_t sa = FastFmt_A(sc, &sf);
                    if (sa == 0) continue;

                    uint8_t sr = FastFmt_R(sc, &sf);
                    uint8_t sg = FastFmt_G(sc, &sf);
                    uint8_t sb = FastFmt_B(sc, &sf);

                    uint32_t* dp = dstRow + (startX + i);

                    if (sa == 255) {
                        *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                    } else {
                        uint32_t dc   = *dp;
                        uint32_t invA = 255u - sa;
                        uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                        uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                        uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                        *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                    }
                }
            }
        } else {
            // ===[ Global alpha path: +1 div255fast на пиксель ]===
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint32_t* srcRow =
                    (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint32_t* dstRow =
                    (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    uint32_t sc = srcRow[spx];
                    // Единственное отличие: масштабируем sa на globalAlpha
                    uint8_t sa = (uint8_t)div255fast(
                        (uint32_t)FastFmt_A(sc, &sf) * globalAlpha);
                    if (sa == 0) continue;

                    uint8_t sr = FastFmt_R(sc, &sf);
                    uint8_t sg = FastFmt_G(sc, &sf);
                    uint8_t sb = FastFmt_B(sc, &sf);

                    uint32_t* dp = dstRow + (startX + i);

                    if (sa == 255) {
                        *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                    } else {
                        uint32_t dc   = *dp;
                        uint32_t invA = 255u - sa;
                        uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                        uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                        uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                        *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                    }
                }
            }
        }

    } else {
        // ===[ 16-бит RGB565 → RGB565 с alpha blending ]===
        if (globalAlpha == 255) {
            // Fast path: просто копируем
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint16_t* srcRow =
                    (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint16_t* dstRow =
                    (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    uint16_t sc = srcRow[spx];
                    if (sc == 0) continue;

                    dstRow[startX + i] = sc;
                }
            }
        } else {
            // Alpha blending
            uint8_t ga = globalAlpha;
            uint8_t invGa = 255u - ga;
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint16_t* srcRow =
                    (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint16_t* dstRow =
                    (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    uint16_t sc = srcRow[spx];
                    if (sc == 0) continue;

                    uint8_t sr = (uint8_t)((sc >> 8) & 0xF8) | (uint8_t)((sc >> 13) & 0x07);
                    uint8_t sg = (uint8_t)((sc >> 3) & 0xFC) | (uint8_t)((sc >> 9)  & 0x03);
                    uint8_t sb = (uint8_t)((sc << 3) & 0xF8) | (uint8_t)((sc >> 2)  & 0x07);

                    uint16_t dc = dstRow[startX + i];
                    uint8_t dr = (uint8_t)((dc >> 8) & 0xF8) | (uint8_t)((dc >> 13) & 0x07);
                    uint8_t dg = (uint8_t)((dc >> 3) & 0xFC) | (uint8_t)((dc >> 9)  & 0x03);
                    uint8_t db = (uint8_t)((dc << 3) & 0xF8) | (uint8_t)((dc >> 2)  & 0x07);

                    uint8_t nr = (uint8_t)div255fast((uint32_t)sr * ga + (uint32_t)dr * invGa);
                    uint8_t ng = (uint8_t)div255fast((uint32_t)sg * ga + (uint32_t)dg * invGa);
                    uint8_t nb = (uint8_t)div255fast((uint32_t)sb * ga + (uint32_t)db * invGa);

                    dstRow[startX + i] = ((uint16_t)(nr >> 3) << 11) | ((uint16_t)(ng >> 2) << 5) | (nb >> 3);
                }
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Threshold blit для тайлов ]===
static void blitScaledThreshold(SDL_Surface* src, const SDL_Rect* srcRect,
                                  SDL_Surface* dst, int dstX, int dstY,
                                  int dstW, int dstH)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    int startX, startY, endX, endY, spanW, spanH;
    if (!computeClip(dst, dstX, dstY, dstW, dstH,
                     &startX, &startY, &endX, &endY, &spanW, &spanH)) return;
    (void)endX; (void)endY;

    buildSpxLUT(spanW, sx0, startX - dstX, sw, dstW);
    buildSpyLUT(spanH, sy0, startY - dstY, sh, dstH);

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    FastFmt sf, df;
    FastFmt_init(&sf, src->format);
    FastFmt_init(&df, dst->format);

    // ===[ 16-бит RGB565 fast path — binary alpha ]===
    if (sf.bpp == 2 && df.bpp == 2) {
        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint16_t* srcRow =
                (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
            uint16_t* dstRow =
                (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint16_t sc = srcRow[spx];
                if (sc == 0) continue; // binary alpha: 0 = прозрачный

                dstRow[startX + i] = sc;
            }
        }

        SDL_UnlockSurface(dst);
        SDL_UnlockSurface(src);
        return;
    }

    if (sf.bpp == 4 && df.bpp == 4) {
        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint32_t* srcRow =
                (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
            uint32_t* dstRow =
                (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint32_t sc = srcRow[spx];
                if (FastFmt_A(sc, &sf) < 128) continue;

                uint32_t* dp = dstRow + (startX + i);
                *dp = FastFmt_pack(FastFmt_R(sc, &sf),
                                   FastFmt_G(sc, &sf),
                                   FastFmt_B(sc, &sf),
                                   255, &df);
            }
        }
    } else {
        // ===[ 16-бит RGB565 fast path — binary alpha ]===
        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint16_t* srcRow =
                (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
            uint16_t* dstRow =
                (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint16_t sc = srcRow[spx];
                if (sc == 0) continue; // binary alpha: 0 = прозрачный

                dstRow[startX + i] = sc;
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Tinted blit для текста и спрайтов с цветом ]===
//
// globalAlpha == 255 → ветка без умножения alpha
// globalAlpha == 0   → ранний выход
// иначе              → sa = div255fast(sa * globalAlpha), полный blend
// dstW/dstH должны быть положительными — нормализуйте ДО вызова
// flipX/flipY: 1 = отзеркалить по оси (LUT строится справа налево)
static void blitScaledAlphaOptimizedTint(SDL_Surface* src, const SDL_Rect* srcRect,
                                          SDL_Surface* dst, int dstX, int dstY,
                                          int dstW, int dstH,
                                          uint32_t tintColor,
                                          uint8_t globalAlpha, int rotation,
                                          int flipX, int flipY)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;
    if (globalAlpha == 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    const uint8_t tintR = BGR_R(tintColor);
    const uint8_t tintG = BGR_G(tintColor);
    const uint8_t tintB = BGR_B(tintColor);
    const bool whiteTint = (tintR == 255 && tintG == 255 && tintB == 255);

    int startX, startY, endX, endY, spanW, spanH;
    if (!computeClip(dst, dstX, dstY, dstW, dstH,
                     &startX, &startY, &endX, &endY, &spanW, &spanH)) return;
    (void)endX; (void)endY;

    int startDx = startX - dstX;
    int startDy = startY - dstY;

    // flipX всегда инвертирует spxLUT, flipY — spyLUT
    if (rotation & 1) {
        if (flipX) {
            buildSpxLUTFlip(spanW, sy0, sh, startDx, dstW);
        } else {
            buildSpxLUT(spanW, sy0, startDx, sh, dstW);
        }
        if (flipY) {
            buildSpyLUTFlip(spanH, sx0, sw, startDy, dstH);
        } else {
            buildSpyLUT(spanH, sx0, startDy, sw, dstH);
        }
    } else {
        if (flipX) {
            buildSpxLUTFlip(spanW, sx0, sw, startDx, dstW);
        } else {
            buildSpxLUT(spanW, sx0, startDx, sw, dstW);
        }
        if (flipY) {
            buildSpyLUTFlip(spanH, sy0, sh, startDy, dstH);
        } else {
            buildSpyLUT(spanH, sy0, startDy, sh, dstH);
        }
    }

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    FastFmt sf, df;
    FastFmt_init(&sf, src->format);
    FastFmt_init(&df, dst->format);

    // ===[ 16-бит RGB565 → RGB565 с tint + alpha blending ]===
    if (sf.bpp == 2 && df.bpp == 2) {
        if (globalAlpha == 255) {
            if (whiteTint) {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;
                    const uint16_t* srcRow = (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint16_t* dstRow = (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);
                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;
                        uint16_t sc = srcRow[spx];
                        if (sc == 0) continue;
                        dstRow[startX + i] = sc;
                    }
                }
            } else {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;
                    const uint16_t* srcRow = (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint16_t* dstRow = (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);
                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;
                        uint16_t sc = srcRow[spx];
                        if (sc == 0) continue;
                        uint8_t sr = R565_SR(sc), sg = R565_SG(sc), sb = R565_SB(sc);
                        uint8_t tr = (uint8_t)div255fast((uint32_t)sr * tintR);
                        uint8_t tg = (uint8_t)div255fast((uint32_t)sg * tintG);
                        uint8_t tb = (uint8_t)div255fast((uint32_t)sb * tintB);
                        dstRow[startX + i] = R565_PACK(tr, tg, tb);
                    }
                }
            }
        } else {
            uint8_t ga = globalAlpha;
            uint8_t invGa = 255u - ga;
            if (whiteTint) {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;
                    const uint16_t* srcRow = (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint16_t* dstRow = (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);
                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;
                        uint16_t sc = srcRow[spx];
                        if (sc == 0) continue;
                        uint16_t dc = dstRow[startX + i];
                        dstRow[startX + i] = R565_PACK(
                            blend8(R565_SR(sc), R565_R(dc), ga, invGa),
                            blend8(R565_SG(sc), R565_G(dc), ga, invGa),
                            blend8(R565_SB(sc), R565_B(dc), ga, invGa));
                    }
                }
            } else {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;
                    const uint16_t* srcRow = (const uint16_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint16_t* dstRow = (uint16_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);
                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;
                        uint16_t sc = srcRow[spx];
                        if (sc == 0) continue;
                        uint8_t sr = R565_SR(sc), sg = R565_SG(sc), sb = R565_SB(sc);
                        uint8_t tr = (uint8_t)div255fast((uint32_t)sr * tintR);
                        uint8_t tg = (uint8_t)div255fast((uint32_t)sg * tintG);
                        uint8_t tb = (uint8_t)div255fast((uint32_t)sb * tintB);
                        uint16_t dc = dstRow[startX + i];
                        dstRow[startX + i] = R565_PACK(
                            blend8(tr, R565_R(dc), ga, invGa),
                            blend8(tg, R565_G(dc), ga, invGa),
                            blend8(tb, R565_B(dc), ga, invGa));
                    }
                }
            }
        }

        SDL_UnlockSurface(dst);
        SDL_UnlockSurface(src);
        return;
    }

    if (sf.bpp == 4 && df.bpp == 4) {

        if (globalAlpha == 255) {
            // ===[ Full opacity path — оригинальное поведение ]===
            if (whiteTint) {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        if (FastFmt_A(sc, &sf) < 128) continue;

                        dstRow[startX + i] = FastFmt_pack(FastFmt_R(sc, &sf),
                                                           FastFmt_G(sc, &sf),
                                                           FastFmt_B(sc, &sf),
                                                           255, &df);
                    }
                }
            } else {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        if (FastFmt_A(sc, &sf) < 128) continue;

                        uint8_t tr = (uint8_t)div255fast((uint32_t)FastFmt_R(sc, &sf) * tintR);
                        uint8_t tg = (uint8_t)div255fast((uint32_t)FastFmt_G(sc, &sf) * tintG);
                        uint8_t tb = (uint8_t)div255fast((uint32_t)FastFmt_B(sc, &sf) * tintB);

                        dstRow[startX + i] = FastFmt_pack(tr, tg, tb, 255, &df);
                    }
                }
            }

        } else {
            // ===[ Global alpha path — полный blend с масштабированием sa ]===
            if (whiteTint) {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        uint8_t sa = (uint8_t)div255fast(
                            (uint32_t)FastFmt_A(sc, &sf) * globalAlpha);
                        if (sa == 0) continue;

                        uint8_t sr = FastFmt_R(sc, &sf);
                        uint8_t sg = FastFmt_G(sc, &sf);
                        uint8_t sb = FastFmt_B(sc, &sf);

                        uint32_t* dp = dstRow + (startX + i);

                        if (sa == 255) {
                            *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                        } else {
                            uint32_t dc   = *dp;
                            uint32_t invA = 255u - sa;
                            uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                            uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                            uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                            *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                        }
                    }
                }
            } else {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        uint8_t sa = (uint8_t)div255fast(
                            (uint32_t)FastFmt_A(sc, &sf) * globalAlpha);
                        if (sa == 0) continue;

                        uint8_t sr = (uint8_t)div255fast((uint32_t)FastFmt_R(sc, &sf) * tintR);
                        uint8_t sg = (uint8_t)div255fast((uint32_t)FastFmt_G(sc, &sf) * tintG);
                        uint8_t sb = (uint8_t)div255fast((uint32_t)FastFmt_B(sc, &sf) * tintB);

                        uint32_t* dp = dstRow + (startX + i);

                        if (sa == 255) {
                            *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                        } else {
                            uint32_t dc   = *dp;
                            uint32_t invA = 255u - sa;
                            uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                            uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                            uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                            *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                        }
                    }
                }
            }
        }

    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Renderer Struct ]===
typedef struct {
    SDLRenderer base;
    TextureCache textureCache;
    FastFmt screenFmt;
    bool    screenFmtReady;
    bool    debugOverlayEnabled;
    bool    debugSpriteBBoxes;    // рисовать хитбоксы спрайтов
    bool    debugSpriteLogging;   // логировать вызовы drawSprite
    SDLDebugInfo debugInfo;
    int     spritesDrawnThisFrame; // счётчик спрайтов за кадр
    DebugBBoxArray debugBBoxes;    // хитбоксы для отрисовки в endFrame

    // Text rendering profiling
    int     textCallsThisFrame;
    int     totalGlyphsThisFrame;
    uint64_t textTotalUs;        // microseconds spent in drawText
    uint64_t blitTotalUs;        // microseconds spent in blitTextGlyph
    uint64_t decodeUtf8TotalUs;  // microseconds spent in UTF-8 decode
    uint64_t findGlyphTotalUs;   // microseconds spent in findGlyph
} SDLRendererOpt;

// ===[ Loading screen ]===
static void drawLoadingText(const char* text, SDL_Surface* screen) {
    if (!screen || !text) return;

    char loadingStr[128];
    snprintf(loadingStr, sizeof(loadingStr), "%s, please wait...", text);

    if (g_loadingFont != NULL) {
        SDL_Color textColor = {255, 255, 255, 255};
        SDL_Surface* textSurface = TTF_RenderText_Blended(g_loadingFont, loadingStr, textColor);
        if (textSurface != NULL) {
            SDL_Rect textRect = {10, 10, 0, 0};
            SDL_BlitSurface(textSurface, NULL, screen, &textRect);
            SDL_FreeSurface(textSurface);
            fprintf(stderr, "Loading: %s\n", loadingStr);
            fflush(stderr);
            SDL_Flip(screen);
            return;
        } else {
            fprintf(stderr, "SDL_ttf: Failed to render text: %s\n", TTF_GetError());
        }
    }

    fprintf(stderr, "Loading: %s\n", loadingStr);
    fflush(stderr);
}

// ===[ Vtable: init ]===
static void sdlOptInit(Renderer* renderer, DataWin* dataWin) {
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    TextureCache_init(&opt->textureCache);
    renderer->dataWin = dataWin;
    opt->screenFmtReady = false;
    opt->debugOverlayEnabled = false;
    opt->debugSpriteBBoxes = false;
    opt->debugSpriteLogging = false;
    opt->spritesDrawnThisFrame = 0;
    opt->debugBBoxes.count = 0;

    initTrigLUT();

    int ttfResult = TTF_Init();
    fprintf(stderr, "SDL-OPT: TTF_Init() returned %d\n", ttfResult);
    if (ttfResult == 0) {
        const char* fontPaths[] = {
            "/mnt/fonts/arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            NULL
        };

        for (int i = 0; fontPaths[i] != NULL; i++) {
            g_loadingFont = TTF_OpenFont(fontPaths[i], 10);
            if (g_loadingFont != NULL) {
                fprintf(stderr, "SDL-OPT: Loaded font: %s\n", fontPaths[i]);
                break;
            } else {
                fprintf(stderr, "SDL-OPT: Failed to load font: %s (%s)\n", fontPaths[i], TTF_GetError());
            }
        }

        if (g_loadingFont == NULL) {
            fprintf(stderr, "SDL-OPT: No fonts found, loading screen will not show text\n");
        }
    } else {
        fprintf(stderr, "SDL-OPT: Failed to initialize SDL_ttf: %s\n", TTF_GetError());
    }

    sdl->screen = SDL_GetVideoSurface();
    if (sdl->screen) {
        drawLoadingText("Caching textures", sdl->screen);
    }

    fprintf(stderr, "SDL-OPT: Renderer initialized (texture cache capacity: %d)\n",
            TEXTURE_CACHE_CAPACITY);
}

// ===[ Vtable: destroy ]===
static void sdlOptDestroy(Renderer* renderer) {
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    TextureCache_cleanup(&opt->textureCache);
    if (opt->base.backbuffer) {
        SDL_FreeSurface(opt->base.backbuffer);
        opt->base.backbuffer = NULL;
    }

    if (g_loadingFont != NULL) {
        TTF_CloseFont(g_loadingFont);
        g_loadingFont = NULL;
    }
    TTF_Quit();

    free(opt);
}

// ===[ Vtable: beginFrame ]===
static void sdlOptBeginFrame(Renderer* renderer,
                              int32_t gameW, int32_t gameH,
                              int32_t windowW, int32_t windowH)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    (void)windowW; (void)windowH;

    sdl->gameW   = gameW;
    sdl->gameH   = gameH;
    sdl->windowW = 320;
    sdl->windowH = 240;
    sdl->screen  = SDL_GetVideoSurface();

    // Precompute game→window coefficients (no divisions during rendering!)
    sdl->invGameW = (gameW > 0) ? 1.0f / (float)gameW : 1.0f;
    sdl->invGameH = (gameH > 0) ? 1.0f / (float)gameH : 1.0f;
    sdl->gameToWindowX = (float)sdl->windowW * sdl->invGameW;
    sdl->gameToWindowY = (float)sdl->windowH * sdl->invGameH;

    if (sdl->screen && !opt->screenFmtReady) {
        FastFmt_init(&opt->screenFmt, sdl->screen->format);
        opt->screenFmtReady = true;
    }

    // Reset per-frame sprite counter
    opt->spritesDrawnThisFrame = 0;

    // Reset per-frame text counters
    opt->textCallsThisFrame = 0;
    opt->totalGlyphsThisFrame = 0;
    opt->textTotalUs = 0;
    opt->blitTotalUs = 0;
    opt->decodeUtf8TotalUs = 0;
    opt->findGlyphTotalUs = 0;

    SDL_SetClipRect(sdl->screen, NULL);
    SDL_FillRect(sdl->screen, NULL,
                 SDL_MapRGB(sdl->screen->format, 0, 0, 0));
}

// ===[ Vtable: endFrame ]===
static void sdlOptEndFrame(Renderer* renderer) {
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    // Update cache count in debug info
    opt->debugInfo.textureCacheCount = (int)opt->textureCache.count;
    opt->debugInfo.textureCacheCapacity = TEXTURE_CACHE_CAPACITY;
    opt->debugInfo.spritesDrawn = opt->spritesDrawnThisFrame;

    // Debug sprite bounding boxes (drawn before overlay text)
    if (opt->debugSpriteBBoxes && opt->debugBBoxes.count > 0) {
        uint32_t bboxColor = SDL_MapRGB(sdl->screen->format, 255, 0, 255); // magenta
        uint32_t bgColor   = SDL_MapRGB(sdl->screen->format, 0, 0, 0);
        uint32_t textColor = SDL_MapRGB(sdl->screen->format, 255, 255, 255);

        for (size_t i = 0; i < opt->debugBBoxes.count; i++) {
            DebugBBox* bbox = &opt->debugBBoxes.boxes[i];
            SDL_Rect rect = { bbox->x, bbox->y, bbox->w, bbox->h };
            // Draw outline (4 edges)
            SDL_Rect top    = { rect.x, rect.y, rect.w, 1 };
            SDL_Rect bottom = { rect.x, rect.y + rect.h - 1, rect.w, 1 };
            SDL_Rect left   = { rect.x, rect.y, 1, rect.h };
            SDL_Rect right  = { rect.x + rect.w - 1, rect.y, 1, rect.h };
            SDL_FillRect(sdl->screen, &top, bboxColor);
            SDL_FillRect(sdl->screen, &bottom, bboxColor);
            SDL_FillRect(sdl->screen, &left, bboxColor);
            SDL_FillRect(sdl->screen, &right, bboxColor);

            // Draw sprite number label
            if (g_loadingFont && bbox->label[0]) {
                SDL_Color labelColor = { 255, 255, 255, 255 };
                SDL_Surface* textSurf = TTF_RenderText_Blended(g_loadingFont, bbox->label, labelColor);
                if (textSurf) {
                    // Label at top-left corner of bbox, with black background
                    int lx = rect.x + 1;
                    int ly = rect.y + 1;
                    if (lx + textSurf->w > rect.x + rect.w) lx = rect.x + rect.w - textSurf->w;
                    if (ly + textSurf->h > rect.y + rect.h) ly = rect.y + rect.h - textSurf->h;

                    // Black background behind text
                    SDL_Rect bg = { lx, ly, (Uint16)textSurf->w, (Uint16)textSurf->h };
                    SDL_FillRect(sdl->screen, &bg, bgColor);
                    SDL_BlitSurface(textSurf, NULL, sdl->screen, &(SDL_Rect){lx, ly, 0, 0});
                    SDL_FreeSurface(textSurf);
                }
            }

            // Color label: yellow for rects (tpag=-1), magenta for sprites
            uint32_t labelColor2 = (bbox->spriteNum < 0)
                ? SDL_MapRGB(sdl->screen->format, 255, 255, 0)  // yellow = rect/line
                : bboxColor;
            SDL_Rect dot = { rect.x, rect.y, 2, 2 };
            SDL_FillRect(sdl->screen, &dot, labelColor2);
        }
        // Reset bbox count for next frame
        opt->debugBBoxes.count = 0;
    }

    // Debug overlay (always shown when debugMode is enabled)
    if (renderer->dataWin && opt->debugOverlayEnabled) {
        drawDebugOverlay(sdl->screen, &opt->debugInfo);
    }

    // Draw debug menu ON TOP of everything (independent of overlay)
    if (g_debugMenu.visible) {
        DebugMenu_draw(renderer, sdl->screen);
    }

    SDL_SetClipRect(sdl->screen, NULL);
    SDL_Flip(sdl->screen);
}

void SDLRendererOpt_updateDebugInfo(Renderer* renderer, const SDLDebugInfo* info) {
    if (!renderer || !info) return;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    opt->debugInfo = *info;
    // Don't auto-enable overlay here — let debug menu control it
}

void SDLRendererOpt_toggleDebugOverlay(Renderer* renderer) {
    if (!renderer) return;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    opt->debugOverlayEnabled = !opt->debugOverlayEnabled;
}

bool SDLRendererOpt_isDebugOverlayEnabled(Renderer* renderer) {
    if (!renderer) return false;
    return ((SDLRendererOpt*)renderer)->debugOverlayEnabled;
}

// ===[ Vtable: beginView ]===
static void sdlOptBeginView(Renderer* renderer,
                             int32_t viewX, int32_t viewY,
                             int32_t viewW, int32_t viewH,
                             int32_t portX, int32_t portY,
                             int32_t portW, int32_t portH,
                             float viewAngle)
{
    (void)viewAngle;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    sdl->viewX = viewX; sdl->viewY = viewY;
    sdl->viewW = viewW; sdl->viewH = viewH;
    sdl->portX = portX; sdl->portY = portY;
    sdl->portW = portW; sdl->portH = portH;

    // Precompute view→port coefficients (no divisions during rendering!)
    sdl->viewToPortX = (viewW > 0) ? (float)portW / (float)viewW : 1.0f;
    sdl->viewToPortY = (viewH > 0) ? (float)portH / (float)viewH : 1.0f;

    SDL_Rect clip = { (Sint16)portX, (Sint16)portY,
                      (Uint16)portW, (Uint16)portH };
    SDL_SetClipRect(sdl->screen, &clip);
}

// ===[ Vtable: endView ]===
static void sdlOptEndView(Renderer* renderer) {
    SDLRenderer* sdl = &((SDLRendererOpt*)renderer)->base;
    SDL_SetClipRect(sdl->screen, NULL);
}

// ===[ Helper: получить surface + tpag ]===
static SDL_Surface* getTPAGSurface(SDLRendererOpt* opt, DataWin* dw,
                                    int32_t tpagIndex, TexturePageItem** outTpag)
{
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return NULL;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    *outTpag = tpag;
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= dw->txtr.count) return NULL;
    TextureCacheEntry* entry = TextureCache_getOrLoad(
        &opt->textureCache, &opt->base, dw, (uint32_t)pageId);
    return TextureCache_getSurface(entry, &opt->base);
}

// ===[ Debug helper: record sprite bbox ]===
static void recordSpriteBBox(SDLRendererOpt* opt, int x, int y, int w, int h,
                              int32_t tpagIndex, float alpha,
                              float angleDeg, float xscale, float yscale)
{
    // Record bbox for rendering in endFrame (sprite counter already incremented by caller)
    if (opt->debugSpriteBBoxes && opt->debugBBoxes.count < MAX_DEBUG_BBOXES) {
        DebugBBox* bbox = &opt->debugBBoxes.boxes[opt->debugBBoxes.count];
        bbox->x = (int16_t)x;
        bbox->y = (int16_t)y;
        bbox->w = (int16_t)w;
        bbox->h = (int16_t)h;
        bbox->spriteNum = opt->spritesDrawnThisFrame;
        snprintf(bbox->label, MAX_BBOX_LABEL, "%d", opt->spritesDrawnThisFrame);
        opt->debugBBoxes.count++;
    }

    // Log sprite draw call
    if (opt->debugSpriteLogging) {
        fprintf(stderr, "[SPRITE #%d] tpag=%d pos=(%d,%d) size=(%dx%d) angle=%.0f scale=(%.2f,%.2f) alpha=%.2f\n",
                opt->spritesDrawnThisFrame, tpagIndex, x, y, w, h,
                angleDeg, xscale, yscale, alpha);
    }
}

// ===[ Vtable: drawSpritePart ]===
static void sdlOptDrawSpritePart(Renderer* renderer,
                                  int32_t tpagIndex,
                                  int32_t srcOffX, int32_t srcOffY,
                                  int32_t srcW, int32_t srcH,
                                  float x, float y,
                                  float xscale, float yscale,
                                  uint32_t color, float alpha)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    
    opt->spritesDrawnThisFrame++;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;

    TexturePageItem* tpag;
    SDL_Surface* src = getTPAGSurface(opt, dw, tpagIndex, &tpag);
    if (!src) return;

    SDL_Rect srcRect = { tpag->sourceX + srcOffX, tpag->sourceY + srcOffY, srcW, srcH };

    // draw_sprite_part не применяет origin — позиция x,y прямая
    float bx, by;
    viewToScreen(sdl, x, y, &bx, &by);

    uint8_t ga = (uint8_t)(alpha * 255.0f);

    uint8_t tintR = BGR_R(color);
    uint8_t tintG = BGR_G(color);
    uint8_t tintB = BGR_B(color);
    bool needsTint = (tintR != 255 || tintG != 255 || tintB != 255);

    int dstW = (int)scaleW(sdl, (float)srcW * xscale);
    int dstH = (int)scaleH(sdl, (float)srcH * yscale);

    // Нормализуем отрицательные размеры (зеркалирование по осям)
    // Позиция сдвигается на отрицательный размер, размер берётся по модулю
    int flipX = 0, flipY = 0;
    if (dstW < 0) { bx += dstW; dstW = -dstW; flipX = 1; }
    if (dstH < 0) { by += dstH; dstH = -dstH; flipY = 1; }

    // Debug: record bbox (абсолютные значения)
    recordSpriteBBox(opt, (int)bx, (int)by, dstW, dstH, tpagIndex, alpha,
                      0.0f, xscale, yscale);

    if (needsTint) {
        blitScaledAlphaOptimizedTint(src, &srcRect, sdl->screen,
                                      (int)bx, (int)by, dstW, dstH, color, ga, 0,
                                      flipX, flipY);
    } else {
        blitScaledAlphaOptimized(src, &srcRect, sdl->screen,
                                  (int)bx, (int)by, dstW, dstH, ga, 0,
                                  flipX, flipY);
    }
}

// ===[ Vtable: drawSprite ]===
static void sdlOptDrawSprite(Renderer* renderer,
                              int32_t tpagIndex,
                              float x, float y,
                              float originX, float originY,
                              float xscale, float yscale,
                              float angleDeg,
                              uint32_t color, float alpha)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    
    opt->spritesDrawnThisFrame++;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;

    TexturePageItem* tpag;
    SDL_Surface* src = getTPAGSurface(opt, dw, tpagIndex, &tpag);
    if (!src) return;

    int srcW = tpag->sourceWidth;
    int srcH = tpag->sourceHeight;

    // Нормализуем угол: 0/90/180/270 — быстрый путь, остальные — affine
    int rotation = 0;
    int useAffine = 0;
    int angleInt = (int)angleDeg;
    if (angleInt != 0) {
        angleInt = (angleInt % 360 + 360) % 360;
        if (angleInt == 90)      rotation = 1;
        else if (angleInt == 180) rotation = 2;
        else if (angleInt == 270) rotation = 3;
        else                      useAffine = 1;
    }

    int dstW, dstH;
    if (rotation & 1) {
        // 90/270: оси перепутаны
        dstW = (int)scaleW(sdl, (float)srcH * xscale);
        dstH = (int)scaleH(sdl, (float)srcW * yscale);
    } else if (useAffine) {
        // Affine: bounding box через sin/cos
        int32_t cos16 = fastCos32(angleInt);
        int32_t sin16 = fastSin32(angleInt);
        int32_t acos = cos16 < 0 ? -cos16 : cos16;
        int32_t asin = sin16 < 0 ? -sin16 : sin16;
        float swF = (float)srcW * xscale;
        float shF = (float)srcH * yscale;
        float bboxW = (swF * acos + shF * asin) / 65536.0f;
        float bboxH = (swF * asin + shF * acos) / 65536.0f;
        dstW = (int)scaleW(sdl, bboxW);
        dstH = (int)scaleH(sdl, bboxH);
    } else {
        dstW = (int)scaleW(sdl, (float)srcW * xscale);
        dstH = (int)scaleH(sdl, (float)srcH * yscale);
    }

    SDL_Rect srcRect = { tpag->sourceX, tpag->sourceY, srcW, srcH };

    // Позиция: при rotation 90/270 origin применяется к повёрнутым осям
    float bx, by;
    if (rotation & 1) {
        viewToScreen(sdl,
                     x + ((float)tpag->targetX - originY) * xscale,
                     y + ((float)tpag->targetY - originX) * yscale,
                     &bx, &by);
    } else if (useAffine) {
        // Для affine rotation позиция = центр спрайта
        viewToScreen(sdl,
                     x + ((float)tpag->targetX - originX) * xscale,
                     y + ((float)tpag->targetY - originY) * yscale,
                     &bx, &by);
    } else {
        viewToScreen(sdl,
                     x + ((float)tpag->targetX - originX) * xscale,
                     y + ((float)tpag->targetY - originY) * yscale,
                     &bx, &by);
    }

    uint8_t ga = (uint8_t)(alpha * 255.0f);

    uint8_t tintR = BGR_R(color);
    uint8_t tintG = BGR_G(color);
    uint8_t tintB = BGR_B(color);
    bool needsTint = (tintR != 255 || tintG != 255 || tintB != 255);

    // Нормализуем отрицательные размеры (зеркалирование по осям)
    int flipX = 0, flipY = 0;
    if (dstW < 0) { bx += dstW; dstW = -dstW; flipX = 1; }
    if (dstH < 0) { by += dstH; dstH = -dstH; flipY = 1; }

        if (useAffine) {
        // Pivot (origin спрайта) на экране — точка вращения
        float pivotScreenX, pivotScreenY;
        viewToScreen(sdl, x, y, &pivotScreenX, &pivotScreenY);

        // Origin внутри src rect (смещение от левого-верхнего угла srcRect)
        int pivotSrcX = (int)originX - tpag->targetX;
        int pivotSrcY = (int)originY - tpag->targetY;

        // Масштаб: screen pixels / src pixel
        // = game scale * view→port * game→window
        float screenScaleX = xscale * sdl->viewToPortX * sdl->gameToWindowX;
        float screenScaleY = yscale * sdl->viewToPortY * sdl->gameToWindowY;

        // Debug: приблизительный bbox вокруг pivot
        recordSpriteBBox(opt,
                         (int)pivotScreenX - dstW / 2,
                         (int)pivotScreenY - dstH / 2,
                         dstW, dstH, tpagIndex, alpha, angleDeg, xscale, yscale);

        blitScaledAlphaAffine(src, &srcRect, sdl->screen,
                               (int)pivotScreenX, (int)pivotScreenY,
                               pivotSrcX, pivotSrcY,
                               screenScaleX, screenScaleY,
                               ga, angleInt);
    } else if (needsTint) {
        
        if (opt->debugSpriteBBoxes) {
            recordSpriteBBox(opt, (int)bx, (int)by, dstW, dstH, tpagIndex, alpha, angleDeg, xscale, yscale);
        }
        blitScaledAlphaOptimizedTint(src, &srcRect, sdl->screen,
                                      (int)bx, (int)by, dstW, dstH, color, ga, rotation,
                                      flipX, flipY);
    } else {
        
        if (opt->debugSpriteBBoxes) {
            recordSpriteBBox(opt, (int)bx, (int)by, dstW, dstH, tpagIndex, alpha, angleDeg, xscale, yscale);
        }
        blitScaledAlphaOptimized(src, &srcRect, sdl->screen,
                                  (int)bx, (int)by, dstW, dstH, ga, rotation,
                                  flipX, flipY);
    }
}

// ===[ Vtable: drawRectangle ]===
static void sdlOptDrawRectangle(Renderer* renderer,
                                 float x1, float y1, float x2, float y2,
                                 uint32_t color, float alpha, bool outline)
{
    (void)alpha;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    float bx1, by1, bx2, by2;
    viewToScreen(sdl, x1, y1, &bx1, &by1);
    viewToScreen(sdl, x2, y2, &bx2, &by2);

    int X1 = (int)floorf(fminf(bx1, bx2));
    int Y1 = (int)floorf(fminf(by1, by2));
    int X2 = (int)ceilf(fmaxf(bx1, bx2));
    int Y2 = (int)ceilf(fmaxf(by1, by2));
    if (X2 <= X1) X2 = X1 + 1;
    if (Y2 <= Y1) Y2 = Y1 + 1;

    // Debug: log thin rectangles (likely lines/bars)
    if (opt->debugSpriteLogging) {
        int w = X2 - X1, h = Y2 - Y1;
        const char* type = outline ? "OUTLINE" : (w <= 2 || h <= 2) ? "LINE" : "FILLED";
        uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
        fprintf(stderr, "[RECT %s] game=(%.0f,%.0f)-(%.0f,%.0f) screen=(%d,%d)-(%d,%d) size=%dx%d color=(%d,%d,%d)\n",
                type, x1, y1, x2, y2, X1, Y1, X2, Y2, w, h, r, g, b);
    }

    // Debug: record bbox for thin rects
    if (opt->debugSpriteBBoxes) {
        int w = X2 - X1, h = Y2 - Y1;
        if (w <= 4 || h <= 4) {
            recordSpriteBBox(opt, X1, Y1, w, h, -1, 1.0f, 0.0f, 1.0f, 1.0f);
        }
    }

    uint32_t c = SDL_MapRGB(sdl->screen->format,
                            BGR_R(color), BGR_G(color), BGR_B(color));
    if (outline) {
        int w = X2 - X1, h = Y2 - Y1;
        SDL_Rect edges[4] = {
            {X1,     Y1,     w, 1},
            {X1,     Y2 - 1, w, 1},
            {X1,     Y1,     1, h},
            {X2 - 1, Y1,     1, h}
        };
        for (int i = 0; i < 4; i++) SDL_FillRect(sdl->screen, &edges[i], c);
    } else {
        SDL_Rect rect = {X1, Y1, X2 - X1, Y2 - Y1};
        SDL_FillRect(sdl->screen, &rect, c);
    }
}

// ===[ Vtable: drawLine ]===
static void sdlOptDrawLine(Renderer* renderer,
                            float x1, float y1, float x2, float y2,
                            float width, uint32_t color, float alpha)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer*    sdl = &opt->base;
    (void)alpha;

    // --- единственное место с float: конвертируем в 16.16 ---
    float sx1f, sy1f, sx2f, sy2f;
    viewToScreen(sdl, x1, y1, &sx1f, &sy1f);
    viewToScreen(sdl, x2, y2, &sx2f, &sy2f);

    int32_t bx1 = (int32_t)(sx1f * 65536.f);
    int32_t by1 = (int32_t)(sy1f * 65536.f);
    int32_t bx2 = (int32_t)(sx2f * 65536.f);
    int32_t by2 = (int32_t)(sy2f * 65536.f);

    float scaledWf = scaleW(sdl, width);
    if (scaledWf < 1.f) scaledWf = 1.f;
    // halfW в 16.16, минимум 0.5px
    int32_t halfW_fp = (int32_t)(scaledWf * 32768.f); // * 0.5 * 65536
    if (halfW_fp < 0x8000) halfW_fp = 0x8000;
    // --- дальше только целые числа ---

    uint32_t c = SDL_MapRGB(sdl->screen->format,
                             BGR_R(color), BGR_G(color), BGR_B(color));

    const SDL_Rect* cr = &sdl->screen->clip_rect;
    int cxMin = cr->x;
    int cyMin = cr->y;
    int cxMax = cr->x + (int)cr->w; if (cxMax > sdl->screen->w) cxMax = sdl->screen->w;
    int cyMax = cr->y + (int)cr->h; if (cyMax > sdl->screen->h) cyMax = sdl->screen->h;

    // dx/dy в целых пикселях (сдвиг из 16.16)
    int32_t dxi = (bx2 - bx1) >> 16;
    int32_t dyi = (by2 - by1) >> 16;
    uint32_t len = isqrt((uint32_t)(dxi*dxi + dyi*dyi));

    if (len == 0) {
        // Точка — квадрат вокруг позиции
        int hw = halfW_fp >> 16;
        int X1 = (bx1 >> 16) - hw,    Y1 = (by1 >> 16) - hw;
        int X2 = (bx1 >> 16) + hw + 1, Y2 = (by1 >> 16) + hw + 1;
        if (X1 < cxMin) X1 = cxMin; if (Y1 < cyMin) Y1 = cyMin;
        if (X2 > cxMax) X2 = cxMax; if (Y2 > cyMax) Y2 = cyMax;
        if (X2 > X1 && Y2 > Y1) {
            SDL_Rect r = { X1, Y1, X2-X1, Y2-Y1 };
            SDL_FillRect(sdl->screen, &r, c);
        }
        return;
    }

    // Перпендикулярный вектор длиной halfW (результат в 16.16)
    // px = -dyi * halfW / len,  py = dxi * halfW / len
    // int64 чтобы не переполнить: dyi(~1000) * halfW_fp(~65536*50) = ~3.2e9
    int32_t px_fp = (int32_t)(((int64_t)(-dyi) * halfW_fp) / (int32_t)len);
    int32_t py_fp = (int32_t)(((int64_t)( dxi) * halfW_fp) / (int32_t)len);

    // 4 вершины параллелограмма в 16.16
    int32_t vx[4] = { bx1 + px_fp, bx1 - px_fp, bx2 - px_fp, bx2 + px_fp };
    int32_t vy[4] = { by1 + py_fp, by1 - py_fp, by2 - py_fp, by2 + py_fp };

    // Y-диапазон сканлайнов
    int32_t yMinFp = vy[0], yMaxFp = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vy[i] < yMinFp) yMinFp = vy[i];
        if (vy[i] > yMaxFp) yMaxFp = vy[i];
    }
    int yStart = yMinFp >> 16;
    int yEnd   = (yMaxFp >> 16) + 1;
    if (yStart < cyMin) yStart = cyMin;
    if (yEnd   > cyMax) yEnd   = cyMax;
    if (yStart >= yEnd) return;

    if (SDL_MUSTLOCK(sdl->screen)) SDL_LockSurface(sdl->screen);

    int bpp = sdl->screen->format->BytesPerPixel;

    for (int y = yStart; y < yEnd; y++) {
        // Центр строки в 16.16
        int32_t fy_fp = (y << 16) + 0x8000;

        int32_t xL =  0x7FFFFFFF;
        int32_t xR = -0x7FFFFFFF;

        for (int e = 0; e < 4; e++) {
            int     ne  = (e + 1) & 3;
            int32_t ey0 = vy[e], ey1 = vy[ne];
            int32_t ex0 = vx[e], ex1 = vx[ne];

            // Сканлайн пересекает ребро, если вершины по разные стороны
            if ((fy_fp >= ey0) == (fy_fp >= ey1)) continue;

            // xi = ex0 + (fy - ey0) * (ex1 - ex0) / (ey1 - ey0)
            // всё в 16.16, используем int64 для промежуточного результата
            int32_t xi_fp = ex0 + (int32_t)(
                (int64_t)(fy_fp - ey0) * (ex1 - ex0) / (ey1 - ey0));

            if (xi_fp < xL) xL = xi_fp;
            if (xi_fp > xR) xR = xi_fp;
        }

        if (xL > xR) continue;

        int xS = xL >> 16;
        int xE = (xR >> 16) + 1;
        if (xS < cxMin) xS = cxMin;
        if (xE > cxMax) xE = cxMax;
        if (xS >= xE) continue;

        uint8_t* row = (uint8_t*)sdl->screen->pixels + y * sdl->screen->pitch;
        if (bpp == 2) {
            uint16_t* p = (uint16_t*)row + xS;
            uint16_t c16 = (uint16_t)c;
            int n = xE - xS;
            while (n--) *p++ = c16;
        } else if (bpp == 4) {
            uint32_t* p = (uint32_t*)row + xS;
            int n = xE - xS;
            while (n--) *p++ = c;
        }
    }

    if (SDL_MUSTLOCK(sdl->screen)) SDL_UnlockSurface(sdl->screen);
}

// ===[ Vtable: drawLineColor ]===
static void sdlOptDrawLineColor(Renderer* renderer,
                                 float x1, float y1, float x2, float y2,
                                 float width, uint32_t color1, uint32_t color2, float alpha)
{
    (void)color2; (void)width;
    sdlOptDrawLine(renderer, x1, y1, x2, y2, width, color1, alpha);
}

// ===[ Vtable: drawTriangle ]===
static void sdlOptDrawTriangle(Renderer* renderer,
                                float x1, float y1, float x2, float y2,
                                float x3, float y3, bool outline)
{
    (void)outline;
    sdlOptDrawLine(renderer, x1, y1, x2, y2, 1, 0xFFFFFF, 1.0f);
    sdlOptDrawLine(renderer, x2, y2, x3, y3, 1, 0xFFFFFF, 1.0f);
    sdlOptDrawLine(renderer, x3, y3, x1, y1, 1, 0xFFFFFF, 1.0f);
}

// ===[ Text glyph blit — с LUT для tint и fast path для белого ]===
static void blitTextGlyph(SDL_Surface* src, int srcX, int srcY, int srcW, int srcH,
                          SDL_Surface* dst, int dstX, int dstY, int dstW, int dstH,
                          uint32_t tintColor, uint8_t globalAlpha)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) return;
    if (globalAlpha == 0) return;

    int cx1 = dst->clip_rect.x;
    int cy1 = dst->clip_rect.y;
    int cx2 = cx1 + (int)dst->clip_rect.w; if (cx2 > dst->w) cx2 = dst->w;
    int cy2 = cy1 + (int)dst->clip_rect.h; if (cy2 > dst->h) cy2 = dst->h;

    int startX = dstX > cx1 ? dstX : cx1;
    int startY = dstY > cy1 ? dstY : cy1;
    int endX = (dstX + dstW) < cx2 ? (dstX + dstW) : cx2;
    int endY = (dstY + dstH) < cy2 ? (dstY + dstH) : cy2;
    if (startX >= endX || startY >= endY) return;

    int32_t stepX = (srcW << 16) / dstW;
    int32_t stepY = (srcH << 16) / dstH;

    uint16_t* dstPixels = (uint16_t*)dst->pixels;
    int dstPitch = dst->pitch / 2;
    const uint8_t* srcPixels = (const uint8_t*)src->pixels;
    int srcPitch = src->pitch;

    // Fast path: white tint (0x00FFFFFF)
    if (tintColor == 0x00FFFFFF) {
        // True 1:1 scale, no clip — skip all fixed-point math
        if (dstW == srcW && dstH == srcH && startX == dstX && startY == dstY &&
            endX == dstX + dstW && endY == dstY + dstH &&
            (unsigned)(srcX + srcW) <= (unsigned)src->w &&
            (unsigned)(srcY + srcH) <= (unsigned)src->h) {
            const uint16_t* srcRow = (const uint16_t*)(srcPixels + srcY * srcPitch) + srcX;
            uint16_t* dstRow = dstPixels + dstY * dstPitch + dstX;
            int srcPitch16 = srcPitch / 2;

            for (int y = 0; y < srcH; y++) {
                for (int x = 0; x < srcW; x++) {
                    uint16_t sc = srcRow[x];
                    if (sc != 0) {
                        if (globalAlpha == 255) {
                            dstRow[x] = sc;
                        } else {
                            uint16_t dc = dstRow[x];
                            uint8_t invA = 255u - globalAlpha;
                            dstRow[x] = R565_PACK(
                                blend8(R565_SR(sc), R565_R(dc), globalAlpha, invA),
                                blend8(R565_SG(sc), R565_G(dc), globalAlpha, invA),
                                blend8(R565_SB(sc), R565_B(dc), globalAlpha, invA));
                        }
                    }
                }
                srcRow += srcPitch16;
                dstRow += dstPitch;
            }
            return;
        }
        // Scaled or clipped — use fixed-point loop
        int32_t accumY = (int64_t)(startY - dstY) * stepY;
        for (int dy = startY; dy < endY; dy++) {
            int sy = srcY + (accumY >> 16);
            accumY += stepY;
            if ((unsigned)sy >= (unsigned)src->h) continue;

            const uint16_t* srcRow = (const uint16_t*)(srcPixels + sy * srcPitch);
            uint16_t* dstRow = dstPixels + dy * dstPitch;

            int32_t accumX = (int64_t)(startX - dstX) * stepX;
            for (int dx = startX; dx < endX; dx++) {
                int sx = srcX + (accumX >> 16);
                accumX += stepX;
                if ((unsigned)sx < (unsigned)src->w) {
                    uint16_t sc = srcRow[sx];
                    if (sc != 0) {
                        if (globalAlpha == 255) {
                            dstRow[dx] = sc;
                        } else {
                            uint16_t dc = dstRow[dx];
                            uint8_t invA = 255u - globalAlpha;
                            dstRow[dx] = R565_PACK(
                                blend8(R565_SR(sc), R565_R(dc), globalAlpha, invA),
                                blend8(R565_SG(sc), R565_G(dc), globalAlpha, invA),
                                blend8(R565_SB(sc), R565_B(dc), globalAlpha, invA));
                        }
                    }
                }
            }
        }
        return;
    }

    // Colored tint — use LUT for fast channel blending
    const uint8_t tintR = BGR_R(tintColor);
    const uint8_t tintG = BGR_G(tintColor);
    const uint8_t tintB = BGR_B(tintColor);

    uint8_t tintLUT_R[256], tintLUT_G[256], tintLUT_B[256];
    for (int i = 0; i < 256; i++) {
        tintLUT_R[i] = (uint8_t)((i * tintR) >> 8);
        tintLUT_G[i] = (uint8_t)((i * tintG) >> 8);
        tintLUT_B[i] = (uint8_t)((i * tintB) >> 8);
    }

    int32_t accumY = (int64_t)(startY - dstY) * stepY;
    for (int dy = startY; dy < endY; dy++) {
        int sy = srcY + (accumY >> 16);
        accumY += stepY;
        if ((unsigned)sy >= (unsigned)src->h) continue;

        const uint16_t* srcRow = (const uint16_t*)(srcPixels + sy * srcPitch);
        uint16_t* dstRow = dstPixels + dy * dstPitch;

        int32_t accumX = (int64_t)(startX - dstX) * stepX;
        for (int dx = startX; dx < endX; dx++) {
            int sx = srcX + (accumX >> 16);
            accumX += stepX;
            if ((unsigned)sx < (unsigned)src->w) {
                uint16_t sc = srcRow[sx];
                if (sc != 0) {
                    uint8_t sr = R565_SR(sc);
                    uint8_t sg = R565_SG(sc);
                    uint8_t sb = R565_SB(sc);
                    uint8_t tr = tintLUT_R[sr];
                    uint8_t tg = tintLUT_G[sg];
                    uint8_t tb = tintLUT_B[sb];
                    if (globalAlpha == 255) {
                        dstRow[dx] = R565_PACK(tr, tg, tb);
                    } else {
                        uint16_t dc = dstRow[dx];
                        uint8_t invA = 255u - globalAlpha;
                        dstRow[dx] = R565_PACK(
                            blend8(tr, R565_R(dc), globalAlpha, invA),
                            blend8(tg, R565_G(dc), globalAlpha, invA),
                            blend8(tb, R565_B(dc), globalAlpha, invA));
                    }
                }
            }
        }
    }
}


// ===[ Vtable: drawText ]===
static void sdlOptDrawText(Renderer* renderer,
                            const char* text,
                            float x, float y,
                            float xscale, float yscale,
                            float angleDeg)
{
    (void)angleDeg;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;
    if (!text || !text[0]) return;

    opt->textCallsThisFrame++;

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    if (font->glyphCount == 0) return;

    int32_t tpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* baseTpag = &dw->tpag.items[tpagIndex];

    int16_t pageId = baseTpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= dw->txtr.count) return;

    TextureCacheEntry* entry = TextureCache_getOrLoad(
        &opt->textureCache, sdl, dw, (uint32_t)pageId);
    SDL_Surface* src = TextureCache_getSurface(entry, sdl);
    if (!src) return;

    // Lock surfaces once for the entire text block
    SDL_LockSurface(src);
    SDL_LockSurface(sdl->screen);

    // Precompute screen scale factors to avoid per-glyph function calls
    const float screenScaleX = sdl->viewToPortX * sdl->gameToWindowX;
    const float screenScaleY = sdl->viewToPortY * sdl->gameToWindowY;
    const float viewOffsetX = sdl->viewW > 0 ? -sdl->viewX * screenScaleX : 0;
    const float viewOffsetY = sdl->viewH > 0 ? -sdl->viewY * screenScaleY : 0;
    const float portBaseX = sdl->portX * sdl->gameToWindowX;
    const float portBaseY = sdl->portY * sdl->gameToWindowY;

    const float sx = xscale * font->scaleX;
    const float sy = yscale * font->scaleY;
    const uint32_t tintColor = renderer->drawColor;
    const uint8_t globalAlpha = (uint8_t)(renderer->drawAlpha * 255.0f);

    // Fast path for single-line, left-aligned text with no alignment math
    if (renderer->drawHalign == 0 && renderer->drawValign == 0) {
        // Simple path: just iterate characters without line measuring
        int32_t pos = 0;
        int32_t textLen = (int32_t)strlen(text);
        float cursorX = 0, cursorY = 0;

        while (pos < textLen) {
            uint16_t ch = TextUtils_decodeUtf8(text, textLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float gx = x + (cursorX + (float)glyph->offset) * sx;
            float gy = y + cursorY * sy;

            float bx = portBaseX + (gx + viewOffsetX) * screenScaleX;
            float by = portBaseY + (gy + viewOffsetY) * screenScaleY;

            int dstW = (int)(glyph->sourceWidth * sx * screenScaleX + 0.5f);
            int dstH = (int)(glyph->sourceHeight * sy * screenScaleY + 0.5f);

            SDL_Rect srcRect = {
                (Sint16)(baseTpag->sourceX + glyph->sourceX),
                (Sint16)(baseTpag->sourceY + glyph->sourceY),
                (Uint16)glyph->sourceWidth,
                (Uint16)glyph->sourceHeight
            };

            blitTextGlyph(src, srcRect.x, srcRect.y, srcRect.w, srcRect.h,
                sdl->screen,
                (int)bx, (int)by,
                dstW, dstH,
                tintColor, globalAlpha);

            opt->totalGlyphsThisFrame++;
            cursorX += glyph->shift;
            if (pos < textLen) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text, textLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        SDL_UnlockSurface(sdl->screen);
        SDL_UnlockSurface(src);
        return;
    }

    // Full path with alignment (rarely used)
    int32_t textLen = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float totalHeight = (float)lineCount * (float)font->emSize;
    float valignOffset = 0;
    if      (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if      (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;

        while (pos < lineLen) {
            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float gx = x + (cursorX + (float)glyph->offset) * sx;
            float gy = y + cursorY * sy;

            float bx = portBaseX + (gx + viewOffsetX) * screenScaleX;
            float by = portBaseY + (gy + viewOffsetY) * screenScaleY;

            int dstW = (int)(glyph->sourceWidth * sx * screenScaleX + 0.5f);
            int dstH = (int)(glyph->sourceHeight * sy * screenScaleY + 0.5f);

            SDL_Rect srcRect = {
                (Sint16)(baseTpag->sourceX + glyph->sourceX),
                (Sint16)(baseTpag->sourceY + glyph->sourceY),
                (Uint16)glyph->sourceWidth,
                (Uint16)glyph->sourceHeight
            };

            blitTextGlyph(src, srcRect.x, srcRect.y, srcRect.w, srcRect.h,
                sdl->screen,
                (int)bx, (int)by,
                dstW, dstH,
                tintColor, globalAlpha);

            opt->totalGlyphsThisFrame++;
            cursorX += glyph->shift;
            if (pos < lineLen) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float)font->emSize;
        if (lineEnd < textLen)
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        else
            break;
    }

    SDL_UnlockSurface(sdl->screen);
    SDL_UnlockSurface(src);
}

// ===[ Vtable: drawTextColor ]===
static void sdlOptDrawTextColor(Renderer* renderer,
                                 const char* text, float x, float y,
                                 float xscale, float yscale, float angleDeg,
                                 int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha)
{
    (void)c2; (void)c3; (void)c4;
    uint32_t saved = renderer->drawColor;
    float savedAlpha = renderer->drawAlpha;
    renderer->drawColor = (uint32_t)c1;
    renderer->drawAlpha = alpha;
    sdlOptDrawText(renderer, text, x, y, xscale, yscale, angleDeg);
    renderer->drawColor = saved;
    renderer->drawAlpha = savedAlpha;
}

// ===[ Vtable: flush ]===
static void sdlOptFlush(Renderer* renderer) { (void)renderer; }

// ===[ Vtable: createSpriteFromSurface ]===
static int32_t sdlOptCreateSpriteFromSurface(Renderer* r,
    int32_t x, int32_t y, int32_t w, int32_t h,
    bool removeback, bool smooth, int32_t xorig, int32_t yorig)
{
    (void)r;(void)x;(void)y;(void)w;(void)h;
    (void)removeback;(void)smooth;(void)xorig;(void)yorig;
    return -1;
}

// ===[ Vtable: deleteSprite ]===
static void sdlOptDeleteSprite(Renderer* r, int32_t idx) { (void)r; (void)idx; }

// ===[ Vtable: drawTile ]===
static void sdlOptDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;

    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(dw, tile);
    if (tpagIndex < 0) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= dw->txtr.count) return;

    int32_t srcX = tile->sourceX, srcY = tile->sourceY;
    int32_t srcW = (int32_t)tile->width, srcH = (int32_t)tile->height;
    float drawX  = (float)tile->x + offsetX;
    float drawY  = (float)tile->y + offsetY;

    int32_t cL = tpag->targetX, cT = tpag->targetY;
    if (cL > srcX) { drawX += (float)(cL - srcX) * tile->scaleX; srcW -= cL - srcX; srcX = cL; }
    if (cT > srcY) { drawY += (float)(cT - srcY) * tile->scaleY; srcH -= cT - srcY; srcY = cT; }
    int32_t cR = tpag->targetX + tpag->sourceWidth;
    int32_t cB = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > cR) srcW = cR - srcX;
    if (srcY + srcH > cB) srcH = cB - srcY;
    if (srcW <= 0 || srcH <= 0) return;

    int32_t atlasOffX = srcX - tpag->targetX;
    int32_t atlasOffY = srcY - tpag->targetY;

    TextureCacheEntry* entry = TextureCache_getOrLoad(
        &opt->textureCache, sdl, dw, (uint32_t)pageId);
    SDL_Surface* src = TextureCache_getSurface(entry, sdl);
    if (!src) return;

    SDL_Rect srcRect = {
        tpag->sourceX + atlasOffX,
        tpag->sourceY + atlasOffY,
        srcW, srcH
    };

    float bx, by;
    viewToScreen(sdl, drawX, drawY, &bx, &by);

    blitScaledThreshold(src, &srcRect, sdl->screen,
                        (int)bx, (int)by,
                        (int)scaleW(sdl, (float)srcW * tile->scaleX),
                        (int)scaleH(sdl, (float)srcH * tile->scaleY));
}

// ===[ Vtable ]===
#ifdef __cplusplus
static RendererVtable sdlOptVtable = {
    sdlOptInit, sdlOptDestroy,
    sdlOptBeginFrame, sdlOptEndFrame,
    sdlOptBeginView, sdlOptEndView,
    sdlOptDrawSprite, sdlOptDrawSpritePart,
    sdlOptDrawRectangle, sdlOptDrawLine, sdlOptDrawLineColor,
    sdlOptDrawTriangle, sdlOptDrawText, sdlOptDrawTextColor,
    sdlOptFlush, sdlOptCreateSpriteFromSurface,
    sdlOptDeleteSprite, sdlOptDrawTile,
};
#else
static RendererVtable sdlOptVtable = {
    .init                    = sdlOptInit,
    .destroy                 = sdlOptDestroy,
    .beginFrame              = sdlOptBeginFrame,
    .endFrame                = sdlOptEndFrame,
    .beginView               = sdlOptBeginView,
    .endView                 = sdlOptEndView,
    .drawSprite              = sdlOptDrawSprite,
    .drawSpritePart          = sdlOptDrawSpritePart,
    .drawRectangle           = sdlOptDrawRectangle,
    .drawLine                = sdlOptDrawLine,
    .drawLineColor           = sdlOptDrawLineColor,
    .drawTriangle            = sdlOptDrawTriangle,
    .drawText                = sdlOptDrawText,
    .drawTextColor           = sdlOptDrawTextColor,
    .flush                   = sdlOptFlush,
    .createSpriteFromSurface = sdlOptCreateSpriteFromSurface,
    .deleteSprite            = sdlOptDeleteSprite,
    .drawTile                = sdlOptDrawTile,
};
#endif

// ===[ Debug toggle helpers ]===
void SDLRendererOpt_toggleDebugBBoxes(Renderer* renderer) {
    if (!renderer) return;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    opt->debugSpriteBBoxes = !opt->debugSpriteBBoxes;
    fprintf(stderr, "Debug sprite bboxes: %s\n", opt->debugSpriteBBoxes ? "ON" : "OFF");
    // Clear any leftover bboxes
    opt->debugBBoxes.count = 0;
}

void SDLRendererOpt_toggleDebugLogging(Renderer* renderer) {
    if (!renderer) return;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    opt->debugSpriteLogging = !opt->debugSpriteLogging;
    fprintf(stderr, "Debug sprite logging: %s\n", opt->debugSpriteLogging ? "ON" : "OFF");
}

bool SDLRendererOpt_isDebugBBoxesEnabled(Renderer* renderer) {
    if (!renderer) return false;
    return ((SDLRendererOpt*)renderer)->debugSpriteBBoxes;
}

bool SDLRendererOpt_isDebugLoggingEnabled(Renderer* renderer) {
    if (!renderer) return false;
    return ((SDLRendererOpt*)renderer)->debugSpriteLogging;
}

TTF_Font* SDLRendererOpt_getLoadingFont(void) {
    return g_loadingFont;
}

// ===[ Public constructor ]===
Renderer* SDLRendererOpt_create(void)
{
    SDLRendererOpt* opt = safeCalloc(1, sizeof(SDLRendererOpt));
    opt->base.base.vtable     = &sdlOptVtable;
    opt->base.base.drawColor  = 0xFFFFFF;
    opt->base.base.drawAlpha  = 1.0f;
    opt->base.base.drawFont   = -1;
    opt->base.base.drawHalign = 0;
    opt->base.base.drawValign = 0;
    opt->debugOverlayEnabled  = false;
    memset(&opt->debugInfo, 0, sizeof(SDLDebugInfo));
    return (Renderer*)opt;
}
