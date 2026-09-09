#ifndef _SW_TEXTURE_LRU_H
#define _SW_TEXTURE_LRU_H

bool swrAddTextureIndexToLRU(SWRenderer* swr, int textureIndex);
int swrTailTextureIndexLRU(SWRenderer* swr, bool remove);
void swrEvictTextureFromCache(SWRenderer* swr, int textureIndex);
bool swrEnsureTextureIsLoaded(SWRenderer* swr, uint32_t pageId);

#endif//_SW_TEXTURE_LRU_H
