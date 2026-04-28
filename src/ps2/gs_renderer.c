#include "gs_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <kernel.h>

#include "binary_reader.h"
#include "binary_utils.h"
#include "utils.h"
#include "text_utils.h"
#include "ps2_utils.h"
#include "matrix_math.h"

#ifdef ENABLE_PS2_RENDERER_LOGS
#define rendererPrintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define rendererPrintf(...) ((void) 0)
#endif

// ===[ Constants ]===
#define ATLAS_WIDTH 512
#define ATLAS_HEIGHT 512
#define PS2_SCREEN_WIDTH 640.0f
#define PS2_SCREEN_HEIGHT 448.0f
#define TEX_HEADER_SIZE 128
#define CLUT4_ENTRY_SIZE 64    // 16 colors * 4 bytes
#define CLUT8_ENTRY_SIZE 1024  // 256 colors * 4 bytes

// ===[ File Loading Helper ]===

// Loads an entire file from host into a memalign'd buffer. Returns size via outSize.
// Aborts on failure.
static uint8_t* loadFileRaw(const char* path, uint32_t* outSize) {
    char* textureBinPath = PS2Utils_createDevicePath(path);

    FILE* f = fopen(textureBinPath, "rb");
    if (f == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", path);
        abort();
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 128-byte aligned for DMA transfers
    uint8_t* data = (uint8_t*) safeMemalign(128, (size_t) size);

    size_t read = fread(data, 1, (size_t) size, f);
    fclose(f);

    if (read != (size_t) size) {
        fprintf(stderr, "GsRenderer: Short read on %s (expected %ld, got %zu)\n", path, size, read);
        abort();
    }

    *outSize = (uint32_t) size;
    free(textureBinPath);
    return data;
}

// ===[ Atlas Loading ]===
static void loadAtlas(GsRenderer* gs) {
    char* atlasBinPath = PS2Utils_createDevicePath("ATLAS.BIN");
    FILE* f = fopen(atlasBinPath, "rb");
    if (f == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", atlasBinPath);
        abort();
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t) ftell(f);
    fseek(f, 0, SEEK_SET);

    BinaryReader reader = BinaryReader_create(f, fileSize);

    uint8_t version = BinaryReader_readUint8(&reader);
    if (version != 0) {
        fprintf(stderr, "GsRenderer: Unsupported ATLAS.BIN version %u\n", version);
        abort();
    }

    gs->atlasTPAGCount = BinaryReader_readUint16(&reader);
    gs->atlasTileCount = BinaryReader_readUint16(&reader);
    gs->atlasCount = BinaryReader_readUint16(&reader);

    // Parse atlas offset table
    gs->atlasOffsets = safeMalloc(gs->atlasCount * sizeof(uint32_t));
    repeat(gs->atlasCount, i) {
        gs->atlasOffsets[i] = BinaryReader_readUint32(&reader);
    }

    // Parse TPAG entries
    gs->atlasTPAGEntries = safeMalloc(gs->atlasTPAGCount * sizeof(AtlasTPAGEntry));

    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        entry->atlasId = BinaryReader_readUint16(&reader);
        entry->atlasX = BinaryReader_readUint16(&reader);
        entry->atlasY = BinaryReader_readUint16(&reader);
        entry->width = BinaryReader_readUint16(&reader);
        entry->height = BinaryReader_readUint16(&reader);
        entry->cropX = BinaryReader_readUint16(&reader);
        entry->cropY = BinaryReader_readUint16(&reader);
        entry->cropW = BinaryReader_readUint16(&reader);
        entry->cropH = BinaryReader_readUint16(&reader);
        entry->clutIndex = BinaryReader_readUint16(&reader);
        entry->bpp = BinaryReader_readUint8(&reader);
    }

    // Parse tile entries
    gs->atlasTileEntries = safeMalloc(gs->atlasTileCount * sizeof(AtlasTileEntry));

    repeat(gs->atlasTileCount, i) {
        AtlasTileEntry* entry = &gs->atlasTileEntries[i];
        entry->bgDef = BinaryReader_readInt16(&reader);
        entry->srcX = BinaryReader_readUint16(&reader);
        entry->srcY = BinaryReader_readUint16(&reader);
        entry->srcW = BinaryReader_readUint16(&reader);
        entry->srcH = BinaryReader_readUint16(&reader);
        entry->atlasId = BinaryReader_readUint16(&reader);
        entry->atlasX = BinaryReader_readUint16(&reader);
        entry->atlasY = BinaryReader_readUint16(&reader);
        entry->width = BinaryReader_readUint16(&reader);
        entry->height = BinaryReader_readUint16(&reader);
        entry->cropX = BinaryReader_readUint16(&reader);
        entry->cropY = BinaryReader_readUint16(&reader);
        entry->cropW = BinaryReader_readUint16(&reader);
        entry->cropH = BinaryReader_readUint16(&reader);
        entry->clutIndex = BinaryReader_readUint16(&reader);
        entry->bpp = BinaryReader_readUint8(&reader);
    }

    fclose(f);

    // Build tile entry hashmap for O(1) lookup
    gs->tileEntryMap = nullptr;
    repeat(gs->atlasTileCount, i) {
        AtlasTileEntry* entry = &gs->atlasTileEntries[i];
        TileLookupKey key = { .bgDef = entry->bgDef, .srcX = entry->srcX, .srcY = entry->srcY, .srcW = entry->srcW, .srcH = entry->srcH };
        hmput(gs->tileEntryMap, key, entry);
    }

    gs->atlasBpp = safeCalloc(gs->atlasCount, sizeof(uint8_t));
    gs->atlasToChunk = safeMalloc(gs->atlasCount * sizeof(int16_t));
    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    // Build bpp table from TPAG and tile entries
    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        if (entry->atlasId != 0xFFFF && gs->atlasCount > entry->atlasId) {
            gs->atlasBpp[entry->atlasId] = entry->bpp;
        }
    }
    repeat(gs->atlasTileCount, i) {
        AtlasTileEntry* entry = &gs->atlasTileEntries[i];
        if (entry->atlasId != 0xFFFF && gs->atlasCount > entry->atlasId) {
            gs->atlasBpp[entry->atlasId] = entry->bpp;
        }
    }

    fprintf(stderr, "GsRenderer: ATLAS.BIN loaded - %u TPAG entries, %u tile entries, %u atlases\n", gs->atlasTPAGCount, gs->atlasTileCount, gs->atlasCount);

    free(atlasBinPath);
}

