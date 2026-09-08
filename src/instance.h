#ifndef _BS_INSTANCE_H_
#define _BS_INSTANCE_H_

#include "common.h"
#include <stdint.h>
#include "rvalue.h"
#include "gml_array.h"
#include "int_rvalue_hashmap.h"

#define GML_ALARM_COUNT 12

// Fake objectIndex ID for GML structs and global scoped instances
#define STRUCT_OBJECT_INDEX (-1)

// Forward decl for Instance_structDecRef
struct Runner;

struct Instance {
    uint32_t instanceId;
    int32_t objectIndex;
    // Reference count for GML structs (objectIndex == STRUCT_OBJECT_INDEX mode). Unused for game-object instances.
    // The runner's structInstances registry holds an implicit +1 ref while the struct is registered, so a refCount of 1 means "only the registry references this"; the per-frame sweep (Runner_sweepDeadStructs) decRefs those to free them. RValues with ownsReference=true on RVALUE_STRUCT contribute one ref each.
    int32_t refCount;
    // When true, the struct will NOT be garbaged collected.
    bool pinned;
    // Position of this struct in runner->structInstances (for O(1) swap-remove when freed). -1 when not registered.
    int32_t structRegistryIndex;
    // Static variables: for a struct created by @@NewGMLObject@@, the code index of the constructor that built it (-1 otherwise).
    // Member reads that miss the instance fallback to the constructor's shared static struct.
    int32_t constructorCodeIndex;
    // Static inheritance: a static struct's parent static struct or nullptr.
    // The member-read fallback walks this chain so a child instance resolves fields declared static on a parent constructor.
    struct Instance* staticParent;
    // Native GMS runner stores all instance built-in variables as float (32-bit),
    // even though RValues use double. This matches the native precision model.
    float x, y;
    float xprevious, yprevious;
    float xstart, ystart;
    bool persistent, solid, active, destroyed, visible, createEventFired, outsideRoom, spatialGridDirty, mouseOver;
    // Used to track which alarms are set without looping through the entire alarm array
    uint16_t activeAlarmMask;
    int32_t maskIndex; // collision mask sprite override (-1 = use spriteIndex)
    int32_t* collisionCells; // Used to track where we are
    uint32_t lastCollisionQueryId;

    // Per-instance self variable storage (sparse open-addressed hashmap, keyed by varID).
    IntRValueHashMap selfVars;

    // Built-in instance properties
    int32_t spriteIndex;
    float imageSpeed;
    float imageIndex; // Even though textureCount is unsigned, games CAN set the image_index to negative values
    float imageXscale, imageYscale, imageAngle, imageAlpha;
    uint32_t imageBlend;
    int32_t depth;
    int32_t layer;

    // Motion properties
    float speed, direction;
    float hspeed, vspeed;
    float friction;
    float gravity, gravityDirection;

    // Path following state
    int32_t pathIndex;           // -1 = no path active
    float pathPosition;           // 0.0-1.0
    float pathPositionPrevious;
    float pathSpeed;
    float pathScale;              // default 1.0
    float pathOrientation;        // degrees, default 0.0
    int32_t pathEndAction;       // 0=stop, 1=restart, 2=continue, 3=reverse
    float pathXStart;             // origin for relative paths
    float pathYStart;

    int32_t alarm[GML_ALARM_COUNT];

    // Timeline following state
    int32_t timelineIndex; // -1 = no timeline assigned
    float timelinePosition;
    float timelineSpeed; // default 1.0
    bool timelineLoop;
    bool timelineRunning;
};

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y);
// Frees an instance's owned contents (selfVars values, collision cells) but NOT the Instance struct itself.
void Instance_freeContents(Instance* instance);
void Instance_free(Instance* instance);

// GML-struct refcount helpers. Only meaningful when inst->objectIndex == STRUCT_OBJECT_INDEX.
// incRef: bumps the count. decRef: drops the count. Never frees on its own; the per-frame sweep (Runner_sweepDeadStructs) is the single point that physically frees a struct (after dropping the registry's implicit ref).
void Instance_structIncRef(Instance* inst);
void Instance_structDecRef(Instance* inst);

// Deep-copy all mutable fields from source to dst: built-in properties, alarms, selfVars.
// Does NOT copy instanceId, objectIndex, destroyed, or createEventFired. Strings are duplicated so ownership stays independent. Arrays bump refCount (shared - CoW handles forking on first write).
void Instance_copyFields(Instance* source, Instance* dst);

// Get a self variable by varID. Returns RVALUE_UNDEFINED if absent. The returned RValue is non-owning (weak view - do not RValue_free unless you incRef/strdup first to strengthen).
static inline RValue Instance_getSelfVar(Instance* inst, int32_t varID) {
    requireNotNull(inst);
    return IntRValueHashMap_get(&inst->selfVars, varID);
}

// Set a self variable by varID. Frees the old value if present (decRefs owned arrays).
// Always takes an independent reference with RValue_makeIndependent.
// The caller retains ownership of their original `val` and remains responsible for freeing it (via RValue_free) when done.
static inline void Instance_setSelfVar(Instance* inst, int32_t varID, RValue val) {
    requireNotNull(inst);
    // One lookup: returns the existing slot, or inserts UNDEFINED and returns the new slot.
    RValue* pointerToSlot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varID);
    RValue independentVal = RValue_makeIndependent(val);
    RValue_free(pointerToSlot);
    *pointerToSlot = independentVal;
}

// Recompute speed/direction from hspeed/vspeed (called when hspeed or vspeed is set)
void Instance_computeSpeedFromComponents(Instance* inst);
// Recompute hspeed/vspeed from speed/direction (called when speed or direction is set)
void Instance_computeComponentsFromSpeed(Instance* inst);

char* Instance_toStringFancy(Instance* inst, DataWin* dataWin);

#endif /* _BS_INSTANCE_H_ */
