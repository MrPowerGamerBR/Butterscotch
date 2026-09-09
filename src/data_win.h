#ifndef _BS_DATA_WIN_H_
#define _BS_DATA_WIN_H_

#include "common.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "stdio_compat.h"
#include "string_compat.h"

#include "utils.h"

// Forward declaration for progress callback
typedef struct DataWin DataWin;

typedef enum {
    DATAWINLOADTYPE_LOAD_PER_CHUNK,
    DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME,
    DATAWINLOADTYPE_MAP_FILE
} DataWinLoadType;

typedef struct {
    bool parseGen8;
    bool parseOptn;
    bool parseLang;
    bool parseExtn;
    bool parseSond;
    bool parseAgrp;
    bool parseSprt;
    bool parseBgnd;
    bool parsePath;
    bool parseScpt;
    bool parseGlob;
    bool parseShdr;
    bool parseFont;
    bool parseTmln;
    bool parseObjt;
    bool parseRoom;
    bool parseTpag;
    bool parseCode;
    bool parseVari;
    bool parseFunc;
    bool parseStrg;
    bool parseTxtr;
    bool parseAudo;
    bool suppressUnknownChunkLogs;
    // If true, precise masks will be skipped when the sprite does not have a precise state set
    bool skipLoadingPreciseMasksForNonPreciseSprites;

    // If true, Room payloads (backgrounds, views, gameObjects, tiles, layers) are parsed on demand via DataWin_loadRoomPayload during gameplay.
    bool lazyLoadRooms;
    // If true, TXTR objects will be loaded on demand via DataWin_loadTxtrIfNeeded, and unloaded if memory is tight.
    bool lazyLoadTextures;
    // If true, AUDO objects will be loaded on demand via DataWin_loadAudoIfNeeded.
    bool lazyLoadAudio;

    // When lazyLoadRooms is true, this list indicates which rooms should be loaded during load time instead of demand. They will also not be freed.
    StringBooleanEntry* eagerlyLoadedRooms;

    DataWinLoadType loadType;

    // Optional progress callback, called before each chunk is parsed.
    // chunkName: 4-character chunk name (e.g. "GEN8", "SPRT")
    // chunkIndex: 0-based index of the current chunk being parsed
    // totalChunks: total number of chunks in the file
    // dataWin: the DataWin being populated (earlier chunks may already be parsed)
    // userData: user-provided pointer passed through from the options
    void (*progressCallback)(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dataWin, void* userData);
    void* progressCallbackUserData;
} DataWinParserOptions;

// ===[ GEN8 - General Info ]===
typedef struct {
    uint8_t isDebuggerDisabled;
    uint8_t wadVersion;
    const char* fileName;
    const char* config;
    uint32_t lastObj;
    uint32_t lastTile;
    uint32_t gameID;
    uint8_t directPlayGuid[16];
    const char* name;
    uint32_t major;
    uint32_t minor;
    uint32_t release;
    uint32_t build;
    uint32_t defaultWindowWidth;
    uint32_t defaultWindowHeight;
    uint32_t info;
    uint32_t licenseCRC32;
    uint8_t licenseMD5[16];
    uint64_t timestamp;
    const char* displayName;
    uint64_t activeTargets;
    uint64_t functionClassifications;
    int32_t steamAppID;
    uint32_t debuggerPort;
    uint32_t roomOrderCount;
    int32_t* roomOrder;
    float gms2FPS;
} Gen8;

// ===[ OPTN - Options ]===
typedef struct {
    const char* name;
    const char* value;
} OptnConstant;

typedef struct {
    uint64_t info;
    int32_t scale;
    uint32_t windowColor;
    uint32_t colorDepth;
    uint32_t resolution;
    uint32_t frequency;
    uint32_t vertexSync;
    uint32_t priority;
    uint32_t backImage;
    uint32_t frontImage;
    uint32_t loadImage;
    uint32_t loadAlpha;
    uint32_t constantCount;
    OptnConstant* constants;
} Optn;

// ===[ LANG - Languages ]===
typedef struct {
    const char* name;
    const char* region;
    uint32_t entryCount;
    const char** entries;
} Language;

typedef struct {
    uint32_t unknown1;
    uint32_t languageCount;
    uint32_t entryCount;
    const char** entryIds;
    Language* languages;
} Lang;