// ===[ CLUT Loading and VRAM Upload ]===
// Each CLUT is uploaded individually to its own VRAM address. This is necessary because
// the PS2 GS VRAM has a block-swizzled layout - bulk-uploading stacked CLUTs and computing
// linear offsets for CBP does NOT work (the BITBLT write path and CLUT read path use
// block-based addressing, so CLUTs don't land at simple linear offsets within a bulk upload).
static void loadAndUploadCLUTs(GsRenderer* gs) {
    GSGLOBAL* gsGlobal = gs->gsGlobal;

    // 128-byte aligned temp buffer for DMA transfers (reused for each CLUT send)
    // Large enough for one 8bpp CLUT (1024 bytes)
    uint8_t* tempBuf = (uint8_t*) safeMemalign(128, CLUT8_ENTRY_SIZE);

    // Load and upload CLUT4 (4bpp palettes: 16 colors * 4 bytes = 64 bytes each)
    {
        uint32_t clut4FileSize;
        uint8_t* clut4Data = loadFileRaw("CLUT4.BIN", &clut4FileSize);
        gs->clut4Count = clut4FileSize / CLUT4_ENTRY_SIZE;
        fprintf(stderr, "GsRenderer: CLUT4.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut4Count, clut4FileSize);

        gs->clut4VramAddrs = safeMalloc(gs->clut4Count * sizeof(uint32_t));

        repeat(gs->clut4Count, i) {
            // gsKit uploads 4bpp CLUTs as 8x2 CT32 (16 entries in 8-wide, 2-tall grid)
            uint32_t vramSize = gsKit_texture_size(8, 2, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                fprintf(stderr, "GsRenderer: Failed to allocate VRAM for CLUT4 index %u\n", i);
                abort();
            }

            // Copy to aligned temp buffer for DMA
            memcpy(tempBuf, clut4Data + i * CLUT4_ENTRY_SIZE, CLUT4_ENTRY_SIZE);
            gsKit_texture_send((u32*) tempBuf, 8, 2, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut4VramAddrs[i] = vramAddr;
        }

        fprintf(stderr, "GsRenderer: CLUT4 uploaded (%u CLUTs)\n", gs->clut4Count);
        free(clut4Data);
    }

    // Load and upload CLUT8 (8bpp palettes: 256 colors * 4 bytes = 1024 bytes each)
    {
        uint32_t clut8FileSize;
        uint8_t* clut8Data = loadFileRaw("CLUT8.BIN", &clut8FileSize);
        gs->clut8Count = clut8FileSize / CLUT8_ENTRY_SIZE;
        fprintf(stderr, "GsRenderer: CLUT8.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut8Count, clut8FileSize);

        gs->clut8VramAddrs = safeMalloc(gs->clut8Count * sizeof(uint32_t));

        repeat(gs->clut8Count, i) {
            // gsKit uploads 8bpp CLUTs as 16x16 CT32 (256 entries in 16-wide, 16-tall grid)
            uint32_t vramSize = gsKit_texture_size(16, 16, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                fprintf(stderr, "GsRenderer: Failed to allocate VRAM for CLUT8 index %u\n", i);
                abort();
            }

            // 8bpp CLUTs are 1024 bytes; source is 128-byte aligned (1024 is a multiple of 128)
            gsKit_texture_send((u32*) (clut8Data + i * CLUT8_ENTRY_SIZE), 16, 16, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut8VramAddrs[i] = vramAddr;
        }

        fprintf(stderr, "GsRenderer: CLUT8 uploaded (%u CLUTs)\n", gs->clut8Count);
        free(clut8Data);
    }

    free(tempBuf);

    fprintf(stderr, "GsRenderer: VRAM after CLUTs: 0x%08X / 0x%08X\n", gsGlobal->CurrentPointer, GS_VRAM_SIZE);
}

// ===[ VRAM Texture Cache (Buddy System with LRU Eviction) ]===
// Manages a pool of 128KB VRAM chunks for atlas textures.
// 4bpp atlases use 1 chunk, 8bpp atlases use 2 consecutive chunks.

#define FONTM_RESERVED_VRAM 65536 // 64KB reserved for gsKit's TexManager (FONTM debug overlay)

// Initializes the chunk pool from the remaining VRAM after CLUTs.
// Reserves 64KB at the end for gsKit's TexManager (used by FONTM).
// Our chunk pool occupies the middle, between CLUTs and the FONTM region.
//
// VRAM layout: [Framebuffers] [CLUTs] [Chunk Pool ...] [64KB FONTM]
static void initTextureCache(GsRenderer* gs) {
    gs->textureVramBase = gs->gsGlobal->CurrentPointer;
    uint32_t availableVram = GS_VRAM_SIZE - gs->textureVramBase - FONTM_RESERVED_VRAM;
    gs->chunkCount = availableVram / VRAM_CHUNK_SIZE;

    gs->chunks = safeMalloc(gs->chunkCount * sizeof(VRAMChunk));
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    gs->frameCounter = 1;

    // Advance CurrentPointer past our chunk pool so the TexManager only
    // manages the 64KB FONTM region at the end of VRAM.
    gs->gsGlobal->CurrentPointer = gs->textureVramBase + gs->chunkCount * VRAM_CHUNK_SIZE;
    gsKit_TexManager_init(gs->gsGlobal);

    uint32_t fontmVram = GS_VRAM_SIZE - gs->gsGlobal->CurrentPointer;
    fprintf(stderr, "GsRenderer: Texture cache initialized - %u chunks (%u KB each), base 0x%08X, %u KB for textures, %u KB for FONTM\n", gs->chunkCount, VRAM_CHUNK_SIZE / 1024, gs->textureVramBase, gs->chunkCount * (VRAM_CHUNK_SIZE / 1024), fontmVram / 1024);
}

// Find the first run of consecutive free chunks.
// Returns the index of the first chunk, or -1 if not found.
static int32_t findConsecutiveFreeChunks(GsRenderer* gs, int chunksNeeded) {
    int consecutive = 0;
    forEachIndexed(VRAMChunk, chunk, i, gs->chunks, gs->chunkCount) {
        if (0 > chunk->atlasId) {
            consecutive++;
            if (consecutive >= chunksNeeded) {
                return (int32_t) (i - (uint32_t) chunksNeeded + 1);
            }
        } else {
            consecutive = 0;
        }
    }
    return -1;
}

// Count total free chunks.
static uint32_t countFreeChunks(GsRenderer* gs) {
    uint32_t count = 0;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (0 > chunk->atlasId)
            count++;
    }
    return count;
}

// Find the atlas with the oldest lastUsed time (LRU victim).
// Returns the atlasId, or -1 if no loaded atlases.
static int16_t findLRUVictim(GsRenderer* gs, bool* wasUsedOnThisFrame) {
    uint64_t oldest = UINT64_MAX;
    int16_t victimAtlas = -1;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->atlasId >= 0 && oldest > chunk->lastUsed) {
            oldest = chunk->lastUsed;
            victimAtlas = chunk->atlasId;
        }
    }
    if (victimAtlas != -1)
        *wasUsedOnThisFrame = oldest == gs->frameCounter;
    return victimAtlas;
}

// Evict an atlas from the cache, freeing its chunk(s).
static void evictAtlas(GsRenderer* gs, int16_t atlasId) {
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->atlasId == atlasId) {
            chunk->atlasId = -1;
            chunk->lastUsed = 0;
        }
    }

    if (atlasId >= 0 && gs->atlasCount > (uint16_t) atlasId) {
        gs->atlasToChunk[atlasId] = -1;
    }

    uint32_t availableChunks = countFreeChunks(gs);
    rendererPrintf("GsRenderer: Evicted atlas %d from VRAM (available chunks = %d)\n", atlasId, availableChunks);
}

// Defragment the texture cache by evicting all loaded atlases.
// They will be reloaded on-demand as needed during subsequent draw calls.
static void defragTextureCache(GsRenderer* gs) {
    rendererPrintf("GsRenderer: Defragmenting VRAM texture cache...\n");

    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    rendererPrintf("GsRenderer: Defrag complete - all %u chunks freed\n", gs->chunkCount);
}

// Allocate consecutive chunks for an atlas. Evicts LRU victims or defrags if needed.
// Returns the first chunk index, or -1 if VRAM is truly exhausted.
static int32_t allocateChunks(GsRenderer* gs, int chunksNeeded) {
    // Attempt 1: find free consecutive chunks
    int32_t idx = findConsecutiveFreeChunks(gs, chunksNeeded);
    if (idx >= 0) return idx;

    // Attempt 2: evict LRU victims one at a time until space is found
    repeat(gs->chunkCount, attempts) {
        bool wasUsedOnThisFrame = false;

        int16_t victim = findLRUVictim(gs, &wasUsedOnThisFrame);
        if (0 > victim)
            break;

        // We only need to flush if the victim was used on this frame
        // If it wasn't, then we can evict with no care in the world
        if (wasUsedOnThisFrame) {
            rendererPrintf("GsRenderer: Flushing draw queue before VRAM evicting because atlas was used on the current frame\n");
            gs->evictedAtlasUsedInCurrentFrame = true;
            gsKit_queue_exec(gs->gsGlobal);
        }

        evictAtlas(gs, victim);

        idx = findConsecutiveFreeChunks(gs, chunksNeeded);

        if (idx >= 0)
            return idx;
    }

    // At this point we are lost, just flush and hope for the best
    gs->evictedAtlasUsedInCurrentFrame = true;
    rendererPrintf("GsRenderer: Flushing draw queue before VRAM defrag\n");
    gsKit_queue_exec(gs->gsGlobal);

    // Attempt 3: defrag - evict ALL and let them reload on demand
    // Handles fragmentation where enough free chunks exist but aren't consecutive
    if (countFreeChunks(gs) >= (uint32_t) chunksNeeded) {
        defragTextureCache(gs);
        idx = findConsecutiveFreeChunks(gs, chunksNeeded);

        if (idx >= 0)
            return idx;
    }

    // VRAM truly exhausted
    return -1;
}

// ===[ EE RAM Atlas Cache (Bump Allocator with LRU Eviction + Compaction) ]===
// Caches uncompressed atlas pixel data in a EE RAM buffer, allowingzero-copy DMA uploads to VRAM without per-upload decompression or temp allocations.

#define EE_CACHE_CAPACITY (2 * 1024 * 1024) // 2 MiB

// Uncompressed pixel data size for a 512x512 atlas at the given bpp.
static uint32_t atlasUncompressedSize(uint8_t bpp) {
    return (bpp == 4) ? (ATLAS_WIDTH * ATLAS_HEIGHT / 2) : (ATLAS_WIDTH * ATLAS_HEIGHT);
}

// Decompress atlas pixel data from a compressed buffer (TEX_HEADER_SIZE header + RLE/raw payload).
// Writes uncompressed indexed pixels into outBuf (must be large enough and 128-byte aligned).
static void decompressAtlasPixels(const uint8_t* compressedData, uint8_t* outBuf) {
    uint16_t width = BinaryUtils_readUint16(compressedData + 1);
    uint16_t height = BinaryUtils_readUint16(compressedData + 3);
    uint8_t bpp = BinaryUtils_readUint8(compressedData + 5);
    uint32_t pixelDataSize = BinaryUtils_readUint32(compressedData + 6);
    uint8_t compressionType = BinaryUtils_readUint8(compressedData + 10);

    uint32_t uncompressedSize = (bpp == 4) ? (uint32_t) ((width * height + 1) / 2) : (uint32_t) (width * height);
    const uint8_t* rawData = compressedData + TEX_HEADER_SIZE;

    if (compressionType == 1) {
        // RLE decompression
        uint32_t srcPos = 0, dstPos = 0;
        while (pixelDataSize > srcPos + 1 && uncompressedSize > dstPos) {
            uint8_t runLength = rawData[srcPos++];
            uint8_t value = rawData[srcPos++];
            for (uint8_t j = 0; runLength > j && uncompressedSize > dstPos; j++) {
                outBuf[dstPos++] = value;
            }
        }
    } else {
        memcpy(outBuf, rawData, uncompressedSize);
    }
}

