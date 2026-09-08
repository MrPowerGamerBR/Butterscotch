#ifndef _BS_RUNNER_H_
#define _BS_RUNNER_H_

#include "common.h"
#include "audio_system.h"
#include "data_win.h"
#include "event_table.h"
#include "file_system.h"
#include "ini.h"
#include "instance.h"
#include "renderer.h"
#include "runner_keyboard.h"
#include "spatial_grid.h"
#include "runner_gamepad.h"
#include "runner_mouse.h"
#include "vm.h"
#include "random.h"

// ===[ Event Type Constants ]===
#define EVENT_ROOM_CREATION  -2
#define EVENT_CREATE     0
#define EVENT_DESTROY    1
#define EVENT_ALARM      2
#define EVENT_STEP       3
#define EVENT_COLLISION  4
#define EVENT_KEYBOARD   5
#define EVENT_MOUSE      6
#define EVENT_OTHER      7
#define EVENT_DRAW       8
#define EVENT_KEYPRESS   9
#define EVENT_KEYRELEASE 10
#define EVENT_CLEANUP    12
#define EVENT_PRECREATE  14

// ===[ Step Sub-event Constants ]===
#define STEP_NORMAL 0
#define STEP_BEGIN  1
#define STEP_END    2

// ===[ Draw Sub-event Constants ]===
#define DRAW_NORMAL    0
#define DRAW_GUI       64
#define DRAW_BEGIN     72
#define DRAW_END       73
#define DRAW_GUI_BEGIN 74
#define DRAW_GUI_END   75
#define DRAW_PRE       76
#define DRAW_POST      77

// ===[ Mouse Sub-event Constants ]===
#define MOUSE_LEFT_BUTTON 0
#define MOUSE_RIGHT_BUTTON 1
#define MOUSE_MIDDLE_BUTTON 2
#define MOUSE_NO_BUTTON 3
#define MOUSE_LEFT_PRESSED 4
#define MOUSE_RIGHT_PRESSED 5
#define MOUSE_MIDDLE_PRESSED 6
#define MOUSE_LEFT_RELEASED 7
#define MOUSE_RIGHT_RELEASED 8
#define MOUSE_MIDDLE_RELEASED 9
#define MOUSE_ENTER 10
#define MOUSE_LEAVE 11
#define MOUSE_GLOB_LEFT_BUTTON 50
#define MOUSE_GLOB_RIGHT_BUTTON 51
#define MOUSE_GLOB_MIDDLE_BUTTON 52
#define MOUSE_GLOB_LEFT_PRESSED 53
#define MOUSE_GLOB_RIGHT_PRESSED 54
#define MOUSE_GLOB_MIDDLE_PRESSED 55
#define MOUSE_GLOB_LEFT_RELEASED 56
#define MOUSE_GLOB_RIGHT_RELEASED 57
#define MOUSE_GLOB_MIDDLE_RELEASED 58
#define MOUSE_WHEEL_UP 60
#define MOUSE_WHEEL_DOWN 61

// ===[ Other Sub-event Constants ]===
#define OTHER_OUTSIDE_ROOM   0
#define OTHER_GAME_START     2
#define OTHER_ROOM_START     4
#define OTHER_ROOM_END       5
#define OTHER_NO_MORE_LIVES  6
#define OTHER_ANIMATION_END  7
#define OTHER_END_OF_PATH    8
#define OTHER_NO_MORE_HEALTH 9
#define OTHER_USER0          10
#define OTHER_OUTSIDE_VIEW0  40
#define OTHER_OUTSIDE_VIEW1  41
#define OTHER_OUTSIDE_VIEW2  42
#define OTHER_OUTSIDE_VIEW3  43
#define OTHER_OUTSIDE_VIEW4  44
#define OTHER_OUTSIDE_VIEW5  45
#define OTHER_OUTSIDE_VIEW6  46
#define OTHER_OUTSIDE_VIEW7  47
#define OTHER_ASYNC_DIALOG   63
#define OTHER_ASYNC_SAVE_LOAD 72
#define OTHER_ASYNC_SYSTEM   75

#define MAX_VIEWS 8
#define MAX_SURFACES 16
#define MAX_DEFAULT_ROOM_CAMERAS MAX_VIEWS
#define MAX_USER_CAMERAS 56
#define MAX_CAMERAS (MAX_DEFAULT_ROOM_CAMERAS + MAX_USER_CAMERAS)

// ===[ Operating System Types ]===
// See GameMaker-HTML5's Globals.js
typedef enum {
    OS_UNKNOWN = -1,
    OS_WINDOWS,
    OS_MACOSX,
    OS_PSP,
    OS_IOS,
    OS_ANDROID,
    OS_SYMBIAN,
    OS_LINUX,
    OS_WINPHONE,
    OS_TIZEN,
    OS_WIN8NATIVE,
    OS_WIIU,
    OS_3DS,
    OS_PSVITA,
    OS_BB10,
    OS_PS4,
    OS_XBOXONE,
    OS_PS3,
    OS_XBOX360,
    OS_UWP,
    OS_AMAZON,
    OS_SWITCH,

    OS_LLVM_WIN32 = 65536,
    OS_LLVM_MACOSX,
    OS_LLVM_PSP,
    OS_LLVM_IOS,
    OS_LLVM_ANDROID,
    OS_LLVM_SYMBIAN,
    OS_LLVM_LINUX,
    OS_LLVM_WINPHONE
} YoYoOperatingSystem;

