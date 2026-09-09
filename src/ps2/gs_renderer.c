#include "gs_renderer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <gsInline.h>
#pragma GCC diagnostic pop

#include <stdlib.h>
#include "string_compat.h"
#include "stdio_compat.h"
#include <malloc.h>
#include <kernel.h>

#include "binary_reader.h"
#include "binary_utils.h"
#include "utils.h"
#include "text_utils.h"
#include "ps2_utils.h"
#include "matrix_math.h"

#ifdef ENABLE_PS2_RENDERER_LOGS
static void rendererPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vLogInfo(fmt, args);
    va_end(args);
}
#else
static void _rendererPrintf(const char* fmt, ...) {
    (void)fmt;
}
#define rendererPrintf if (0) _rendererPrintf
#endif

// ===[ Constants ]===
#define PS2_SCREEN_WIDTH 640.0f
#define PS2_SCREEN_HEIGHT 448.0f
#define CLUT4_ENTRY_SIZE 64    // 16 colors * 4 bytes
#define CLUT8_ENTRY_SIZE 1024  // 256 colors * 4 bytes

// ===[ File Loading Helper ]===

// Loads an entire file from host into a memalign'd buffer. Returns size via outSize.
// Aborts on failure.
static uint8_t* loadFileRaw(const char* path, uint32_t* outSize) {
    char* textureBinPath = PS2Utils_createDevicePath(path);

    FILE* f = fopen(textureBinPath, "rb");
    if (f == nullptr) {
        logError("GsRenderer: Failed to open %s\n", path);
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
        logError("GsRenderer: Short read on %s (expected %ld, got %zu)\n", path, size, read);
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
        logError("GsRenderer: Failed to open %s\n", atlasBinPath);
        abort();
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t) ftell(f);
    fseek(f, 0, SEEK_SET);

    BinaryReader reader = BinaryReader_create(f, fileSize);

    uint8_t version = BinaryReader_readUint8(&reader);
    if (version != 0) {
        logError("GsRenderer: Unsupported ATLAS.BIN version %u\n", version);
        abort();
    }

    gs->atlasTPAGCount = BinaryReader_readUint16(&reader);
    gs->atlasTileCount = BinaryReader_readUint16(&reader);
    gs->atlasCount = BinaryReader_readUint16(&reader);

    // Parse atlas table
    gs->atlasOffsets = (uint32_t *)safeMalloc(gs->atlasCount * sizeof(uint32_t));
    gs->atlasWidth = (uint16_t *)safeMalloc(gs->atlasCount * sizeof(uint16_t));
    gs->atlasHeight = (uint16_t *)safeMalloc(gs->atlasCount * sizeof(uint16_t));
    gs->atlasBpp = (uint8_t *)safeMalloc(gs->atlasCount * sizeof(uint8_t));
    gs->atlasDataSizes = (uint32_t *)safeMalloc(gs->atlasCount * sizeof(uint32_t));
    gs->atlasCompressionType = (uint8_t *)safeMalloc(gs->atlasCount * sizeof(uint8_t));
    repeat(gs->atlasCount, i) {
        gs->atlasOffsets[i] = BinaryReader_readUint32(&reader);
        gs->atlasWidth[i] = BinaryReader_readUint16(&reader);
        gs->atlasHeight[i] = BinaryReader_readUint16(&reader);
        gs->atlasBpp[i] = BinaryReader_readUint8(&reader);
        gs->atlasDataSizes[i] = BinaryReader_readUint32(&reader);
        gs->atlasCompressionType[i] = BinaryReader_readUint8(&reader);
        if (gs->atlasBpp[i] != 4 && gs->atlasBpp[i] != 8) {
            logError("GsRenderer: Atlas %u has unsupported bpp %u\n", i, gs->atlasBpp[i]);
            abort();
        }
    }

    // Parse TPAG entries
    gs->atlasTPAGEntries = (AtlasTPAGEntry *)safeMalloc(gs->atlasTPAGCount * sizeof(AtlasTPAGEntry));

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
    }

    // Parse tile entries
    gs->atlasTileEntries = (AtlasTileEntry *)safeMalloc(gs->atlasTileCount * sizeof(AtlasTileEntry));

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
    }

    fclose(f);

    // Build tile entry hashmap for O(1) lookup
    gs->tileEntryMap = nullptr;
    repeat(gs->atlasTileCount, i) {
        AtlasTileEntry* entry = &gs->atlasTileEntries[i];
        TileLookupKey key = {0};
        key.bgDef = entry->bgDef;
        key.srcX = entry->srcX;
        key.srcY = entry->srcY;
        key.srcW = entry->srcW;
        key.srcH = entry->srcH;
        hmput(gs->tileEntryMap, key, entry);
    }

    gs->atlasToChunk = (int16_t *)safeMalloc(gs->atlasCount * sizeof(int16_t));
    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    logInfo("GsRenderer: ATLAS.BIN loaded - %u TPAG entries, %u tile entries, %u atlases\n", gs->atlasTPAGCount, gs->atlasTileCount, gs->atlasCount);

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
        logInfo("GsRenderer: CLUT4.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut4Count, clut4FileSize);

        gs->clut4VramAddrs = (uint32_t *)safeMalloc(gs->clut4Count * sizeof(uint32_t));

        repeat(gs->clut4Count, i) {
            // gsKit uploads 4bpp CLUTs as 8x2 CT32 (16 entries in 8-wide, 2-tall grid)
            uint32_t vramSize = gsKit_texture_size(8, 2, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                logError("GsRenderer: Failed to allocate VRAM for CLUT4 index %u\n", i);
                abort();
            }

            // Copy to aligned temp buffer for DMA
            memcpy(tempBuf, clut4Data + i * CLUT4_ENTRY_SIZE, CLUT4_ENTRY_SIZE);
            gsKit_texture_send((u32*) tempBuf, 8, 2, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut4VramAddrs[i] = vramAddr;
        }

        logInfo("GsRenderer: CLUT4 uploaded (%u CLUTs)\n", gs->clut4Count);
        free(clut4Data);
    }

    // Load and upload CLUT8 (8bpp palettes: 256 colors * 4 bytes = 1024 bytes each)
    {
        uint32_t clut8FileSize;
        uint8_t* clut8Data = loadFileRaw("CLUT8.BIN", &clut8FileSize);
        gs->clut8Count = clut8FileSize / CLUT8_ENTRY_SIZE;
        logInfo("GsRenderer: CLUT8.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut8Count, clut8FileSize);

        gs->clut8VramAddrs = (uint32_t *)safeMalloc(gs->clut8Count * sizeof(uint32_t));

        repeat(gs->clut8Count, i) {
            // gsKit uploads 8bpp CLUTs as 16x16 CT32 (256 entries in 16-wide, 16-tall grid)
            uint32_t vramSize = gsKit_texture_size(16, 16, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                logError("GsRenderer: Failed to allocate VRAM for CLUT8 index %u\n", i);
                abort();
            }

            // 8bpp CLUTs are 1024 bytes; source is 128-byte aligned (1024 is a multiple of 128)
            gsKit_texture_send((u32*) (clut8Data + i * CLUT8_ENTRY_SIZE), 16, 16, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut8VramAddrs[i] = vramAddr;
        }

        logInfo("GsRenderer: CLUT8 uploaded (%u CLUTs)\n", gs->clut8Count);
        free(clut8Data);
    }

    free(tempBuf);

    logInfo("GsRenderer: VRAM after CLUTs: 0x%08X / 0x%08X\n", gsGlobal->CurrentPointer, GS_VRAM_SIZE);
}

// ===[ VRAM Texture Cache (Buddy System with LRU Eviction) ]===
// Manages a pool of 128KB VRAM chunks for atlas textures.
// 4bpp atlases use 1 chunk, 8bpp atlases use 2 consecutive chunks.

// Initializes the chunk pool from the remaining VRAM after framebuffers + CLUTs + Debug Font.
//
// VRAM layout: [Framebuffers] [Debug Font atlas + CLUT] [CLUTs] [Chunk Pool ...]
static void initTextureCache(GsRenderer* gs) {
    // GS FRAME register's FBP is in 8192-byte page units, so a surface used as a render target needs its VRAM address page-aligned.
    // VRAM_CHUNK_SIZE is a multiple of 8192, so chunk starts are page-aligned if textureVramBase is.
    uint32_t base = gs->gsGlobal->CurrentPointer;
    base = (base + 8191u) & ~8191u;
    gs->textureVramBase = base;
    gs->gsGlobal->CurrentPointer = base;

    uint32_t availableVram = GS_VRAM_SIZE - gs->textureVramBase;
    gs->chunkCount = availableVram / VRAM_CHUNK_SIZE;

    gs->chunks = (VRAMChunk *)safeMalloc(gs->chunkCount * sizeof(VRAMChunk));
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->snapshotIdx = -1;
        chunk->surfaceIdx = -1;
        chunk->lastUsed = 0;
    }

    gs->frameCounter = 1;

    // Advance CurrentPointer past our chunk pool so any future gsKit allocations fail loudly.
    gs->gsGlobal->CurrentPointer = gs->textureVramBase + gs->chunkCount * VRAM_CHUNK_SIZE;

    logInfo("GsRenderer: Texture cache initialized - %u chunks (%u KB each), base 0x%08X, %u KB for textures\n", gs->chunkCount, VRAM_CHUNK_SIZE / 1024, gs->textureVramBase, gs->chunkCount * (VRAM_CHUNK_SIZE / 1024));
}

// A chunk is free if no atlas, snapshot, or surface occupies it. Snapshots and surfaces both pin the chunk against LRU eviction.
static inline bool chunkIsFree(const VRAMChunk* chunk) {
    return 0 > chunk->atlasId && 0 > chunk->snapshotIdx && 0 > chunk->surfaceIdx;
}

// Find the first run of consecutive free chunks.
// Returns the index of the first chunk, or -1 if not found.
static int32_t findConsecutiveFreeChunks(GsRenderer* gs, int chunksNeeded, uint32_t startIdx) {
    int consecutive = 0;
    if (gs->chunkCount > startIdx) {
        for (uint32_t i = startIdx; gs->chunkCount > i; i++) {
            VRAMChunk* chunk = &gs->chunks[i];
            if (chunkIsFree(chunk)) {
                consecutive++;
                if (consecutive >= chunksNeeded) {
                    return (int32_t) (i - (uint32_t) chunksNeeded + 1);
                }
            } else {
                consecutive = 0;
            }
        }
    }
    return -1;
}

// Count total free chunks.
static uint32_t countFreeChunks(GsRenderer* gs) {
    uint32_t count = 0;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunkIsFree(chunk))
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
// Pinned snapshot chunks are NOT touched - their VRAM contents are the only copy and we cannot reload them.
static void defragTextureCache(GsRenderer* gs) {
    rendererPrintf("GsRenderer: Defragmenting VRAM texture cache...\n");

    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->snapshotIdx >= 0) continue; // pinned snapshot, cannot be discarded
        if (chunk->surfaceIdx >= 0) continue;  // pinned surface, ditto
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    rendererPrintf("GsRenderer: Defrag complete - free chunks = %u\n", countFreeChunks(gs));
}

// Allocate consecutive chunks. Evicts LRU atlas victims or defrags if needed.
// startIdx restricts the scan to [startIdx, chunkCount); snapshot callers pass
// reservedAtlasChunks so the reserved fail-safe region stays atlas-only.
// Returns the first chunk index, or -1 if VRAM is truly exhausted.
static int32_t allocateChunks(GsRenderer* gs, int chunksNeeded, uint32_t startIdx) {
    // Attempt 1: find free consecutive chunks
    int32_t idx = findConsecutiveFreeChunks(gs, chunksNeeded, startIdx);
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

        idx = findConsecutiveFreeChunks(gs, chunksNeeded, startIdx);

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
        idx = findConsecutiveFreeChunks(gs, chunksNeeded, startIdx);

        if (idx >= 0)
            return idx;
    }

    // VRAM truly exhausted
    return -1;
}

// ===[ EE RAM Atlas Cache (Bump Allocator with LRU Eviction + Compaction) ]===
// Caches uncompressed atlas pixel data in a EE RAM buffer, allowing zero-copy DMA uploads to VRAM without per-upload decompression or temp allocations.

// Uncompressed pixel data size for an atlas of the given size and bpp.
static uint32_t atlasUncompressedSize(uint16_t width, uint16_t height, uint8_t bpp) {
    return (bpp == 4) ? (width * height / 2) : (width * height);
}