// Initialize the EE RAM cache. Called from gsInit after opening TEXTURES.BIN.
static void initEeCache(GsRenderer* gs) {
    gs->eeCacheCapacity = EE_CACHE_CAPACITY;
    gs->eeCacheBumpPtr = 0;
    gs->eeCache = (uint8_t*) safeMemalign(128, EE_CACHE_CAPACITY);

    gs->eeCacheEntries = safeMalloc(gs->atlasCount * sizeof(EeAtlasCacheEntry));
    repeat(gs->atlasCount, i) {
        gs->eeCacheEntries[i].atlasId = -1;
        gs->eeCacheEntries[i].offset = 0;
        gs->eeCacheEntries[i].size = 0;
        gs->eeCacheEntries[i].lastUsed = 0;
    }

    // Compute on-disk sizes from offset table
    gs->atlasDataSizes = safeMalloc(gs->atlasCount * sizeof(uint32_t));

    // Get total file size for the last atlas
    fseek(gs->texturesFile, 0, SEEK_END);
    uint32_t texturesFileSize = (uint32_t) ftell(gs->texturesFile);

    repeat(gs->atlasCount, i) {
        if (gs->atlasCount - 1 > i) {
            gs->atlasDataSizes[i] = gs->atlasOffsets[i + 1] - gs->atlasOffsets[i];
        } else {
            gs->atlasDataSizes[i] = texturesFileSize - gs->atlasOffsets[i];
        }
    }
}

// Preload atlases sequentially into the EE cache until the buffer is full.
// Reads compressed data from disc, decompresses, and stores uncompressed pixels in the cache.
static void preloadEeCache(GsRenderer* gs) {
    uint32_t preloaded = 0;

    // Allocate a temp buffer for reading compressed data from disc
    uint32_t maxDiskSize = 0;
    repeat(gs->atlasCount, i) {
        if (gs->atlasDataSizes[i] > maxDiskSize) maxDiskSize = gs->atlasDataSizes[i];
    }
    uint8_t* tempBuf = (uint8_t*) safeMemalign(128, maxDiskSize);

    repeat(gs->atlasCount, i) {
        uint8_t bpp = gs->atlasBpp[i];
        uint32_t uncompSize = atlasUncompressedSize(bpp);
        if (gs->eeCacheBumpPtr + uncompSize > gs->eeCacheCapacity) {
            break;
        }

        // Read compressed data from disc into temp buffer
        uint32_t dataSize = gs->atlasDataSizes[i];
        fseek(gs->texturesFile, (long) gs->atlasOffsets[i], SEEK_SET);
        size_t bytesRead = fread(tempBuf, 1, dataSize, gs->texturesFile);
        if (bytesRead != dataSize) {
            fprintf(stderr, "GsRenderer: EE cache preload short read for atlas %u (expected %u, got %zu)\n", i, dataSize, bytesRead);
            break;
        }

        // Decompress directly into the EE cache
        decompressAtlasPixels(tempBuf, gs->eeCache + gs->eeCacheBumpPtr);

        gs->eeCacheEntries[i].atlasId = (int16_t) i;
        gs->eeCacheEntries[i].offset = gs->eeCacheBumpPtr;
        gs->eeCacheEntries[i].size = uncompSize;
        gs->eeCacheEntries[i].lastUsed = gs->frameCounter;

        gs->eeCacheBumpPtr += uncompSize;
        preloaded++;
    }

    free(tempBuf);

    fprintf(stderr, "GsRenderer: EE cache initialized - %u MB, %u atlases preloaded (%u KB used)\n", EE_CACHE_CAPACITY / (1024 * 1024), preloaded, gs->eeCacheBumpPtr / 1024);
}

// Look up an atlas in the EE cache. Returns pointer to cached data or nullptr.
static uint8_t* eeCacheLookup(GsRenderer* gs, uint16_t atlasId) {
    if (atlasId >= gs->atlasCount) return nullptr;
    if (0 > gs->eeCacheEntries[atlasId].atlasId) return nullptr;

    gs->eeCacheEntries[atlasId].lastUsed = gs->frameCounter;
    return gs->eeCache + gs->eeCacheEntries[atlasId].offset;
}

// Compact the EE cache by closing gaps from evicted entries.
static void compactEeCache(GsRenderer* gs) {
    // Collect live entries sorted by offset using insertion sort
    // (max 146 atlases, so a stack array + insertion sort is fine)
    uint16_t liveIds[256]; // More than enough for 146 atlases
    uint32_t liveCount = 0;

    repeat(gs->atlasCount, i) {
        if (gs->eeCacheEntries[i].atlasId >= 0) {
            // Insertion sort by offset
            uint32_t insertPos = liveCount;
            while (insertPos > 0 && gs->eeCacheEntries[liveIds[insertPos - 1]].offset > gs->eeCacheEntries[i].offset) {
                liveIds[insertPos] = liveIds[insertPos - 1];
                insertPos--;
            }
            liveIds[insertPos] = (uint16_t) i;
            liveCount++;
        }
    }

    // Walk and memmove each entry down to close gaps
    uint32_t writePtr = 0;
    repeat(liveCount, i) {
        EeAtlasCacheEntry* entry = &gs->eeCacheEntries[liveIds[i]];
        if (entry->offset != writePtr) {
            memmove(gs->eeCache + writePtr, gs->eeCache + entry->offset, entry->size);
            entry->offset = writePtr;
        }
        writePtr += entry->size;
    }

    gs->eeCacheBumpPtr = writePtr;
}

// Evict LRU entries until spaceNeeded bytes are available. Returns true on success.
static bool eeCacheEvictLRU(GsRenderer* gs, uint32_t spaceNeeded) {
    // Calculate total live bytes to determine how much space we can free
    uint32_t liveBytes = 0;
    repeat(gs->atlasCount, i) {
        if (gs->eeCacheEntries[i].atlasId >= 0) {
            liveBytes += gs->eeCacheEntries[i].size;
        }
    }

    // Evict LRU entries until enough space would be freed after compaction
    while (gs->eeCacheCapacity - liveBytes < spaceNeeded) {
        // Find entry with smallest lastUsed
        uint64_t oldest = UINT64_MAX;
        int16_t victimId = -1;

        repeat(gs->atlasCount, i) {
            if (gs->eeCacheEntries[i].atlasId >= 0 && oldest > gs->eeCacheEntries[i].lastUsed) {
                oldest = gs->eeCacheEntries[i].lastUsed;
                victimId = (int16_t) i;
            }
        }

        if (0 > victimId) {
            break;
        }

        liveBytes -= gs->eeCacheEntries[victimId].size;
        gs->eeCacheEntries[victimId].atlasId = -1;
    }

    compactEeCache(gs);

    return gs->eeCacheCapacity - gs->eeCacheBumpPtr >= spaceNeeded;
}

// Insert atlas data into the EE cache. Evicts LRU entries if needed.
static void eeCacheInsert(GsRenderer* gs, uint16_t atlasId, const uint8_t* data, uint32_t size) {
    if (size > gs->eeCacheCapacity) {
        // Atlas too large to ever fit in the cache
        return;
    }

    if (gs->eeCacheBumpPtr + size > gs->eeCacheCapacity) {
        if (!eeCacheEvictLRU(gs, size)) {
            rendererPrintf("GsRenderer: EE cache eviction failed for atlas %u (%u bytes)\n", atlasId, size);
            return;
        }
    }

    memcpy(gs->eeCache + gs->eeCacheBumpPtr, data, size);

    gs->eeCacheEntries[atlasId].atlasId = (int16_t) atlasId;
    gs->eeCacheEntries[atlasId].offset = gs->eeCacheBumpPtr;
    gs->eeCacheEntries[atlasId].size = size;
    gs->eeCacheEntries[atlasId].lastUsed = gs->frameCounter;

    gs->eeCacheBumpPtr += size;
}