typedef struct {
    bool enabled;
    int32_t portX;
    int32_t portY;
    int32_t portWidth;
    int32_t portHeight;
    int32_t cameraId;
    int32_t surfaceId;
} RuntimeView;

typedef struct {
    bool allocated; // slot in use (default cameras: set when the room enables the view; user cameras: camera_create/destroy)
    float viewX;
    float viewY;
    int32_t viewWidth;
    int32_t viewHeight;
    uint32_t borderX;
    uint32_t borderY;
    int32_t speedX;
    int32_t speedY;
    int32_t objectId; // follow target (object index), -1 = none
    float viewAngle;
    Matrix4f viewMatrix;
    Matrix4f projectionMatrix;
} GMLCamera;

typedef struct {
    bool visible;
    bool foreground;
    int32_t backgroundIndex;  // BGND resource index (mutable at runtime)
    float x, y;               // float for sub-pixel scrolling accumulation
    bool tileX, tileY;
    float speedX, speedY;
    float xScale, yScale;     // legacy background_xscale[]/background_yscale[] (default 1.0)
    bool stretch;
    float alpha;
} RuntimeBackground;

typedef struct {
    bool visible;
    float offsetX;
    float offsetY;
} TileLayerState;

// Runtime background element on a layer
typedef struct {
    int32_t spriteIndex; // SPRT index (-1 = none)
    bool visible;
    bool hTiled;
    bool vTiled;
    bool stretch;
    float xScale;
    float yScale;
    uint32_t blend; // BGR
    float alpha;
    float xOffset; // element-local offset (in addition to layer offset)
    float yOffset;
    int32_t imageIndex;
} RuntimeBackgroundElement;

typedef struct {
    int32_t id;
    bool active;
    bool repeat;
    bool executing;
    int32_t units;
    double period;
    double elapsed;
    RValue callback;
} CallLaterEntry;

// Mutable sprite element on an Assets layer. Populated from RoomLayerAssetsData.sprites at room init, can be removed at runtime via layer_sprite_destroy (used by language variant selection).
typedef struct {
    const char* name; // not owned, can be null if dynamically created
    int32_t spriteIndex; // SPRT index (-1 = none/destroyed)
    int32_t x;
    int32_t y;
    float scaleX;
    float scaleY;
    uint32_t color; // BGR + alpha
    float animationSpeed;
    uint32_t animationSpeedType;
    float frameIndex;
    float rotation;
} RuntimeSpriteElement;

// Values match GML layerelementtype_* enum so layer_get_element_type can return them as-is.
typedef enum {
    RuntimeLayerElementType_Background = 1,
    RuntimeLayerElementType_Instance = 2,
    RuntimeLayerElementType_Sprite = 4,
    RuntimeLayerElementType_Tilemap = 5,
    RuntimeLayerElementType_Tile = 7,
} RuntimeLayerElementType;

typedef struct {
    uint32_t id;
    RuntimeLayerElementType type;
    bool visible;
    float alpha; // GameMaker-HTML5's m_imageAlpha
    uint32_t blend; // GameMaker-HTML5's m_imageBlend
    RuntimeBackgroundElement* backgroundElement; // owned; set for every background element
    RuntimeSpriteElement* spriteElement; // owned; nullptr if type != Sprite
    RoomTile* tileElement; // borrowed (legacyTiles) or owned (layer_tile_create); nullptr if type != Tile
    bool tileElementOwned; // true if tileElement was heap-allocated and must be freed
    RoomLayerTilesData* tilemapData; // borrowed, points into the parsed RoomLayer; nullptr if type != Tilemap
    int32_t instanceId; // only valid if type == Instance; the instance may have died since, so callers must check liveness
} RuntimeLayerElement;

// Runtime-mutable state for a GMS2 room layer. Parsed layers are populated at room load from RoomLayer and share IDs with the parsed data.
// Dynamic layers are created via layer_create and carry their own name + element list; they don't correspond to any RoomLayer.
typedef struct {
    uint32_t id;
    int32_t depth;
    bool visible;
    float xOffset;
    float yOffset;
    float hSpeed;
    float vSpeed;
    bool dynamic; // true = created at runtime via layer_create
    char* dynamicName; // owned
    int32_t beginScript;
    int32_t endScript;
    RuntimeLayerElement* elements; // stb_ds array
} RuntimeLayer;

// stb_ds hashmap entry: depth -> tile layer state
typedef struct {
    int32_t key;
    TileLayerState value;
} TileLayerMapEntry;

// A single entry in the depth-sorted draw list. Cached on Runner and rebuilt lazily based on Runner.drawableListStructureDirty / drawableListSortDirty.
// Filtering on instance->active/visible and runtimeLayer->visible happens at draw time so toggling those does not require invalidating the cache.
typedef enum { DRAWABLE_TILE, DRAWABLE_INSTANCE, DRAWABLE_LAYER, DRAWABLE_PARTICLE_SYSTEM } DrawableType;