// Decompress atlas pixel data from a payload buffer.
// Writes uncompressed indexed pixels into outBuf (must be large enough and 128-byte aligned).
static void decompressAtlasPixels(const uint8_t* pixelData, uint32_t pixelDataSize, uint8_t compressionType, uint16_t width, uint16_t height, uint8_t bpp, uint8_t* outBuf) {
    uint32_t uncompressedSize = (bpp == 4) ? (uint32_t) ((width * height + 1) / 2) : (uint32_t) (width * height);

    if (compressionType == 1) {
        // RLE decompression
        uint32_t srcPos = 0, dstPos = 0;
        while (pixelDataSize > srcPos + 1 && uncompressedSize > dstPos) {
            uint8_t runLength = pixelData[srcPos++];
            uint8_t value = pixelData[srcPos++];
            for (uint8_t j = 0; runLength > j && uncompressedSize > dstPos; j++) {
                outBuf[dstPos++] = value;
            }
        }
    } else {
        memcpy(outBuf, pixelData, uncompressedSize);
    }
}

// Number of VRAM_CHUNK_SIZE chunks needed to hold an atlas at the given size/bpp.
static int atlasChunkCount(uint16_t width, uint16_t height, uint8_t bpp) {
    uint32_t bytes = atlasUncompressedSize(width, height, bpp);
    return (int) ((bytes + VRAM_CHUNK_SIZE - 1) / VRAM_CHUNK_SIZE);
}

// As a fail-safe, we'll reserve the first N chunks for atlases only, to avoid sprite snapshots pinning all chunks.
static void computeAtlasReservation(GsRenderer* gs) {
    uint32_t worst = 0;
    repeat(gs->atlasCount, i) {
        uint32_t need = (uint32_t) atlasChunkCount(gs->atlasWidth[i], gs->atlasHeight[i], 8);
        if (need > worst)
            worst = need;
    }
    if (worst > gs->chunkCount) worst = gs->chunkCount;
    gs->reservedAtlasChunks = worst;
    logInfo("GsRenderer: Reserving first %u chunk(s) (%u KB) as atlas-only fail-safe (largest atlas @ 8bpp)\n", worst, worst * (VRAM_CHUNK_SIZE / 1024));
}

// Initialize the EE RAM cache. Called from gsInit after opening TEXTURES.BIN.
static void initEeCache(GsRenderer* gs) {
    gs->eeCacheCapacity = gs->eeAtlasCacheBytes;
    gs->eeCacheBumpPtr = 0;
    gs->eeCache = (uint8_t*) safeMemalign(128, gs->eeAtlasCacheBytes);

    gs->eeCacheEntries = (EeAtlasCacheEntry *)safeMalloc(gs->atlasCount * sizeof(EeAtlasCacheEntry));
    repeat(gs->atlasCount, i) {
        gs->eeCacheEntries[i].atlasId = -1;
        gs->eeCacheEntries[i].offset = 0;
        gs->eeCacheEntries[i].size = 0;
        gs->eeCacheEntries[i].lastUsed = 0;
    }
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
        // Cache miss: read pixel data from TEXTURES.BIN and decompress (if RLE'd)
        uint32_t dataSize = gs->atlasDataSizes[atlasId];
        uint8_t* compressedBuf = (uint8_t*) safeMemalign(128, dataSize);

        fseek(gs->texturesFile, (long) gs->atlasOffsets[atlasId], SEEK_SET);
        size_t bytesRead = fread(compressedBuf, 1, dataSize, gs->texturesFile);
        if (bytesRead != dataSize) {
            logError("GsRenderer: Short read for atlas %u (expected %u, got %zu)\n", atlasId, dataSize, bytesRead);
            abort();
        }

        uint8_t bpp = gs->atlasBpp[atlasId];
        uint16_t width = gs->atlasWidth[atlasId];
        uint16_t height = gs->atlasHeight[atlasId];
        uint32_t uncompSize = atlasUncompressedSize(width, height, bpp);
        tempPixelData = (uint8_t*) safeMemalign(128, uncompSize);
        decompressAtlasPixels(compressedBuf, dataSize, gs->atlasCompressionType[atlasId], width, height, bpp, tempPixelData);
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
    } else {
        gs->ramLoadsThisFrame++;
    }

    // Upload pixel data to VRAM
    uint8_t bpp = gs->atlasBpp[atlasId];
    uint16_t width = gs->atlasWidth[atlasId];
    uint16_t height = gs->atlasHeight[atlasId];
    uint8_t psm = (bpp == 4) ? GS_PSM_T4 : GS_PSM_T8;
    uint32_t tbw = width / 64;
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) firstChunk * VRAM_CHUNK_SIZE;

    // We use GS_CLUT_NONE instead of GS_CLUT_TEXTURE so gsKit_texture_send appends a GS_TEXFLUSH after the DMA.
    // If we use GS_CLUT_TEXTURE, we'll have texture corruption if the chunk is reused for a different atlas in the same frame.
    // It is a bit hacky, but it works.
    gsKit_texture_send((u32*) uploadData, width, height, vramAddr, psm, tbw, GS_CLUT_NONE);

    // Update chunk state
    int chunksUsed = atlasChunkCount(width, height, bpp);
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
        logWarn("GsRenderer: Atlas ID %u out of range (max %u)\n", atlasId, gs->atlasCount - 1);
        return false;
    }

    // Already loaded? Just touch LRU timestamp
    if (gs->atlasToChunk[atlasId] >= 0) {
        int16_t firstChunk = gs->atlasToChunk[atlasId];
        uint8_t bpp = gs->atlasBpp[atlasId];
        int chunksUsed = atlasChunkCount(gs->atlasWidth[atlasId], gs->atlasHeight[atlasId], bpp);

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
        logWarn("GsRenderer: Atlas %u has unknown bpp %u\n", atlasId, bpp);
        return false;
    }
    int chunksNeeded = atlasChunkCount(gs->atlasWidth[atlasId], gs->atlasHeight[atlasId], bpp);

    // Fresh load is always a new unique atlas this frame
    gs->uniqueAtlasesThisFrame++;
    gs->chunksNeededThisFrame += (uint16_t) chunksNeeded;

    // Allocate chunks (may evict or defrag)
    int32_t chunkIdx = allocateChunks(gs, chunksNeeded, 0);
    if (0 > chunkIdx) {
        logError("GsRenderer: VRAM exhausted! Cannot allocate %d chunk(s) for atlas %u (%ubpp)\n", chunksNeeded, atlasId, bpp);
        abort();
    }

    // Load TEX file and upload to the allocated chunk(s)
    uploadAtlasToChunk(gs, atlasId, chunkIdx);
    return true;
}

// ===[ Snapshot allocator (shared chunk pool) ]===

// Allocates VRAM chunks for a w x h CT16 snapshot. Returns the snapshotChunks table index, or -1 on failure.
static int32_t allocateSnapshotChunk(GsRenderer* gs, int32_t w, int32_t h) {
    if (0 >= w || 0 >= h) return -1;

    uint16_t tbw = (uint16_t) ((w + 63) / 64);
    if (tbw == 0) tbw = 1;
    uint32_t neededBytes = gsKit_texture_size(tbw * 64, h, GS_PSM_CT16);
    int chunksNeeded = (int) ((neededBytes + VRAM_CHUNK_SIZE - 1) / VRAM_CHUNK_SIZE);
    if (chunksNeeded <= 0) chunksNeeded = 1;

    // Reuse a table row whose chunks have already been released (inUse=false). The chunks themselves get freshly allocated below; only the row index is recycled so tpagToSnapshot pointers can remain stable until sprite_delete reaps them.
    int32_t snapIdx = -1;
    uint32_t rowCount = (uint32_t) arrlen(gs->snapshotChunks);
    repeat(rowCount, i) {
        if (!gs->snapshotChunks[i].inUse) {
            snapIdx = (int32_t) i;
            break;
        }
    }
    if (0 > snapIdx) {
        SnapshotChunk zero = {0};
        arrput(gs->snapshotChunks, zero);
        snapIdx = (int32_t) (arrlen(gs->snapshotChunks) - 1);
    }

    // Acquire chunks via the shared atlas allocator (LRU-evicts atlases as needed; skips pinned snapshots).
    int32_t firstChunk = allocateChunks(gs, chunksNeeded, gs->reservedAtlasChunks);
    if (0 > firstChunk) {
        rendererPrintf("GsRenderer: Cannot allocate %d chunks for snapshot (VRAM exhausted by pinned snapshots?)\n", chunksNeeded);
        return -1;
    }

    // Pin the chunks: atlasId stays -1, snapshotIdx points at this row.
    repeat(chunksNeeded, c) {
        VRAMChunk* chunk = &gs->chunks[firstChunk + c];
        chunk->atlasId = -1;
        chunk->snapshotIdx = (int16_t) snapIdx;
        chunk->lastUsed = 0;
    }

    SnapshotChunk* row = &gs->snapshotChunks[snapIdx];
    row->firstChunk = (uint16_t) firstChunk;
    row->chunkCount = (uint16_t) chunksNeeded;
    row->width = (uint16_t) w;
    row->height = (uint16_t) h;
    row->tbw = tbw;
    row->inUse = true;
    return snapIdx;
}

static void freeSnapshotChunk(GsRenderer* gs, int32_t snapIdx) {
    if (0 > snapIdx || (uint32_t) snapIdx >= (uint32_t) arrlen(gs->snapshotChunks)) return;
    SnapshotChunk* row = &gs->snapshotChunks[snapIdx];
    if (!row->inUse) return;
    // Release the underlying chunks back to the atlas pool.
    for (uint32_t c = 0; row->chunkCount > c; c++) {
        VRAMChunk* chunk = &gs->chunks[row->firstChunk + c];
        chunk->snapshotIdx = -1;
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }
    row->inUse = false;
    row->chunkCount = 0;
}