// ===[ EXTN - Extensions ]===
typedef struct {
    const char* name;
    uint32_t id;
    uint32_t kind;
    uint32_t retType;
    const char* extName;
    uint32_t argumentCount;
    uint32_t* arguments;
} ExtensionFunction;

typedef struct {
    const char* filename;
    const char* cleanupScript;
    const char* initScript;
    uint32_t kind;
    uint32_t functionCount;
    ExtensionFunction* functions;
} ExtensionFile;

typedef struct {
    const char* folderName;
    const char* name;
    const char* className;
    uint32_t fileCount;
    ExtensionFile* files;
} Extension;

typedef struct {
    uint32_t count;
    Extension* extensions;
} Extn;

// The "present" field can be false for deleted/null pointer-table slots on asset structs below
// This will MOST LIKELY ALWAYS be true on pre-2024.11+ games, but CAN be false in 2022.11+ games if the asset was deleted

// ===[ SOND - Sounds ]===
typedef enum {
    AUDIO_ENTRY_FLAG_IS_EMBEDDED = 0x01, // Sound data is embedded
    AUDIO_ENTRY_FLAG_IS_COMPRESSED = 0x02, // Sound data is compressed
    AUDIO_ENTRY_FLAG_IS_DECOMPRESSED_ON_LOAD = 0x03, // Decompress when loading
    AUDIO_ENTRY_FLAG_REGULAR = 0x64  // Uses new audio system
} AudioEntryFlags;

typedef struct {
    bool present;
    const char* name;
    uint32_t flags;
    const char* type;
    const char* file;
    uint32_t effects;
    float volume;
    float pitch;
    float pan; // -1.0 = full left, 0.0 = center, +1.0 = full right. Legacy field that is not used in WAD11+.
    int32_t audioGroup;
    int32_t audioFile;
} Sound;

typedef struct {
    uint32_t count;
    Sound* sounds;
} Sond;

// ===[ AGRP - Audio Groups ]===
typedef struct {
    bool present;
    const char* name;
    const char* path; // nullptr for pre-GM 2024.14+ games
} AudioGroup;

typedef struct {
    uint32_t count;
    AudioGroup* audioGroups;
} Agrp;

// ===[ SPRT - Sprites ]===
typedef struct {
    bool present;
    const char* name;
    uint32_t width;
    uint32_t height;
    int32_t marginLeft;
    int32_t marginRight;
    int32_t marginBottom;
    int32_t marginTop;
    bool transparent;
    bool smooth;
    bool preload;
    uint32_t bboxMode;
    uint32_t sepMasks;
    int32_t originX;
    int32_t originY;
    uint32_t sVersion;
    uint32_t sSpriteType;
    float gms2PlaybackSpeed;
    bool gms2PlaybackSpeedType;
    bool specialType;
    uint32_t textureCount;
    int32_t* tpagIndices;    // resolved TPAG indices (one per frame); -1 for unresolved
    uint32_t maskCount;       // number of collision masks (one per frame, or 0)
    uint8_t** masks;          // array of maskCount packed bit arrays (nullptr if none)
    // Collision mask storage dimensions. Pre-2024.6 these equal the full sprite width/height with zero offset.
    // GMS 2024.6+ stores masks at bounding-box dimensions, so the mask covers only [maskOffsetX, maskOffsetX+maskWidth).
    uint32_t maskWidth;
    uint32_t maskHeight;
    int32_t maskOffsetX;      // sprite-local X of the mask's left edge (marginLeft on 2024.6+, else 0)
    int32_t maskOffsetY;      // sprite-local Y of the mask's top edge (marginTop on 2024.6+, else 0)
    // Nine-slice (GMS2 sVersion >= 3). Present iff the sprite stored a non-zero nineSliceOffset.
    bool nineSliceEnabled;
    int32_t nsLeft;
    int32_t nsTop;
    int32_t nsRight;
    int32_t nsBottom;
    uint8_t nsTileModes[5];   // order: Left, Top, Right, Bottom, Center. 0=Stretch, 1=Repeat, 2=Mirror, 3=BlankRepeat, 4=Hide
} Sprite;

typedef struct {
    uint32_t count;
    uint32_t parsedCount; // number of sprites loaded from SPRT; slots >= parsedCount are runtime-allocated and own their `name`
    Sprite* sprites;
} Sprt;