typedef struct {
    DrawableType type;
    int32_t depth;
    union {
        Instance* instance;
        int32_t tileIndex;
        // Stored as an ID (resolved via Runner_findRuntimeLayerById) instead of a pointer, because layer_create can call arrput on runner->runtimeLayers mid-draw and realloc the array, invalidating any cached pointers.
        int32_t runtimeLayerId;
        // Same reasoning as runtimeLayerId: part_system_create during a draw event can realloc the pool.
        int32_t particleSystemId;
    };
} Drawable;

// stb_ds hashmap entry for ds_map: string key -> RValue
typedef struct {
    char* key;
    RValue value;
} DsMapEntry;

// ds_priority queue item
typedef struct {
    GMLReal depth;
    RValue item;
} DsPriorityItem;

// ds_list: dynamic array of RValues
typedef struct {
    RValue* items; // stb_ds dynamic array of RValues
    bool freed;    // true when the slot is destroyed and available for reuse by ds_list_create (matches native GMS)
} DsList;

// ds_queue: FIFO, items[0] is the head (next to dequeue), last item is the tail.
typedef struct {
    RValue* items; // stb_ds dynamic array of RValues
    bool freed;    // true when the slot is destroyed and available for reuse by ds_queue_create
} DsQueue;

typedef struct {
    RValue* items; // stb_ds dynamic array of RValues
    bool freed;    // true when the slot is destroyed and available for reuse by ds_stack_create
} DsStack;

typedef struct {
    DsPriorityItem* items; // stb_ds dynamic array of DsPriorityItems
    bool freed;    // true when the slot is destroyed and available for reuse by ds_priority_queue_create
} DsPriority;

typedef struct {
    RValue* items; // malloc'd array of items
    int32_t width;
    int32_t height;
    bool freed; // true when the slot is destroyed and available for reuse by ds_grid_create
} DsGrid;

// ===[ GML Buffer System ]===

// Buffer type constants (matching GML)
#define GML_BUFFER_FIXED 0
#define GML_BUFFER_GROW  1
#define GML_BUFFER_WRAP  2
#define GML_BUFFER_FAST  3

// Buffer data type constants (matching GML)
#define GML_BUFTYPE_U8      1
#define GML_BUFTYPE_S8      2
#define GML_BUFTYPE_U16     3
#define GML_BUFTYPE_S16     4
#define GML_BUFTYPE_U32     5
#define GML_BUFTYPE_S32     6
#define GML_BUFTYPE_F16     7
#define GML_BUFTYPE_F32     8
#define GML_BUFTYPE_F64     9
#define GML_BUFTYPE_BOOL   10
#define GML_BUFTYPE_STRING 11
#define GML_BUFTYPE_U64    12
#define GML_BUFTYPE_TEXT   13

// Buffer seek mode constants (matching GML)
#define GML_BUFFER_SEEK_START    0
#define GML_BUFFER_SEEK_RELATIVE 1
#define GML_BUFFER_SEEK_END      2

typedef struct {
    uint8_t* data;       // raw byte storage
    int32_t size;        // allocated size in bytes
    int32_t position;    // current read/write cursor
    int32_t usedSize;    // high-water mark for grow buffers
    int32_t alignment;   // byte alignment for read/write operations
    int32_t type;        // GML_BUFFER_FIXED, _GROW, _WRAP, _FAST
    bool isValid;        // false after buffer_delete (tombstone)
} GmlBuffer;

// ===[ Async buffer save/load ]===

// A single queued buffer load/save operation, accumulated inside an async group.
typedef struct {
    int32_t bufferId;  // buffer to read from (save) or write into (load)
    char* filename;    // owned; raw file name (the group name is applied as a directory prefix when the op is kicked)
    int32_t offset;    // byte offset within the buffer
    int32_t size;      // byte count; -1 means "the whole file" for loads
    bool isSave;       // true = save, false = load
} AsyncBufferOp;

// A completed buffer save/load request waiting to fire its "Async - Save/Load" event.
typedef struct {
    int32_t requestId; // posted to the async_load map as "id"; also returned by buffer_async_group_end
    int32_t status;    // posted as "status": 1 on success, 0 on failure (matches the native runner)
    int32_t error;     // posted as "error"; always 0 here (matches the native runner)
} AsyncSaveLoadCompletion;

// Motion planning grid used by mp_grid_* builtins. Cell value 1 = blocked.
typedef struct {
    bool inUse;
    GMLReal left;
    GMLReal top;
    int32_t hcells;
    int32_t vcells;
    GMLReal cellWidth;
    GMLReal cellHeight;
    uint8_t* cells;
} MpGrid;

// ===[ Particle System ]===
// Backs the part_* builtins, which are implemented in vm_builtins.c. GameMaker splits particles into
// three resources:
//   * a SYSTEM owns the live particles and the emitters that spawn them, and decides when they are drawn
//   * a TYPE describes how a particle looks and moves; types are global, so any system can stream any type
//   * an EMITTER is owned by one system and spawns particles of a given type inside a region
//
// Ids are indices into the pools below, with a "used" tombstone so a destroyed id can be handed out
// again. Same convention as the ds_* pools, and games do rely on it.

// part_emitter_region() shape constants
#define PS_SHAPE_RECTANGLE 0
#define PS_SHAPE_ELLIPSE   1
#define PS_SHAPE_DIAMOND   2
#define PS_SHAPE_LINE      3