// ===[ Local-to-local GS bitblt (TRXDIR=2) ]===
// Hand-rolled GIF packet - gsKit_texture_send only does host->local (TRXDIR=0).
// BITBLTBUF SBP/DBP are VRAM byte address / 256 (block units). TBW is in 64-pixel blocks.
static void gsLocalToLocalBlit(u32 srcVramBytes, u32 srcTbw, u32 srcPsm, u32 srcX, u32 srcY, u32 dstVramBytes, u32 dstTbw, u32 dstPsm, u32 dstX, u32 dstY, u32 w, u32 h) {
    // Layout: [DMA_TAG CNT qwc=5][GIFTAG nloop=4][BITBLTBUF][TRXPOS][TRXREG][TRXDIR][DMA_TAG END qwc=2][GIFTAG nloop=1][TEXFLUSH] = 9 qw
    const int p_size = 9;
    u64* p_store = memalign(64, p_size * 16);
    u64* p_data = p_store;

    FlushCache(0);

    *p_data++ = DMA_TAG(5, 0, DMA_CNT, 0, 0, 0);
    *p_data++ = 0;

    *p_data++ = GIF_TAG(4, 0, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_BITBLTBUF(srcVramBytes / 256, srcTbw, srcPsm, dstVramBytes / 256, dstTbw, dstPsm);
    *p_data++ = GS_BITBLTBUF;

    *p_data++ = GS_SETREG_TRXPOS(srcX, srcY, dstX, dstY, 0);
    *p_data++ = GS_TRXPOS;

    *p_data++ = GS_SETREG_TRXREG(w, h);
    *p_data++ = GS_TRXREG;

    *p_data++ = GS_SETREG_TRXDIR(2);
    *p_data++ = GS_TRXDIR;

    *p_data++ = DMA_TAG(2, 0, DMA_END, 0, 0, 0);
    *p_data++ = 0;

    *p_data++ = GIF_TAG(1, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *p_data++ = GIF_AD;

    *p_data++ = 0;
    *p_data++ = GS_TEXFLUSH;

    dmaKit_wait_fast();
    dmaKit_send_chain(DMA_CHANNEL_GIF, p_store, p_size);
    dmaKit_wait_fast();
    free(p_store);
}

// ===[ Runtime TPAG slot allocator ]===
// Scans dynamic-allocation tail of dw->tpag.items[] for a free slot (texturePageId == -1) and appends a new entry if none are free.
// The parallel "tpagToSnapshot" array is kept in sync (grown alongside dw->tpag).
static uint32_t findOrAllocTpagSlot(GsRenderer* gs, DataWin* dw) {
    for (uint32_t i = gs->originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1)
            return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;

    // Grow the snapshot mapping array to cover the new tpag slot
    while (newIndex >= (uint32_t) arrlen(gs->tpagToSnapshot)) {
        int32_t sentinel = -1;
        arrput(gs->tpagToSnapshot, sentinel);
    }

    return newIndex;
}

// Returns the snapshot chunk index for a TPAG, or -1 if this TPAG is not a snapshot.
static int32_t tpagSnapshotIndex(GsRenderer* gs, int32_t tpagIndex) {
    if (0 > tpagIndex) return -1;
    if ((uint32_t) tpagIndex >= (uint32_t) arrlen(gs->tpagToSnapshot)) return -1;
    return gs->tpagToSnapshot[tpagIndex];
}

// ===[ GSTEXTURE setup for a given TPAG entry ]===
// Configures a GSTEXTURE struct for rendering a specific atlas region.
// The GSTEXTURE points to the atlas's VRAM location and the appropriate CLUT.
static bool setupTextureForTPAG(GsRenderer* gs, GSTEXTURE* tex, int32_t tpagIndex) {
    // Snapshot TPAGs (synthetic, created by sprite_create_from_surface) get their own CT16 direct-color path - no CLUT, no atlas lookup.
    int32_t snapshotIdx = tpagSnapshotIndex(gs, tpagIndex);
    if (snapshotIdx >= 0) {
        SnapshotChunk* chunk = &gs->snapshotChunks[snapshotIdx];
        memset(tex, 0, sizeof(GSTEXTURE));
        tex->Width = chunk->width;
        tex->Height = chunk->height;
        tex->TBW = chunk->tbw;
        tex->Vram = gs->textureVramBase + (uint32_t) chunk->firstChunk * VRAM_CHUNK_SIZE;
        tex->PSM = GS_PSM_CT16;
        tex->Filter = GS_FILTER_NEAREST;
        return true;
    }

    if (0 > tpagIndex || (uint32_t) tpagIndex >= gs->atlasTPAGCount) return false;

    AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
    if (entry->atlasId == 0xFFFF) return false;

    // Ensure the atlas texture is loaded into VRAM (may trigger LRU eviction)
    if (!ensureAtlasLoaded(gs, entry->atlasId))
        return false;

    // Compute VRAM address from chunk index
    int16_t chunkIdx = gs->atlasToChunk[entry->atlasId];
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) chunkIdx * VRAM_CHUNK_SIZE;

    uint16_t pageWidth = gs->atlasWidth[entry->atlasId];
    uint16_t pageHeight = gs->atlasHeight[entry->atlasId];
    uint8_t pageBpp = gs->atlasBpp[entry->atlasId];

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = pageWidth;
    tex->Height = pageHeight;
    tex->TBW = pageWidth / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (pageBpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            logError("GsRenderer: CLUT4 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut4Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            logError("GsRenderer: CLUT8 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut8Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Tile Lookup and Texture Setup ]===

// Finds a tile entry by (bgDef, srcX, srcY, srcW, srcH). Returns nullptr if not found.
static AtlasTileEntry* findTileEntry(GsRenderer* gs, int16_t bgDef, uint16_t srcX, uint16_t srcY, uint16_t srcW, uint16_t srcH) {
    TileLookupKey key = {0};
    key.bgDef = bgDef;
    key.srcX = srcX;
    key.srcY = srcY;
    key.srcW = srcW;
    key.srcH = srcH;
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

    uint16_t pageWidth = gs->atlasWidth[entry->atlasId];
    uint16_t pageHeight = gs->atlasHeight[entry->atlasId];
    uint8_t pageBpp = gs->atlasBpp[entry->atlasId];

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = pageWidth;
    tex->Height = pageHeight;
    tex->TBW = pageWidth / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (pageBpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            logError("GsRenderer: CLUT4 index %u out of range (max %u) for tile (bg=%d)\n", entry->clutIndex, gs->clut4Count - 1, entry->bgDef);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            logError("GsRenderer: CLUT8 index %u out of range (max %u) for tile (bg=%d)\n", entry->clutIndex, gs->clut8Count - 1, entry->bgDef);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Vtable Implementations ]===

// Identity blend - source passes through unchanged. Used when GML disables blending but we still need PrimAlphaEnable=ON for TCC.
// Equation: (Cs - 0) * 128/128 + 0 = Cs.
// We do this because disabling blending (setting PrimAlphaEnable to OFF) makes GS stop honoring alpha writes from textures, which breaks masks.
#define GS_ALPHA_NO_BLEND GS_SETREG_ALPHA(0, 2, 2, 2, 0x80)

// Re-emits the FRAME_1 register with the current FBMSK. Called whenever the color write mask changes.
static void gsApplyFBMask(GsRenderer* gs, u32 fbmsk) {
    GSGLOBAL* g = gs->gsGlobal;
    u64* p = (u64*) gsKit_heap_alloc(g, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_FRAME(g->ScreenBuffer[g->ActiveBuffer & 1] / 8192, g->Width / 64, g->PSM, fbmsk);
    *p++ = GS_FRAME_1 + g->PrimContext;
    gs->fbmsk = fbmsk;
}

// Re-emits the FBA_1 (Framebuffer Alpha) register. fba=1 forces bit 7 of the alpha to 1 at framebuffer writeback (after the blend equation has consumed As, so blending is unaffected).
// fba=0 passes alpha through unchanged - required while the script is in alpha-only write mode so the intended mask value lands in FB.A verbatim.
static void gsApplyFBA(GsRenderer* gs, uint8_t fba) {
    if (gs->fba == fba) return;
    GSGLOBAL* g = gs->gsGlobal;
    u64* p = (u64*) gsKit_heap_alloc(g, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = (u64) (fba & 1);
    *p++ = GS_FBA_1 + g->PrimContext;
    gs->fba = fba;
}

static void gsCommitBlend(GsRenderer* gs) {
    gsKit_set_primalpha(gs->gsGlobal, gs->blendEnabled ? gs->currentBlendAlpha : GS_ALPHA_NO_BLEND, 0);
}

static void gsInit(Renderer* renderer, DataWin* dataWin) {
    GsRenderer* gs = (GsRenderer*) renderer;

    renderer->dataWin = dataWin;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;
    renderer->circlePrecision = 24;

    // Enable alpha blending
    gs->gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gs->blendEnabled = true;
    gs->colorWriteR = true;
    gs->colorWriteG = true;
    gs->colorWriteB = true;
    gs->colorWriteA = true;
    gs->currentBlendAlpha = GS_SETREG_ALPHA(0, 1, 0, 1, 0);

    // gsKit defaults Test->AREF to 0x80, but GMS's gpu_get_alphatestref() defaults to 0. Scripts that enable alpha test without calling gpu_set_alphatestref expect ref=0.
    // With ATST=GREATER and the post-MODULATE source alpha capped at 0x80, an AREF of 0x80 makes "0x80 > 0x80" fail, hiding all opaque pixels.
    gs->gsGlobal->Test->AREF = 0;
    gs->gsGlobal->Test->ATST = 6; // GREATER (matches GMS semantics)
    gs->gsGlobal->Test->AFAIL = 0; // KEEP

    // Force FB.A bit = 1 on every writeback via the GS FBA register so bm_dest_alpha / bm_inv_dest_alpha see opaque alpha for normal sprites.
    // This mimicks how OpenGL works
    gsApplyFBA(gs, 1);

    // Alpha blend: (Cs - Cd) * As / 128 + Cd (standard source-over)
    gsKit_set_primalpha(gs->gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    // Load atlas metadata
    loadAtlas(gs);

    // Open TEXTURES.BIN and keep it open for on-demand atlas loading
    char* texturesBinPath = PS2Utils_createDevicePath("TEXTURES.BIN");
    gs->texturesFile = fopen(texturesBinPath, "rb");
    if (gs->texturesFile == nullptr) {
        logError("GsRenderer: Failed to open %s\n", texturesBinPath);
        abort();
    }
    setvbuf(gs->texturesFile, nullptr, _IOFBF, 128 * 1024);
    free(texturesBinPath);

    // Upload CLUTs to VRAM
    loadAndUploadCLUTs(gs);

    // Initialize the texture cache chunk pool (uses remaining VRAM after CLUTs)
    initTextureCache(gs);

    // Capture original asset counts so we know which dw->tpag / dw->sprt slots are static (load-time) vs dynamic (runtime, created by sprite_create_from_surface).
    gs->originalTpagCount = dataWin->tpag.count;
    gs->originalSpriteCount = dataWin->sprt.count;

    computeAtlasReservation(gs);

    // Initialize EE RAM cache for compressed atlas data
    initEeCache(gs);

    logInfo("GsRenderer: Initialized (textured mode)\n");
}

static void gsDestroy(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (gs->texturesFile != nullptr) {
        fclose(gs->texturesFile);
    }
    free(gs->atlasOffsets);
    free(gs->atlasCompressionType);
    free(gs->atlasTPAGEntries);
    free(gs->atlasTileEntries);
    hmfree(gs->tileEntryMap);
    free(gs->chunks);
    free(gs->atlasToChunk);
    free(gs->atlasBpp);
    free(gs->atlasWidth);
    free(gs->atlasHeight);
    free(gs->clut4VramAddrs);
    free(gs->clut8VramAddrs);
    free(gs->eeCache);
    free(gs->eeCacheEntries);
    free(gs->atlasDataSizes);
    // free(gs->primitiveVertices);
    arrfree(gs->snapshotChunks);
    arrfree(gs->tpagToSnapshot);
    arrfree(gs->surfaces);
    free(gs);
}

static void gsBeginFrame(Renderer* renderer, MAYBE_UNUSED int32_t gameW, MAYBE_UNUSED int32_t gameH, MAYBE_UNUSED int32_t windowW, MAYBE_UNUSED int32_t windowH) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->frameCounter++;
    gs->evictedAtlasUsedInCurrentFrame = false;
    gs->uniqueAtlasesThisFrame = 0;
    gs->chunksNeededThisFrame = 0;
    gs->ramLoadsThisFrame = 0;
    gs->diskLoadsThisFrame = 0;

    // gsKit_setactive (called by sync_flip) re-emits FRAME with FBMSK=0, so any color-write mask we set last frame is gone. Re-apply it here for cases where GML leaves it asserted across frames.
    if (gs->fbmsk != 0) {
        gsApplyFBMask(gs, gs->fbmsk);
    }
}

static void gsEndFrameInit(MAYBE_UNUSED Renderer* renderer) {
    // No-op: GS draws directly to the display buffer.
}

static void gsEndFrameEnd(MAYBE_UNUSED Renderer* renderer) {
    // No-op: flip happens in main loop.
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

static void gsApplyProjection(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED const Matrix4f* viewMatrix, MAYBE_UNUSED const Matrix4f* projectionMatrix) {
    // No-op
    //Um but I do feel like the PS2 should be capable of this though?
}

static void gsSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED bool renderingToUserSurface) {
    GsRenderer* gs = (GsRenderer*) renderer;
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

// targetSurfaceId is unused here because the renderer always draws to the screen buffer here
static void gsBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED int32_t targetSurfaceId) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->viewX = 0;
    gs->viewY = 0;
    gsSetGuiProjection(renderer, guiW, guiH, portW, portH, false);
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

    // The atlas entry has the actual sprite dimensions in the atlas (post-crop, post-resize).
    // The screen rect covers cropW x cropH game-space pixels, positioned at (cropX, cropY)
    // within the original bounding box. The GS hardware stretches the atlas texels to fill.

    // UV coords within the 512x512 atlas (in texels for gsKit). Snapshot tpags cover their own dedicated CT16 VRAM region from (0,0) to (width,height).
    float u0, v0, u1, v1;
    int32_t snapshotIdx = tpagSnapshotIndex(gs, tpagIndex);
    if (snapshotIdx >= 0) {
        SnapshotChunk* chunk = &gs->snapshotChunks[snapshotIdx];
        u0 = 0.0f; v0 = 0.0f;
        u1 = (float) chunk->width;
        v1 = (float) chunk->height;
    } else {
        AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
        u0 = (float) atlasEntry->atlasX;
        v0 = (float) atlasEntry->atlasY;
        u1 = u0 + (float) atlasEntry->width;
        v1 = v0 + (float) atlasEntry->height;
    }

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

static void gsDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
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

    float u0, v0, u1, v1;
    int32_t tiledSnapshotIdx = tpagSnapshotIndex(gs, tpagIndex);
    if (tiledSnapshotIdx >= 0) {
        SnapshotChunk* chunk = &gs->snapshotChunks[tiledSnapshotIdx];
        u0 = 0.0f; v0 = 0.0f;
        u1 = (float) chunk->width;
        v1 = (float) chunk->height;
    } else {
        AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
        u0 = (float) atlasEntry->atlasX;
        v0 = (float) atlasEntry->atlasY;
        u1 = u0 + (float) atlasEntry->width;
        v1 = v0 + (float) atlasEntry->height;
    }

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

static void gsDrawTiledPart(Renderer* renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Snapshot tpags have no atlas remap: UV origin (0,0), no crop, 1:1 ratio.
    float cX, cY, cW, cH;
    float atlasU0, atlasV0;
    float ratioX, ratioY;
    int32_t tpSnapIdx = tpagSnapshotIndex(gs, tpagIndex);
    if (tpSnapIdx >= 0) {
        SnapshotChunk* chunk = &gs->snapshotChunks[tpSnapIdx];
        cX = 0.0f; cY = 0.0f;
        cW = (float) chunk->width;
        cH = (float) chunk->height;
        atlasU0 = 0.0f; atlasV0 = 0.0f;
        ratioX = 1.0f; ratioY = 1.0f;
    } else {
        AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
        cX = (float) atlasEntry->cropX - (float) tpag->targetX;
        cY = (float) atlasEntry->cropY - (float) tpag->targetY;
        cW = (float) atlasEntry->cropW;
        cH = (float) atlasEntry->cropH;
        atlasU0 = (float) atlasEntry->atlasX;
        atlasV0 = (float) atlasEntry->atlasY;
        ratioX = cW > 0.0f ? (float) atlasEntry->width  / cW : 1.0f;
        ratioY = cH > 0.0f ? (float) atlasEntry->height / cH : 1.0f;
    }

    uint8_t r, g, b;
    if (color == 0xFFFFFFu) { r = g = b = 0x80; } else { r = BGR_R(color) >> 1; g = BGR_G(color) >> 1; b = BGR_B(color) >> 1; }
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    float viewBaseX = -(float) gs->viewX;
    float viewBaseY = -(float) gs->viewY;
    float viewScaleX = gs->scaleX;
    float viewScaleY = gs->scaleY;
    float viewOffX = gs->offsetX;
    float viewOffY = gs->offsetY;

    int32_t tilesY = (int32_t)(dstH / (float) srcH) + 2;
    int32_t tilesX = (int32_t)(dstW / (float) srcW) + 2;

    repeat(tilesY, iy) {
        float rowDstY = dstY + (float) iy * (float) srcH;
        if (rowDstY >= dstY + dstH) break;
        int32_t rowSrcH = srcH < (int32_t)((dstY + dstH) - rowDstY) ? srcH : (int32_t)((dstY + dstH) - rowDstY);

        float intY1 = cY > (float) srcY ? cY : (float) srcY;
        float intY2 = (cY + cH) < (float)(srcY + rowSrcH) ? (cY + cH) : (float)(srcY + rowSrcH);
        if (intY1 >= intY2) continue;
        float clipOffY = intY1 - (float) srcY;
        float visH = intY2 - intY1;
        float v0 = atlasV0 + (intY1 - cY) * ratioY;
        float v1 = v0 + visH * ratioY;

        float sy0 = (rowDstY + clipOffY + viewBaseY) * viewScaleY + viewOffY;
        float sy1 = sy0 + visH * viewScaleY;
        float minSY = sy0 < sy1 ? sy0 : sy1;
        float maxSY = sy0 > sy1 ? sy0 : sy1;
        if (0.0f > maxSY || minSY > PS2_SCREEN_HEIGHT) continue;

        repeat(tilesX, ix) {
            float colDstX = dstX + (float) ix * (float) srcW;
            if (colDstX >= dstX + dstW) break;
            int32_t colSrcW = srcW < (int32_t)((dstX + dstW) - colDstX) ? srcW : (int32_t)((dstX + dstW) - colDstX);

            float intX1 = cX > (float) srcX ? cX : (float) srcX;
            float intX2 = (cX + cW) < (float)(srcX + colSrcW) ? (cX + cW) : (float)(srcX + colSrcW);
            if (intX1 >= intX2) continue;
            float clipOffX = intX1 - (float) srcX;
            float visW = intX2 - intX1;
            float u0 = atlasU0 + (intX1 - cX) * ratioX;
            float u1 = u0 + visW * ratioX;

            float sx0 = (colDstX + clipOffX + viewBaseX) * viewScaleX + viewOffX;
            float sx1 = sx0 + visW * viewScaleX;
            float minSX = sx0 < sx1 ? sx0 : sx1;
            float maxSX = sx0 > sx1 ? sx0 : sx1;
            if (0.0f > maxSX || minSX > PS2_SCREEN_WIDTH) continue;

            gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx1, sy1, u1, v1, 0, gsColor);
        }
    }
}

static void gsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= renderer->dataWin->tpag.count) return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    bool hasTexture = setupTextureForTPAG(gs, &tex, tpagIndex);

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];
    int32_t snapPartIdx = tpagSnapshotIndex(gs, tpagIndex);
    SnapshotChunk* snapPartChunk = (snapPartIdx >= 0) ? &gs->snapshotChunks[snapPartIdx] : nullptr;
    AtlasTPAGEntry* atlasEntry = (hasTexture && snapPartIdx < 0) ? &gs->atlasTPAGEntries[tpagIndex] : nullptr;

    // srcOffX/srcOffY are in source-page space (Renderer_drawSpritePartExt subtracts tpag->targetX/Y to convert from GML sprite-bounding space).
    // The preprocessor's cropX/cropY, however, are in sprite-bounding space (extractFromTPAG builds a boundingWidth x boundingHeight image with pixels offset by targetX/targetY, then cropTransparentBorders runs on that).
    // Subtract targetX/targetY here so both sides of the intersection live in the same coordinate system.
    // Snapshot tpags have no crop/atlas remap: the full chunk IS the sprite.
    float cX, cY, cW, cH;
    if (snapPartChunk != nullptr) {
        cX = 0.0f; cY = 0.0f;
        cW = (float) snapPartChunk->width;
        cH = (float) snapPartChunk->height;
    } else if (hasTexture) {
        cX = (float) atlasEntry->cropX - (float) tpag->targetX;
        cY = (float) atlasEntry->cropY - (float) tpag->targetY;
        cW = (float) atlasEntry->cropW;
        cH = (float) atlasEntry->cropH;
    } else {
        cX = 0.0f; cY = 0.0f;
        cW = (float) tpag->sourceWidth;
        cH = (float) tpag->sourceHeight;
    }

    float intX1 = fmaxf((float) srcOffX, cX);
    float intY1 = fmaxf((float) srcOffY, cY);
    float intX2 = fminf((float)(srcOffX + srcW), cX + cW);
    float intY2 = fminf((float)(srcOffY + srcH), cY + cH);

    if (intX1 >= intX2 || intY1 >= intY2) return;

    // Compute clip offset and visible region dimensions
    float clipOffX = intX1 - (float) srcOffX;
    float clipOffY = intY1 - (float) srcOffY;
    float visW = intX2 - intX1;
    float visH = intY2 - intY1;

    // World-space corners of the visible sub-rect (before rotation)
    float gx0 = x + clipOffX * xscale;
    float gy0 = y + clipOffY * yscale;
    float gx1 = gx0 + visW * xscale;
    float gy1 = gy0;
    float gx2 = gx0;
    float gy2 = gy0 + visH * yscale;
    float gx3 = gx1;
    float gy3 = gy2;

    if (angleDeg != 0.0f) {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float dx, dy, rx, ry;
#define ROTATE_CORNER(gxi, gyi) do { dx = (gxi) - pivotX; dy = (gyi) - pivotY; rx = cosA * dx - sinA * dy + pivotX; ry = sinA * dx + cosA * dy + pivotY; (gxi) = rx; (gyi) = ry; } while(0)
        ROTATE_CORNER(gx0, gy0);
        ROTATE_CORNER(gx1, gy1);
        ROTATE_CORNER(gx2, gy2);
        ROTATE_CORNER(gx3, gy3);
#undef ROTATE_CORNER
    }

    // Convert game-space corners to screen space
    float sx0 = (gx0 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy0 = (gy0 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx1 = (gx1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (gy1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (gx2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (gy2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx3 = (gx3 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy3 = (gy3 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // View frustum culling
    float minSX = fminf(fminf(sx0, sx1), fminf(sx2, sx3));
    float maxSX = fmaxf(fmaxf(sx0, sx1), fmaxf(sx2, sx3));
    float minSY = fminf(fminf(sy0, sy1), fminf(sy2, sy3));
    float maxSY = fmaxf(fmaxf(sy0, sy1), fmaxf(sy2, sy3));
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT) return;

    bool hasRotation = angleDeg != 0.0f;

    if (!hasTexture) {
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

    // Map intersection region to atlas UV space. Snapshot tpags are 1:1 with their own VRAM region.
    float ratioX, ratioY, atlasU0, atlasV0;
    if (snapPartChunk != nullptr) {
        ratioX = 1.0f; ratioY = 1.0f;
        atlasU0 = 0.0f; atlasV0 = 0.0f;
    } else {
        ratioX = (cW > 0) ? ((float) atlasEntry->width / cW) : 1.0f;
        ratioY = (cH > 0) ? ((float) atlasEntry->height / cH) : 1.0f;
        atlasU0 = (float) atlasEntry->atlasX;
        atlasV0 = (float) atlasEntry->atlasY;
    }
    float u0 = atlasU0 + (intX1 - cX) * ratioX;
    float v0 = atlasV0 + (intY1 - cY) * ratioY;
    float u1 = u0 + visW * ratioX;
    float v1 = v0 + visH * ratioY;

    // GS modulate mode: Output = Texture * Vertex / 128
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    if (hasRotation) {
        gsKit_prim_quad_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx1, sy1, u1, v0, sx2, sy2, u0, v1, sx3, sy3, u1, v1, 0, gsColor);
    } else {
        gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx3, sy3, u1, v1, 0, gsColor);
    }
}

// GS textured prims are single-coloured, so per-corner colours fall back to corner 1 (same as gsDrawSurfaceColor).
static void gsDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED uint32_t color4, float alpha) {
    gsDrawSpritePart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color1, alpha);
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

    // Map the entire atlas entry (the trimmed source content in atlas texels) to the user's quad. Snapshot tpags cover (0,0)..(w,h) of their VRAM region.
    float u0, v0, u1, v1;
    int32_t posSnapIdx = tpagSnapshotIndex(gs, tpagIndex);
    if (posSnapIdx >= 0) {
        SnapshotChunk* chunk = &gs->snapshotChunks[posSnapIdx];
        u0 = 0.0f; v0 = 0.0f;
        u1 = (float) chunk->width;
        v1 = (float) chunk->height;
    } else {
        AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
        u0 = (float) atlasEntry->atlasX;
        v0 = (float) atlasEntry->atlasY;
        u1 = u0 + (float) atlasEntry->width;
        v1 = v0 + (float) atlasEntry->height;
    }

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

static void gsPrimitiveBegin(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t primitiveType) {
    return;
}

static void gsPrimitiveBeginTexture(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t primitiveType, MAYBE_UNUSED int32_t texture) {
    return;
}

static void gsPrimitiveEnd(MAYBE_UNUSED Renderer* renderer) {
    return;
}

static void gsDrawVertex(
    MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float z,
    MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha,
    MAYBE_UNUSED float u, MAYBE_UNUSED float v
) {
    return;
}

static void gsDrawVertexBuffer(
    MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED VertexBuffer* buffer,
    MAYBE_UNUSED int32_t primitive, MAYBE_UNUSED int32_t texture,
    MAYBE_UNUSED int32_t offset, MAYBE_UNUSED int32_t count
) {
    return;
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

static void gsDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED uint32_t color4, float alpha, bool outline) {
    // Stub! Please implement me later. :3
    gsDrawRectangle(renderer, x1, y1, x2, y2, color1, alpha, outline);
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

        // Sprite-font glyphs sit at the cell offset; GM 2023.2+ subtracts the sprite origin, pre-2023.2 it cancels.
        // (See GameMaker-HTML5's commit a7c5b909209d5a28602fedfe2031965386a99921)
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) (int32_t) glyphTpag->targetY - (float) font->spriteOriginYAdjust;
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

static void gsDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, MAYBE_UNUSED float angleDeg, float lineSeparation) {
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
    // Caller-supplied separation is in world pre-scale pixels; divide by font->scaleY so the transform restores it.
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
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

static void gsDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, MAYBE_UNUSED float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha, float lineSeparation) {
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
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    uint8_t ga = alphaToGS(alpha);

    while (textLen >= lineStart) {
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
        // Pixel-position cursor for the gradient
        float gradientX = 0.0f;

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
                float advance = (float) glyph->shift;
                float leftFrac  = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                int32_t c1 = Color_lerp(_c1, _c2, leftFrac);
                int32_t c2 = Color_lerp(_c1, _c2, rightFrac);
                int32_t c3 = Color_lerp(_c4, _c3, rightFrac);
                int32_t c4 = Color_lerp(_c4, _c3, leftFrac);

                // GS modulate mode: Output = Texture * Vertex / 128
                // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 1.0x multiplier
                u64 textColor1 = GS_SETREG_RGBAQ(BGR_R(c1) >> 1, BGR_G(c1) >> 1, BGR_B(c1) >> 1, ga, 0x00);
                u64 textColor2 = GS_SETREG_RGBAQ(BGR_R(c2) >> 1, BGR_G(c2) >> 1, BGR_B(c2) >> 1, ga, 0x00);
                u64 textColor3 = GS_SETREG_RGBAQ(BGR_R(c3) >> 1, BGR_G(c3) >> 1, BGR_B(c3) >> 1, ga, 0x00);
                u64 textColor4 = GS_SETREG_RGBAQ(BGR_R(c4) >> 1, BGR_G(c4) >> 1, BGR_B(c4) >> 1, ga, 0x00);

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
                gradientX   += (float) glyph->shift;
                if (resolveOk && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX   += kern;
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

static void gsDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline)
{
    GsRenderer* gs = (GsRenderer*) renderer;
    if(outline)
    {
        gsDrawLine(renderer, x1, y1, x2, y2, 1, color1, alpha);
        gsDrawLine(renderer, x2, y2, x3, y3, 1, color2, alpha);
        gsDrawLine(renderer, x3, y3, x1, y1, 1, color3, alpha);
    } else {
        float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        float sx3 = (x3 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        float sy3 = (y3 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

        uint8_t a = alphaToGS(alpha);
        u64 triColor1 = GS_SETREG_RGBAQ(BGR_R(color1), BGR_G(color1), BGR_B(color1), a, 0x00);
        u64 triColor2 = GS_SETREG_RGBAQ(BGR_R(color2), BGR_G(color2), BGR_B(color2), a, 0x00);
        u64 triColor3 = GS_SETREG_RGBAQ(BGR_R(color3), BGR_G(color3), BGR_B(color3), a, 0x00);
        gsKit_prim_triangle_gouraud_3d(gs->gsGlobal, sx1, sy1, 0,sx2, sy2, 0,sx3, sy3, 0,triColor1, triColor2, triColor3);
    }
}

static void gsFlush(MAYBE_UNUSED Renderer* renderer) {
    // No-op: gsKit queues commands, executed in main loop
}

static void gsClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    // draw_clear_alpha writes RGBA directly to the framebuffer in real GameMaker.
    // The PS2 renderer keeps PRIM.ABE permanently on (so TEX0.TCC stays 1 for textured sprites), which means without overriding ALPHA here, clearing with alpha=0 would resolve to (Cs-Cd)*0 + Cd = Cd and not write anything.
    // We also need to bypass two surface-only states for the duration of the clear: FBA (which would force the cleared a-bit back to 1) and alpha test (which would reject the alpha=0 clear quad outright).
    uint8_t savedFba = gs->fba;
    uint8_t savedAte = gs->gsGlobal->Test->ATE;
    if (savedFba) gsApplyFBA(gs, 0);
    if (savedAte) {
        gs->gsGlobal->Test->ATE = 0;
        gsKit_set_test(gs->gsGlobal, GS_ATEST_OFF);
    }
    gsKit_set_primalpha(gs->gsGlobal, GS_ALPHA_NO_BLEND, 0);
    gsKit_clear(gs->gsGlobal, GS_SETREG_RGBAQ(r, g, b, a, 0x00));
    gsKit_set_primalpha(gs->gsGlobal, gs->blendEnabled ? gs->currentBlendAlpha : GS_ALPHA_NO_BLEND, 0);
    if (savedAte) {
        gs->gsGlobal->Test->ATE = 1;
        gsKit_set_test(gs->gsGlobal, GS_ATEST_ON);
    }
    if (savedFba) gsApplyFBA(gs, 1);
}

static int32_t gsCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth, int32_t xorig, int32_t yorig) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (surfaceID != -1) {
        rendererPrintf("GsRenderer: Trying to call sprite_create_from_surface on a non-main surface!\n");
        return -1;
    }

    if (0 >= w || 0 >= h) return -1;

    // The (x, y, w, h) arguments are in world/view-space (Undertale's sprite_create_from_screen_x wrapper passes world coords).
    // The PS2 framebuffer is the rasterized output of the renderer's view transform: fb_pos = (world_pos - viewOrigin) * scale + offset.
    // For Undertale on a 640x448 fb with a 320x240 view, scale=2 and offsetY=-16 (480 rendered, clipped to 448).
    // Apply the same transform here so we read the actual fb pixels that correspond to the requested world rect.
    float fbXf = ((float) x - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float fbYf = ((float) y - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float fbWf = (float) w * gs->scaleX;
    float fbHf = (float) h * gs->scaleY;

    // Round to integer fb pixels (floor origin, ceil far edge to avoid losing a fractional row/column).
    int32_t fbRectX = (int32_t) floorf(fbXf);
    int32_t fbRectY = (int32_t) floorf(fbYf);
    int32_t fbRectRight = (int32_t) ceilf(fbXf + fbWf);
    int32_t fbRectBottom = (int32_t) ceilf(fbYf + fbHf);

    // Clip to live framebuffer bounds (the rect may extend past the top/bottom because Undertale renders a logical 480 into a 448 fb with offsetY=-16).
    int32_t fbW = (int32_t) gs->gsGlobal->Width;
    int32_t fbH = (int32_t) gs->gsGlobal->Height;
    if (fbRectX < 0) fbRectX = 0;
    if (fbRectY < 0) fbRectY = 0;
    if (fbRectRight > fbW) fbRectRight = fbW;
    if (fbRectBottom > fbH) fbRectBottom = fbH;
    int32_t fbRectW = fbRectRight - fbRectX;
    int32_t fbRectH = fbRectBottom - fbRectY;
    if (0 >= fbRectW || 0 >= fbRectH) return -1;

    // Snapshot chunk holds fb-space pixels. The sprite stays at world-space (w, h) so draw_sprite at world coords reverses the projection symmetrically (UV stretches the fb-rect snapshot back over the world rect).
    int32_t chunkIdx = allocateSnapshotChunk(gs, fbRectW, fbRectH);
    if (0 > chunkIdx) return -1;
    SnapshotChunk* chunk = &gs->snapshotChunks[chunkIdx];
    uint32_t snapshotVramAddr = gs->textureVramBase + (uint32_t) chunk->firstChunk * VRAM_CHUNK_SIZE;

    // Drain any pending gsKit draw commands into the GIF before snapshotting, so the bitblt reads a coherent framebuffer.
    gsKit_queue_exec(gs->gsGlobal);
    dmaKit_wait_fast();

    uint32_t fbVramBase = gs->gsGlobal->ScreenBuffer[gs->gsGlobal->ActiveBuffer & 1];
    uint32_t fbTbw = gs->gsGlobal->Width / 64;

    gsLocalToLocalBlit(
        fbVramBase, fbTbw, GS_PSM_CT16, (uint32_t) fbRectX, (uint32_t) fbRectY,
        snapshotVramAddr, chunk->tbw, GS_PSM_CT16, 0, 0,
        (uint32_t) fbRectW, (uint32_t) fbRectH
    );

    // Sprite/TPAG dimensions are in world space (what GML thinks the sprite is sized as). UV uses chunk->width/height (fb-space) to stretch the fb-rect back over the world rect when drawn.
    int32_t spriteW = w;
    int32_t spriteH = h;

    // Allocate a synthetic TPAG slot pointing at the snapshot chunk.
    uint32_t tpagIndex = findOrAllocTpagSlot(gs, dw);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) spriteW;
    tpag->sourceHeight = (uint16_t) spriteH;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) spriteW;
    tpag->targetHeight = (uint16_t) spriteH;
    tpag->boundingWidth = (uint16_t) spriteW;
    tpag->boundingHeight = (uint16_t) spriteH;
    tpag->texturePageId = 0; // Real page id is irrelevant for snapshot tpags; the sentinel is the tpagToSnapshot mapping. 0 (not -1) so findOrAllocTpagSlot won't reclaim this slot.

    // Wire the TPAG -> snapshot chunk mapping (parallel array, indexed by tpagIndex).
    while ((uint32_t) arrlen(gs->tpagToSnapshot) <= tpagIndex) {
        int32_t sentinel = -1;
        arrput(gs->tpagToSnapshot, sentinel);
    }
    gs->tpagToSnapshot[tpagIndex] = chunkIdx;

    // Allocate a sprite slot. DataWin_allocSpriteSlot handles name + slot reuse.
    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, gs->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t) spriteW;
    sprite->height = (uint32_t) spriteH;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t *)safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    rendererPrintf("GsRenderer: Snapshot sprite %u world=%dx%d fb=%dx%d@(%d,%d) tpag=%u row=%d firstChunk=%u count=%u vram=0x%08X\n", spriteIndex, spriteW, spriteH, fbRectW, fbRectH, fbRectX, fbRectY, tpagIndex, chunkIdx, chunk->firstChunk, chunk->chunkCount, snapshotVramAddr);
    return (int32_t) spriteIndex;
}

static void gsDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || (uint32_t) spriteIndex >= dw->sprt.count) return;
    // Refuse to delete original data.win sprites - their tpagIndices point into the static atlas, not snapshot pool.
    if (gs->originalSpriteCount > (uint32_t) spriteIndex) {
        logWarn("GsRenderer: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Free snapshot chunk(s) and reclaim TPAG slot(s) owned by this sprite.
    repeat(sprite->textureCount, i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (0 > tpagIdx) continue;
        if (gs->originalTpagCount > (uint32_t) tpagIdx) continue; // static atlas TPAG; not ours to free
        int32_t snapIdx = tpagSnapshotIndex(gs, tpagIdx);
        if (snapIdx >= 0) {
            freeSnapshotChunk(gs, snapIdx);
            gs->tpagToSnapshot[tpagIdx] = -1;
        }
        dw->tpag.items[tpagIdx].texturePageId = -1; // mark free for findOrAllocTpagSlot reuse
    }

    // Clear the sprite slot. Preserve "name" for asset_get_index across the memset (slot stays in sprt.count).
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
}

// PS2 GS only supports a single blend equation:
//   Cv = (A - B) * (C / 128) + D
// Where A, B, D pick from {Cs=0, Cd=1, 0=2} and C picks from {As=0, Ad=1, FIX=2}.
// This is a much smaller space than GL's sf*Cs + df*Cd, so most non-trivial GMS blend modes get approximated.
// The cases that DO map exactly are the ones DELTARUNE relies on for the dest-alpha mask trick (bm_dest_alpha + bm_inv_dest_alpha) and standard alpha blending (bm_normal).
// Anything we cannot express falls back to bm_normal.

// Builds a GS_SETREG_ALPHA value from (sfactor, dfactor) pair semantics: result = sf*Cs + df*Cd.
// Returns true on exact match, false if the pair was approximated.
static bool gmsFactorPairToGSAlpha(int32_t sf, int32_t df, u64* outAlpha) {
    // (src_alpha, inv_src_alpha) -> (Cs - Cd) * As + Cd
    if (sf == bm_src_alpha && df == bm_inv_src_alpha) { *outAlpha = GS_SETREG_ALPHA(0, 1, 0, 1, 0); return true; }
    // (src_alpha, one) -> Cs*As + Cd
    if (sf == bm_src_alpha && df == bm_one) { *outAlpha = GS_SETREG_ALPHA(0, 2, 0, 1, 0); return true; }
    // (one, one) -> Cs + Cd  (FIX=128 makes C/128 == 1)
    if (sf == bm_one && df == bm_one) { *outAlpha = GS_SETREG_ALPHA(0, 2, 2, 1, 0x80); return true; }
    // (one, inv_src_alpha) -> approximate as bm_normal (premultiplied alpha case)
    if (sf == bm_one && df == bm_inv_src_alpha) { *outAlpha = GS_SETREG_ALPHA(0, 1, 0, 1, 0); return true; }
    // (zero, one) -> Cd (no source contribution)
    if (sf == bm_zero && df == bm_one) { *outAlpha = GS_SETREG_ALPHA(2, 2, 2, 1, 0); return true; }
    // (one, zero) -> Cs (replace dest)
    if (sf == bm_one && df == bm_zero) { *outAlpha = GS_SETREG_ALPHA(0, 2, 2, 2, 0x80); return true; }
    // (zero, zero) -> 0 (clear)
    if (sf == bm_zero && df == bm_zero) { *outAlpha = GS_SETREG_ALPHA(2, 2, 2, 2, 0); return true; }
    // (dest_alpha, inv_dest_alpha) -> (Cs - Cd) * Ad + Cd
    if (sf == bm_dest_alpha && df == bm_inv_dest_alpha) { *outAlpha = GS_SETREG_ALPHA(0, 1, 1, 1, 0); return true; }
    // (inv_dest_alpha, dest_alpha) -> (Cd - Cs) * Ad + Cs
    if (sf == bm_inv_dest_alpha && df == bm_dest_alpha) { *outAlpha = GS_SETREG_ALPHA(1, 0, 1, 0, 0); return true; }
    // (dest_alpha, one) -> Cs*Ad + Cd
    if (sf == bm_dest_alpha && df == bm_one) { *outAlpha = GS_SETREG_ALPHA(0, 2, 1, 1, 0); return true; }
    // (zero, src_alpha) -> Cd*As (modulate dest by source alpha)
    if (sf == bm_zero && df == bm_src_alpha) { *outAlpha = GS_SETREG_ALPHA(2, 1, 0, 1, 0); return true; }
    // (zero, inv_src_alpha) -> Cd * (1 - As) -> (0 - Cd) * As + Cd
    if (sf == bm_zero && df == bm_inv_src_alpha) { *outAlpha = GS_SETREG_ALPHA(2, 1, 0, 1, 0); return false; } // off-by-(approximation), tolerable

    // Fallback: behave like bm_normal so things stay roughly visible.
    *outAlpha = GS_SETREG_ALPHA(0, 1, 0, 1, 0);
    return false;
}

// Maps the simple GMS blend modes (bm_normal/add/subtract/etc) to a GS ALPHA register value.
static u64 gmsBlendModeToGSAlpha(int32_t mode) {
    switch (mode) {
        case bm_normal:           return GS_SETREG_ALPHA(0, 1, 0, 1, 0);    // (Cs-Cd)*As + Cd
        case bm_add:              return GS_SETREG_ALPHA(0, 2, 0, 1, 0);    // Cs*As + Cd
        case bm_subtract:         return GS_SETREG_ALPHA(1, 0, 2, 2, 0x80); // Cd - Cs (clamped to 0)
        case bm_reverse_subtract: return GS_SETREG_ALPHA(2, 0, 0, 1, 0);    // Cd - Cs*As
        case bm_min:              return GS_SETREG_ALPHA(0, 1, 0, 1, 0);    // No GS min, fall back to normal
        case bm_max:              return GS_SETREG_ALPHA(0, 1, 0, 1, 0);    // No GS max, fall back to normal
        default:                  return GS_SETREG_ALPHA(0, 1, 0, 1, 0);
    }
}

static BlendFactors gsGpuGetBlendFactors(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*)renderer;
    return (BlendFactors){
        gs->currentSFactor,
        gs->currentDFactor,
        gs->currentSFactorAlpha,
        gs->currentDFactorAlpha
    };
}

static int32_t gsGpuGetBlendMode(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;
    return gs->currentBlendMode;
}

static void gsGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->currentBlendAlpha = gmsBlendModeToGSAlpha(mode);
    gsCommitBlend(gs);
}

static void gsGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, MAYBE_UNUSED int32_t sfactor_alpha, MAYBE_UNUSED int32_t dfactor_alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->currentBlendMode = bm_complex;
    gs->currentSFactor = sfactor;
    gs->currentDFactor = dfactor;
    gs->currentSFactorAlpha = sfactor_alpha;
    gs->currentDFactorAlpha = dfactor_alpha;
    u64 alpha;
    if (!gmsFactorPairToGSAlpha(sfactor, dfactor, &alpha) && !gs->blendModeWarned) {
        logWarn("GsRenderer: blend mode (sf=%d, df=%d) not exactly representable on PS2; approximating\n", sfactor, dfactor);
        gs->blendModeWarned = true;
    }
    gs->currentBlendAlpha = alpha;
    gsCommitBlend(gs);
}

// While a surface is bound, FBA + alpha-test follow the GML's blend-enable signal:
// * blend ON  -> FBA=1, ATE=1: sprite-friendly. Opaque texels get a-bit forced to 1; transparent atlas texels (alpha=0) are discarded so the cleared a-bit=0 survives the surrounding region.
// * blend OFF -> FBA=0, ATE=0: mask-friendly. Source alpha passes straight through to the CT16 a-bit so that masks can carve actual transparent holes in the surface.
// Outside a surface this helper is a no-op; the main FB keeps its existing FBA-via-colorwriteenable + ATE-via-alphatestenable behavior.
static void gsApplySurfaceWriteMode(GsRenderer* gs) {
    if (gs->currentSurface == -1) return;
    if (gs->surfaces[gs->currentSurface].chunkCount == 0) return; // phantom: leave FBMSK masking everything
    uint8_t wantFba = gs->blendEnabled ? 1 : 0;
    uint8_t wantAte = gs->blendEnabled ? 1 : 0;
    if (gs->fba != wantFba) gsApplyFBA(gs, wantFba);
    if (gs->gsGlobal->Test->ATE != wantAte) {
        gs->gsGlobal->Test->ATE = wantAte;
        gs->gsGlobal->Test->ATST = 6;   // GREATER
        gs->gsGlobal->Test->AREF = 0;
        gs->gsGlobal->Test->AFAIL = 0;  // KEEP (no write on fail)
        gsKit_set_test(gs->gsGlobal, wantAte ? GS_ATEST_ON : GS_ATEST_OFF);
    }
}

static void gsGpuSetBlendEnable(Renderer* renderer, bool enable) {
    GsRenderer* gs = (GsRenderer*) renderer;
    // PrimAlphaEnable is OR'd into the PRIM bits gsKit emits with each primitive,
    // so toggling it affects every subsequent draw without needing to flush.
    if (gs->blendEnabled == enable) return;
    gs->blendEnabled = enable;
    gsCommitBlend(gs);
    // Inside a surface, FBA + alpha-test track blend state so that "gpu_set_blendenable(false); draw alpha=0" punches an actual hole, while regular blended sprite draws keep their force-opaque + discard-transparent semantics.
    gsApplySurfaceWriteMode(gs);
}

static bool gsGpuGetBlendEnable(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;

    return gs->blendEnabled;
}

static void gsGpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    *red = gs->colorWriteR;
    *green = gs->colorWriteG;
    *blue = gs->colorWriteB;
    *alpha = gs->colorWriteA;
}


static void gsGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    GsRenderer* gs = (GsRenderer*) renderer;
    GSGLOBAL* g = gs->gsGlobal;
    // Default to GREATER comparison when first enabling. If the ref hasn't been touched yet,
    // initialize to 0 so behavior matches GL (test passes when src_alpha > ref).
    if (enable) {
        g->Test->ATST = 6; // GREATER
        g->Test->AFAIL = 0; // KEEP (skip writing entirely)
    }
    gsKit_set_test(g, enable ? GS_ATEST_ON : GS_ATEST_OFF);
}