// ===[ BGND - Backgrounds ]===
typedef struct {
    bool present;
    const char* name;
    bool transparent;
    bool smooth;
    bool preload;
    int32_t tpagIndex;      // resolved TPAG index, -1 if unresolved
    uint32_t gms2UnknownAlways2;
    uint32_t gms2TileWidth;
    uint32_t gms2TileHeight;
    uint32_t gms2TileSeparationX;
    uint32_t gms2TileSeparationY;
    uint32_t gms2OutputBorderX;
    uint32_t gms2OutputBorderY;
    uint32_t gms2TileColumns;
    uint32_t gms2ItemsPerTileCount;
    uint32_t gms2TileCount;
    int gms2ExportedSpriteIndex;
    int64_t gms2FrameLength;
    uint32_t *gms2TileIds;
} Background;

typedef struct {
    uint32_t count;
    Background* backgrounds;
} Bgnd;

// ===[ PATH - Paths ]===
typedef struct {
    float x;
    float y;
    float speed;
} PathPoint;

typedef struct {
    float x;
    float y;
    float speed;
    float l; // cumulative arc length from start
} InternalPathPoint;

typedef struct {
    float x;
    float y;
    float speed;
} PathPositionResult;

typedef struct {
    bool present;
    const char* name;
    bool isSmooth;
    bool isClosed;
    uint32_t precision;
    uint32_t pointCount;
    PathPoint* points;
    uint32_t internalPointCount;
    InternalPathPoint* internalPoints;
    float length; // total arc length
    bool exists;
} GamePath;

typedef struct {
    uint32_t count;
    GamePath* paths;
} PathChunk;

// ===[ SCPT - Scripts ]===
typedef struct {
    bool present;
    const char* name;
    int32_t codeId;
} Script;

typedef struct {
    uint32_t count;
    Script* scripts;
} Scpt;

// ===[ GLOB - Global Init Scripts ]===
typedef struct {
    uint32_t count;
    int32_t* codeIds;
} Glob;

// ===[ SHDR - Shaders ]===
typedef struct {
    bool present;
    const char* name;
    uint32_t type;
    const char* glslES_Vertex;
    const char* glslES_Fragment;
    const char* glsl_Vertex;
    const char* glsl_Fragment;
    const char* hlsl9_Vertex;
    const char* hlsl9_Fragment;
    uint32_t hlsl11_VertexOffset;
    uint32_t hlsl11_PixelOffset;
    uint32_t vertexAttributeCount;
    const char** vertexAttributes;
    int32_t version;
    uint32_t pssl_VertexOffset;
    uint32_t pssl_VertexLen;
    uint32_t pssl_PixelOffset;
    uint32_t pssl_PixelLen;
    uint32_t cgVita_VertexOffset;
    uint32_t cgVita_VertexLen;
    uint32_t cgVita_PixelOffset;
    uint32_t cgVita_PixelLen;
    uint32_t cgPS3_VertexOffset;
    uint32_t cgPS3_VertexLen;
    uint32_t cgPS3_PixelOffset;
    uint32_t cgPS3_PixelLen;
} Shader;

typedef struct {
    uint32_t count;
    Shader* shaders;
} Shdr;

// ===[ FONT - Fonts ]===
typedef struct {
    int16_t character;
    int16_t shiftModifier;
} KerningPair;

typedef struct {
    uint16_t character;
    uint16_t sourceX;
    uint16_t sourceY;
    uint16_t sourceWidth;
    uint16_t sourceHeight;
    int16_t shift;
    int16_t offset;
    uint16_t kerningCount;
    KerningPair* kerning;
} FontGlyph;