// part_emitter_region() distribution constants
#define PS_DISTR_LINEAR       0
#define PS_DISTR_GAUSSIAN     1
#define PS_DISTR_INVGAUSSIAN  2

// Upper bound on live particles per system. GameMaker itself has no such limit, but an emitter left
// streaming in a room the player never leaves will grow without bound, and the consoles this runner
// targets cannot absorb that. Spawns past the cap are dropped (warned about once per system).
#define PARTICLE_SYSTEM_MAX_PARTICLES 8192

typedef struct {
    bool used;

    int32_t sprite;       // sprite asset index, -1 when the type has no sprite (draws nothing)
    bool spriteAnimate;   // advance the subimage as the particle ages
    bool spriteStretch;   // stretch one full animation cycle across the particle's whole life
    bool spriteRandom;    // start from a random subimage

    // Every "min/max/incr/wiggle" quadruple works the same way: the initial value is picked uniformly
    // in [min, max], "incr" is added every step, and "wiggle" oscillates the value used for motion and
    // drawing without accumulating into the base.
    GMLReal sizeMin, sizeMax, sizeIncr, sizeWiggle;
    GMLReal scaleX, scaleY;
    GMLReal speedMin, speedMax, speedIncr, speedWiggle;
    GMLReal dirMin, dirMax, dirIncr, dirWiggle;

    GMLReal gravityAmount;
    GMLReal gravityDirection;

    // Drawn orientation. Independent of the direction of travel unless angRelative is set, in which
    // case the angle is measured from it.
    GMLReal angMin, angMax, angIncr, angWiggle;
    bool angRelative;

    int32_t lifeMin, lifeMax;

    // Alpha and colour both run through three stops across the particle's life: start, middle at the
    // halfway mark, end. part_type_alpha1/alpha2 and part_type_colour1/colour2 collapse the stops.
    // part_type_colour_mix instead picks a fixed random blend of two colours per particle at spawn;
    // when colourMix is set, colourStart/colourMixEnd are the blend endpoints and the per-particle
    // result is stored in Particle.colour (colourFixed), so the lifetime curve is bypassed.
    GMLReal alphaStart, alphaMiddle, alphaEnd;
    uint32_t colourStart, colourMiddle, colourEnd; // GML packed BGR, as the drawing functions take it
    bool colourMix;
    uint32_t colourMixEnd; // second blend endpoint for part_type_colour_mix
    bool additive;

    int32_t deathType;   // type id spawned when a particle of this type dies, -1 when none
    int32_t deathNumber; // how many to spawn; negative means a 1-in-|n| chance
    int32_t stepType;    // type id spawned every step a particle of this type lives, -1 when none
    int32_t stepNumber;  // same "negative means a chance" convention as deathNumber

    // Particles alive that were born from this type. A destroyed type keeps its slot until the
    // count reaches zero: in GameMaker a particle outlives the type it came from, and holding the
    // slot is far cheaper than copying every type field into every particle.
    int32_t refCount;
} ParticleType;

typedef struct {
    int32_t typeId;
    GMLReal x, y;
    GMLReal speed;      // base speed, before wiggle
    GMLReal direction;  // base direction in degrees, before wiggle
    GMLReal size;       // base size, before wiggle
    GMLReal angle;      // drawn orientation in degrees, before wiggle
    int32_t life;       // steps remaining
    int32_t lifeTotal;  // steps this particle started with, for the alpha/animation curves
    int32_t subimgBase; // starting subimage
    uint32_t colour;    // set by part_particles_create_colour; overrides the type's colour curve
    bool colourFixed;   // true when "colour" above is in force
    int32_t seed;       // per-particle random; phase-shifts this particle's wiggle oscillations
} Particle;

typedef struct {
    bool used;
    GMLReal xmin, xmax, ymin, ymax;
    int32_t shape;
    int32_t distribution;
    int32_t streamType;   // type id streamed every step, -1 when the emitter is idle
    // Particles per step. Negative means a 1-in-|n| chance; a fraction is a chance of one more, so
    // an emitter streaming 0.5 spawns on roughly every other step.
    GMLReal streamNumber;
} ParticleEmitter;

typedef struct {
    bool used;
    bool automaticUpdate; // step the system at the end of every frame (on by default, as in GML)
    bool automaticDraw;   // draw the system from the depth list (on by default, as in GML)
    int32_t depth;
    GMLReal originX, originY;  // part_system_position: added to every particle when drawing
    bool warnedFull;      // the "hit PARTICLE_SYSTEM_MAX_PARTICLES" warning fires once per system
    Particle* particles;  // stb_ds array
    ParticleEmitter* emitters; // stb_ds array, index = emitter id within this system
} ParticleSystem;

// Implemented in vm_builtins.c next to the part_* builtins; these four are the only entry points the
// runner itself needs (depth list, frame step, teardown).
ParticleSystem* Particles_systemGet(Runner* runner, int32_t systemId);
// Steps every system with automaticUpdate set. Called once at the end of Runner_step.
void Particles_updateAutomatic(Runner* runner);
// Draws one system at the current draw state. Backs part_system_drawit and the depth-list entry.
void Particles_drawSystem(Runner* runner, int32_t systemId);
// Frees both pools. Called from the Runner's cleanup path.
void Particles_freeAll(Runner* runner);

