#ifndef _BS_GS_RENDERER_H_
#define _BS_GS_RENDERER_H_

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
// Each chunk is 128KB of VRAM (fits one 4bpp 256x256 atlas).
// An 8bpp atlas uses 2 consecutive chunks.
#define VRAM_CHUNK_SIZE 32768 // 32KB = gsKit_texture_size(256, 256, GS_PSM_T4)

typedef struct {
    int16_t atlasId;     // Atlas occupying this chunk (-1 = not an atlas chunk). Mutually exclusive with snapshotIdx and surfaceIdx.
    int16_t snapshotIdx; // Snapshot occupying this chunk (-1 = not a snapshot chunk). Pinned: LRU eviction skips these.
    int16_t surfaceIdx;  // GML surface occupying this chunk (-1 = not a surface chunk). Pinned the same way snapshots are.
    uint64_t lastUsed;   // Frame number when last accessed (used by atlas LRU; ignored for snapshot/surface chunks)
} VRAMChunk;

// ===[ EE RAM Atlas Cache Entry ]===
// Caches uncompressed atlas pixel data in EE RAM for zero-copy VRAM uploads to avoid repeated CDVD reads and decompression
typedef struct {
    int16_t atlasId;    // Which atlas (-1 = free)
    uint32_t offset;    // Byte offset within eeCache buffer (128-byte aligned)
    uint32_t size;      // Total bytes stored (uncompressed indexed pixels: 128KB for 4bpp, 256KB for 8bpp)
    uint64_t lastUsed;  // Frame counter for LRU
} EeAtlasCacheEntry;

// ===[ Snapshot Chunk ]===
typedef struct {
    uint16_t firstChunk;  // Index of first chunk in the consecutive run
    uint16_t chunkCount;  // Number of consecutive chunks occupied
    uint16_t width;       // Snapshot width in pixels (fb-space)
    uint16_t height;      // Snapshot height in pixels (fb-space)
    uint16_t tbw;         // Buffer width in 64-pixel blocks (matches GS BITBLTBUF DBW)
    bool inUse;           // Table row owned by a live sprite. When false, chunks have already been released back to the atlas pool; the row index is held only so tpagToSnapshot pointers stay stable until the sprite slot is fully reaped.
} SnapshotChunk;

// ===[ GML Surface ]===
// Backs surface_create / surface_set_target / draw_surface. Lives in a CT16 region of VRAM pinned via VRAMChunk.surfaceIdx.
typedef struct {
    uint16_t firstChunk;   // Index of first chunk in the consecutive run
    uint16_t chunkCount;   // Number of consecutive chunks occupied
    uint16_t width;        // Logical width in pixels (what GML asked for)
    uint16_t height;       // Logical height in pixels
    uint16_t tbw;          // Buffer width in 64-pixel blocks (= ceil(width/64)). Padded width is tbw * 64.
    bool inUse;            // false = freed row, kept so the table doesn't shrink
} Surface;

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} GsPrimitiveVertex;