static bool gsGpuGetAlphaTestEnable(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;
    return gs->gsGlobal->Test->ATST == 6 && gs->gsGlobal->Test->AFAIL == 0;
}

static void gsGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    GsRenderer* gs = (GsRenderer*) renderer;
    GSGLOBAL* g = gs->gsGlobal;
    g->Test->AREF = alphaToGS(ref);
    g->Test->ATST = 6;  // GREATER (matches GMS semantics: pass when src_alpha > ref)
    g->Test->AFAIL = 0; // KEEP
    // Preset 0 doesn't match any branch in gsKit_set_test, so it just re-emits TEST with current state.
    gsKit_set_test(g, 0);
}

static void gsGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->colorWriteR = red;
    gs->colorWriteG = green;
    gs->colorWriteB = blue;
    gs->colorWriteA = alpha;
    // FBMSK: bit=1 means MASK that bit (don't write). Layout is the conceptual RGBA8888 mapping
    // even when the framebuffer is CT16 - the GS remaps the relevant bits internally.
    u32 fbmsk = 0;
    if (!red)   fbmsk |= 0x000000FF;
    if (!green) fbmsk |= 0x0000FF00;
    if (!blue)  fbmsk |= 0x00FF0000;
    if (!alpha) fbmsk |= 0xFF000000;
    gsApplyFBMask(gs, fbmsk);

    // Alpha-only write mode (color writes off, alpha on) is the dest-alpha mask write half: the script wants the source alpha to land in FB.A verbatim, so disable FBA.
    // Anything else keeps FBA=1 so normal blended sprites leave FB.A=1 and the mask read half (bm_dest_alpha / bm_inv_dest_alpha) sees opaque pixels.
    bool alphaOnly = !red && !green && !blue && alpha;
    gsApplyFBA(gs, alphaOnly ? 0 : 1);
}