// Open text file handle for GML file_text_* functions
#define MAX_OPEN_TEXT_FILES 32
typedef struct {
    char* content; // full file content (for read mode)
    char* writeBuffer; // accumulated text (for write mode)
    char* filePath; // relative path (for write mode, to flush on close)
    int32_t readPos; // current byte position in content (read mode)
    int32_t contentLen; // length of content string
    bool isWriteMode;
    bool isOpen;
} OpenTextFile;

// Open binary file handle for GML file_bin_* functions.
#define MAX_OPEN_BINARY_FILES 32
typedef struct {
    void* handle; // FileSystem-owned; passed to binaryRead/Write/Seek/etc.
    bool isOpen;
} OpenBinaryFile;

// Saved state for persistent rooms. When leaving a persistent room, instance state
// and visual properties are saved here. When returning, they are restored instead
// of re-creating from the room definition.
typedef struct {
    bool initialized;
    Instance** instances; // stb_ds array of saved Instance*
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;
    bool drawBackgroundColor;
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    RuntimeLayer* runtimeLayers; // stb_ds array, index-parallel to currentRoom->layers
    RuntimeView views[MAX_VIEWS];
    bool viewsEnabled;
    GMLCamera defaultCameras[MAX_DEFAULT_ROOM_CAMERAS]; // whole-array snapshot of Runner.defaultCameras (room-scoped)
} SavedRoomState;

// One flattened collision event entry. Mirrors ObjectEvent but adds the resolved codeId and ownerObjectIndex (the ancestor that actually defines the event) so dispatch needs no event-table lookup.
typedef struct {
    uint32_t targetObjectIndex; // partner-side object index this handler matches against (eventSubtype in the GML object file)
    int32_t codeId; // resolved bytecode id for this handler
    int32_t ownerObjectIndex; // object that actually defines the handler (i for own events, ancestor index for inherited)
} FlattenedCollisionEvent;

typedef struct {
    uint32_t eventCount;
    FlattenedCollisionEvent* events;
} FlattenedCollisionEventList;

typedef struct { char* key; int32_t value; } AssetsByNameEntry;
typedef struct { char* key; int value; } DisabledObjEntry;

