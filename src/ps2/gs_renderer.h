#pragma once

#include "common.h"
#include "renderer.h"
#include <gsKit.h>
#include "stb_ds.h"

// ===[ Atlas Entry (from ATLAS.BIN TPAG entries) ]===
typedef struct {
    uint16_t atlasId;   // TEX atlas index (0xFFFF = not mapped)
    uint16_t atlasX;    // X offset within the atlas
    uint16_t atlasY;    // Y offset within the atlas
    uint16_t width;     // Image width in the atlas (post-crop, post-resize)
    uint16_t height;    // Image height in the atlas (post-crop, post-resize)
    uint16_t cropX;     // X offset of cropped content within original bounding box
    uint16_t cropY;     // Y offset of cropped content within original bounding box
    uint16_t cropW;     // Pre-resize width of the cropped content
    uint16_t cropH;     // Pre-resize height of the cropped content
    uint16_t clutIndex; // CLUT index within the corresponding CLUT file
    uint8_t bpp;        // 4 or 8
} AtlasTPAGEntry;

// ===[ Atlas Tile Entry (from ATLAS.BIN tile entries) ]===
typedef struct {
    int16_t bgDef;      // Background definition index
    uint16_t srcX;      // Source X in the original background image
    uint16_t srcY;      // Source Y in the original background image
    uint16_t srcW;      // Original tile width in pixels
    uint16_t srcH;      // Original tile height in pixels
    uint16_t atlasId;   // TEX atlas index (0xFFFF = not mapped)
    uint16_t atlasX;    // X offset within the atlas
    uint16_t atlasY;    // Y offset within the atlas
    uint16_t width;     // Tile width in the atlas (post-crop, post-resize)
    uint16_t height;    // Tile height in the atlas (post-crop, post-resize)
    uint16_t cropX;     // X offset of cropped content within original tile
    uint16_t cropY;     // Y offset of cropped content within original tile
    uint16_t cropW;     // Pre-resize width of the cropped content
    uint16_t cropH;     // Pre-resize height of the cropped content
    uint16_t clutIndex; // CLUT index within the corresponding CLUT file
    uint8_t bpp;        // 4 or 8
} AtlasTileEntry;

// ===[ Tile Lookup Key (for O(1) hashmap lookup) ]===
typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
} TileLookupKey;

// stb_ds hashmap entry: TileLookupKey -> AtlasTileEntry*
typedef struct {
    TileLookupKey key;
    AtlasTileEntry* value;
} TileEntryMap;

// ===[ VRAM Chunk (buddy system unit) ]===
// Each chunk is 128KB of VRAM (fits one 4bpp 512x512 atlas).
// An 8bpp atlas uses 2 consecutive chunks.
#define VRAM_CHUNK_SIZE 131072 // 128KB = gsKit_texture_size(512, 512, GS_PSM_T4)

typedef struct {
    int16_t atlasId;    // Which atlas occupies this chunk (-1 = free)
    uint64_t lastUsed;  // Frame number when last accessed
} VRAMChunk;

// ===[ EE RAM Atlas Cache Entry ]===
// Caches uncompressed atlas pixel data in EE RAM for zero-copy VRAM uploads to avoid repeated CDVD reads and decompression
typedef struct {
    int16_t atlasId;    // Which atlas (-1 = free)
    uint32_t offset;    // Byte offset within eeCache buffer (128-byte aligned)
    uint32_t size;      // Total bytes stored (uncompressed indexed pixels: 128KB for 4bpp, 256KB for 8bpp)
    uint64_t lastUsed;  // Frame counter for LRU
} EeAtlasCacheEntry;

// ===[ GsRenderer Struct ]===
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GSGLOBAL* gsGlobal;

    // View transform state
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    int32_t viewX;
    int32_t viewY;

    // ATLAS.BIN data
    uint16_t atlasTPAGCount;
    uint16_t atlasTileCount;
    AtlasTPAGEntry* atlasTPAGEntries;
    AtlasTileEntry* atlasTileEntries;
    TileEntryMap* tileEntryMap; // stb_ds hashmap: (bgDef, srcX, srcY, srcW, srcH) -> AtlasTileEntry*

    // CLUT VRAM addresses (one per CLUT, individually uploaded)
    uint32_t clut4Count;       // Number of 4bpp CLUTs
    uint32_t* clut4VramAddrs;  // Per-CLUT VRAM addresses [clut4Count]

    uint32_t clut8Count;       // Number of 8bpp CLUTs
    uint32_t* clut8VramAddrs;  // Per-CLUT VRAM addresses [clut8Count]

    // TEXTURES.BIN file handle (kept open for on-demand atlas loading)
    FILE* texturesFile;
    uint32_t* atlasOffsets;    // Byte offset of each atlas within TEXTURES.BIN [atlasCount]

    // VRAM texture cache (buddy system with LRU eviction)
    uint32_t textureVramBase;  // Start of texture region in VRAM (after framebuffers + CLUTs)
    uint32_t chunkCount;       // Number of 128KB chunks available
    VRAMChunk* chunks;         // Per-chunk state [chunkCount]
    int16_t* atlasToChunk;     // atlasId -> first chunk index (-1 = not loaded) [atlasCount]
    uint16_t atlasCount;       // Number of atlas IDs from ATLAS.BIN header
    uint8_t* atlasBpp;         // Bits per pixel per atlas (4 or 8), from ATLAS.BIN [atlasCount]
    uint64_t frameCounter;     // Incremented each frame for LRU tracking
    bool evictedAtlasUsedInCurrentFrame; // Used for debugging, true if a atlas that was used on the current frame was evicted (VRAM thrashing)
    uint16_t uniqueAtlasesThisFrame;     // Number of distinct atlases touched this frame
    uint16_t chunksNeededThisFrame;      // Total VRAM chunks needed by all atlases touched this frame
    uint16_t diskLoadsThisFrame;         // Number of atlas loads from TEXTURES.BIN this frame (EE cache misses)

    // EE RAM atlas cache (stores uncompressed atlas pixel data for zero-copy VRAM uploads)
    uint8_t* eeCache;                  // Contiguous buffer with uncompressed texture data
    uint32_t eeCacheCapacity;          // Total size (See EE_CACHE_CAPACITY)
    uint32_t eeCacheBumpPtr;           // End of live data
    EeAtlasCacheEntry* eeCacheEntries; // Per-atlas cache state [atlasCount]
    uint32_t* atlasDataSizes;          // On-disk size per atlas (header + compressed data) [atlasCount]
} GsRenderer;

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal);