typedef struct {
    bool present;
    const char* name;
    const char* displayName;
    float emSize;
    bool bold;
    bool italic;
    uint16_t rangeStart;
    uint8_t charset;
    uint8_t antiAliasing;
    uint32_t rangeEnd;
    int32_t tpagIndex;      // resolved TPAG index, -1 if unresolved
    float scaleX;
    float scaleY;
    int32_t ascenderOffset; // wadVersion >= 17 only
    uint32_t ascender;  // GMS 2022.2+ (0 when absent)
    uint32_t sdfSpread; // GMS 2023.2 nonLTS+ (0 when absent)
    uint32_t lineHeight; // GMS 2023.6+ (0 when absent)
    bool hasAscender;
    bool hasSDFSpread;
    bool hasLineHeight;
    uint32_t glyphCount;
    FontGlyph* glyphs;
    uint32_t maxGlyphHeight; // Computed after glyph parse: max sourceHeight across glyphs; HTML5 runner uses this for line stride (see yyFont.TextHeight)
    // ASCII fast-path lookup: glyphLUT[ch] for ch < 128, populated by Font_buildGlyphLUT after glyphs[] is filled.
    // Lets TextUtils_findGlyph skip the linear scan over glyphs[] for the (overwhelmingly common) ASCII case.
    FontGlyph* glyphLUT[128];
    // Sprite font fields (only valid when isSpriteFont is true)
    bool isSpriteFont;
    int32_t spriteIndex; // source sprite index (-1 for regular fonts)
    // Amount to subtract from each glyph's Y at draw time, ONLY used for sprite fonts.
    int16_t spriteOriginYAdjust;
} Font;

// Builds the ASCII fast-path lookup table from font->glyphs. Call after glyphs[] is fully populated.
static inline void Font_buildGlyphLUT(Font* font) {
    memset(font->glyphLUT, 0, sizeof(font->glyphLUT));
    repeat(font->glyphCount, i) {
        FontGlyph* g = &font->glyphs[i];
        if (128 > g->character && font->glyphLUT[g->character] == nullptr) {
            font->glyphLUT[g->character] = g;
        }
    }
}

typedef struct {
    uint32_t count;
    Font* fonts;
} FontChunk;

// ===[ EventAction (shared by TMLN and OBJT) ]===
typedef struct {
    uint32_t libID;
    uint32_t id;
    uint32_t kind;
    bool useRelative;
    bool isQuestion;
    bool useApplyTo;
    uint32_t exeType;
    const char* actionName;
    int32_t codeId;
    uint32_t argumentCount;
    int32_t who;
    bool relative;
    bool isNot;
    uint32_t unknownAlwaysZero;
} EventAction;

// ===[ TMLN - Timelines ]===
typedef struct {
    uint32_t step;
    uint32_t actionCount;
    EventAction* actions;
} TimelineMoment;

typedef struct {
    bool present;
    const char* name;
    uint32_t momentCount;
    TimelineMoment* moments;
} Timeline;

typedef struct {
    uint32_t count;
    Timeline* timelines;
} Tmln;

// ===[ OBJT - Game Objects ]===
#define OBJT_EVENT_TYPE_COUNT 15

typedef struct {
    uint32_t eventSubtype;
    uint32_t actionCount;
    EventAction* actions;
} ObjectEvent;

typedef struct {
    uint32_t eventCount;
    ObjectEvent* events;
} ObjectEventList;

typedef struct {
    float x;
    float y;
} PhysicsVertex;

typedef struct {
    bool present;
    const char* name;
    int32_t spriteId;
    bool visible;
    bool managed; // GMS 2022.5+
    bool solid;
    int32_t depth;
    bool persistent;
    int32_t parentId;
    int32_t textureMaskId;
    bool usesPhysics;
    bool isSensor;
    uint32_t collisionShape;
    float density;
    float restitution;
    uint32_t group;
    float linearDamping;
    float angularDamping;
    int32_t physicsVertexCount;
    float friction;
    bool awake;
    bool kinematic;
    PhysicsVertex* physicsVertices;
    ObjectEventList eventLists[OBJT_EVENT_TYPE_COUNT];
} GameObject;

typedef struct {
    uint32_t count;
    GameObject* objects;
} Objt;

// ===[ ROOM - Rooms ]===
typedef struct {
    bool enabled;
    bool foreground;
    int32_t backgroundDefinition;
    int32_t x;
    int32_t y;
    int32_t tileX;
    int32_t tileY;
    int32_t speedX;
    int32_t speedY;
    bool stretch;
} RoomBackground;

typedef struct {
    bool enabled;
    int32_t viewX;
    int32_t viewY;
    int32_t viewWidth;
    int32_t viewHeight;
    int32_t portX;
    int32_t portY;
    int32_t portWidth;
    int32_t portHeight;
    uint32_t borderX;
    uint32_t borderY;
    int32_t speedX;
    int32_t speedY;
    int32_t objectId;
} RoomView;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t objectDefinition;
    uint32_t instanceID;
    int32_t creationCode;
    float scaleX;
    float scaleY;
    float imageSpeed; // GMS >= 2.2.2.302 only, otherwise 0.0f
    int32_t imageIndex; // GMS >= 2.2.2.302 only, otherwise 0
    uint32_t color;
    float rotation;
    int32_t preCreateCode;
} RoomGameObject;