struct Runner {
    DataWin* dataWin;
    VMContext* vmContext;
    Renderer* renderer;
    FileSystem* fileSystem;
    AudioSystem* audioSystem;
    Room* currentRoom;
    int32_t currentRoomIndex;
    int32_t currentRoomOrderPosition;
    Instance** instances; // stb_ds array of Instance*
    // Per-object instance lists: for each object index, a stb_ds array of Instance*.
    // An instance appears in its own object's list AND in every ancestor object's list (descendant-inclusive).
    // This lets collision dispatch iterate only the instances of a target object (and its descendants) instead of scanning all instances in the room.
    // Must be kept in sync with any instance creation, change, or deletion.
    Instance*** instancesByObject;
    // Same as instancesByObject but each instance only appears in the bucket of its EXACT objectIndex (no ancestors). Used by event dispatch so we don't double-fire when both a child and its parent declare the same event.
    Instance*** instancesByExactObject;
    // Precomputed (eventType, eventSubtype) -> dense slot remap. Built once at Runner_create, never mutated.
    EventSlotMap eventSlotMap;
    // Precomputed per-object and per-slot CSR tables of resolved event handlers. Replaces the per-dispatch parent-chain walk in findEventCodeIdAndOwner.
    ResolvedEventTable eventTable;
    // Precomputed assets map.
    AssetsByNameEntry* assetsByName;
    // For each event type, the deduplicated list of object indices that respond to ANY subtype of that event (including via inheritance). Derived from the event table; used by collision dispatch to skip non-collision objects in the outer loop.
    // Length = OBJT_EVENT_TYPE_COUNT.
    int32_t** objectsWithAnyEventOfType;
    // Per-object flattened collision event list (one FlattenedCollisionEventList per objectIndex, length = dataWin->objt.count).
    // Flattens parent-chain collision inheritance: each child's list contains its own collision events plus
    // every ancestor target the child does not override, deduplicated. Each entry stores the resolved codeId
    // and the ownerObjectIndex (the ancestor that actually defines the event), so collision dispatch needs
    // no parent-chain walk and no resolved-event-table lookup. Owned by the Runner; dataWin->objt is left
    // untouched so the parsed file remains the source of truth.
    FlattenedCollisionEventList* flattenedCollisionEvents;
    // Reusable scratch array for Runner_executeEventForAll. Pre-grown to avoid stb_ds arrput overhead and repeated allocations on the per-frame dispatch path. Owned via stb_ds; truncated at the start of each call.
    Instance** eventDispatchInstances;
    // LIFO arena used to snapshot per-object instance lists before iteration.
    // Any loop that might fire user code iterates a copy so that in-flight mutations (instance_change swap-remove, spawns, destroys) don't corrupt it.
    // Each call pushes its snapshot (append) and pops on normal loop exit; nesting is safe because pushes/pops are LIFO and outer ranges stay untouched under newer pushes.
    Instance** instanceSnapshots;
    SpatialGrid* spatialGrid;
    uint32_t collisionQueryCounter;
    int32_t pendingRoom;  // -1 = none
    bool gameStartFired;
    int frameCount;
    uint32_t nextInstanceId;
    RunnerKeyboardState* keyboard;
    RunnerMouseState* mouse;
    RuntimeView views[MAX_VIEWS];
    GMLCamera defaultCameras[MAX_DEFAULT_ROOM_CAMERAS];
    GMLCamera userCameras[MAX_USER_CAMERAS];
    GMLCamera surfaceCamera;
    GMLCamera guiCamera;
    RunnerGamepadState* gamepads;
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;      // runtime-mutable (BGR format)
    bool drawBackgroundColor;
    bool shouldExit;
    bool debugMode;
    // application_surface runtime state (mirrors GML toggles)
    bool appSurfaceEnabled;
    bool appSurfaceAutoDraw;
    bool usingAppSurface;
    int32_t applicationWidth;
    int32_t applicationHeight;
    int32_t oldApplicationWidth;
    int32_t oldApplicationHeight;
    int32_t widescreenExtraWidth;
    int32_t widescreenExtraHeight;
    float displayScaleX;
    float displayScaleY;
    float freeCamPanX, freeCamPanY, freeCamZoom; // Visual-only free camera.
    // ID returned by renderer->vtable->ensureApplicationSurface each frame. Real surface ID on GL/GL-legacy,
    // APPLICATION_SURFACE_ID (-1) on PS2. This is what BUILTIN_VAR_APPLICATION_SURFACE returns to GML.
    int32_t applicationSurfaceId;
    void (*setWindowTitle)(const char* title);
    bool (*getWindowSize)(int32_t* outW, int32_t* outH);
    void (*setWindowSize)(int32_t width, int32_t height);
    bool (*windowHasFocus)(void);
    void (*setCursor)(int32_t cursorType);
    int32_t currentCursor;  // last value passed to window_set_cursor
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    RuntimeLayer* runtimeLayers; // stb_ds array, index-parallel to currentRoom->layers for parsed entries; dynamic entries appended
    uint32_t nextLayerId;        // counter for IDs of layers/elements created at runtime
    SavedRoomState* savedRoomStates; // array of size dataWin->room.count, for persistent room support
    int32_t viewCurrent; // index of the view currently being drawn (for view_current)
    bool viewsEnabled;   // runtime-mutable global view system toggle (view_enabled); seeded from room->flags & 1 on room enter
    uint32_t renderGameW; // FBO width used by the last frame (= max port bound), 0 if not yet rendered
    uint32_t renderGameH; // FBO height used by the last frame (= max port bound), 0 if not yet rendered
    int32_t viewportX;   // X offset in window (letterboxing)
    int32_t viewportY;   // Y offset in window (letterboxing)
    int32_t viewportW;   // Scaled game width in window
    int32_t viewportH;   // Scaled game height in window
    DisabledObjEntry* disabledObjects; // stb_ds string hashmap, nullptr = no filtering
    struct { int key; Instance* value; }* instancesById;
    bool forceDrawDepth;
    bool applyOffsetForPrimitives;
    // Depth-sorted unified list of all drawables (instances + tiles + runtime layers) for the current room.
    // Active/visible filtering happens at draw time, so toggling those flags does not invalidate the cache.
    //
    // Two-tier invalidation:
    //   structureDirty - the SET of entries changed (instance/layer create or destroy, room change). Full rebuild.
    //   sortDirty      - the entries are the same but .depth values may have shifted. Refresh depths and only re-sort if order broke. Cheap when small depth shifts don't cross neighbors (typical depth=-y games).
    Drawable* cachedDrawables; // stb_ds array
    bool drawableListStructureDirty;
    bool drawableListSortDirty;
    // Struct instances created by @@NewGMLObject@@. Reuses Instance with objectIndex=STRUCT_OBJECT_INDEX.
    // Tracked separately so event/step/draw iteration over runner->instances stays clean.
    Instance** structInstances;
    int32_t forcedDepth;
    // The time between the last frame and the current frame, stored in microseconds.
    GMLReal deltaTime;
    // Current frame rate (capped at room speed)
    double fps;                   // last measured frames-per-second value returned to GML
    uint64_t fpsWindowStartNanos;  // nowNanos() at the start of the measurement window
    int fpsWindowStartFrame;   // runner->frameCount at the start of the measurement window
    // Real-time measured frame rate for the "fps_real" builtin, measured as 1 / frame work time (uncapped).
    double fpsReal;
    uint64_t fpsRealFrameStartNanos; // nowNanos() at start of frame work for fps_real (set in Runner_step, consumed in Runner_drawGUI)
    char* windowTitle;

    // ===[ Builtin function state ]===
    DsMapEntry** dsMapPool; // stb_ds array of stb_ds hashmaps
    DsList* dsListPool; // stb_ds array of DsList
    DsQueue* dsQueuePool; // stb_ds array of DsQueue
    DsStack* dsStackPool; // stb_ds array of DsStack
    DsPriority* dsPriorityPool; // stb_ds array of DsPriority
    DsGrid* dsGridPool; // stb_ds array of DsGrid
    GmlBuffer* gmlBufferPool; // stb_ds array of GmlBuffer
    MpGrid* mpGridPool; // stb_ds array of motion-planning grids
    // Particle systems own their emitters and live particles; types are global and can be streamed by
    // any system's emitters. Both pools reuse destroyed slots, matching the ds_* id behaviour.
    ParticleSystem* particleSystemPool; // stb_ds array of ParticleSystem
    ParticleType* particleTypePool; // stb_ds array of ParticleType