// Upload atlas pixel data to the given VRAM chunk(s).
// On cache hit: zero-copy DMA directly from EE cache (no decompression, no temp allocations).
// On cache miss: reads compressed data from TEXTURES.BIN, decompresses, inserts into EE cache, then uploads.
static void uploadAtlasToChunk(GsRenderer* gs, uint16_t atlasId, int32_t firstChunk) {
    uint8_t* uploadData = eeCacheLookup(gs, atlasId);
    uint8_t* tempPixelData = nullptr; // Non-null only if we need to free it after upload
    const char* atlasSource = "RAM";

    if (uploadData == nullptr) {
        // Cache miss: read compressed data from TEXTURES.BIN and decompress
        uint32_t dataSize = gs->atlasDataSizes[atlasId];
        uint8_t* compressedBuf = (uint8_t*) safeMemalign(128, dataSize);

        fseek(gs->texturesFile, (long) gs->atlasOffsets[atlasId], SEEK_SET);
        size_t bytesRead = fread(compressedBuf, 1, dataSize, gs->texturesFile);
        if (bytesRead != dataSize) {
            fprintf(stderr, "GsRenderer: Short read for atlas %u (expected %u, got %zu)\n", atlasId, dataSize, bytesRead);
            abort();
        }

        uint8_t bpp = gs->atlasBpp[atlasId];
        uint32_t uncompSize = atlasUncompressedSize(bpp);
        tempPixelData = (uint8_t*) safeMemalign(128, uncompSize);
        decompressAtlasPixels(compressedBuf, tempPixelData);
        free(compressedBuf);

        // Try to insert uncompressed data into EE cache
        eeCacheInsert(gs, atlasId, tempPixelData, uncompSize);
        atlasSource = "disk";
        gs->diskLoadsThisFrame++;

        uploadData = eeCacheLookup(gs, atlasId);
        if (uploadData != nullptr) {
            // Insert succeeded, use cached copy for DMA upload
            free(tempPixelData);
            tempPixelData = nullptr;
        } else {
            // EE cache insert failed, upload directly from temp buffer
            rendererPrintf("GsRenderer: EE cache insert failed for atlas %u, uploading directly\n", atlasId);
            uploadData = tempPixelData;
        }
    }

    // Upload pixel data to VRAM
    uint8_t bpp = gs->atlasBpp[atlasId];
    uint8_t psm = (bpp == 4) ? GS_PSM_T4 : GS_PSM_T8;
    uint32_t tbw = ATLAS_WIDTH / 64;
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) firstChunk * VRAM_CHUNK_SIZE;

    gsKit_texture_send((u32*) uploadData, ATLAS_WIDTH, ATLAS_HEIGHT, vramAddr, psm, tbw, GS_CLUT_TEXTURE);

    // Update chunk state
    int chunksUsed = (bpp == 8) ? 2 : 1;
    repeat(chunksUsed, i) {
        gs->chunks[firstChunk + i].atlasId = (int16_t) atlasId;
        gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
    }
    gs->atlasToChunk[atlasId] = (int16_t) firstChunk;

    rendererPrintf("GsRenderer: Atlas %u uploaded to chunk %d (VRAM 0x%08X, %ubpp, src: %s)\n", atlasId, firstChunk, vramAddr, bpp, atlasSource);

    free(tempPixelData);
}

// Ensure an atlas is loaded into VRAM, using LRU eviction if needed.
// Returns true on success, false on failure.
static bool ensureAtlasLoaded(GsRenderer* gs, uint16_t atlasId) {
    if (atlasId >= gs->atlasCount) {
        fprintf(stderr, "GsRenderer: Atlas ID %u out of range (max %u)\n", atlasId, gs->atlasCount - 1);
        return false;
    }

    // Already loaded? Just touch LRU timestamp
    if (gs->atlasToChunk[atlasId] >= 0) {
        int16_t firstChunk = gs->atlasToChunk[atlasId];
        uint8_t bpp = gs->atlasBpp[atlasId];
        int chunksUsed = (bpp == 8) ? 2 : 1;

        // Track unique atlases per frame (first touch = lastUsed hasn't been updated yet)
        if (gs->chunks[firstChunk].lastUsed != gs->frameCounter) {
            gs->uniqueAtlasesThisFrame++;
            gs->chunksNeededThisFrame += (uint16_t) chunksUsed;
        }

        repeat(chunksUsed, i) {
            gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
        }
        return true;
    }

    // Determine how many chunks we need
    uint8_t bpp = gs->atlasBpp[atlasId];
    if (bpp != 4 && bpp != 8) {
        fprintf(stderr, "GsRenderer: Atlas %u has unknown bpp %u\n", atlasId, bpp);
        return false;
    }
    int chunksNeeded = (bpp == 8) ? 2 : 1;

    // Fresh load is always a new unique atlas this frame
    gs->uniqueAtlasesThisFrame++;
    gs->chunksNeededThisFrame += (uint16_t) chunksNeeded;

    // Allocate chunks (may evict or defrag)
    int32_t chunkIdx = allocateChunks(gs, chunksNeeded);
    if (0 > chunkIdx) {
        fprintf(stderr, "GsRenderer: VRAM exhausted! Cannot allocate %d chunk(s) for atlas %u (%ubpp)\n", chunksNeeded, atlasId, bpp);
        abort();
    }

    // Load TEX file and upload to the allocated chunk(s)
    uploadAtlasToChunk(gs, atlasId, chunkIdx);
    return true;
}

// ===[ GSTEXTURE setup for a given TPAG entry ]===
// Configures a GSTEXTURE struct for rendering a specific atlas region.
// The GSTEXTURE points to the atlas's VRAM location and the appropriate CLUT.
static bool setupTextureForTPAG(GsRenderer* gs, GSTEXTURE* tex, int32_t tpagIndex) {
    if (0 > tpagIndex || (uint32_t) tpagIndex >= gs->atlasTPAGCount) return false;

    AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
    if (entry->atlasId == 0xFFFF) return false;

    // Ensure the atlas texture is loaded into VRAM (may trigger LRU eviction)
    if (!ensureAtlasLoaded(gs, entry->atlasId))
        return false;

    // Compute VRAM address from chunk index
    int16_t chunkIdx = gs->atlasToChunk[entry->atlasId];
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) chunkIdx * VRAM_CHUNK_SIZE;

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = ATLAS_WIDTH;
    tex->Height = ATLAS_HEIGHT;
    tex->TBW = ATLAS_WIDTH / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (entry->bpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            fprintf(stderr, "GsRenderer: CLUT4 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut4Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            fprintf(stderr, "GsRenderer: CLUT8 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut8Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Tile Lookup and Texture Setup ]===

// Finds a tile entry by (bgDef, srcX, srcY, srcW, srcH). Returns nullptr if not found.
static AtlasTileEntry* findTileEntry(GsRenderer* gs, int16_t bgDef, uint16_t srcX, uint16_t srcY, uint16_t srcW, uint16_t srcH) {
    TileLookupKey key = { .bgDef = bgDef, .srcX = srcX, .srcY = srcY, .srcW = srcW, .srcH = srcH };
    ptrdiff_t idx = hmgeti(gs->tileEntryMap, key);
    if (idx == -1) return nullptr;
    return gs->tileEntryMap[idx].value;
}

// Configures a GSTEXTURE for rendering a tile entry. Same logic as setupTextureForTPAG but for AtlasTileEntry.
static bool setupTextureForTile(GsRenderer* gs, GSTEXTURE* tex, AtlasTileEntry* entry) {
    if (entry->atlasId == 0xFFFF) return false;

    if (!ensureAtlasLoaded(gs, entry->atlasId))
        return false;

    int16_t chunkIdx = gs->atlasToChunk[entry->atlasId];
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) chunkIdx * VRAM_CHUNK_SIZE;

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = ATLAS_WIDTH;
    tex->Height = ATLAS_HEIGHT;
    tex->TBW = ATLAS_WIDTH / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (entry->bpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            fprintf(stderr, "GsRenderer: CLUT4 index %u out of range (max %u) for tile (bg=%d)\n", entry->clutIndex, gs->clut4Count - 1, entry->bgDef);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            fprintf(stderr, "GsRenderer: CLUT8 index %u out of range (max %u) for tile (bg=%d)\n", entry->clutIndex, gs->clut8Count - 1, entry->bgDef);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Vtable Implementations ]===

static void gsInit(Renderer* renderer, DataWin* dataWin) {
    GsRenderer* gs = (GsRenderer*) renderer;

    renderer->dataWin = dataWin;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;

    // Enable alpha blending
    gs->gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    // Alpha blend: (Cs - Cd) * As / 128 + Cd (standard source-over)
    gsKit_set_primalpha(gs->gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    // Load atlas metadata
    loadAtlas(gs);

    // Open TEXTURES.BIN and keep it open for on-demand atlas loading
    char* texturesBinPath = PS2Utils_createDevicePath("TEXTURES.BIN");
    gs->texturesFile = fopen(texturesBinPath, "rb");
    if (gs->texturesFile == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", texturesBinPath);
        abort();
    }
    setvbuf(gs->texturesFile, nullptr, _IOFBF, 128 * 1024);
    free(texturesBinPath);

    // Upload CLUTs to VRAM
    loadAndUploadCLUTs(gs);

    // Initialize the texture cache chunk pool (uses remaining VRAM after CLUTs)
    initTextureCache(gs);

    // Initialize EE RAM cache for compressed atlas data
    initEeCache(gs);
    preloadEeCache(gs);

    fprintf(stderr, "GsRenderer: Initialized (textured mode)\n");
}

static void gsDestroy(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (gs->texturesFile != nullptr) {
        fclose(gs->texturesFile);
    }
    free(gs->atlasOffsets);
    free(gs->atlasTPAGEntries);
    free(gs->atlasTileEntries);
    hmfree(gs->tileEntryMap);
    free(gs->chunks);
    free(gs->atlasToChunk);
    free(gs->atlasBpp);
    free(gs->clut4VramAddrs);
    free(gs->clut8VramAddrs);
    free(gs->eeCache);
    free(gs->eeCacheEntries);
    free(gs->atlasDataSizes);
    free(gs);
}

static void gsBeginFrame(Renderer* renderer, MAYBE_UNUSED int32_t gameW, MAYBE_UNUSED int32_t gameH, MAYBE_UNUSED int32_t windowW, MAYBE_UNUSED int32_t windowH) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->frameCounter++;
    gs->evictedAtlasUsedInCurrentFrame = false;
    gs->uniqueAtlasesThisFrame = 0;
    gs->chunksNeededThisFrame = 0;
    gs->diskLoadsThisFrame = 0;
}

static void gsEndFrame(MAYBE_UNUSED Renderer* renderer) {
    // No-op: flip happens in main loop
}

static void gsBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED float viewAngle) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->viewX = viewX;
    gs->viewY = viewY;

    // Scale game view to PS2 screen (640x448 NTSC interlaced)
    if (viewW > 0 && viewH > 0) {
        gs->scaleX = 640.0f / (float) viewW;
        gs->scaleY = gs->scaleX;
    } else {
        gs->scaleX = 2.0f;
        gs->scaleY = 2.0f;
    }

    // Center vertically
    float renderedH = (float) viewH * gs->scaleY;
    gs->offsetX = 0.0f;
    gs->offsetY = (448.0f - renderedH) / 2.0f;
}