// Fallback for tiles not pre-baked into ATLAS.BIN.
// Mirrors the generic Renderer_drawTile path: clamp the tile's source rect to the TPAG content area, then draw as a sub-rect of the background TPAG.
static void gsDrawTileFromTPAG(GsRenderer* gs, RoomTile* tile, float offsetX, float offsetY) {
    Renderer* renderer = &gs->base;
    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tile);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

    int32_t srcX = tile->sourceX;
    int32_t srcY = tile->sourceY;
    int32_t srcW = (int32_t) tile->width;
    int32_t srcH = (int32_t) tile->height;
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;

    int32_t contentLeft = tpag->targetX;
    int32_t contentTop = tpag->targetY;
    if (contentLeft > srcX) {
        int32_t clip = contentLeft - srcX;
        drawX += (float) clip * tile->scaleX;
        srcW -= clip;
        srcX = contentLeft;
    }
    if (contentTop > srcY) {
        int32_t clip = contentTop - srcY;
        drawY += (float) clip * tile->scaleY;
        srcH -= clip;
        srcY = contentTop;
    }

    int32_t contentRight = tpag->targetX + tpag->sourceWidth;
    int32_t contentBottom = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > contentRight) srcW = contentRight - srcX;
    if (srcY + srcH > contentBottom) srcH = contentBottom - srcY;

    if (0 >= srcW || 0 >= srcH) return;

    int32_t atlasOffX = srcX - tpag->targetX;
    int32_t atlasOffY = srcY - tpag->targetY;

    uint32_t bgr = tile->color & 0x00FFFFFF;
    gsDrawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH, drawX, drawY, tile->scaleX, tile->scaleY, 0.0f, 0.0f, 0.0f, bgr, tile->alpha);
}