typedef struct {
    int32_t x;
    int32_t y;
    bool useSpriteDefinition;
    int32_t backgroundDefinition;
    int32_t sourceX;
    int32_t sourceY;
    uint32_t width;
    uint32_t height;
    int32_t tileDepth;
    uint32_t instanceID;
    float scaleX;
    float scaleY;
    uint32_t color;
    float alpha;
} RoomTile;

typedef enum 
{
    RoomLayerType_Path = 0,
    RoomLayerType_Background = 1,
    RoomLayerType_Instances = 2,
    RoomLayerType_Assets = 3,
    RoomLayerType_Tiles = 4,
    RoomLayerType_Effect = 6,
    RoomLayerType_Path2 = 7
} RoomLayerType;

typedef struct {
    const char* name;
    int32_t spriteIndex; // Direct index into SPRT chunk
    int32_t x;
    int32_t y;
    float scaleX;
    float scaleY;
    uint32_t color;
    float animationSpeed;
    uint32_t animationSpeedType;
    float frameIndex;
    float rotation;
} SpriteInstance;

typedef struct {
    uint32_t legacyTileCount;
    RoomTile *legacyTiles;
    uint32_t spriteCount;
    SpriteInstance *sprites;
} RoomLayerAssetsData;

typedef struct {
    bool visible;
    bool foreground;
    int32_t spriteIndex; // into SPRT (-1 = none)
    bool hTiled;
    bool vTiled;
    bool stretch;
    uint32_t color;
    float firstFrame;
    float animSpeed;
    uint32_t animSpeedType;
    int32_t imageIndex;
} RoomLayerBackgroundData;

typedef struct {
    uint32_t instanceCount;
    uint32_t* instanceIds;
} RoomLayerInstancesData;

typedef struct {
    int32_t backgroundIndex; // tileset (BGND index)
    uint32_t tilesX; // grid width in tiles
    uint32_t tilesY; // grid height in tiles
    uint32_t* tileData; // flat array of tilesX * tilesY tile values (row-major)
} RoomLayerTilesData;

typedef struct {
    const char* name;
    uint32_t id;
    uint32_t type;
    int32_t depth;
    float xOffset;
    float yOffset;
    float hSpeed;
    float vSpeed;
    bool visible;
    RoomLayerAssetsData *assetsData;
    RoomLayerBackgroundData *backgroundData;
    RoomLayerInstancesData *instancesData;
    RoomLayerTilesData *tilesData;
} RoomLayer;

typedef struct {
    bool present;
    // Scalar header: always valid regardless of payloadLoaded.
    const char* name;
    const char* caption;
    uint32_t width;
    uint32_t height;
    uint32_t speed;
    bool persistent;
    uint32_t backgroundColor;
    bool drawBackgroundColor;
    int32_t creationCodeId;
    uint32_t flags;
    bool world;
    uint32_t top;
    uint32_t left;
    uint32_t right;
    uint32_t bottom;
    float gravityX;
    float gravityY;
    float metersPerPixel;

    // Lazy-load offsets: absolute file offsets to the PointerList head for each payload section.
    // Captured during the header pass of parseROOM so DataWin_loadRoomPayload can seek directly.
    uint32_t backgroundsFileOffset;
    uint32_t viewsFileOffset;
    uint32_t gameObjectsFileOffset;
    uint32_t tilesFileOffset;
    uint32_t layersFileOffset; // 0 if pre-GMS2
    bool payloadLoaded;
    bool eagerlyLoaded; // set if this room's name matched DataWinParserOptions.eagerlyLoadedRooms; payload is preserved across transitions

    // Payload: valid only when payloadLoaded is true. Zeroed/null otherwise. Backgrounds/views point to a heap array of 8 entries when loaded.
    RoomBackground* backgrounds;
    RoomView* views;
    uint32_t gameObjectCount;
    RoomGameObject* gameObjects;
    uint32_t tileCount;
    RoomTile* tiles;
    uint32_t layerCount;
    RoomLayer* layers;
} Room;

