#include "instance.h"
#include "vm.h"

#include <stdlib.h>
#include "string_compat.h"
#include "math_compat.h"

#include "stb_ds.h"
#include "utils.h"
#include "int_rvalue_hashmap.h"

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) {
    Instance* inst = (Instance *)safeCalloc(1, sizeof(Instance));
    inst->instanceId = instanceId;
    inst->objectIndex = objectIndex;
    inst->refCount = 0;
    inst->pinned = false;
    inst->structRegistryIndex = -1;
    inst->constructorCodeIndex = -1;
    inst->staticParent = nullptr;
    inst->x = (float) x;
    inst->y = (float) y;
    inst->xprevious = (float) x;
    inst->yprevious = (float) y;
    inst->xstart = (float) x;
    inst->ystart = (float) y;
    inst->maskIndex = -1;
    inst->persistent = false;
    inst->solid = false;
    inst->active = true;
    inst->visible = true;
    inst->destroyed = false;
    inst->outsideRoom = false;
    inst->spatialGridDirty = false;
    inst->spriteIndex = -1;
    inst->imageSpeed = 1.0f;
    inst->imageIndex = 0.0f;
    inst->imageXscale = 1.0f;
    inst->imageYscale = 1.0f;
    inst->imageAngle = 0.0f;
    inst->imageAlpha = 1.0f;
    inst->imageBlend = 0xFFFFFF;
    inst->depth = 0;
    inst->layer = -1;
    inst->speed = 0.0f;
    inst->direction = 0.0f;
    inst->hspeed = 0.0f;
    inst->vspeed = 0.0f;
    inst->friction = 0.0f;
    inst->gravity = 0.0f;
    inst->gravityDirection = 270.0f;
    inst->pathIndex = -1;
    inst->pathScale = 1.0f;
    inst->timelineIndex = -1;
    inst->timelinePosition = 0.0f;
    inst->timelineSpeed = 1.0f;
    inst->timelineLoop = false;
    inst->timelineRunning = true;

    // Initialize alarms to -1 (inactive)
    repeat(GML_ALARM_COUNT, i) {
        inst->alarm[i] = -1;
    }

    return inst;
}

void Instance_structIncRef(Instance* inst) {
    if (inst == nullptr) return;
    if (inst->pinned) return;
    inst->refCount++;
}

void Instance_structDecRef(Instance* inst) {
    if (inst == nullptr) return;
    if (inst->pinned) return;
    require(inst->refCount > 0);
    inst->refCount--;
    // Never free here. The runner-side sweep reaps structs whose refCount has dropped to 1. (that is: when only the structInstances holds it)
}

uint32_t Instance_getInstanceId(Instance* inst) {
    return inst != nullptr ? inst->instanceId : 0;
}

void Instance_freeContents(Instance* instance) {
    if (instance == nullptr) return;

    // Free owned strings and decRef owned arrays in selfVars hashmap, then release the entries buffer.
    IntRValueHashMap_freeAllValues(&instance->selfVars);
    arrfree(instance->collisionCells);
    instance->collisionCells = nullptr;
}

void Instance_free(Instance* instance) {
    if (instance == nullptr) return;

    Instance_freeContents(instance);
    free(instance);
}

void Instance_copyFields(Instance* source, Instance* destination) {
    destination->x = source->x;
    destination->y = source->y;
    destination->xprevious = source->xprevious;
    destination->yprevious = source->yprevious;
    destination->xstart = source->xstart;
    destination->ystart = source->ystart;
    destination->persistent = source->persistent;
    destination->solid = source->solid;
    destination->active = source->active;
    destination->visible = source->visible;
    destination->outsideRoom = source->outsideRoom;
    destination->maskIndex = source->maskIndex;
    destination->spriteIndex = source->spriteIndex;
    destination->imageSpeed = source->imageSpeed;
    destination->imageIndex = source->imageIndex;
    destination->imageXscale = source->imageXscale;
    destination->imageYscale = source->imageYscale;
    destination->imageAngle = source->imageAngle;
    destination->imageAlpha = source->imageAlpha;
    destination->imageBlend = source->imageBlend;
    destination->depth = source->depth;
    destination->layer = source->layer;
    destination->speed = source->speed;
    destination->direction = source->direction;
    destination->hspeed = source->hspeed;
    destination->vspeed = source->vspeed;
    destination->friction = source->friction;
    destination->gravity = source->gravity;
    destination->gravityDirection = source->gravityDirection;
    destination->pathIndex = source->pathIndex;
    destination->pathPosition = source->pathPosition;
    destination->pathPositionPrevious = source->pathPositionPrevious;
    destination->pathSpeed = source->pathSpeed;
    destination->pathScale = source->pathScale;
    destination->pathOrientation = source->pathOrientation;
    destination->pathEndAction = source->pathEndAction;
    destination->pathXStart = source->pathXStart;
    destination->pathYStart = source->pathYStart;
    repeat(GML_ALARM_COUNT, i) {
        destination->alarm[i] = source->alarm[i];
    }
    destination->activeAlarmMask = source->activeAlarmMask;
    destination->timelineIndex = source->timelineIndex;
    destination->timelinePosition = source->timelinePosition;
    destination->timelineSpeed = source->timelineSpeed;
    destination->timelineLoop = source->timelineLoop;
    destination->timelineRunning = source->timelineRunning;

    // Deep-copy self variables (Instance_setSelfVar handles string duplication + array incRef)
    {
    repeat(source->selfVars.capacity, i) {
        IntRValueEntry* entry = &source->selfVars.entries[i];
        if (entry->key != INT_RVALUE_HASHMAP_EMPTY_KEY) {
            Instance_setSelfVar(destination, entry->key, entry->value);
        }
    }
    }
}