static void gsDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    GsRenderer* gs = (GsRenderer*) renderer;

    // Look up the tile in the atlas tile entries
    AtlasTileEntry* tileEntry = findTileEntry(gs, (int16_t) tile->backgroundDefinition, (uint16_t) tile->sourceX, (uint16_t) tile->sourceY, (uint16_t) tile->width, (uint16_t) tile->height);
    if (tileEntry == nullptr) {
        // Runtime-added tiles (tile_add) won't be in the pre-baked atlas. Fall back to rendering the sub-rect from the background's TPAG entry.
        gsDrawTileFromTPAG(gs, tile, offsetX, offsetY);
        return;
    }

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

    uint32_t bgr = tile->color & 0x00FFFFFF;

    // GS modulate mode: scale RGB from 0-255 to 0-128
    uint8_t r = BGR_R(bgr) >> 1;
    uint8_t g = BGR_G(bgr) >> 1;
    uint8_t b = BGR_B(bgr) >> 1;
    uint8_t a = alphaToGS(tile->alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, 0, gsColor);
}

// ===[ Surfaces ]===

#define SURFACE_MAX_BYTES (1024 * 1024) // Hard cap per surface

static bool gsSurfaceIsLive(GsRenderer* gs, int32_t surfaceID) {
    if (0 > surfaceID) return false;
    if ((uint32_t) surfaceID >= (uint32_t) arrlen(gs->surfaces)) return false;
    return gs->surfaces[surfaceID].inUse;
}

// Re-emit FRAME_1/SCISSOR_1 (both contexts) for the current ScreenBuffer/Width/Height/PSM in gsGlobal.
// Drains the queue first so any pending draws still hit the previous target.
static void gsApplyFrameSwitch(GsRenderer* gs) {
    gsKit_queue_exec(gs->gsGlobal);
    dmaKit_wait_fast();
    gsKit_setactive(gs->gsGlobal);
}

// Zero out a CT16 region in VRAM (writes 0x0000 = ARGB1555 fully transparent black) by uploading a zero buffer via host->local DMA.
static void gsClearSurfaceVram(uint32_t vramAddr, uint32_t tbw, uint16_t paddedWidth, uint16_t height) {
    size_t bytes = (size_t) paddedWidth * (size_t) height * 2;
    // 128-byte alignment required by gsKit_texture_send/DMA.
    uint8_t* zeros = (uint8_t*) safeMemalign(128, bytes);
    memset(zeros, 0, bytes);
    gsKit_texture_send((u32*) zeros, paddedWidth, height, vramAddr, GS_PSM_CT16, tbw, GS_CLUT_TEXTURE);
    dmaKit_wait_fast();
    free(zeros);
}