static void gsEndView(MAYBE_UNUSED Renderer* renderer) {
    // No-op
}

static void gsBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->viewX = 0;
    gs->viewY = 0;

    if (guiW > 0 && guiH > 0) {
        gs->scaleX = 640.0f / (float) guiW;
        gs->scaleY = gs->scaleX;
    } else {
        gs->scaleX = 2.0f;
        gs->scaleY = 2.0f;
    }

    float renderedH = (float) guiH * gs->scaleY;
    gs->offsetX = 0.0f;
    gs->offsetY = (448.0f - renderedH) / 2.0f;
}

static void gsEndGUI(MAYBE_UNUSED Renderer* renderer) {
    // No-op
}

static void gsDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Get crop region from atlas entry (falls back to full bounding box if unmapped)
    float cropX = 0.0f, cropY = 0.0f;
    float cropW = (float) tpag->boundingWidth;
    float cropH = (float) tpag->boundingHeight;
    if (gs->atlasTPAGCount > (uint32_t) tpagIndex) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
        if (entry->atlasId != 0xFFFF) {
            cropX = (float) entry->cropX;
            cropY = (float) entry->cropY;
            cropW = (float) entry->cropW;
            cropH = (float) entry->cropH;
        }
    }

    // Compute 4 screen-space corners (tristrip Z-pattern: top-left, top-right, bottom-left, bottom-right)
    // sx0/sy0 = top-left, sx1/sy1 = top-right, sx2/sy2 = bottom-left, sx3/sy3 = bottom-right
    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    bool hasRotation = angleDeg != 0.0f;

    if (hasRotation) {
        // Rotated: compute 4 transformed corners via matrix, same approach as the GLFW renderer
        // Position the cropped region within the original bounding box
        float localX0 = cropX - originX;
        float localY0 = cropY - originY;
        float localX1 = cropX + cropW - originX;
        float localY1 = cropY + cropH - originY;

        // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
        // Negate angle because Y-down coordinate system
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        Matrix4f transform;
        Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

        float gx0, gy0, gx1, gy1, gx2, gy2, gx3, gy3;
        Matrix4f_transformPoint(&transform, localX0, localY0, &gx0, &gy0); // top-left
        Matrix4f_transformPoint(&transform, localX1, localY0, &gx1, &gy1); // top-right
        Matrix4f_transformPoint(&transform, localX0, localY1, &gx2, &gy2); // bottom-left
        Matrix4f_transformPoint(&transform, localX1, localY1, &gx3, &gy3); // bottom-right

        // Apply view offset and scale
        sx0 = (gx0 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy0 = (gy0 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx1 = (gx1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy1 = (gy1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx2 = (gx2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy2 = (gy2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx3 = (gx3 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy3 = (gy3 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    } else {
        // Axis-aligned: simple rect math
        // Position the cropped region within the original bounding box
        float gameX1 = x + (cropX - originX) * xscale;
        float gameY1 = y + (cropY - originY) * yscale;
        float gameX2 = x + (cropX + cropW - originX) * xscale;
        float gameY2 = y + (cropY + cropH - originY) * yscale;

        // Apply view offset and scale
        sx0 = (gameX1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy0 = (gameY1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx1 = (gameX2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy1 = (gameY1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx2 = (gameX1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy2 = (gameY2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx3 = (gameX2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy3 = (gameY2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    }

    // View frustum culling: skip if entirely off-screen (handles negative scales via min/max)
    float minSX = fminf(fminf(sx0, sx1), fminf(sx2, sx3));
    float maxSX = fmaxf(fmaxf(sx0, sx1), fmaxf(sx2, sx3));
    float minSY = fminf(fminf(sy0, sy1), fminf(sy2, sy3));
    float maxSY = fmaxf(fmaxf(sy0, sy1), fmaxf(sy2, sy3));
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT)
        return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        // Fallback: draw colored quad if no atlas mapping
        uint8_t r = BGR_R(color);
        uint8_t g = BGR_G(color);
        uint8_t b = BGR_B(color);
        uint8_t a = alphaToGS(alpha);
        u64 fallbackColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
        if (hasRotation) {
            gsKit_prim_quad(gs->gsGlobal, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, 0, fallbackColor);
        } else {
            gsKit_prim_sprite(gs->gsGlobal, sx0, sy0, sx3, sy3, 0, fallbackColor);
        }
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];

    // The atlas entry has the actual sprite dimensions in the atlas (post-crop, post-resize).
    // The screen rect covers cropW x cropH game-space pixels, positioned at (cropX, cropY)
    // within the original bounding box. The GS hardware stretches the atlas texels to fill.

    // UV coords within the 512x512 atlas (in texels for gsKit)
    float u0 = (float) atlasEntry->atlasX;
    float v0 = (float) atlasEntry->atlasY;
    float u1 = u0 + (float) atlasEntry->width;
    float v1 = v0 + (float) atlasEntry->height;

    // GS modulate mode: Output = Texture * Vertex / 128
    // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 128 (1.0x multiplier)
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    if (hasRotation) {
        // Tristrip Z-pattern: needs 4 vertices for rotated quads
        gsKit_prim_quad_texture(
            gs->gsGlobal,
            &tex,
            sx0, sy0, u0, v0, // top-left
            sx1, sy1, u1, v0, // top-right
            sx2, sy2, u0, v1, // bottom-left
            sx3, sy3, u1, v1, // bottom-right
            0,
            gsColor
        );
    } else {
        gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx3, sy3, u1, v1, 0, gsColor);
    }
}

static void gsDrawTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    float cropX = 0.0f, cropY = 0.0f;
    float cropW = (float) tpag->boundingWidth;
    float cropH = (float) tpag->boundingHeight;
    if (gs->atlasTPAGCount > (uint32_t) tpagIndex) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
        if (entry->atlasId != 0xFFFF) {
            cropX = (float) entry->cropX;
            cropY = (float) entry->cropY;
            cropW = (float) entry->cropW;
            cropH = (float) entry->cropH;
        }
    }

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;
    if (0 >= tileW || 0 >= tileH) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(x - originX * axScale, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = x - originX * axScale;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(y - originY * ayScale, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = y - originY * ayScale;
        endY = startY + tileH;
    }

    // Per-tile quad layout in world space, derived from gsDrawSprite's axis-aligned path.
    // For tile origin dx, the wrapper-equivalent x_call = dx + originX * axScale, then:
    //   gameX1 = x_call + (cropX - originX) * xscale = dx + cropX * xscale + originX * (axScale - xscale)
    // Same shape for Y. Both reduce to dx + cropX*xscale when xscale > 0 (the common case).
    float dxLocalX0 = cropX * xscale + originX * (axScale - xscale);
    float dyLocalY0 = cropY * yscale + originY * (ayScale - yscale);
    float tileGameW = cropW * xscale;
    float tileGameH = cropH * yscale;

    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) return;

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
    float u0 = (float) atlasEntry->atlasX;
    float v0 = (float) atlasEntry->atlasY;
    float u1 = u0 + (float) atlasEntry->width;
    float v1 = v0 + (float) atlasEntry->height;

    // 0x80 is the exact 1.0x multiplier in GS modulate mode (output = texture * vertex / 128).
    // So, to avoid dimming the texture (BGR_R(0xFFFFFF) >> 1 = 0x7F is 0.992x) we'll hand-pick the white case.
    uint8_t r, g, b;
    if (color == 0xFFFFFFu) {
        r = g = b = 0x80;
    } else {
        r = BGR_R(color) >> 1;
        g = BGR_G(color) >> 1;
        b = BGR_B(color) >> 1;
    }
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    float viewBaseX = -(float) gs->viewX;
    float viewBaseY = -(float) gs->viewY;
    float viewScaleX = gs->scaleX;
    float viewScaleY = gs->scaleY;
    float viewOffX = gs->offsetX;
    float viewOffY = gs->offsetY;

    // Integer tile counts avoid FP-comparison drift; the inner break handles overshoot at the boundary
    int32_t tilesX = tileX ? ((int32_t) ((endX - startX) / tileW) + 1) : 1;
    int32_t tilesY = tileY ? ((int32_t) ((endY - startY) / tileH) + 1) : 1;
    if (0 >= tilesX || 0 >= tilesY) return;

    repeat(tilesY, iy) {
        float dy = startY + (float) iy * tileH;
        if (dy >= endY) break;

        float gameY1 = dy + dyLocalY0 + viewBaseY;
        float gameY2 = gameY1 + tileGameH;
        float sy0 = gameY1 * viewScaleY + viewOffY;
        float sy1 = gameY2 * viewScaleY + viewOffY;

        float minSY = sy0 < sy1 ? sy0 : sy1;
        float maxSY = sy0 > sy1 ? sy0 : sy1;
        if (0.0f > maxSY || minSY > PS2_SCREEN_HEIGHT) continue;

        repeat(tilesX, ix) {
            float dx = startX + (float) ix * tileW;
            if (endX <= dx) break;

            float gameX1 = dx + dxLocalX0 + viewBaseX;
            float gameX2 = gameX1 + tileGameW;
            float sx0 = gameX1 * viewScaleX + viewOffX;
            float sx1 = gameX2 * viewScaleX + viewOffX;

            float minSX = sx0 < sx1 ? sx0 : sx1;
            float maxSX = sx0 > sx1 ? sx0 : sx1;
            if (0.0f > maxSX || minSX > PS2_SCREEN_WIDTH) continue;

            gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx1, sy1, u1, v1, 0, gsColor);
        }
    }
}

static void gsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= renderer->dataWin->tpag.count) return;

    // Compute screen position
    float gameX1 = x - (float) gs->viewX;
    float gameY1 = y - (float) gs->viewY;
    float gameX2 = gameX1 + (float) srcW * xscale;
    float gameY2 = gameY1 + (float) srcH * yscale;

    float sx1 = gameX1 * gs->scaleX + gs->offsetX;
    float sy1 = gameY1 * gs->scaleY + gs->offsetY;
    float sx2 = gameX2 * gs->scaleX + gs->offsetX;
    float sy2 = gameY2 * gs->scaleY + gs->offsetY;

    // View frustum culling: skip if entirely off-screen (handles negative scales via min/max)
    float minSX = (sx1 < sx2) ? sx1 : sx2;
    float maxSX = (sx1 > sx2) ? sx1 : sx2;
    float minSY = (sy1 < sy2) ? sy1 : sy2;
    float maxSY = (sy1 > sy2) ? sy1 : sy2;
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT) return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        // Fallback: draw colored rectangle
        uint8_t r = BGR_R(color);
        uint8_t g = BGR_G(color);
        uint8_t b = BGR_B(color);
        uint8_t a = alphaToGS(alpha);
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, 0, GS_SETREG_RGBAQ(r, g, b, a, 0x00));
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

    // srcOffX/srcOffY are in source-page space (Renderer_drawSpritePartExt subtracts tpag->targetX/Y to convert from GML sprite-bounding space).
    // The preprocessor's cropX/cropY, however, are in sprite-bounding space (extractFromTPAG builds a boundingWidth x boundingHeight image with pixels offset by targetX/targetY, then cropTransparentBorders runs on that).
    // Subtract targetX/targetY here so both sides of the intersection live in the same coordinate system.
    float cX = (float) atlasEntry->cropX - (float) tpag->targetX;
    float cY = (float) atlasEntry->cropY - (float) tpag->targetY;
    float cW = (float) atlasEntry->cropW;
    float cH = (float) atlasEntry->cropH;

    float intX1 = fmaxf((float) srcOffX, cX);
    float intY1 = fmaxf((float) srcOffY, cY);
    float intX2 = fminf((float)(srcOffX + srcW), cX + cW);
    float intY2 = fminf((float)(srcOffY + srcH), cY + cH);

    if (intX1 >= intX2 || intY1 >= intY2) return;

    // Adjust screen position if the crop clipped the start of the requested rect
    float clipOffX = intX1 - (float) srcOffX;
    float clipOffY = intY1 - (float) srcOffY;
    float visW = intX2 - intX1;
    float visH = intY2 - intY1;

    sx1 = (x + clipOffX * xscale - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    sy1 = (y + clipOffY * yscale - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    sx2 = (x + (clipOffX + visW) * xscale - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    sy2 = (y + (clipOffY + visH) * yscale - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // Map intersection region to atlas UV space
    float ratioX = (cW > 0) ? ((float) atlasEntry->width / cW) : 1.0f;
    float ratioY = (cH > 0) ? ((float) atlasEntry->height / cH) : 1.0f;

    float u1 = (float) atlasEntry->atlasX + (intX1 - cX) * ratioX;
    float v1 = (float) atlasEntry->atlasY + (intY1 - cY) * ratioY;
    float u2 = u1 + visW * ratioX;
    float v2 = v1 + visH * ratioY;

    // GS modulate mode: Output = Texture * Vertex / 128
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, 0, gsColor);
}

static void gsDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    // Apply view transform. Z-pattern tristrip ordering: (0)=TL, (1)=TR, (2)=BL, (3)=BR.
    float sx0 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy0 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx1 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x4 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y4 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx3 = (x3 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy3 = (y3 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    float minSX = fminf(fminf(sx0, sx1), fminf(sx2, sx3));
    float maxSX = fmaxf(fmaxf(sx0, sx1), fmaxf(sx2, sx3));
    float minSY = fminf(fminf(sy0, sy1), fminf(sy2, sy3));
    float maxSY = fmaxf(fmaxf(sy0, sy1), fmaxf(sy2, sy3));
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT) return;

    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        uint8_t a = alphaToGS(alpha);
        u64 fallbackColor = GS_SETREG_RGBAQ(0xFF, 0xFF, 0xFF, a, 0x00);
        gsKit_prim_quad(gs->gsGlobal, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, 0, fallbackColor);
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];

    // Map the entire atlas entry (the trimmed source content in atlas texels) to the user's quad.
    float u0 = (float) atlasEntry->atlasX;
    float v0 = (float) atlasEntry->atlasY;
    float u1 = u0 + (float) atlasEntry->width;
    float v1 = v0 + (float) atlasEntry->height;

    // GS modulate mode: Output = Texture * Vertex / 128
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, a, 0x00);

    gsKit_prim_quad_texture(
        gs->gsGlobal,
        &tex,
        sx0, sy0, u0, v0, // TL
        sx1, sy1, u1, v0, // TR
        sx2, sy2, u0, v1, // BL
        sx3, sy3, u1, v1, // BR
        0,
        gsColor
    );
}

static void gsDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GsRenderer* gs = (GsRenderer*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(alpha);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 rectColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        float pw = gs->scaleX; // one pixel width in screen coords
        float ph = gs->scaleY; // one pixel height in screen coords
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2 + pw, sy1 + ph, 0, rectColor); // top
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy2, sx2 + pw, sy2 + ph, 0, rectColor); // bottom
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1 + ph, sx1 + pw, sy2, 0, rectColor); // left
        gsKit_prim_sprite(gs->gsGlobal, sx2, sy1 + ph, sx2 + pw, sy2, 0, rectColor); // right
    } else {
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, 0, rectColor);
    }
}

static void gsDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, MAYBE_UNUSED float width, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(alpha);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 lineColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
    gsKit_prim_line(gs->gsGlobal, sx1, sy1, sx2, sy2, 0, lineColor);
}