// Compute speed and direction from hspeed/vspeed (HTML5: Compute_Speed1)
void Instance_computeSpeedFromComponents(Instance* inst) {
    // Direction
    if (inst->hspeed == 0.0f) {
        if (inst->vspeed > 0.0f) {
            inst->direction = 270.0f;
        } else if (inst->vspeed < 0.0f) {
            inst->direction = 90.0f;
        }
        // If both are 0, direction stays unchanged
    } else {
        GMLReal dd = clampFloat(180.0 * GMLReal_atan2(inst->vspeed, inst->hspeed) / M_PI);
        if (dd <= 0.0) {
            inst->direction = (float) -dd;
        } else {
            inst->direction = (float) (360.0 - dd);
        }
    }

    // Round direction if very close to integer
    if (GMLReal_fabs(inst->direction - GMLReal_round(inst->direction)) < 0.0001) {
        inst->direction = (float) GMLReal_round(inst->direction);
    }
    inst->direction = (float) GMLReal_fmod((GMLReal) inst->direction, 360.0);

    // Speed
    inst->speed = (float) GMLReal_sqrt(inst->hspeed * inst->hspeed + inst->vspeed * inst->vspeed);
    if (GMLReal_fabs(inst->speed - GMLReal_round(inst->speed)) < 0.0001) {
        inst->speed = (float) GMLReal_round(inst->speed);
    }
}

// Compute hspeed/vspeed from speed and direction (HTML5: Compute_Speed2)
void Instance_computeComponentsFromSpeed(Instance* inst) {
    inst->hspeed = (float) (inst->speed * clampFloat(GMLReal_cos(inst->direction * (M_PI / 180.0))));
    inst->vspeed = (float) (-inst->speed * clampFloat(GMLReal_sin(inst->direction * (M_PI / 180.0))));

    // Round if very close to integer
    if (GMLReal_fabs(inst->hspeed - GMLReal_round(inst->hspeed)) < 0.0001) {
        inst->hspeed = (float) GMLReal_round(inst->hspeed);
    }
    if (GMLReal_fabs(inst->vspeed - GMLReal_round(inst->vspeed)) < 0.0001) {
        inst->vspeed = (float) GMLReal_round(inst->vspeed);
    }
}

char* Instance_toStringFancy(Instance* inst, DataWin* dataWin) {
    const IntRValueHashMap* map = &inst->selfVars;
    if (map->capacity == 0) return safeStrdup("{}");
    const IntRValueEntry* entries = map->entries;
    uint32_t mask = map->mask;

    size_t requiredSize = 4; // "{ }\0"
    bool first = true;
    for (uint32_t idx = 0; idx <= mask; ++idx) {
        int32_t slotKey = entries[idx].key;
        if (slotKey == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;

        const char* name = "?";
        if (dataWin != nullptr) {
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->instanceType == INSTANCE_SELF && var->varID == slotKey) {
                    name = var->name;
                    break;
                }
            }
        }

        if (name == nullptr) name = "?";
        RValue val = entries[idx].value;
        char* valStr = RValue_toStringFancy(val, dataWin);
        requiredSize += strlen(name) + strlen(valStr) + (first ? 0 : 2) + 5; // "name : value" + separators
        free(valStr);
        first = false;
    }

    size_t bufSize = requiredSize > 128 ? requiredSize : 128;
    char* buf = (char*) safeCalloc(bufSize, sizeof(char));
    size_t pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "{ ");
    first = true;
    {
    for (uint32_t idx = 0; idx <= mask; ++idx) {
        int32_t slotKey = entries[idx].key;
        if (slotKey != INT_RVALUE_HASHMAP_EMPTY_KEY) {
            const char* name = "?";
            if (dataWin != nullptr) {
                repeat(dataWin->vari.variableCount, varIdx) {
                    Variable* var = &dataWin->vari.variables[varIdx];
                    if (var->instanceType == INSTANCE_SELF && var->varID == slotKey) {
                        name = var->name;
                        break;
                    }
                }
            }
            if (name == nullptr) name = "?";

            RValue val = entries[idx].value;
            char* valStr = RValue_toStringFancy(val, dataWin);
            if (!first) {
                pos += snprintf(buf + pos, bufSize - pos, ", ");
            }
            first = false;
            pos += snprintf(buf + pos, bufSize - pos, "%s : %s", name, valStr);
            free(valStr);
        }
    }
    }
    pos += snprintf(buf + pos, bufSize - pos, " }");
    return buf;
}