static int32_t gsCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    GsRenderer* gs = (GsRenderer*) renderer;

    if (0 >= width || 0 >= height) {
        rendererPrintf("GsRenderer: surface_create rejected bad dims %dx%d\n", width, height);
        return -1;
    }

    uint16_t tbw = (uint16_t) ((width + 63) / 64);
    if (tbw == 0) tbw = 1;
    uint32_t paddedWidth = (uint32_t) tbw * 64;
    uint32_t bytes = gsKit_texture_size(paddedWidth, height, GS_PSM_CT16);

    // Reuse a freed row if possible so the table doesn't grow unbounded.
    int32_t row = -1;
    uint32_t rowCount = (uint32_t) arrlen(gs->surfaces);
    for (uint32_t i = 0; rowCount > i; i++) {
        if (!gs->surfaces[i].inUse) { row = (int32_t) i; break; }
    }
    if (0 > row) {
        Surface zero = {0};
        arrput(gs->surfaces, zero);
        row = (int32_t) (arrlen(gs->surfaces) - 1);
    }

    // When we aren't able to allocate this, we return a "phantom" row
    int chunksNeeded = (int) ((bytes + VRAM_CHUNK_SIZE - 1) / VRAM_CHUNK_SIZE);
    if (0 >= chunksNeeded) chunksNeeded = 1;

    int32_t firstChunk = -1;
    const char* phantomReason = nullptr;
    if (bytes > SURFACE_MAX_BYTES) {
        phantomReason = "exceeds per-surface byte cap";
    } else {
        // Allocate from the non-reserved tail of the chunk pool (same as snapshots).
        firstChunk = allocateChunks(gs, chunksNeeded, gs->reservedAtlasChunks);
        if (0 > firstChunk)
            phantomReason = "out of VRAM";
    }

    if (phantomReason != nullptr) {
        logWarn("GsRenderer: surface_create(%d, %d) phantom (%s); needed %d chunks (%u bytes)\n", width, height, phantomReason, chunksNeeded, bytes);
        Surface* s = &gs->surfaces[row];
        s->firstChunk = 0;
        s->chunkCount = 0;
        s->width = (uint16_t) width;
        s->height = (uint16_t) height;
        s->tbw = tbw;
        s->inUse = true;
        return row;
    }

    // Pin chunks to this surface.
    for (int c = 0; chunksNeeded > c; c++) {
        VRAMChunk* chunk = &gs->chunks[firstChunk + c];
        chunk->atlasId = -1;
        chunk->snapshotIdx = -1;
        chunk->surfaceIdx = (int16_t) row;
        chunk->lastUsed = 0;
    }

    Surface* s = &gs->surfaces[row];
    s->firstChunk = (uint16_t) firstChunk;
    s->chunkCount = (uint16_t) chunksNeeded;
    s->width = (uint16_t) width;
    s->height = (uint16_t) height;
    s->tbw = tbw;
    s->inUse = true;

    // Fully transparent initial state. GameMaker games rely on this even when they immediately follow up with draw_clear_alpha.
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) firstChunk * VRAM_CHUNK_SIZE;
    gsClearSurfaceVram(vramAddr, tbw, (uint16_t) paddedWidth, (uint16_t) height);

    rendererPrintf("GsRenderer: surface_create %d -> %dx%d (padded %u, tbw=%u, chunks=%d@%d, vram=0x%08X)\n", row, width, height, paddedWidth, tbw, chunksNeeded, firstChunk, vramAddr);
    return row;
}

static bool gsSurfaceExists(Renderer* renderer, int32_t surfaceID) {
    GsRenderer* gs = (GsRenderer*) renderer;
    // The application_surface (sentinel ID on PS2) is always live: it's the GS screen framebuffer at a fixed VRAM address.
    if (surfaceID == APPLICATION_SURFACE_ID) return true;
    return gsSurfaceIsLive(gs, surfaceID);
}

// PS2's application surface IS the GS screen framebuffer, which lives at a fixed VRAM address outside the chunk pool.
// There's nothing to allocate, so we just return the sentinel ID.
static int32_t gsEnsureApplicationSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {
    return APPLICATION_SURFACE_ID;
}

static bool gsSetRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface) {
    GsRenderer* gs = (GsRenderer*) renderer;

    if (surfaceID == APPLICATION_SURFACE_ID && implicitApplicationSurface) {
        if (gs->currentSurface == -1) return true; // already on the main FB
        bool wasPhantom = gs->surfaces[gs->currentSurface].chunkCount == 0;
        if (wasPhantom) {
            // Phantom pop: we never touched the FRAME/view; just restore the writemask we clobbered to discard the bar's draws.
            gsApplyFBMask(gs, gs->savedFbmsk);
            gs->currentSurface = -1;
            return true;
        }
        // Restore framebuffer state captured on the most recent push.
        gs->gsGlobal->ScreenBuffer[gs->gsGlobal->ActiveBuffer & 1] = gs->savedScreenBufferAddr;
        gs->gsGlobal->Width = gs->savedFbWidth;
        gs->gsGlobal->Height = gs->savedFbHeight;
        gs->gsGlobal->PSM = gs->savedFbPSM;
        gsApplyFrameSwitch(gs);
        // Restore the alpha-test enable state the GML had asked for, and the main FB's FBA (typically 1 to keep its a-bit opaque).
        gs->gsGlobal->Test->ATE = gs->savedAte;
        gsKit_set_test(gs->gsGlobal, gs->savedAte ? GS_ATEST_ON : GS_ATEST_OFF);
        if (gs->fba != gs->savedFba) gsApplyFBA(gs, gs->savedFba);
        // Restore view transform so subsequent draws use the GML view coords again.
        gs->scaleX = gs->savedScaleX;
        gs->scaleY = gs->savedScaleY;
        gs->offsetX = gs->savedOffsetX;
        gs->offsetY = gs->savedOffsetY;
        gs->viewX = gs->savedViewX;
        gs->viewY = gs->savedViewY;
        gs->currentSurface = -1;
        return true;
    }

    if (!gsSurfaceIsLive(gs, surfaceID)) {
        rendererPrintf("GsRenderer: surface_set_target on invalid surface %d\n", surfaceID);
        return false;
    }
    if (gs->currentSurface != -1) {
        // Nested surface targets are not supported yet; the tension bar (our only consumer) never nests.
        rendererPrintf("GsRenderer: surface_set_target while another surface (%d) is already bound; ignoring nest into %d\n", gs->currentSurface, surfaceID);
        return false;
    }

    Surface* s = &gs->surfaces[surfaceID];

    if (s->chunkCount == 0) {
        // Phantom push: nothing to render into. Mask all FB writes for the duration so the bar's draws fall on the floor, then mark currentSurface so reset_target knows to undo this.
        gs->savedFbmsk = gs->fbmsk;
        gsApplyFBMask(gs, 0xFFFFFFFFu);
        gs->currentSurface = surfaceID;
        return true;
    }

    uint32_t vramAddr = gs->textureVramBase + (uint32_t) s->firstChunk * VRAM_CHUNK_SIZE;

    // Save framebuffer + view state so surface_reset_target can restore them.
    gs->savedScreenBufferAddr = gs->gsGlobal->ScreenBuffer[gs->gsGlobal->ActiveBuffer & 1];
    gs->savedFbWidth = gs->gsGlobal->Width;
    gs->savedFbHeight = gs->gsGlobal->Height;
    gs->savedFbPSM = gs->gsGlobal->PSM;
    gs->savedAte = gs->gsGlobal->Test->ATE;
    gs->savedFba = gs->fba;
    gs->savedScaleX = gs->scaleX;
    gs->savedScaleY = gs->scaleY;
    gs->savedOffsetX = gs->offsetX;
    gs->savedOffsetY = gs->offsetY;
    gs->savedViewX = gs->viewX;
    gs->savedViewY = gs->viewY;

    // Switch the active screen buffer to the surface's VRAM region. gsKit_setactive re-emits FRAME_1/SCISSOR_1 (both contexts) from these fields.
    gs->gsGlobal->ScreenBuffer[gs->gsGlobal->ActiveBuffer & 1] = vramAddr;
    gs->gsGlobal->Width = (uint16_t) (s->tbw * 64); // padded width must match TBW for the FRAME register
    gs->gsGlobal->Height = s->height;
    gs->gsGlobal->PSM = GS_PSM_CT16;
    gsApplyFrameSwitch(gs);

    // Identity view: surface-local draws like draw_sprite(spr_tensionbar, 1, 0, 0) must land at exactly (0, 0) in surface space.
    gs->scaleX = 1.0f;
    gs->scaleY = 1.0f;
    gs->offsetX = 0.0f;
    gs->offsetY = 0.0f;
    gs->viewX = 0;
    gs->viewY = 0;
    gs->currentSurface = surfaceID;

    // Pick FBA + alpha-test based on the GML's current blend-enable state. See gsApplySurfaceWriteMode for the rationale.
    gsApplySurfaceWriteMode(gs);
    return true;
}

static float gsGetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float) gs->gsGlobal->Width;
    if (!gsSurfaceIsLive(gs, surfaceID)) return 0.0f;
    return (float) gs->surfaces[surfaceID].width;
}

static float gsGetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float) gs->gsGlobal->Height;
    if (!gsSurfaceIsLive(gs, surfaceID)) return 0.0f;
    return (float) gs->surfaces[surfaceID].height;
}

static void gsDrawSurfaceTiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
    // No-op
}


static void gsDrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    int32_t surfaceW;
    int32_t surfaceH;
    uint32_t srcVram;
    uint32_t srcTbw;

    if (surfaceID == APPLICATION_SURFACE_ID) {
        // Sample the live GS screen framebuffer; flush pending draws first so the texture read sees a coherent FB.
        gsKit_queue_exec(gs->gsGlobal);
        dmaKit_wait_fast();
        surfaceW = (int32_t) gs->gsGlobal->Width;
        surfaceH = (int32_t) gs->gsGlobal->Height;
        srcVram = gs->gsGlobal->ScreenBuffer[gs->gsGlobal->ActiveBuffer & 1];
        srcTbw = (uint32_t) gs->gsGlobal->Width / 64;
    } else {
        if (!gsSurfaceIsLive(gs, surfaceID)) return;
        Surface* s = &gs->surfaces[surfaceID];
        if (s->chunkCount == 0) return; // phantom surface — fully transparent, nothing to draw
        surfaceW = s->width;
        surfaceH = s->height;
        srcVram = gs->textureVramBase + (uint32_t) s->firstChunk * VRAM_CHUNK_SIZE;
        srcTbw = s->tbw;
    }

    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = surfaceW; srcHeight = surfaceH; }

    float worldW = (float) srcWidth * xscale;
    float worldH = (float) srcHeight * yscale;

    if (angleDeg != 0.0f) {
        // Tension bar (our only current consumer) never rotates. Add a quad_texture path when a game needs it.
        rendererPrintf("GsRenderer: draw_surface ignoring non-zero angle %f\n", (double) angleDeg);
    }

    // Apply the renderer's view transform to land the quad in framebuffer space.
    float sx0 = (x          - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy0 = (y          - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx1 = (x + worldW - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y + worldH - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // Off-screen cull.
    float minSX = fminf(sx0, sx1);
    float maxSX = fmaxf(sx0, sx1);
    float minSY = fminf(sy0, sy1);
    float maxSY = fmaxf(sy0, sy1);
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT) return;

    GSTEXTURE tex;
    memset(&tex, 0, sizeof(tex));
    tex.Width = surfaceW;
    tex.Height = surfaceH;
    tex.TBW = srcTbw;
    tex.Vram = srcVram;
    tex.PSM = GS_PSM_CT16;
    tex.Filter = GS_FILTER_NEAREST;

    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    float u0 = (float) srcLeft;
    float v0 = (float) srcTop;
    float u1 = (float) (srcLeft + srcWidth);
    float v1 = (float) (srcTop + srcHeight);

    // REGION_CLAMP the sampler to the src rect so any scale rounding never reads padded columns/rows. See project_ps2_ct16_region_clamp.
    gs->gsGlobal->Clamp->MINU = srcLeft;
    gs->gsGlobal->Clamp->MAXU = srcLeft + srcWidth  - 1;
    gs->gsGlobal->Clamp->MINV = srcTop;
    gs->gsGlobal->Clamp->MAXV = srcTop  + srcHeight - 1;
    gsKit_set_clamp(gs->gsGlobal, GS_CMODE_REGION_CLAMP);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx1, sy1, u1, v1, 0, gsColor);

    // Restore default REPEAT so subsequent atlas draws aren't stuck on this region.
    gsKit_set_clamp(gs->gsGlobal, GS_CMODE_REPEAT);
}

static void gsDrawSurfaceColor(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1, MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3, MAYBE_UNUSED uint32_t color4, float alpha) {
    return gsDrawSurface(renderer, surfaceID, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg, color1, alpha);
}

static void gsSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    (void)renderer;
    (void)surfaceID;
    (void)width;
    (void)height;
    // No-op: PS2 doesn't actually resize anything
}

static void gsSurfaceFree(Renderer* renderer, int32_t surfaceID) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (!gsSurfaceIsLive(gs, surfaceID)) return;
    if (gs->currentSurface == surfaceID) {
        // Caller is freeing the surface while it's still bound as target. Pop back to the main FB first to keep gsGlobal coherent.
        rendererPrintf("GsRenderer: surface_free %d while bound; auto-popping to main FB\n", surfaceID);
        gsSetRenderTarget(renderer, APPLICATION_SURFACE_ID, true);
    }

    Surface* s = &gs->surfaces[surfaceID];
    for (uint32_t c = 0; s->chunkCount > c; c++) {
        VRAMChunk* chunk = &gs->chunks[s->firstChunk + c];
        chunk->surfaceIdx = -1;
        chunk->atlasId = -1;
        chunk->snapshotIdx = -1;
        chunk->lastUsed = 0;
    }
    s->inUse = false;
    s->chunkCount = 0;
}