    // Motion planning potential field settings
    GMLReal mpPotMaxrot;
    GMLReal mpPotStep;
    GMLReal mpPotAhead;
    bool mpPotOnSpot;

    // Legacy audio_play_music / audio_stop_music tracking
    int32_t lastMusicInstance;

    // INI file state
    IniFile* currentIni;
    char* currentIniPath;
    bool currentIniDirty;
    // Some games (like Undertale) open and close the same INI file EVERY SINGLE FRAME!
    // While on modern devices this isn't a huge deal, this WILL cause issues on devices that have less than stellar file systems (like the PlayStation 2)
    // To avoid unnecessary disk reads, we cache the last-closed INI and reuse it on reopen
    IniFile* cachedIni; // Cache of last-closed INI (for fast reopen)
    char* cachedIniPath;

    // Text file handles for file_text_* functions
    OpenTextFile openTextFiles[MAX_OPEN_TEXT_FILES];
    OpenBinaryFile openBinaryFiles[MAX_OPEN_BINARY_FILES];

    // Single active file_find_* enumeration session.
    char** fileFindResults; // stb_ds array of heap-dup'd matched file names (name only, no path)
    int32_t fileFindPosition; // index of the entry returned by the next file_find_next call

    // Pending call_later callbacks.
    CallLaterEntry* callLaterEntries; // stb_ds array
    int32_t nextCallLaterId;

    // Async map ID
    int32_t asyncLoadMapId;

    // Async buffer save/load state
    char* asyncBufferGroupName;                   // current group name (nullptr when no group is open); applied as a directory prefix
    bool asyncBufferGroupActive;                  // true between buffer_async_group_begin and buffer_async_group_end
    AsyncBufferOp* asyncBufferGroupOps;           // stb_ds array of ops accumulated in the open group
    AsyncSaveLoadCompletion* asyncSaveLoadQueue;  // stb_ds array of completions waiting to fire their event
    int32_t asyncBufferNextRequestId;             // monotonic request id handed out per kicked group/op

    // Pending Xbox One account-picker async result.
    int32_t xboxAccountPickerPendingId; // -1 when nothing is pending
    int32_t xboxAccountPickerPadIndex; // pad index reported back in the async map
    int32_t xboxAsyncIdCounter; // hands out a unique async id per picker call

    // Legacy GMS 1.x globals
    GMLReal score;
    GMLReal lives;
    GMLReal health;

    // Used by the "os_type" built-in
    YoYoOperatingSystem osType;

    // GUI layer size (display_set_gui_size). 0 = auto-match the current view's port size.
    int32_t guiWidth;
    int32_t guiHeight;

    // GMS legacy (pre 2022.1) collision behavior: AABB overlap treats touching edges as overlap.
    bool collisionCompatibilityMode;

    // GameMaker surface "stack".
    int32_t surfaceStack[MAX_SURFACES];

    // GUI-pass state: when inGuiPass is set, popping the surface stack empty must restore the GUI target + projection, not the room view.
    bool inGuiPass;
    int32_t guiPassW, guiPassH;
    int32_t guiPassPortW, guiPassPortH;
    int32_t guiPassTarget;

    // Both must be set
    // The original runner actually spawns a new process when game_change is called
    char* pendingWorkingDirectory;
    char* pendingLaunchParameters;

    // GameMaker launcher parameters
    // Just like the original runner, argv[0] is included in gameArgs
    char** gameArgs;

    // Offset between game start time and nowNanos()
    uint64_t gameStartTime;

    Random random;
};

