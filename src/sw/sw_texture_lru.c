#include <stdlib.h>
#include <assert.h>
#include "sw_renderer_private.h"
#include "image/image_decoder.h"

bool swrAddTextureIndexToLRU(SWRenderer* swr, int textureIndex)
{
    uint32_t newIndex = (swr->textureIndexLRUHead + 1) % TEXTURE_LRU_LENGTH;
    if (newIndex == swr->textureIndexLRUTail) {
        // about to collide with tail from the other side -- nope.
        return false;
    }
    
    swr->textureIndexLRU[swr->textureIndexLRUHead] = textureIndex;
    swr->textureIndexLRUHead = newIndex;
    return true;
}

int swrTailTextureIndexLRU(SWRenderer* swr, bool remove)
{
    if (swr->textureIndexLRUHead == swr->textureIndexLRUTail)
        return -1;
    
    uint32_t textureIndex = swr->textureIndexLRU[swr->textureIndexLRUTail];
    
    if (remove)
        swr->textureIndexLRUTail = (swr->textureIndexLRUTail + 1) % TEXTURE_LRU_LENGTH;
    
    return textureIndex;
}

void swrEvictTextureFromCache(SWRenderer* swr, int textureIndex)
{
    SWTexture* texture = swr->textures[textureIndex];
    swr->textures[textureIndex] = NULL;
    
    swrFreeTexture(texture);
}

// Lazily decodes and uploads a TXTR page on first access.
// Returns true if the texture is ready, false if it failed to decode.
bool swrEnsureTextureIsLoaded(SWRenderer* swr, uint32_t pageId)
{
    if (swr->textures[pageId])
        return true;

    DataWin* dw = swr->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    int w, h;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    
    uint8_t* pixels = NULL;
    
    do
    {
        if (!txtr->blobData) {
            DataWin_loadTxtrIfNeeded(dw, pageId);
        }
        
        if (txtr->blobData) {
            pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t) txtr->blobSize, gm2022_5, &w, &h);
            if (pixels) {
                if (!txtr->mapped) {
                    free(txtr->blobData);
                    txtr->blobData = NULL;
                }
                break;
            }
            
            fprintf(stderr, "swr: Failed to decode TXTR page %u.  This is likely because we're out of memory, so evicting a texture.\n", pageId);
        } else {
            fprintf(stderr, "swr: Failed to load TXTR page %u.  This is likely because we're out of memory, so evicting a texture.\n", pageId);
        }
        
        int tail = swrTailTextureIndexLRU(swr, true);
        if (tail == -1) {
            fprintf(stderr, "swr: Looks like we can't fit this texture in memory at all. Bummer.\n");
            break;
        }
        
        swrEvictTextureFromCache(swr, tail);
        fprintf(stderr, "swr: Evicted texture %d, trying again.\n", tail);
    }
    while (!pixels);
    
    if (pixels == nullptr) {
        fprintf(stderr, "swr: Failed to decode TXTR page %u.\n", pageId);
        return false;
    }

    swr->textures[pageId] = swrCreateTexture(pixels, w, h);
    free(pixels);
    
    fprintf(stderr, "SWR: Loaded TXTR page %u (%dx%d)\n", pageId, w, h);
    
    // add it to the LRU
    do
    {
        bool added = swrAddTextureIndexToLRU(swr, pageId);
        if (added)
            break;
        
        int tail = swrTailTextureIndexLRU(swr, true);
        if (tail == -1) {
            fprintf(stderr, "swr: Come on now.\n");
            assert(tail != -1);
            return false;
        }
        
        swrEvictTextureFromCache(swr, tail);
    }
    while (true);
    
    return true;
}