// PS2 gsKit doesn't support per-vertex colors on lines, so we just use color1
static void gsDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, MAYBE_UNUSED uint32_t color2, float alpha) {
    renderer->vtable->drawLine(renderer, x1, y1, x2, y2, width, color1, alpha);
}

// Resolved font state shared between gsDrawText and gsDrawTextColor
typedef struct {
    Font* font;
    GSTEXTURE tex; // GL equivalent: GLuint texId + int32_t texW/texH
    AtlasTPAGEntry* atlasEntry; // GL equivalent: TexturePageItem* fontTpag
    float ratioX, ratioY; // atlas-to-original scale (GL doesn't need this, uses texW/texH directly)
    Sprite* spriteFontSprite; // source sprite for sprite fonts (nullptr for regular fonts)
} GsFontState;

// Resolves font texture state
// Returns false if the font can't be drawn
static bool gsResolveFontState(GsRenderer* gs, DataWin* dw, Font* font, GsFontState* state) {
    state->font = font;
    state->atlasEntry = nullptr;
    state->ratioX = 1.0f;
    state->ratioY = 1.0f;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (0 > fontTpagIndex) return false;

        if (!setupTextureForTPAG(gs, &state->tex, fontTpagIndex)) return false;

        state->atlasEntry = &gs->atlasTPAGEntries[fontTpagIndex];
        TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];

        float origW = (float) fontTpag->sourceWidth;
        float origH = (float) fontTpag->sourceHeight;
        state->ratioX = (origW > 0) ? ((float) state->atlasEntry->width / origW) : 1.0f;
        state->ratioY = (origH > 0) ? ((float) state->atlasEntry->height / origH) : 1.0f;
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    return true;
}