typedef struct {
    uint32_t count;
    Room* rooms;
} RoomChunk;

// ===[ ACRV - Animation Curves ]===
typedef enum {
    ANIMCURVE_TYPE_LINEAR = 0,
    ANIMCURVE_TYPE_SMOOTH = 1,
    ANIMCURVE_TYPE_BEZIER = 2,
} AnimCurveType;

typedef struct {
    float x;        // position along curve, normally in [0, 1]
    float value;    // output value at this point
    // Only meaningful when the channel uses ANIMCURVE_TYPE_BEZIER (GMS 2.3.1+ format).
    float bezierX0, bezierY0, bezierX1, bezierY1;
} AnimCurvePoint;

typedef struct {
    const char* name;
    AnimCurveType curveType;
    uint32_t iterations;
    uint32_t pointCount;
    AnimCurvePoint* points;
    int32_t globalId;   // index into Acrv.allChannels
} AnimCurveChannel;

typedef struct {
    bool present;
    const char* name;
    uint32_t graphType;
    uint32_t channelCount;
    AnimCurveChannel* channels;
} AnimCurve;

typedef struct {
    uint32_t count;
    AnimCurve* curves;
    // Flat global table of channel pointers, used as the handle returned by animcurve_get_channel.
    // animcurve_channel_evaluate uses this to resolve the int handle back to a channel.
    uint32_t allChannelsCount;
    AnimCurveChannel** allChannels;
} Acrv;

// ===[ TPAG - Texture Page Items ]===
typedef struct {
    bool present;
    uint16_t sourceX;
    uint16_t sourceY;
    uint16_t sourceWidth;
    uint16_t sourceHeight;
    uint16_t targetX;
    uint16_t targetY;
    uint16_t targetWidth;
    uint16_t targetHeight;
    uint16_t boundingWidth;
    uint16_t boundingHeight;
    int16_t texturePageId;
} TexturePageItem;

typedef struct {
    uint32_t count;
    TexturePageItem* items;
} Tpag;

// ===[ CODE - Code Entries ]===
typedef struct {
    bool present;
    const char* name;
    uint32_t length;
    uint16_t localsCount;
    uint16_t argumentsCount;
    uint32_t bytecodeAbsoluteOffset;
    uint32_t offset;
} CodeEntry;

typedef struct {
    uint32_t count;
    CodeEntry* entries;
} Code;

// ===[ VARI - Variables ]===
typedef struct {
    const char* name;
    int32_t instanceType;
    int32_t varID;
    uint32_t occurrences;
    uint32_t firstAddress;
    int16_t builtinVarId; // Pre-resolved enum ID for built-in variables (varID == -6), -1 otherwise
} Variable;

typedef struct {
    uint32_t varCount1;
    uint32_t varCount2;
    uint32_t maxLocalVarCount;
    uint32_t variableCount;
    Variable* variables;
} Vari;

// ===[ FUNC - Functions & Code Locals ]===
typedef struct {
    const char* name;
    uint32_t occurrences;
    uint32_t firstAddress;
} Function;

typedef struct {
    // UndertaleModTool calls this field "Index", but that's because that's how it seemingly worked in pre-WAD version 17
    // After WAD version 17+, this has shown that this is actually the varID of the local variable (it matches the Variable.varID)
    uint32_t varID;
    const char* name;
} LocalVar;

typedef struct {
    const char* name;
    uint32_t localVarCount;
    LocalVar* locals;
} CodeLocals;

typedef struct {
    uint32_t functionCount;
    Function* functions;
    uint32_t codeLocalsCount;
    CodeLocals* codeLocals;
} Func;

// ===[ STRG - Strings ]===
typedef struct {
    uint32_t count;
    const char** strings; // pointers into strgBuffer
} Strg;

// ===[ TXTR - Embedded Textures ]===
typedef struct {
    bool present;
    bool mapped;
    uint32_t scaled;
    uint32_t generatedMips; // GMS 2.0.6+: number of generated mipmaps (0 for GMS 1.x)
    uint32_t textureBlockSize; // GMS 2022.3+: size of the texture block (0 for older versions)
    int32_t textureWidth;  // GMS 2022.9+
    int32_t textureHeight; // GMS 2022.9+
    int32_t indexInGroup;  // GMS 2022.9+
    uint32_t blobOffset; // absolute file offset to PNG data
    uint32_t blobSize; // computed size of blob data
    uint8_t* blobData; // owned copy of PNG data
} Texture;