// surface_copy / surface_copy_part. Both source and destination are CT16 in the chunk pool (or the main framebuffer when SrcSurfaceID == APPLICATION_SURFACE_ID), so we can do a single local-to-local GS bitblt instead of going through the rasterizer.
static void gsSurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (!gsSurfaceIsLive(gs, destSurfaceID)) return;
    Surface* dst = &gs->surfaces[destSurfaceID];
    if (dst->chunkCount == 0) return; // phantom dest - nowhere to write

    uint32_t srcVram;
    uint32_t srcTbw;
    int32_t  srcWidth;
    int32_t  srcHeight;
    if (srcSurfaceID == APPLICATION_SURFACE_ID) {
        srcVram = gs->gsGlobal->ScreenBuffer[gs->gsGlobal->ActiveBuffer & 1];
        srcTbw = (uint32_t) gs->gsGlobal->Width / 64;
        srcWidth = (int32_t) gs->gsGlobal->Width;
        srcHeight = (int32_t) gs->gsGlobal->Height;
    } else {
        if (!gsSurfaceIsLive(gs, srcSurfaceID)) return;
        Surface* src = &gs->surfaces[srcSurfaceID];
        if (src->chunkCount == 0) return; // phantom src - nothing to read
        srcVram = gs->textureVramBase + (uint32_t) src->firstChunk * VRAM_CHUNK_SIZE;
        srcTbw = src->tbw;
        srcWidth = src->width;
        srcHeight = src->height;
    }

    int32_t w, h;
    if (part) {
        w = srcW;
        h = srcH;
    } else {
        srcX = 0; srcY = 0;
        w = srcWidth;
        h = srcHeight;
    }

    // Same fix as the one from gsCreateSpriteFromSurface
    if (srcSurfaceID == APPLICATION_SURFACE_ID && part) {
        srcX += (int32_t) floorf(gs->offsetX);
        srcY += (int32_t) floorf(gs->offsetY);
    }

    // Clip source rect to source bounds, propagating the offset into the destination so we copy the right sub-region.
    if (0 > srcX) { destX -= srcX; w += srcX; srcX = 0; }
    if (0 > srcY) { destY -= srcY; h += srcY; srcY = 0; }
    if (srcX + w > srcWidth)  w = srcWidth  - srcX;
    if (srcY + h > srcHeight) h = srcHeight - srcY;
    // Clip dest rect to dest bounds.
    if (0 > destX) { srcX -= destX; w += destX; destX = 0; }
    if (0 > destY) { srcY -= destY; h += destY; destY = 0; }
    if (destX + w > dst->width)  w = dst->width  - destX;
    if (destY + h > dst->height) h = dst->height - destY;
    if (0 >= w || 0 >= h) return;

    // Drain any pending gsKit draw commands into the GIF so the bitblt source is coherent. Same flush pattern gsCreateSpriteFromSurface uses.
    gsKit_queue_exec(gs->gsGlobal);
    dmaKit_wait_fast();

    uint32_t dstVram = gs->textureVramBase + (uint32_t) dst->firstChunk * VRAM_CHUNK_SIZE;
    gsLocalToLocalBlit(
        srcVram, srcTbw, GS_PSM_CT16, (uint32_t) srcX, (uint32_t) srcY,
        dstVram, dst->tbw, GS_PSM_CT16, (uint32_t) destX, (uint32_t) destY,
        (uint32_t) w, (uint32_t) h
    );
}
static bool gsSurfaceGetPixels(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED uint8_t* outRGBA) { return false; }

// ===[ Vtable ]===

// Decode a texture handle produced by gsSpriteGetTexture back into its page dimensions and sub-rect (in texels).
// Returns false for the 0 ("no texture") handle or an unresolvable one.
static bool gsResolveTextureHandle(GsRenderer* gs, uint32_t texHandle, uint16_t* outPageW, uint16_t* outPageH, uint16_t* outSubX, uint16_t* outSubY, uint16_t* outSubW, uint16_t* outSubH) {
    if (texHandle == 0) return false;
    int32_t tpagIndex = (int32_t) texHandle - 1;
    if (tpagIndex < 0) return false;

    int32_t snapshotIdx = tpagSnapshotIndex(gs, tpagIndex);
    if (snapshotIdx >= 0) {
        SnapshotChunk* chunk = &gs->snapshotChunks[snapshotIdx];
        *outPageW = chunk->width;
        *outPageH = chunk->height;
        *outSubX = 0;
        *outSubY = 0;
        *outSubW = chunk->width;
        *outSubH = chunk->height;
        return true;
    }

    if ((uint32_t) tpagIndex >= gs->atlasTPAGCount) return false;
    AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
    if (entry->atlasId == 0xFFFF) return false;
    *outPageW = gs->atlasWidth[entry->atlasId];
    *outPageH = gs->atlasHeight[entry->atlasId];
    *outSubX = entry->atlasX;
    *outSubY = entry->atlasY;
    *outSubW = entry->width;
    *outSubH = entry->height;
    return true;
}

static uint32_t gsSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (tpagIndex < 0) return 0;
    uint16_t pw, ph, sx, sy, sw, sh;
    if (!gsResolveTextureHandle(gs, (uint32_t) (tpagIndex + 1), &pw, &ph, &sx, &sy, &sw, &sh)) return 0;
    return (uint32_t) (tpagIndex + 1);
}

static uint32_t gsSurfaceGetTexture(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) {
    return 0;
}

static float gsTextureGetTexelWidth(Renderer* renderer, uint32_t texHandle) {
    GsRenderer* gs = (GsRenderer*) renderer;
    uint16_t pw, ph, sx, sy, sw, sh;
    if (!gsResolveTextureHandle(gs, texHandle, &pw, &ph, &sx, &sy, &sw, &sh) || pw == 0) return 1.0f;
    return 1.0f / (float) pw;
}

static float gsTextureGetTexelHeight(Renderer* renderer, uint32_t texHandle) {
    GsRenderer* gs = (GsRenderer*) renderer;
    uint16_t pw, ph, sx, sy, sw, sh;
    if (!gsResolveTextureHandle(gs, texHandle, &pw, &ph, &sx, &sy, &sw, &sh) || ph == 0) return 1.0f;
    return 1.0f / (float) ph;
}

static bool gsTextureGetUVs(Renderer* renderer, uint32_t texHandle, float* outUVs) {
    GsRenderer* gs = (GsRenderer*) renderer;
    uint16_t pw, ph, sx, sy, sw, sh;
    if (!gsResolveTextureHandle(gs, texHandle, &pw, &ph, &sx, &sy, &sw, &sh) || pw == 0 || ph == 0) return false;
    float divW = 1.0f / (float) pw;
    float divH = 1.0f / (float) ph;
    outUVs[0] = (float) sx * divW;              // left
    outUVs[1] = (float) sy * divH;              // top
    outUVs[2] = (float) (sx + sw) * divW;       // right
    outUVs[3] = (float) (sy + sh) * divH;       // bottom
    return true;
}

static void gsTextureSetStage(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t slot, MAYBE_UNUSED uint32_t texHandle) {
}

static void gsGpuSetShader(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex) {}
static void gsGpuResetShader(MAYBE_UNUSED Renderer* renderer) {}
static int32_t gsShaderGetUniform(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char* uniform) { return -1; }
static int32_t gsShaderGetSamplerIndex(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char* uniform) { return -1; }
static void gsShaderSetUniformF(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count, MAYBE_UNUSED float value1, MAYBE_UNUSED float value2, MAYBE_UNUSED float value3, MAYBE_UNUSED float value4) {}
static void gsShaderSetUniformFArray(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED float* values, MAYBE_UNUSED uint32_t count) {}
static void gsShaderSetUniformI(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count, MAYBE_UNUSED int32_t value1, MAYBE_UNUSED int32_t value2, MAYBE_UNUSED int32_t value3, MAYBE_UNUSED int32_t value4) {}
static bool gsShaderIsCompiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shader) { return false; }
static bool gsShadersSupported(void) { return false; }

static RendererVtable gsVtable;

// ===[ Public API ]===

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal, int64_t eeAtlasCacheMiB) {
    GsRenderer* gs = (GsRenderer *)safeCalloc(1, sizeof(GsRenderer));
    gs->eeAtlasCacheBytes = eeAtlasCacheMiB;
    gs->base.vtable = &gsVtable;
    gsVtable.init = gsInit;
    gsVtable.destroy = gsDestroy;
    gsVtable.beginFrame = gsBeginFrame;
    gsVtable.endFrameInit = gsEndFrameInit;
    gsVtable.endFrameEnd = gsEndFrameEnd;
    gsVtable.beginView = gsBeginView;
    gsVtable.endView = gsEndView;
    gsVtable.applyProjection = gsApplyProjection;
    gsVtable.beginGUI = gsBeginGUI;
    gsVtable.setGuiProjection = gsSetGuiProjection;
    gsVtable.endGUI = gsEndGUI;
    gsVtable.drawSprite = gsDrawSprite;
    gsVtable.drawSpritePos = gsDrawSpritePos;
    gsVtable.drawSpritePart = gsDrawSpritePart;
    gsVtable.drawSpritePartColor = gsDrawSpritePartColor;
    gsVtable.drawRectangle = gsDrawRectangle;
    gsVtable.drawRectangleColor = gsDrawRectangleColor;
    gsVtable.drawLine = gsDrawLine;
    gsVtable.drawLineColor = gsDrawLineColor;
    gsVtable.primitiveBegin = gsPrimitiveBegin;
    gsVtable.primitiveBeginTexture = gsPrimitiveBeginTexture;
    gsVtable.primitiveEnd = gsPrimitiveEnd;
    gsVtable.drawVertex = gsDrawVertex;
    gsVtable.drawVertexBuffer = gsDrawVertexBuffer;
    gsVtable.drawText = gsDrawText;
    gsVtable.drawTextColor = gsDrawTextColor;
    gsVtable.drawTriangle = gsDrawTriangle;
    gsVtable.flush = gsFlush;
    gsVtable.clearScreen = gsClearScreen;
    gsVtable.createSpriteFromSurface = gsCreateSpriteFromSurface;
    gsVtable.deleteSprite = gsDeleteSprite;
    gsVtable.gpuGetBlendFactors = gsGpuGetBlendFactors;
    gsVtable.gpuGetBlendMode = gsGpuGetBlendMode;
    gsVtable.gpuSetBlendMode = gsGpuSetBlendMode;
    gsVtable.gpuSetBlendModeExt = gsGpuSetBlendModeExt;
    gsVtable.gpuSetBlendEnable = gsGpuSetBlendEnable;
    gsVtable.gpuGetBlendEnable = gsGpuGetBlendEnable;
    gsVtable.gpuSetAlphaTestEnable = gsGpuSetAlphaTestEnable;
    gsVtable.gpuGetAlphaTestEnable = gsGpuGetAlphaTestEnable;
    gsVtable.gpuSetAlphaTestRef = gsGpuSetAlphaTestRef;
    gsVtable.gpuSetColorWriteEnable = gsGpuSetColorWriteEnable;
    gsVtable.gpuGetColorWriteEnable = gsGpuGetColorWriteEnable;
    gsVtable.drawTile = gsDrawTile;
    gsVtable.drawSpriteTiled = gsDrawSpriteTiled;
    gsVtable.drawTiledPart = gsDrawTiledPart;
    gsVtable.createSurface = gsCreateSurface;
    gsVtable.surfaceExists = gsSurfaceExists;
    gsVtable.setRenderTarget = gsSetRenderTarget;
    gsVtable.ensureApplicationSurface = gsEnsureApplicationSurface;
    gsVtable.getSurfaceWidth = gsGetSurfaceWidth;
    gsVtable.getSurfaceHeight = gsGetSurfaceHeight;
    gsVtable.drawSurface = gsDrawSurface;
    gsVtable.drawSurfaceTiled = gsDrawSurfaceTiled;
    gsVtable.drawSurfaceColor = gsDrawSurfaceColor;
    gsVtable.surfaceResize = gsSurfaceResize;
    gsVtable.surfaceFree = gsSurfaceFree;
    gsVtable.surfaceCopy = gsSurfaceCopy;
    gsVtable.surfaceGetPixels = gsSurfaceGetPixels;
    gsVtable.spriteGetTexture = gsSpriteGetTexture;
    gsVtable.surfaceGetTexture = gsSurfaceGetTexture;
    gsVtable.textureGetTexelWidth = gsTextureGetTexelWidth;
    gsVtable.textureGetTexelHeight = gsTextureGetTexelHeight;
    gsVtable.textureGetUVs = gsTextureGetUVs;
    gsVtable.textureSetStage = gsTextureSetStage;
    gsVtable.gpuSetShader = gsGpuSetShader;
    gsVtable.gpuResetShader = gsGpuResetShader;
    gsVtable.shaderGetUniform = gsShaderGetUniform;
    gsVtable.shaderGetSamplerIndex = gsShaderGetSamplerIndex;
    gsVtable.shaderSetUniformF = gsShaderSetUniformF;
    gsVtable.shaderSetUniformFArray = gsShaderSetUniformFArray;
    gsVtable.shaderSetUniformI = gsShaderSetUniformI;
    gsVtable.shaderIsCompiled = gsShaderIsCompiled;
    gsVtable.shadersSupported = gsShadersSupported;
    gs->gsGlobal = gsGlobal;
    gs->scaleX = 2.0f;
    gs->scaleY = 2.0f;
    gs->currentSurface = -1; // main framebuffer
    return (Renderer*) gs;
}