// Resolves UV coordinates, texture ID, and local position for a single glyph
// Returns false if the glyph can't be drawn
static bool gsResolveGlyph(GsRenderer* gs, DataWin* dw, GsFontState* state, FontGlyph* glyph, float cursorX, float cursorY, GSTEXTURE* outTex, float* outU0, float* outV0, float* outU1, float* outV1, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex || glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        if (!setupTextureForTPAG(gs, outTex, tpagIdx)) return false;

        AtlasTPAGEntry* glyphAtlas = &gs->atlasTPAGEntries[tpagIdx];
        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        float gOrigW = (float) glyphTpag->sourceWidth;
        float gOrigH = (float) glyphTpag->sourceHeight;
        float gRatioX = (gOrigW > 0) ? ((float) glyphAtlas->width / gOrigW) : 1.0f;
        float gRatioY = (gOrigH > 0) ? ((float) glyphAtlas->height / gOrigH) : 1.0f;

        *outU0 = (float) glyphAtlas->atlasX;
        *outV0 = (float) glyphAtlas->atlasY;
        *outU1 = *outU0 + (float) glyph->sourceWidth * gRatioX;
        *outV1 = *outV0 + (float) glyph->sourceHeight * gRatioY;

        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) ((int32_t) glyphTpag->targetY - sprite->originY);
    } else {
        *outTex = state->tex;

        *outU0 = (float) state->atlasEntry->atlasX + (float) glyph->sourceX * state->ratioX;
        *outV0 = (float) state->atlasEntry->atlasY + (float) glyph->sourceY * state->ratioY;
        *outU1 = *outU0 + (float) glyph->sourceWidth * state->ratioX;
        *outV1 = *outV0 + (float) glyph->sourceHeight * state->ratioY;

        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void gsDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, MAYBE_UNUSED float angleDeg) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > renderer->drawFont || (uint32_t) renderer->drawFont >= dw->font.count) return;

    Font* font = &dw->font.fonts[renderer->drawFont];

    GsFontState fontState;
    if (!gsResolveFontState(gs, dw, font, &fontState)) return;

    // GS modulate mode: Output = Texture * Vertex / 128
    // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 1.0x multiplier
    uint32_t color = renderer->drawColor;
    uint8_t a = alphaToGS(renderer->drawAlpha);
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    u64 textColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    float screenScaleX = xscale * font->scaleX * gs->scaleX;
    float screenScaleY = yscale * font->scaleY * gs->scaleY;
    float screenBaseX = (x - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float screenBaseY = (y - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    int32_t textLen = (int32_t) strlen(text);

    // Vertical alignment
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = TextUtils_lineStride(font);
    float valignOffset = 0;
    if (renderer->drawValign != 0) {
        float totalHeight = (float) lineCount * lineStride;
        if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
        else if (renderer->drawValign == 2) valignOffset = -totalHeight;
    }

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    while (textLen >= lineStart) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }

        int32_t lineLen = lineEnd - lineStart;
        const char* line = text + lineStart;

        // Horizontal alignment
        float halignOffset = 0;
        if (renderer->drawHalign != 0) {
            float lineWidth = TextUtils_measureLineWidth(font, line, lineLen);
            if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
            else if (renderer->drawHalign == 2) halignOffset = -lineWidth;
        }

        float cursorX = halignOffset;

        // Draw each glyph - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(line, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(line, lineLen, &pos);

            if (glyph != nullptr) {
                bool resolveOk = true;
                if (glyph->sourceWidth > 0 && glyph->sourceHeight > 0) {
                    GSTEXTURE glyphTex;
                    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
                    float localX0, localY0;

                    if (gsResolveGlyph(gs, dw, &fontState, glyph, cursorX, cursorY, &glyphTex, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        float sx1 = localX0 * screenScaleX + screenBaseX;
                        float sy1 = localY0 * screenScaleY + screenBaseY;
                        float sx2 = sx1 + (float) glyph->sourceWidth * screenScaleX;
                        float sy2 = sy1 + (float) glyph->sourceHeight * screenScaleY;

                        gsKit_prim_sprite_texture(gs->gsGlobal, &glyphTex, sx1, sy1, u0, v0, sx2, sy2, u1, v1, 0, textColor);
                    } else {
                        resolveOk = false;
                    }
                }

                cursorX += (float) glyph->shift;
                if (resolveOk && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        // Next line
        cursorY += lineStride;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            break;
        }
    }
}

static void gsDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, MAYBE_UNUSED float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > renderer->drawFont || (uint32_t) renderer->drawFont >= dw->font.count) return;

    Font* font = &dw->font.fonts[renderer->drawFont];

    GsFontState fontState;
    if (!gsResolveFontState(gs, dw, font, &fontState)) return;

    int32_t textLen = (int32_t) strlen(text);
    if(textLen == 0) return;

    float screenScaleX = xscale * font->scaleX * gs->scaleX;
    float screenScaleY = yscale * font->scaleY * gs->scaleY;
    float screenBaseX = (x - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float screenBaseY = (y - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // Vertical alignment
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = TextUtils_lineStride(font);
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    // get delta's  (16.16 format)
	int32_t left_r_dx = ((_c2 & 0xff0000) - (_c1 & 0xff0000)) / textLen;
	int32_t left_g_dx = ((((_c2 & 0xff00) << 8) - ((_c1 & 0xff00) << 8))) / textLen;
	int32_t left_b_dx = ((((_c2 & 0xff) << 16) - ((_c1 & 0xff) << 16))) / textLen;

	int32_t right_r_dx = ((_c3 & 0xff0000) - (_c4 & 0xff0000)) / textLen;
	int32_t right_g_dx = ((((_c3 & 0xff00) << 8) - ((_c4 & 0xff00) << 8))) / textLen;
	int32_t right_b_dx = ((((_c3 & 0xff) << 16) - ((_c4 & 0xff) << 16))) / textLen;

    int32_t left_delta_r = left_r_dx;
	int32_t left_delta_g = left_g_dx;
	int32_t left_delta_b = left_b_dx;
	int32_t right_delta_r = right_r_dx;
	int32_t right_delta_g = right_g_dx;
	int32_t right_delta_b = right_b_dx;

    int32_t c1 = _c1;
    int32_t c4 = _c4;

    while (textLen >= lineStart) {
        // do 16.16 maths
        int32_t c2 = ((c1 & 0xff0000) + (left_delta_r & 0xff0000)) & 0xff0000;
            c2 |= ((c1 & 0xff00) + (left_delta_g >> 8) & 0xff00) & 0xff00;
            c2 |= ((c1 & 0xff) + (left_delta_b >> 16)) & 0xff;
        int32_t c3 = ((c4 & 0xff0000) + (right_delta_r & 0xff0000)) & 0xff0000;
            c3 |= ((c4 & 0xff00) + (right_delta_g >> 8) & 0xff00) & 0xff00;
            c3 |= ((c4 & 0xff) + (right_delta_b >> 16)) & 0xff;

        // GS modulate mode: Output = Texture * Vertex / 128
        // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 1.0x multiplier
        uint8_t ga = alphaToGS(alpha);
        uint8_t r1 = BGR_R(c1) >> 1;
        uint8_t g1 = BGR_G(c1) >> 1;
        uint8_t b1 = BGR_B(c1) >> 1;
        u64 textColor1 = GS_SETREG_RGBAQ(r1, g1, b1, ga, 0x00);

        uint8_t r2 = BGR_R(c2) >> 1;
        uint8_t g2 = BGR_G(c2) >> 1;
        uint8_t b2 = BGR_B(c2) >> 1;
        u64 textColor2 = GS_SETREG_RGBAQ(r2, g2, b2, ga, 0x00);

        uint8_t r3 = BGR_R(c3) >> 1;
        uint8_t g3 = BGR_G(c3) >> 1;
        uint8_t b3 = BGR_B(c3) >> 1;
        u64 textColor3 = GS_SETREG_RGBAQ(r3, g3, b3, ga, 0x00);

        uint8_t r4 = BGR_R(c4) >> 1;
        uint8_t g4 = BGR_G(c4) >> 1;
        uint8_t b4 = BGR_B(c4) >> 1;
        u64 textColor4 = GS_SETREG_RGBAQ(r4, g4, b4, ga, 0x00);

        left_delta_r += left_r_dx;
        left_delta_g += left_g_dx;
        left_delta_b += left_b_dx;
        right_delta_r += right_r_dx;
        right_delta_g += right_g_dx;
        right_delta_b += right_b_dx;

        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }

        int32_t lineLen = lineEnd - lineStart;
        const char* line = text + lineStart;

        // Horizontal alignment
        float lineWidth = TextUtils_measureLineWidth(font, line, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Draw each glyph - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(line, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(line, lineLen, &pos);

            if (glyph != nullptr) {
                bool resolveOk = true;
                if (glyph->sourceWidth > 0 && glyph->sourceHeight > 0) {
                    GSTEXTURE glyphTex;
                    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
                    float localX0, localY0;

                    if (gsResolveGlyph(gs, dw, &fontState, glyph, cursorX, cursorY, &glyphTex, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        float sx1 = localX0 * screenScaleX + screenBaseX;
                        float sy1 = localY0 * screenScaleY + screenBaseY;
                        float sx2 = sx1 + (float) glyph->sourceWidth * screenScaleX;
                        float sy2 = sy1 + (float) glyph->sourceHeight * screenScaleY;

                        gsKit_prim_triangle_goraud_texture_3d(gs->gsGlobal, &glyphTex,
                                sx1, sy1, 0, u0, v0,
                                sx2, sy1, 0, u1, v0,
                                sx2, sy2, 0, u1, v1,
                                textColor1, textColor2, textColor3);
                        gsKit_prim_triangle_goraud_texture_3d(gs->gsGlobal, &glyphTex,
                            sx1, sy1, 0, u0, v0,
                            sx2, sy2, 0, u1, v1,
                            sx1, sy2, 0, u0, v1,
                            textColor1, textColor3, textColor4);
                    } else {
                        resolveOk = false;
                    }
                }

                cursorX += (float) glyph->shift;
                if (resolveOk && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        // Next line
        cursorY += lineStride;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            break;
        }
        c4 = c3;    // set left edge to be what the last right edge was....
		c1 = c2;    //
    }
}

static void gsDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline)
{
    GsRenderer* gs = (GsRenderer*) renderer;
    if(outline)
    {
        gsDrawLine(renderer, x1, y1, x2, y2, 1, renderer->drawColor, 1.0);
        gsDrawLine(renderer, x2, y2, x3, y3, 1, renderer->drawColor, 1.0);
        gsDrawLine(renderer, x3, y3, x1, y1, 1, renderer->drawColor, 1.0);
    } else {
        float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        float sx3 = (x3 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        float sy3 = (y3 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

        float r = (float) BGR_R(renderer->drawColor);
        float g = (float) BGR_G(renderer->drawColor);
        float b = (float) BGR_B(renderer->drawColor);

        u64 triColor = GS_SETREG_RGBAQ(r, g, b, alphaToGS(renderer->drawAlpha), 0x00);
        gsKit_prim_triangle_gouraud_3d(gs->gsGlobal, sx1, sy1, 0,sx2, sy2, 0,sx3, sy3, 0,triColor, triColor, triColor);
    }
}

static void gsFlush(MAYBE_UNUSED Renderer* renderer) {
    // No-op: gsKit queues commands, executed in main loop
}

static int32_t gsCreateSpriteFromSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y, MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h, MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth, MAYBE_UNUSED int32_t xorig, MAYBE_UNUSED int32_t yorig) {
    rendererPrintf("GsRenderer: createSpriteFromSurface not supported on PS2\n");
    return -1;
}

static void gsDeleteSprite(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t spriteIndex) {
    // No-op
}

// Kinda possible with gsKit_set_primalpha, 
// as it just lets you set the value with GS_SETREG_ALPHA
// However I am not big brained enough to emulate these
// using the PS2 limitations
static void gsGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    // TODO
}

static void gsGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor) {
    // TODO
}

static void gsGpuSetBlendEnable(Renderer* renderer, bool enable) {
    gsGlobal->PrimAlphaEnable = enable ? GS_SETTING_ON : GS_SETTING_OFF;
}

static void gsGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    GsRenderer* gs = (GsRenderer*) renderer;
    GSGLOBAL* gsGlobal = gs->gsGlobal;

    gsKit_set_test(gsGlobal, enable ? GS_ATEST_ON : GS_ATEST_OFF);
}

static void gsGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    GsRenderer* gs = (GsRenderer*) renderer;
    GSGLOBAL* gsGlobal = gs->gsGlobal;

    gsGlobal->Test->ATST = 6;
    gsGlobal->Test->AREF = ref;
    gsGlobal->Test->AFAIL = 0;

    // Doing this so the above values get set
    gsKit_set_test(gsGlobal, gsGlobal->Test->ATE ? GS_ATEST_ON : GS_ATEST_OFF);
}