const char* Runner_getEventName(int32_t eventType, int32_t eventSubtype);
void Runner_reset(Runner* runner);
Runner* Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem, uint32_t randomSeed);
void Runner_setGameArgs(Runner* runner, char** argv, int32_t argc);
void Runner_initFirstRoom(Runner* runner);
void Runner_step(Runner* runner);
void Runner_handlePendingRoomChange(Runner* runner);
void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype);
// Sets the "lives" global variable.
// Used to fire events when the values are equal to or lesser than 0.
void Runner_setLives(Runner* runner, GMLReal value);
// Sets the "health" global variable.
// Used to fire events when the values are equal to or lesser than 0.
void Runner_setHealth(Runner* runner, GMLReal value);
void Runner_draw(Runner* runner);
// Ensures the application_surface exists at the right size, mirrors the renderer's ID into runner+renderer state, then
// invokes renderer->vtable->beginFrame. Every platform main should call this instead of beginFrame directly.
void Runner_beginFrame(Runner* runner, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH, int32_t framebufferW, int32_t framebufferH);
void Runner_drawGUI(Runner* runner, int32_t windowW, int32_t windowH, int32_t targetW, int32_t targetH);
void Runner_drawPre(Runner* runner, int32_t windowW, int32_t windowH);
void Runner_drawPost(Runner* runner, int32_t windowW, int32_t windowH);
void Runner_computeViewDisplayScale(Runner* runner, int32_t gameW, int32_t gameH, float* outScaleX, float* outScaleY);
void Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks);
void Runner_updateMousePosition(Runner* runner, int32_t windowWidth, int32_t windowHeight, double mouseXInWindow, double mouseYInWindow);
// Converts the cached screen-space cursor (RunnerMouseState.screenX/screenY) to room/world coordinates using the LIVE camera/view state.
void Runner_getMouseRoomPosition(Runner* runner, GMLReal* outX, GMLReal* outY);
// Resolves a camera id (slot index) to its pool entry, or nullptr if out of range / not allocated.
GMLCamera* Runner_getCameraById(Runner* runner, int32_t id);
// Resolves the camera assigned to a view, or nullptr if the view index is invalid or has no allocated camera.
GMLCamera* Runner_getCameraForView(Runner* runner, int32_t viewIndex);
void Runner_scrollBackgrounds(Runner* runner);
void Runner_drawTileLayer(Runner* runner, RoomLayerTilesData* data, float layerOffsetX, float layerOffsetY);
// Allocates a fresh GML struct and registers it in instancesById and structInstances.
// refCount starts at 1 (the registry's implicit ref); callers that retain a reference must Instance_structIncRef.
Instance* Runner_createStruct(Runner* runner);
Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex);
Instance* Runner_createInstanceWithDepth(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t depth);
Instance* Runner_createInstanceWithLayer(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t layerId);
Instance* Runner_copyInstance(Runner* runner, Instance* source, bool performEvent);
void Runner_destroyInstance(Runner* runner, Instance* inst, bool runDestroyEvent);
void Runner_cleanupDestroyedInstances(Runner* runner);
// Add inst to the per-object lists of its object and every ancestor.
void Runner_addInstanceToObjectLists(Runner* runner, Instance* inst);
// Remove inst from the per-object lists of its object and every ancestor, preserving creation order (stable remove).
void Runner_removeInstanceFromObjectLists(Runner* runner, Instance* inst);
// Reset every per-object list to length 0 without releasing the backing arrays.
void Runner_clearAllObjectLists(Runner* runner);

// Update The Camera For Basic Views!
void Runner_updateCameraViewSimple(GMLCamera* camera);

// Push a snapshot of instancesByObject[targetObjIndex] onto runner->instanceSnapshots. Returns the base offset where this snapshot begins.
// The length is arrlen(runner->instanceSnapshots) - base.
// Invalid indices or empty buckets push zero entries (base == current arena length).
// Pair with Runner_popInstanceSnapshot(runner, base) when done.
int32_t Runner_pushInstancesOfObject(Runner* runner, int32_t targetObjIndex);
// Push a snapshot matching "target", which GML can pass in several forms: an object index (push the descendant-inclusive bucket), INSTANCE_ALL (push every instance in the room), or an instance ID >= 100000 (push that single instance if it exists).
// Returns base offset for pairing with Runner_popInstanceSnapshot.
int32_t Runner_pushInstancesForTarget(Runner* runner, int32_t target);
// Truncate the snapshot arena back to "base", releasing everything pushed after it.
void Runner_popInstanceSnapshot(Runner* runner, int32_t base);

// Push the surfaceID onto the runner's surface stack and bind it as the active render target.
// Returns false if the stack is full.
bool Runner_surfaceSetTarget(Runner* runner, int32_t surfaceID);
// Tracks when the GUI size has changed.
// When we are NOT in a GUI pass, we do nothing.
// When we ARE in a GUI pass, we recalculate the GUI width and height and re-start with beginGUI to match the new dimensions.
void Runner_guiSizeChanged(Runner* runner);
// Pops the top of the surface stack and bind whatever is below (or the main framebuffer when the stack is empty).
// Returns false when there was nothing to pop.
bool Runner_surfaceResetTarget(Runner* runner);
// Returns the surfaceID at the top of the surface stack, or -1 when no surface is bound (the application surface is active).
int32_t Runner_surfaceGetTarget(Runner* runner);

void Runner_dumpState(Runner* runner);
char* Runner_dumpStateJson(Runner* runner);
void Runner_free(Runner* runner);
RuntimeLayer* Runner_findRuntimeLayerByName(Runner* runner, char* name);
RuntimeLayer* Runner_findRuntimeLayerById(Runner* runner, int32_t id);
RoomLayer* Runner_findRoomLayerById(Room* room, int32_t id);
RuntimeLayerElement* Runner_findLayerElementById(Runner* runner, int32_t elementId, RuntimeLayer** outLayer);
void Runner_addInstanceLayerElement(Runner* runner, int32_t layerId, int32_t instanceId);
void Runner_removeInstanceLayerElement(Runner* runner, int32_t instanceId);
uint32_t Runner_getNextLayerId(Runner* runner);
void Runner_freeRuntimeLayer(RuntimeLayer* runtimeLayer);
// Sets the active state of the instance
static inline void Runner_setActiveState(Runner* runner, Instance* instance, bool active) {
#ifdef ENABLE_VM_TRACING
    if (active != instance->active) {
        GameObject* objDef = &runner->dataWin->objt.objects[instance->objectIndex];

        if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
            logInfo("VM: Instance %s (instanceId=%d,objectIndex=%d) marked as %s at (%f, %f)\n", objDef->name, instance->instanceId, instance->objectIndex, active ? "active" : "inactive", instance->x, instance->y);
        }
    }
#else
    (void)runner;
#endif

    instance->active = active;
}

#endif /* _BS_RUNNER_H_ */