typedef struct {
    uint32_t count;
    Texture* textures;
} Txtr;

// ===[ AUDO - Embedded Audio ]===
typedef struct {
    bool present;
    uint32_t dataOffset; // absolute file offset to audio data
    uint32_t dataSize;   // length of audio data
    uint8_t* data;       // owned copy of audio data
} AudioEntry;

typedef struct {
    uint32_t count;
    AudioEntry* entries;
} Audo;

// ===[ Detected Format ]===
// The effective GMS version after heuristic detection. GEN8.version is unreliable since GM:S 2,
// so chunk parsers probe the data and bump these fields upward when they detect newer-format features.
typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t release;
    uint32_t build;
} DetectedFormat;

// ===[ Top-level DataWin container ]===
struct DataWin {
    uint8_t* strgBuffer;        // owned copy of STRG chunk raw data
    // Absolute file offset of strgBuffer[0], we need this because data.win stores absolute offsets (from the beginning of the data.win file) instead of relative offsets
    size_t strgBufferBase;

    uint8_t* bytecodeBuffer;     // owned copy of CODE bytecode blob
    // Absolute file offset of bytecodeBuffer[0], we need this because data.win stores absolute offsets (from the beginning of the data.win file) instead of relative offsets
    size_t bytecodeBufferBase;

    Gen8 gen8;
    Optn optn;
    Lang lang;
    Extn extn;
    Sond sond;
    Agrp agrp;
    Sprt sprt;
    Bgnd bgnd;
    PathChunk path;
    Scpt scpt;
    Glob glob;
    Shdr shdr;
    FontChunk font;
    Tmln tmln;
    Objt objt;
    RoomChunk room;
    // DAFL is empty, no field needed
    Acrv acrv;
    Tpag tpag;
    Code code;
    Vari vari;
    Func func;
    Strg strg;
    Txtr txtr;
    Audo audo;

    DetectedFormat detectedFormat;

    // Held open across the whole session when DataWinParserOptions.lazyLoadRooms is true.
    // Used by DataWin_loadRoomPayload to satisfy on-demand room payload reads.
    // nullptr when lazy loading is disabled. Closed by DataWin_free.
    FILE* lazyLoadFile;
    uint8_t* mappedFile;
    size_t fileSize; // cached size of the DataWin, captured at parse time. Used for platforms where fseek(SEEK_END)+ftell is unreliable due to buffering (like the PlayStation 2).
    bool lazyLoadRooms; // mirrors the parser option so Runner can branch without re-reading options
    bool lazyLoadTextures; // ditto, but with TXTR pages
    bool lazyLoadAudio; // ditto, but with AUDO entries
};

DataWin* DataWin_parse(const char* filePath, DataWinParserOptions options);
void DataWin_free(DataWin* dataWin);
void DataWin_printDebugSummary(DataWin* dataWin);
// Lazy room payload management. DataWin_loadRoomPayload is a no-op when the payload is already loaded.
void DataWin_loadRoomPayload(DataWin* dw, int32_t roomIndex);
void DataWin_freeRoomPayload(Room* room);
// Finds a reusable dynamic Sprite slot (textureCount == 0) at or above `startIndex`, or appends a new one.
uint32_t DataWin_allocSpriteSlot(DataWin* dw, uint32_t startIndex);
// Compares the detected effective GMS version (not the raw GEN8 version) against a lower bound.
// Returns true if the detected version >= (major, minor, release, build).
//
// Mirrors UndertaleModTool's IsVersionAtLeast.
bool DataWin_isVersionAtLeast(const DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build);
// Raises the detected effective version to at least (major, minor, release, build). No-op if the detected version is already >= the target.
void DataWin_bumpVersionTo(DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build);
void GamePath_computeInternal(GamePath* path);
PathPositionResult GamePath_getPosition(GamePath* path, float t);
void DataWin_loadTxtrIfNeeded(DataWin* dw, uint32_t textureId);
void DataWin_loadAudoIfNeeded(DataWin* dw, uint32_t audioEntryId);

#endif /* _BS_DATA_WIN_H_ */