static void gsGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    GSGLOBAL* gsGlobal = gs->gsGlobal;

    unsigned int mask = 0;
    if (r_enabled == false) mask |= 0xff;
    if (g_enabled == false) mask |= 0xff00;
    if (b_enabled == false) mask |= 0xff0000;
    if (a_enabled == false) mask |= 0xff000000;

    u64 *p_data;
	u64 *p_store;
	int qsize = 3;

	p_data = p_store = (u64*)gsKit_heap_alloc(gsGlobal, qsize, (qsize * 16), GIF_AD);

    *p_data++ = GS_SETREG_FRAME_1( sGlobal->ScreenBuffer[0] / 8192, gsGlobal->Width / 64, gsGlobal->PSM, mask );
	*p_data++ = GS_FRAME_1;

    *p_data++ = GS_SETREG_FRAME_2( gsGlobal->ScreenBuffer[0] / 8192, gsGlobal->Width / 64, gsGlobal->PSM, mask );
	*p_data++ = GS_FRAME_2;

	*p_data++ = GIF_TAG_AD(qsize);
	*p_data++ = GIF_AD;

    dmaKit_wait_fast();
	dmaKit_send_ucab(DMA_CHANNEL_GIF, p_store, 5);
}

static void gsDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    GsRenderer* gs = (GsRenderer*) renderer;

    // Look up the tile in the atlas tile entries
    AtlasTileEntry* tileEntry = findTileEntry(gs, (int16_t) tile->backgroundDefinition, (uint16_t) tile->sourceX, (uint16_t) tile->sourceY, (uint16_t) tile->width, (uint16_t) tile->height);
    if (tileEntry == nullptr)
        return;

    // Set up GSTEXTURE for this tile entry
    GSTEXTURE tex;
    if (!setupTextureForTile(gs, &tex, tileEntry))
        return;

    // Compute screen rect in game coordinates
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;
    float drawW = (float) tile->width * tile->scaleX;
    float drawH = (float) tile->height * tile->scaleY;

    float sx1 = (drawX - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (drawY - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (drawX + drawW - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (drawY + drawH - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // View frustum culling
    float minSX = (sx1 < sx2) ? sx1 : sx2;
    float maxSX = (sx1 > sx2) ? sx1 : sx2;
    float minSY = (sy1 < sy2) ? sy1 : sy2;
    float maxSY = (sy1 > sy2) ? sy1 : sy2;
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT)
        return;

    // UV coordinates in atlas texels
    float u1 = (float) tileEntry->atlasX;
    float v1 = (float) tileEntry->atlasY;
    float u2 = u1 + (float) tileEntry->width;
    float v2 = v1 + (float) tileEntry->height;

    // Extract alpha from tile color high byte, default to 1.0 if 0
    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : (float) alphaByte / 255.0f;
    uint32_t bgr = tile->color & 0x00FFFFFF;

    // GS modulate mode: scale RGB from 0-255 to 0-128
    uint8_t r = BGR_R(bgr) >> 1;
    uint8_t g = BGR_G(bgr) >> 1;
    uint8_t b = BGR_B(bgr) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, 0, gsColor);
}

// ===[ Vtable ]===

static RendererVtable gsVtable = {
    .init = gsInit,
    .destroy = gsDestroy,
    .beginFrame = gsBeginFrame,
    .endFrame = gsEndFrame,
    .beginView = gsBeginView,
    .endView = gsEndView,
    .beginGUI = gsBeginGUI,
    .endGUI = gsEndGUI,
    .drawSprite = gsDrawSprite,
    .drawSpritePos = gsDrawSpritePos,
    .drawSpritePart = gsDrawSpritePart,
    .drawRectangle = gsDrawRectangle,
    .drawLine = gsDrawLine,
    .drawLineColor = gsDrawLineColor,
    .drawText = gsDrawText,
    .drawTextColor = gsDrawTextColor,
    .drawTriangle = gsDrawTriangle,
    .flush = gsFlush,
    .createSpriteFromSurface = gsCreateSpriteFromSurface,
    .deleteSprite = gsDeleteSprite,
    .gpuSetBlendMode = gsGpuSetBlendMode,
    .gpuSetBlendModeExt = gsGpuSetBlendModeExt,
    .gpuSetBlendEnable = gsGpuSetBlendEnable,
    .gpuSetAlphaTestEnable = gsGpuSetAlphaTestEnable,
    .gpuSetAlphaTestRef = gsGpuSetAlphaTestRef,
    .gpuSetColorWriteEnable = gsGpuSetColorWriteEnable,
    .drawTile = gsDrawTile,
    .drawTiled = gsDrawTiled,
};

// ===[ Public API ]===

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal) {
    GsRenderer* gs = safeCalloc(1, sizeof(GsRenderer));
    gs->base.vtable = &gsVtable;
    gs->gsGlobal = gsGlobal;
    gs->scaleX = 2.0f;
    gs->scaleY = 2.0f;
    return (Renderer*) gs;
}