// ===[ GsRenderer Struct ]===
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GSGLOBAL* gsGlobal;

    GsPrimitiveVertex* primitiveVertices;
    int32_t primitiveVertexCount;
    int32_t primitiveCapacity;
    int32_t primitiveType;
    uint32_t primitiveTextureId;
    bool primitiveHasTexture;

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
    uint32_t* atlasOffsets;    // Byte offset of each atlas's pixel data within TEXTURES.BIN [atlasCount]
    uint8_t* atlasCompressionType; // Per-atlas compression: 0 = raw, 1 = RLE [atlasCount]

    // VRAM texture cache (buddy system with LRU eviction)
    uint32_t textureVramBase;  // Start of texture region in VRAM (after framebuffers + CLUTs)
    uint32_t chunkCount;       // Number of 128KB chunks available
    uint32_t reservedAtlasChunks; // First N chunks are atlas-only to avoid starving all chunks with pinned chunks.
    VRAMChunk* chunks;         // Per-chunk state [chunkCount]
    int16_t* atlasToChunk;     // atlasId -> first chunk index (-1 = not loaded) [atlasCount]
    uint16_t atlasCount;       // Number of atlas IDs from ATLAS.BIN header
    uint8_t* atlasBpp;         // Bits per pixel per atlas (4 or 8), from ATLAS.BIN [atlasCount]
    uint16_t* atlasWidth;       // Width per atlas
    uint16_t* atlasHeight;      // Height per atlas
    uint64_t frameCounter;     // Incremented each frame for LRU tracking
    bool evictedAtlasUsedInCurrentFrame; // Used for debugging, true if a atlas that was used on the current frame was evicted (VRAM thrashing)
    uint16_t uniqueAtlasesThisFrame;     // Number of distinct atlases touched this frame
    uint16_t chunksNeededThisFrame;      // Total VRAM chunks needed by all atlases touched this frame
    uint16_t ramLoadsThisFrame;          // Number of atlas loads from RAM this frame (VRAM cache misses)
    uint16_t diskLoadsThisFrame;         // Number of atlas loads from TEXTURES.BIN this frame (EE cache misses)

    // EE RAM atlas cache (stores uncompressed atlas pixel data for zero-copy VRAM uploads)
    uint64_t eeAtlasCacheBytes;
    uint8_t* eeCache;                  // Contiguous buffer with uncompressed texture data
    uint32_t eeCacheCapacity;          // Total size (See EE_CACHE_CAPACITY)
    uint32_t eeCacheBumpPtr;           // End of live data
    EeAtlasCacheEntry* eeCacheEntries; // Per-atlas cache state [atlasCount]
    uint32_t* atlasDataSizes;          // On-disk pixel data size per atlas (post-compression) [atlasCount]

    // GPU state (mirrors what was last sent to GS so we can re-apply after sync_flip clobbers FRAME)
    uint32_t fbmsk;          // Current FRAME register FBMSK (0 = all channels writable)
    uint8_t fba;             // Current FBA_1 register value (1 = force FB.A bit to 1 on writeback, 0 = pass through)
    bool blendModeWarned;    // Set the first time an unsupported blend factor pair is seen

    // gsKit packs PrimAlphaEnable into BOTH the PRIM.ABE bit AND TEX0.TCC.
    // So toggling ABE off also forces TCC=0, which makes the GS ignore the texture's per-pixel alpha and pull alpha from TA0 (default 0x00) instead.
    // That breaks textured alpha-mask sprites.
    // To keep TCC=1 always, we leave PrimAlphaEnable=ON and emulate blend-disable by switching ALPHA to an identity equation (Cs passes through unchanged).
    bool blendEnabled;       // What the GML last requested via gpu_set_blendenable
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA; // What the GML last requested via gpu_set_colorwriteenable
    u64 currentBlendAlpha;   // The ALPHA register value the GML last requested via gpu_set_blendmode[_ext]

    // Snapshot table for sprite_create_from_surface(application_surface, ...). VRAM lives in the shared chunk pool, pinned via VRAMChunk.snapshotIdx.
    SnapshotChunk* snapshotChunks; // stb_ds dynamic array of snapshot table rows
    uint32_t originalTpagCount;    // dw->tpag.count at init time; slots >= this are dynamic and ours to free
    uint32_t originalSpriteCount;  // dw->sprt.count at init time
    int32_t* tpagToSnapshot;       // stb_ds dynamic array indexed by tpagIndex; -1 = not a snapshot, otherwise index into snapshotChunks

    // GML surface table. Each row owns a CT16 region in the chunk pool, pinned via VRAMChunk.surfaceIdx.
    Surface* surfaces;             // stb_ds dynamic array of surface rows
    int32_t currentSurface;        // -1 = main framebuffer, otherwise index into surfaces[]

    // Saved framebuffer state captured on the first push into a surface. Used to restore the screen FRAME on surface_reset_target.
    uint32_t savedScreenBufferAddr; // gsGlobal->ScreenBuffer[ActiveBuffer] before the switch
    uint16_t savedFbWidth;
    uint16_t savedFbHeight;
    uint8_t  savedFbPSM;
    uint8_t  savedAte;              // Test->ATE at the moment of push (so we can restore the GML's last alpha-test enable choice on pop).
    uint8_t  savedFba;              // gs->fba at the moment of push (main FB normally has FBA=1; surface flips it based on blend state).
    uint32_t savedFbmsk;            // FBMSK at the moment of push (so a phantom-surface set_target can mask all writes and restore on pop).

    // Saved view transform captured on push, restored on pop, so the GML view scale/offset doesn't bleed into surface-local draws.
    float    savedScaleX;
    float    savedScaleY;
    float    savedOffsetX;
    float    savedOffsetY;
    int32_t  savedViewX;
    int32_t  savedViewY;

    // Blending mode + factors
    int32_t  currentBlendMode;
    int32_t  currentSFactor;
    int32_t  currentDFactor;
    int32_t  currentSFactorAlpha;
    int32_t  currentDFactorAlpha;
} GsRenderer;

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal, int64_t eeAtlasCacheMiB);

#endif /* _BS_GS_RENDERER_H_ */
