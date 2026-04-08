#include "vm_builtins.h"
#include "instance.h"
#include "json_reader.h"
#include "runner.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rvalue.h"
#include "stb_ds.h"
#include "text_utils.h"
#include "collision.h"
#include "ini.h"
#include "audio_system.h"
#include "file_system.h"

#define MAX_VIEWS 8
#define MAX_BACKGROUNDS 8

// ===[ BUILTIN FUNCTION REGISTRY ]===
typedef struct {
    char* key;
    BuiltinFunc value;
} BuiltinEntry;

static bool initialized = false;
static BuiltinEntry* builtinMap = nullptr;

static void registerBuiltin(const char* name, BuiltinFunc func) {
    requireMessage(shgeti(builtinMap, name) == -1, "Trying to register an already registered builtin function!");
    shput(builtinMap, (char*) name, func);
}

BuiltinFunc VMBuiltins_find(const char* name) {
    ptrdiff_t idx = shgeti(builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    return builtinMap[idx].value;
}

// ===[ STUB LOGGING ]===

static void logStubbedFunction(VMContext* ctx, const char* funcName) {
    const char* callerName = VM_getCallerName(ctx);
    char* dedupKey = VM_createDedupKey(callerName, funcName);

    if (0 > shgeti(ctx->loggedStubbedFuncs, dedupKey)) {
        // shput stores the key pointer, so don't free it when inserting
        shput(ctx->loggedStubbedFuncs, dedupKey, true);
        fprintf(stderr, "VM: [%s] Stubbed function \"%s\"!\n", callerName, funcName);
    } else {
        free(dedupKey);
    }
}

// ===[ DS_MAP SYSTEM ]===

typedef struct {
    char* key;
    RValue value;
} DsMapEntry;

static DsMapEntry** dsMapPool = nullptr; // stb_ds array of shash maps

static int32_t dsMapCreate(void) {
    DsMapEntry* newMap = nullptr;
    int32_t id = (int32_t) arrlen(dsMapPool);
    arrput(dsMapPool, newMap);
    return id;
}

static DsMapEntry** dsMapGet(int32_t id) {
    if (id < 0 || (int32_t) arrlen(dsMapPool) <= id) return nullptr;
    return &dsMapPool[id];
}

// ===[ DS_LIST SYSTEM ]===

typedef struct {
    RValue* items; // stb_ds dynamic array of RValues
} DsList;

static DsList* dsListPool = nullptr; // stb_ds array of DsList

static int32_t dsListCreate(void) {
    DsList newList = { .items = nullptr };
    int32_t id = (int32_t) arrlen(dsListPool);
    arrput(dsListPool, newList);
    return id;
}

static DsList* dsListGet(int32_t id) {
    if (0 > id || id > (int32_t) arrlen(dsListPool)) return nullptr;
    return &dsListPool[id];
}

// ===[ BUILT-IN VARIABLE GET/SET ]===

/**
 * Gets the argument number from the name
 *
 * If it returns -1, then the name is not an argument variable
 *
 * @param name The name
 * @return The argument number, -1 if it is not an argument variable
 */
static int extractArgumentNumber(const char* name) {
    if (strncmp(name, "argument", 8) == 0) {
        char* end;
        long argNumber = strtol(name + 8, &end, 10);
        if (end == name + 8 || *end != '\0' || 0 > argNumber || argNumber > 15) return -1;
        return (int) argNumber;
    }
    return -1;
}

static bool isValidAlarmIndex(int alarmIndex) {
    return alarmIndex >= 0 && GML_ALARM_COUNT > alarmIndex;
}

RValue VMBuiltins_getVariable(VMContext* ctx, const char* name, int32_t arrayIndex) {
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = (Runner*) ctx->runner;

    // File system
    if (strcmp(name, "working_directory") == 0) {
        FileSystem* fs = runner->fileSystem;
        char* path = fs->vtable->resolvePath(fs, "");
        return RValue_makeOwnedString(path);
    }

    // OS constants
    if (strcmp(name, "os_type") == 0) return RValue_makeReal(4.0); // os_linux
    if (strcmp(name, "os_windows") == 0) return RValue_makeReal(0.0);
    if (strcmp(name, "os_ps4") == 0) return RValue_makeReal(6.0);
    if (strcmp(name, "os_psvita") == 0) return RValue_makeReal(12.0);
    if (strcmp(name, "os_3ds") == 0) return RValue_makeReal(14.0);
    if (strcmp(name, "os_switch_") == 0) return RValue_makeReal(19.0);

    // Per-instance properties
    if (inst != nullptr) {
        if (strcmp(name, "image_speed") == 0) return RValue_makeReal(inst->imageSpeed);
        if (strcmp(name, "image_index") == 0) return RValue_makeReal(inst->imageIndex);
        if (strcmp(name, "image_xscale") == 0) return RValue_makeReal(inst->imageXscale);
        if (strcmp(name, "image_yscale") == 0) return RValue_makeReal(inst->imageYscale);
        if (strcmp(name, "image_angle") == 0) return RValue_makeReal(inst->imageAngle);
        if (strcmp(name, "image_alpha") == 0) return RValue_makeReal(inst->imageAlpha);
        if (strcmp(name, "image_blend") == 0) return RValue_makeReal((GMLReal) inst->imageBlend);
        if (strcmp(name, "image_number") == 0) {
            if (inst->spriteIndex >= 0) {
                Sprite* sprite = &ctx->runner->dataWin->sprt.sprites[inst->spriteIndex];
                return RValue_makeReal((GMLReal) sprite->textureCount);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "sprite_index") == 0) return RValue_makeReal((GMLReal) inst->spriteIndex);
        if (strcmp(name, "sprite_width") == 0) {
            if (inst->spriteIndex >= 0 && runner != nullptr && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
                return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].width * inst->imageXscale);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "sprite_height") == 0) {
            if (inst->spriteIndex >= 0 && runner != nullptr && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
                return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].height * inst->imageYscale);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "bbox_left") == 0 || strcmp(name, "bbox_right") == 0 || strcmp(name, "bbox_top") == 0 || strcmp(name, "bbox_bottom") == 0) {
            if (runner != nullptr) {
                InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
                if (bbox.valid) {
                    if (strcmp(name, "bbox_left") == 0) return RValue_makeReal(bbox.left);
                    if (strcmp(name, "bbox_right") == 0) return RValue_makeReal(bbox.right);
                    if (strcmp(name, "bbox_top") == 0) return RValue_makeReal(bbox.top);
                    return RValue_makeReal(bbox.bottom);
                }
            }
            if (strcmp(name, "bbox_left") == 0 || strcmp(name, "bbox_right") == 0) return RValue_makeReal(inst->x);
            return RValue_makeReal(inst->y);
        }
        if (strcmp(name, "visible") == 0) return RValue_makeBool(inst->visible);
        if (strcmp(name, "depth") == 0) return RValue_makeReal((GMLReal) inst->depth);
        if (strcmp(name, "x") == 0) return RValue_makeReal(inst->x);
        if (strcmp(name, "y") == 0) return RValue_makeReal(inst->y);
        if (strcmp(name, "xprevious") == 0) return RValue_makeReal(inst->xprevious);
        if (strcmp(name, "yprevious") == 0) return RValue_makeReal(inst->yprevious);
        if (strcmp(name, "xstart") == 0) return RValue_makeReal(inst->xstart);
        if (strcmp(name, "ystart") == 0) return RValue_makeReal(inst->ystart);
        if (strcmp(name, "mask_index") == 0) return RValue_makeReal((GMLReal) inst->maskIndex);
        if (strcmp(name, "id") == 0) return RValue_makeReal((GMLReal) inst->instanceId);
        if (strcmp(name, "object_index") == 0) return RValue_makeReal((GMLReal) inst->objectIndex);
        if (strcmp(name, "persistent") == 0) return RValue_makeBool(inst->persistent);
        if (strcmp(name, "solid") == 0) return RValue_makeBool(inst->solid);
        if (strcmp(name, "speed") == 0) return RValue_makeReal(inst->speed);
        if (strcmp(name, "direction") == 0) return RValue_makeReal(inst->direction);
        if (strcmp(name, "hspeed") == 0) return RValue_makeReal(inst->hspeed);
        if (strcmp(name, "vspeed") == 0) return RValue_makeReal(inst->vspeed);
        if (strcmp(name, "friction") == 0) return RValue_makeReal(inst->friction);
        if (strcmp(name, "gravity") == 0) return RValue_makeReal(inst->gravity);
        if (strcmp(name, "gravity_direction") == 0) return RValue_makeReal(inst->gravityDirection);
        if (strcmp(name, "alarm") == 0) {
            if (isValidAlarmIndex(arrayIndex)) {
                return RValue_makeReal((GMLReal) inst->alarm[arrayIndex]);
            }
            return RValue_makeReal(-1.0);
        }

        // Path instance variables
        if (strcmp(name, "path_index") == 0) return RValue_makeReal((GMLReal) inst->pathIndex);
        if (strcmp(name, "path_position") == 0) return RValue_makeReal(inst->pathPosition);
        if (strcmp(name, "path_positionprevious") == 0) return RValue_makeReal(inst->pathPositionPrevious);
        if (strcmp(name, "path_speed") == 0) return RValue_makeReal(inst->pathSpeed);
        if (strcmp(name, "path_scale") == 0) return RValue_makeReal(inst->pathScale);
        if (strcmp(name, "path_orientation") == 0) return RValue_makeReal(inst->pathOrientation);
        if (strcmp(name, "path_endaction") == 0) return RValue_makeReal((GMLReal) inst->pathEndAction);
    }

    // Room properties
    if (runner != nullptr) {
        if (strcmp(name, "room") == 0) return RValue_makeReal((GMLReal) runner->currentRoomIndex);
        if (strcmp(name, "room_speed") == 0) return RValue_makeReal((GMLReal) runner->currentRoom->speed);
        if (strcmp(name, "room_width") == 0) return RValue_makeReal((GMLReal) runner->currentRoom->width);
        if (strcmp(name, "room_height") == 0) return RValue_makeReal((GMLReal) runner->currentRoom->height);
        if (strcmp(name, "room_persistent") == 0) return RValue_makeBool(runner->currentRoom->persistent);
        if (strcmp(name, "view_current") == 0) return RValue_makeReal((GMLReal) runner->viewCurrent);
        if (strcmp(name, "view_xview") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewX);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_yview") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewY);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_wview") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewWidth);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_hview") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewHeight);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_xport") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portX);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_yport") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portY);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_wport") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portWidth);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_hport") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portHeight);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_visible") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeBool(runner->currentRoom->views[arrayIndex].enabled);
            }
            return RValue_makeBool(false);
        }
        if (strcmp(name, "view_angle") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->viewAngles[arrayIndex]);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_hborder") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].borderX);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_vborder") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].borderY);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_object") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].objectId);
            }
            return RValue_makeReal(-4.0);
        }
        if (strcmp(name, "view_hspeed") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].speedX);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "view_vspeed") == 0) {
            if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
                return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].speedY);
            }
            return RValue_makeReal(0.0);
        }

        // Background properties
        if (strcmp(name, "background_visible") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeBool(runner->backgrounds[arrayIndex].visible);
            return RValue_makeBool(false);
        }
        if (strcmp(name, "background_index") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].backgroundIndex);
            return RValue_makeReal(-1.0);
        }
        if (strcmp(name, "background_x") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].x);
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "background_y") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].y);
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "background_hspeed") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].speedX);
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "background_vspeed") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].speedY);
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "background_width") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
                int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, runner->backgrounds[arrayIndex].backgroundIndex);
                if (tpagIndex >= 0) return RValue_makeReal((GMLReal) runner->dataWin->tpag.items[tpagIndex].boundingWidth);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "background_height") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
                int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, runner->backgrounds[arrayIndex].backgroundIndex);
                if (tpagIndex >= 0) return RValue_makeReal((GMLReal) runner->dataWin->tpag.items[tpagIndex].boundingHeight);
            }
            return RValue_makeReal(0.0);
        }
        if (strcmp(name, "background_alpha") == 0) {
            if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].alpha);
            return RValue_makeReal(1.0);
        }
        if (strcmp(name, "background_color") == 0 || strcmp(name, "background_colour") == 0) {
            return RValue_makeReal((GMLReal) runner->backgroundColor);
        }
    }

    // Timing
    if (strcmp(name, "current_time") == 0) {
        #ifdef _WIN32
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        GMLReal ms = (GMLReal) counter.QuadPart / (GMLReal) freq.QuadPart * 1000.0;
        #else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        GMLReal ms = (GMLReal) ts.tv_sec * 1000.0 + (GMLReal) ts.tv_nsec / 1000000.0;
        #endif
        return RValue_makeReal(ms);
    }

    // argument_count
    if (strcmp(name, "argument_count") == 0) return RValue_makeReal((GMLReal) ctx->scriptArgCount);

    // argument[N] - array-style access to script arguments
    if (strcmp(name, "argument") == 0) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
            RValue val = ctx->scriptArgs[arrayIndex];
            val.ownsString = false;
            return val;
        }
        return RValue_makeUndefined();
    }

    // Argument variables (argument0..argument15 are built-in in GMS bytecode, stored in scriptArgs)
    const int argNumber = extractArgumentNumber(name);
    if (argNumber != -1) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argNumber) {
            RValue val = ctx->scriptArgs[argNumber];
            val.ownsString = false;
            return val;
        }
        return RValue_makeUndefined();
    }

    // Keyboard variables
    if (runner != nullptr) {
        if (strcmp(name, "keyboard_key") == 0) return RValue_makeReal((GMLReal) runner->keyboard->lastKey);
        if (strcmp(name, "keyboard_lastkey") == 0) return RValue_makeReal((GMLReal) runner->keyboard->lastKey);
    }

    // Surfaces
    if (strcmp(name, "application_surface") == 0) return RValue_makeReal(-1.0); // sentinel ID for the application surface

    // Constants that GMS defines
    if (strcmp(name, "true") == 0) return RValue_makeBool(true);
    if (strcmp(name, "false") == 0) return RValue_makeBool(false);
    if (strcmp(name, "pi") == 0) return RValue_makeReal(3.14159265358979323846);
    if (strcmp(name, "undefined") == 0) return RValue_makeUndefined();

    // Path action constants
    if (strcmp(name, "path_action_stop") == 0) return RValue_makeReal(0.0);
    if (strcmp(name, "path_action_restart") == 0) return RValue_makeReal(1.0);
    if (strcmp(name, "path_action_continue") == 0) return RValue_makeReal(2.0);
    if (strcmp(name, "path_action_reverse") == 0) return RValue_makeReal(3.0);

    if (strcmp(name, "fps") == 0) return RValue_makeReal(ctx->dataWin->gen8.gms2FPS);

    fprintf(stderr, "VM: Unhandled built-in variable read '%s' (arrayIndex=%d)\n", name, arrayIndex);
    return RValue_makeReal(0.0);
}

void VMBuiltins_setVariable(VMContext* ctx, const char* name, RValue val, int32_t arrayIndex) {
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = (Runner*) requireNotNullMessage(ctx->runner, "VM: setVariable called but no runner!");

    // Per-instance properties
    if (inst != nullptr) {
        if (strcmp(name, "image_speed") == 0) { inst->imageSpeed = (float) RValue_toReal(val); return; }
        if (strcmp(name, "image_index") == 0) { inst->imageIndex = (float) RValue_toReal(val); return; }
        if (strcmp(name, "image_xscale") == 0) { inst->imageXscale = (float) RValue_toReal(val); return; }
        if (strcmp(name, "image_yscale") == 0) { inst->imageYscale = (float) RValue_toReal(val); return; }
        if (strcmp(name, "image_angle") == 0) { inst->imageAngle = (float) RValue_toReal(val); return; }
        if (strcmp(name, "image_alpha") == 0) { inst->imageAlpha = (float) RValue_toReal(val); return; }
        if (strcmp(name, "image_blend") == 0) { inst->imageBlend = (uint32_t) RValue_toReal(val); return; }
        if (strcmp(name, "sprite_index") == 0) { inst->spriteIndex = RValue_toInt32(val); return; }
        if (strcmp(name, "visible") == 0) { inst->visible = RValue_toBool(val); return; }
        if (strcmp(name, "depth") == 0) { inst->depth = RValue_toInt32(val); return; }
        if (strcmp(name, "x") == 0) { inst->x = (float) RValue_toReal(val); return; }
        if (strcmp(name, "y") == 0) { inst->y = (float) RValue_toReal(val); return; }
        if (strcmp(name, "persistent") == 0) { inst->persistent = RValue_toBool(val); return; }
        if (strcmp(name, "solid") == 0) { inst->solid = RValue_toBool(val); return; }
        if (strcmp(name, "xprevious") == 0) { inst->xprevious = (float) RValue_toReal(val); return; }
        if (strcmp(name, "yprevious") == 0) { inst->yprevious = (float) RValue_toReal(val); return; }
        if (strcmp(name, "xstart") == 0) { inst->xstart = (float) RValue_toReal(val); return; }
        if (strcmp(name, "ystart") == 0) { inst->ystart = (float) RValue_toReal(val); return; }
        if (strcmp(name, "mask_index") == 0) { inst->maskIndex = RValue_toInt32(val); return; }
        if (strcmp(name, "speed") == 0) { inst->speed = (float) RValue_toReal(val); Instance_computeComponentsFromSpeed(inst); return; }
        if (strcmp(name, "direction") == 0) {
            GMLReal d = GMLReal_fmod(RValue_toReal(val), 360.0);
            if (d < 0.0) d += 360.0;
            inst->direction = (float) d;
            Instance_computeComponentsFromSpeed(inst);
            return;
        }
        if (strcmp(name, "hspeed") == 0) { inst->hspeed = (float) RValue_toReal(val); Instance_computeSpeedFromComponents(inst); return; }
        if (strcmp(name, "vspeed") == 0) { inst->vspeed = (float) RValue_toReal(val); Instance_computeSpeedFromComponents(inst); return; }
        if (strcmp(name, "friction") == 0) { inst->friction = (float) RValue_toReal(val); return; }
        if (strcmp(name, "gravity") == 0) { inst->gravity = (float) RValue_toReal(val); return; }
        if (strcmp(name, "gravity_direction") == 0) { inst->gravityDirection = (float) RValue_toReal(val); return; }
        if (strcmp(name, "alarm") == 0) {
            if (isValidAlarmIndex(arrayIndex)) {
                int32_t newValue = RValue_toInt32(val);

#ifndef DISABLE_VM_TRACING
                if (shgeti(ctx->alarmsToBeTraced, "*") != -1 || shgeti(ctx->alarmsToBeTraced, runner->dataWin->objt.objects[inst->objectIndex].name) != -1) {
                    fprintf(stderr, "VM: [%s] Setting Alarm[%d] = %d (instanceId=%d)\n", runner->dataWin->objt.objects[inst->objectIndex].name, arrayIndex, newValue, inst->instanceId);
                }
#endif

                inst->alarm[arrayIndex] = newValue;
            }
            return;
        }

        // Path instance variables (writable)
        if (strcmp(name, "path_position") == 0) {
            // Native GMS runner clamps path_position to [0.0, 1.0] on set
            float pos = (float) RValue_toReal(val);
            if (pos < 0.0f) pos = 0.0f;
            else if (pos > 1.0f) pos = 1.0f;
            inst->pathPosition = pos;
            return;
        }
        if (strcmp(name, "path_speed") == 0) { inst->pathSpeed = (float) RValue_toReal(val); return; }
        if (strcmp(name, "path_scale") == 0) { inst->pathScale = (float) RValue_toReal(val); return; }
        if (strcmp(name, "path_orientation") == 0) { inst->pathOrientation = (float) RValue_toReal(val); return; }
        if (strcmp(name, "path_endaction") == 0) { inst->pathEndAction = RValue_toInt32(val); return; }
    }

    // Keyboard variables
    if (strcmp(name, "keyboard_key") == 0) {
        runner->keyboard->lastKey = RValue_toInt32(val);
        return;
    }
    if (strcmp(name, "keyboard_lastkey") == 0) {
        runner->keyboard->lastKey = RValue_toInt32(val);
        return;
    }

    // View properties
    if (strcmp(name, "view_xview") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].viewX = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_yview") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].viewY = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_wview") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].viewWidth = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_hview") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].viewHeight = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_xport") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].portX = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_yport") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].portY = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_wport") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].portWidth = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_hport") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].portHeight = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_visible") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].enabled = RValue_toBool(val);
        }
        return;
    }
    if (strcmp(name, "view_angle") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->viewAngles[arrayIndex] = (float) RValue_toReal(val);
        }
        return;
    }
    if (strcmp(name, "view_hborder") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].borderX = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_vborder") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].borderY = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_object") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].objectId = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_hspeed") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].speedX = RValue_toInt32(val);
        }
        return;
    }
    if (strcmp(name, "view_vspeed") == 0) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
            runner->currentRoom->views[arrayIndex].speedY = RValue_toInt32(val);
        }
        return;
    }

    // Background properties
    if (strcmp(name, "background_visible") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].visible = RValue_toBool(val);
        return;
    }
    if (strcmp(name, "background_index") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].backgroundIndex = RValue_toInt32(val);
        return;
    }
    if (strcmp(name, "background_x") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].x = (float) RValue_toReal(val);
        return;
    }
    if (strcmp(name, "background_y") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].y = (float) RValue_toReal(val);
        return;
    }
    if (strcmp(name, "background_hspeed") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].speedX = (float) RValue_toReal(val);
        return;
    }
    if (strcmp(name, "background_vspeed") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].speedY = (float) RValue_toReal(val);
        return;
    }
    if (strcmp(name, "background_alpha") == 0) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].alpha = (float) RValue_toReal(val);
        return;
    }
    if (strcmp(name, "background_color") == 0 || strcmp(name, "background_colour") == 0) {
        runner->backgroundColor = (uint32_t) RValue_toInt32(val);
        return;
    }

    // Room properties
    if (strcmp(name, "room") == 0) {
        runner->pendingRoom = RValue_toInt32(val);
        return;
    }
    if (strcmp(name, "room_persistent") == 0) {
        runner->currentRoom->persistent = RValue_toBool(val);
        return;
    }
    if (strcmp(name, "room_width") == 0) {
        runner->currentRoom->width = (uint32_t) RValue_toInt32(val);
        return;
    }
    if (strcmp(name, "room_height") == 0) {
        runner->currentRoom->height = (uint32_t) RValue_toInt32(val);
        return;
    }
    if (strcmp(name, "room_speed") == 0) {
        runner->currentRoom->speed = (uint32_t) RValue_toInt32(val);
        return;
    }

    // Read-only variables (silently ignore)
    if (strcmp(name, "os_type") == 0 || strcmp(name, "os_windows") == 0 ||
        strcmp(name, "os_ps4") == 0 || strcmp(name, "os_psvita") == 0 ||
        strcmp(name, "id") == 0 || strcmp(name, "object_index") == 0 ||
        strcmp(name, "current_time") == 0 ||
        strcmp(name, "view_current") == 0 || strcmp(name, "path_index") == 0) {
        fprintf(stderr, "VM: Warning - attempted write to read-only built-in '%s'\n", name);
        return;
    }

    // argument[N] - array-style write to script arguments
    if (strcmp(name, "argument") == 0) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
            RValue_free(&ctx->scriptArgs[arrayIndex]);
            ctx->scriptArgs[arrayIndex] = val;
        }
        return;
    }

    // Argument variables
    const int argNumber = extractArgumentNumber(name);
    if (argNumber != -1) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argNumber) {
            RValue_free(&ctx->scriptArgs[argNumber]);
            ctx->scriptArgs[argNumber] = val;
        }
        return;
    }

    fprintf(stderr, "VM: Unhandled built-in variable write '%s' (arrayIndex=%d)\n", name, arrayIndex);
}

// ===[ BUILTIN FUNCTION IMPLEMENTATIONS ]===

static RValue builtinShowDebugMessage(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[show_debug_message] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    char* val = RValue_toString(args[0]);
    printf("Game: %s\n", val);
    free(val);

    return RValue_makeUndefined();
}

static RValue builtinStringLength(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeInt32(0);
    // GML converts non-string arguments to string before measuring length
    char* str = RValue_toString(args[0]);
    int32_t len = (int32_t) strlen(str);
    free(str);
    return RValue_makeInt32(len);
}

static RValue builtinReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]));
}

static RValue builtinString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    char* result = RValue_toString(args[0]);
    return RValue_makeOwnedString(result);
}

static RValue builtinFloor(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_floor(RValue_toReal(args[0])));
}

static RValue builtinCeil(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_ceil(RValue_toReal(args[0])));
}

static RValue builtinRound(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_round(RValue_toReal(args[0])));
}

static RValue builtinAbs(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_fabs(RValue_toReal(args[0])));
}

static RValue builtinSign(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal val = RValue_toReal(args[0]);
    GMLReal result = (val > 0.0) ? 1.0 : ((0.0 > val) ? -1.0 : 0.0);
    return RValue_makeReal(result);
}

static RValue builtinMax(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal result = -INFINITY;
    repeat(argCount, i) {
        GMLReal val = RValue_toReal(args[i]);
        if (val > result) result = val;
    }
    return RValue_makeReal(result);
}

static RValue builtinMin(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal result = INFINITY;
    repeat(argCount, i) {
        GMLReal val = RValue_toReal(args[i]);
        if (result > val) result = val;
    }
    return RValue_makeReal(result);
}

static RValue builtinPower(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_pow(RValue_toReal(args[0]), RValue_toReal(args[1])));
}

static RValue builtinSqrt(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_sqrt(RValue_toReal(args[0])));
}

static RValue builtinSqr(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal val = RValue_toReal(args[0]);
    return RValue_makeReal(val * val);
}

static RValue builtinIsString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    return RValue_makeBool(args[0].type == RVALUE_STRING);
}

static RValue builtinIsReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    bool result = args[0].type == RVALUE_REAL || args[0].type == RVALUE_INT32 || args[0].type == RVALUE_INT64 || args[0].type == RVALUE_BOOL;
    return RValue_makeBool(result);
}

static RValue builtinIsUndefined(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(true);
    return RValue_makeBool(args[0].type == RVALUE_UNDEFINED);
}

// ===[ STRING FUNCTIONS ]===

static RValue builtinStringUpper(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    char* result = safeStrdup(args[0].string != nullptr ? args[0].string : "");
    for (char* p = result; *p; p++) *p = (char) toupper((unsigned char) *p);
    return RValue_makeOwnedString(result);
}

static RValue builtinStringLower(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    char* result = safeStrdup(args[0].string != nullptr ? args[0].string : "");
    for (char* p = result; *p; p++) *p = (char) tolower((unsigned char) *p);
    return RValue_makeOwnedString(result);
}

static RValue builtinStringCopy(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // GMS is 1-based
    int32_t len = RValue_toInt32(args[2]);
    int32_t strLen = (int32_t) strlen(str);

    if (0 > pos) pos = 0;
    if (pos >= strLen || 0 >= len) return RValue_makeOwnedString(safeStrdup(""));
    if (pos + len > strLen) len = strLen - pos;

    char* result = safeMalloc(len + 1);
    memcpy(result, str + pos, len);
    result[len] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringRepeat(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t count = RValue_toInt32(args[1]);
    if (0 >= count || str[0] == '\0') return RValue_makeOwnedString(safeStrdup(""));

    size_t strLen = strlen(str);
    size_t totalLen = strLen * (size_t) count;
    char* result = safeMalloc(totalLen + 1);
    repeat(count, i) {
        memcpy(result + i * strLen, str, strLen);
    }
    result[totalLen] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinOrd(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING || args[0].string == nullptr || args[0].string[0] == '\0') {
        return RValue_makeReal(0.0);
    }
    return RValue_makeReal((GMLReal) (unsigned char) args[0].string[0]);
}

static RValue builtinChr(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    char buf[2] = { (char) RValue_toInt32(args[0]), '\0' };
    return RValue_makeOwnedString(safeStrdup(buf));
}

static RValue builtinStringPos(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING) return RValue_makeReal(0.0);
    const char* needle = args[0].string != nullptr ? args[0].string : "";
    const char* haystack = args[1].string != nullptr ? args[1].string : "";
    const char* found = strstr(haystack, needle);
    if (found == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) (found - haystack + 1)); // 1-based
}

static RValue builtinStringCharAt(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // 1-based
    int32_t strLen = (int32_t) strlen(str);
    if (0 > pos || pos >= strLen) return RValue_makeOwnedString(safeStrdup(""));
    char buf[2] = { str[pos], '\0' };
    return RValue_makeOwnedString(safeStrdup(buf));
}

static RValue builtinStringDelete(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // 1-based
    int32_t count = RValue_toInt32(args[2]);
    int32_t strLen = (int32_t) strlen(str);

    if (0 > pos || pos >= strLen || 0 >= count) return RValue_makeOwnedString(safeStrdup(str));
    if (pos + count > strLen) count = strLen - pos;

    char* result = safeMalloc(strLen - count + 1);
    memcpy(result, str, pos);
    memcpy(result + pos, str + pos + count, strLen - pos - count);
    result[strLen - count] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringInsert(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* substr = args[0].string != nullptr ? args[0].string : "";
    const char* str = args[1].string != nullptr ? args[1].string : "";
    int32_t pos = RValue_toInt32(args[2]) - 1; // 1-based
    int32_t strLen = (int32_t) strlen(str);
    int32_t subLen = (int32_t) strlen(substr);

    if (0 > pos) pos = 0;
    if (pos > strLen) pos = strLen;

    char* result = safeMalloc(strLen + subLen + 1);
    memcpy(result, str, pos);
    memcpy(result + pos, substr, subLen);
    memcpy(result + pos + subLen, str + pos, strLen - pos);
    result[strLen + subLen] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringReplaceAll(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING || args[2].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    const char* needle = args[1].string != nullptr ? args[1].string : "";
    const char* replacement = args[2].string != nullptr ? args[2].string : "";
    int32_t needleLen = (int32_t) strlen(needle);
    if (0 == needleLen) return RValue_makeOwnedString(safeStrdup(str));
    int32_t replacementLen = (int32_t) strlen(replacement);

    // Count occurrences to pre-allocate
    int32_t count = 0;
    const char* p = str;
    while ((p = strstr(p, needle)) != nullptr) { count++; p += needleLen; }

    int32_t strLen = (int32_t) strlen(str);
    int32_t resultLen = strLen + count * (replacementLen - needleLen);
    char* result = safeMalloc(resultLen + 1);
    char* out = result;
    p = str;
    const char* match;
    while ((match = strstr(p, needle)) != nullptr) {
        int32_t before = (int32_t) (match - p);
        memcpy(out, p, before);
        out += before;
        memcpy(out, replacement, replacementLen);
        out += replacementLen;
        p = match + needleLen;
    }
    strcpy(out, p);
    return RValue_makeOwnedString(result);
}

// ===[ MATH FUNCTIONS ]===

static RValue builtinDarctan2(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal y = RValue_toReal(args[0]);
    GMLReal x = RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_atan2(y, x) * (180.0 / M_PI));
}

static RValue builtinSin(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_sin(RValue_toReal(args[0])));
}

static RValue builtinCos(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_cos(RValue_toReal(args[0])));
}

static RValue builtinDegtorad(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]) * (M_PI / 180.0));
}

static RValue builtinRadtodeg(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]) * (180.0 / M_PI));
}

static RValue builtinClamp(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    GMLReal val = RValue_toReal(args[0]);
    GMLReal lo = RValue_toReal(args[1]);
    GMLReal hi = RValue_toReal(args[2]);
    if (lo > val) val = lo;
    if (val > hi) val = hi;
    return RValue_makeReal(val);
}

static RValue builtinLerp(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    GMLReal a = RValue_toReal(args[0]);
    GMLReal b = RValue_toReal(args[1]);
    GMLReal t = RValue_toReal(args[2]);
    return RValue_makeReal(a + (b - a) * t);
}

static RValue builtinPointDistance(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) return RValue_makeReal(0.0);
    GMLReal dx = RValue_toReal(args[2]) - RValue_toReal(args[0]);
    GMLReal dy = RValue_toReal(args[3]) - RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_sqrt(dx * dx + dy * dy));
}

static RValue builtinDistanceToPoint(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);

    Instance* inst = ctx->currentInstance;
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;
    if (0 > sprIdx || (uint32_t) sprIdx >= ctx->dataWin->sprt.count) {
        return RValue_makeReal(0.0);
    }

    Sprite* spr = &ctx->dataWin->sprt.sprites[sprIdx];

    // Compute bounding box (no-rotation path)
    GMLReal bboxLeft = inst->x + inst->imageXscale * (spr->marginLeft - spr->originX);
    GMLReal bboxRight = inst->x + inst->imageXscale * ((spr->marginRight + 1) - spr->originX);
    if (bboxLeft > bboxRight) {
        GMLReal t = bboxLeft;
        bboxLeft = bboxRight;
        bboxRight = t;
    }

    GMLReal bboxTop = inst->y + inst->imageYscale * (spr->marginTop - spr->originY);
    GMLReal bboxBottom = inst->y + inst->imageYscale * ((spr->marginBottom + 1) - spr->originY);
    if (bboxTop > bboxBottom) {
        GMLReal t = bboxTop;
        bboxTop = bboxBottom;
        bboxBottom = t;
    }

    // Distance from point to nearest edge of bbox (0 if inside)
    GMLReal xd = 0.0;
    GMLReal yd = 0.0;
    if (px > bboxRight)  xd = px - bboxRight;
    if (px < bboxLeft)   xd = px - bboxLeft;
    if (py > bboxBottom) yd = py - bboxBottom;
    if (py < bboxTop)    yd = py - bboxTop;

    return RValue_makeReal(GMLReal_sqrt(xd * xd + yd * yd));
}

// distance_to_object(obj)
// Returns the minimum bbox-to-bbox distance between the calling instance and the nearest instance of the given object.
static RValue builtinDistanceToObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);

    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr) return RValue_makeReal(0.0);

    int32_t targetObjIndex = RValue_toInt32(args[0]);
    Instance* self = ctx->currentInstance;

    // Compute self bbox
    Sprite* selfSpr = Collision_getSprite(ctx->dataWin, self);
    if (selfSpr == nullptr) return RValue_makeReal(0.0);
    InstanceBBox selfBBox = Collision_computeBBox(ctx->dataWin, self);
    if (!selfBBox.valid) return RValue_makeReal(0.0);

    GMLReal minDist = 10000000000.0;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active || inst == self) continue;
        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox otherBBox = Collision_computeBBox(ctx->dataWin, inst);
        if (!otherBBox.valid) continue;

        GMLReal xd = 0.0;
        GMLReal yd = 0.0;
        if (otherBBox.left > selfBBox.right)  xd = otherBBox.left - selfBBox.right;
        if (selfBBox.left > otherBBox.right)  xd = selfBBox.left - otherBBox.right;
        if (otherBBox.top > selfBBox.bottom)  yd = otherBBox.top - selfBBox.bottom;
        if (selfBBox.top > otherBBox.bottom)  yd = selfBBox.top - otherBBox.bottom;

        GMLReal dist = GMLReal_sqrt(xd * xd + yd * yd);
        if (minDist > dist) minDist = dist;
    }

    return RValue_makeReal(minDist);
}

static RValue builtinPointDirection(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) return RValue_makeReal(0.0);
    GMLReal dx = RValue_toReal(args[2]) - RValue_toReal(args[0]);
    GMLReal dy = RValue_toReal(args[3]) - RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_atan2(-dy, dx) * (180.0 / M_PI));
}

static RValue builtinMoveTowardsPoint(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal targetX = RValue_toReal(args[0]);
    GMLReal targetY = RValue_toReal(args[1]);
    GMLReal spd = RValue_toReal(args[2]);
    Instance* inst = ctx->currentInstance;
    GMLReal dx = targetX - inst->x;
    GMLReal dy = targetY - inst->y;
    GMLReal dir = GMLReal_atan2(-dy, dx) * (180.0 / M_PI);
    if (dir < 0.0) dir += 360.0;
    inst->direction = (float) dir;
    inst->speed = (float) spd;
    Instance_computeComponentsFromSpeed(inst);
    return RValue_makeReal(0.0);
}

static RValue builtinMoveSnap(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal hsnap = RValue_toReal(args[0]);
    GMLReal vsnap = RValue_toReal(args[1]);
    Instance* inst = ctx->currentInstance;
    if (hsnap > 0.0) inst->x = (float) (GMLReal_floor((inst->x / hsnap) + 0.5) * hsnap);
    if (vsnap > 0.0) inst->y = (float) (GMLReal_floor((inst->y / vsnap) + 0.5) * vsnap);
    return RValue_makeReal(0.0);
}

static RValue builtinLengthdir_x(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal len = RValue_toReal(args[0]);
    GMLReal dir = RValue_toReal(args[1]) * (M_PI / 180.0);
    return RValue_makeReal(len * GMLReal_cos(dir));
}

static RValue builtinLengthdir_y(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal len = RValue_toReal(args[0]);
    GMLReal dir = RValue_toReal(args[1]) * (M_PI / 180.0);
    return RValue_makeReal(-len * GMLReal_sin(dir));
}

// ===[ RANDOM FUNCTIONS ]===

static RValue builtinRandom(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal n = RValue_toReal(args[0]);
    return RValue_makeReal(((GMLReal) rand() / (GMLReal) RAND_MAX) * n);
}

static RValue builtinRandomRange(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal lo = RValue_toReal(args[0]);
    GMLReal hi = RValue_toReal(args[1]);
    return RValue_makeReal(lo + ((GMLReal) rand() / (GMLReal) RAND_MAX) * (hi - lo));
}

static RValue builtinIrandom(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t n = RValue_toInt32(args[0]);
    if (0 >= n) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) (rand() % (n + 1)));
}

static RValue builtinIrandomRange(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    int32_t lo = RValue_toInt32(args[0]);
    int32_t hi = RValue_toInt32(args[1]);
    if (lo > hi) { int32_t tmp = lo; lo = hi; hi = tmp; }
    int32_t range = hi - lo + 1;
    if (0 >= range) return RValue_makeReal((GMLReal) lo);
    return RValue_makeReal((GMLReal) (lo + rand() % range));
}

static RValue builtinChoose(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t idx = rand() % argCount;
    // Must duplicate the value since args will be freed
    RValue val = args[idx];
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(val.string));
    }
    return val;
}

static RValue builtinRandomize(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (ctx->hasFixedSeed) return RValue_makeUndefined();
    srand((unsigned int) time(nullptr));
    return RValue_makeUndefined();
}

// ===[ ROOM FUNCTIONS ]===

static RValue builtinRoomGetName(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Room* room = &ctx->dataWin->room.rooms[RValue_toInt32(args[0])];
    return RValue_makeOwnedString(safeStrdup(room->name));
}

static RValue builtinRoomGotoNext(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto_next called but no runner!");

    int32_t nextPos = runner->currentRoomOrderPosition + 1;
    if ((int32_t) runner->dataWin->gen8.roomOrderCount > nextPos) {
        runner->pendingRoom = runner->dataWin->gen8.roomOrder[nextPos];
    } else {
        fprintf(stderr, "VM: room_goto_next - already at last room!\n");
    }
    return RValue_makeUndefined();
}

static RValue builtinRoomGotoPrevious(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto_previous called but no runner!");

    int32_t previousPos = runner->currentRoomOrderPosition - 1;
    if (previousPos >= 0) {
        runner->pendingRoom = runner->dataWin->gen8.roomOrder[previousPos];
    } else {
        fprintf(stderr, "VM: room_goto_previous - already at first room!\n");
    }
    return RValue_makeUndefined();
}

static RValue builtinRoomGoto(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto called but no runner!");
    runner->pendingRoom = RValue_toInt32(args[0]);
    return RValue_makeUndefined();
}

static RValue builtinRoomRestart(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_restart called but no runner!");
    runner->pendingRoom = runner->currentRoomIndex;
    return RValue_makeUndefined();
}

static RValue builtinRoomNext(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_next called but no runner!");
    int32_t roomId = RValue_toInt32(args[0]);
    DataWin* dw = runner->dataWin;
    repeat(dw->gen8.roomOrderCount, i) {
        if (dw->gen8.roomOrder[i] == roomId && dw->gen8.roomOrderCount > i + 1) {
            return RValue_makeReal(dw->gen8.roomOrder[i + 1]);
        }
    }
    return RValue_makeReal(-1);
}

static RValue builtinRoomPrevious(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_previous called but no runner!");
    int32_t roomId = RValue_toInt32(args[0]);
    DataWin* dw = runner->dataWin;
    repeat(dw->gen8.roomOrderCount, i) {
        if (dw->gen8.roomOrder[i] == roomId && i > 0) {
            return RValue_makeReal(dw->gen8.roomOrder[i - 1]);
        }
    }
    return RValue_makeReal(-1);
}

static RValue builtinRoomSetPersistent(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();

    int32_t roomId = RValue_toInt32(args[0]);
    bool persistent = RValue_toBool(args[1]);
    // The HTML5 room_set_persistent does do this (it checks if the room is null)
    if (0 > roomId || (uint32_t) roomId >= ctx->runner->dataWin->room.count) return RValue_makeUndefined();
    ctx->runner->dataWin->room.rooms[roomId].persistent = persistent;

    return RValue_makeUndefined();
}

// GMS2 camera compatibility - we treat view index as camera ID
static RValue builtinViewGetCamera(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    int32_t viewIndex = RValue_toInt32(args[0]);
    if (viewIndex >= 0 && MAX_VIEWS > viewIndex) {
        return RValue_makeReal((double) viewIndex);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewX(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_x called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewX);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewY(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_y called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewY);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewWidth(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_width called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewWidth);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewHeight(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_height called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewHeight);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraSetViewPos(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_set_view_pos called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    int32_t x = RValue_toInt32(args[1]);
    int32_t y = RValue_toInt32(args[2]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        runner->currentRoom->views[cameraId].viewX = x;
        runner->currentRoom->views[cameraId].viewY = y;
    }
    return RValue_makeUndefined();
}

// ===[ VARIABLE FUNCTIONS ]===

static RValue builtinVariableGlobalExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeReal(0.0);
    const char* name = args[0].string;
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) return RValue_makeReal(0.0);
    int32_t varID = ctx->globalVarNameMap[idx].value;
    if (ctx->globalVarCount > (uint32_t) varID && ctx->globalVars[varID].type != RVALUE_UNDEFINED) {
        return RValue_makeReal(1.0);
    }
    return RValue_makeReal(0.0);
}

static RValue builtinVariableGlobalGet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeUndefined();
    const char* name = args[0].string;
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) return RValue_makeUndefined();
    int32_t varID = ctx->globalVarNameMap[idx].value;
    if (ctx->globalVarCount > (uint32_t) varID) {
        RValue val = ctx->globalVars[varID];
        // Duplicate owned strings
        if (val.type == RVALUE_STRING && val.ownsString && val.string != nullptr) {
            return RValue_makeOwnedString(safeStrdup(val.string));
        }
        return val;
    }
    return RValue_makeUndefined();
}

static RValue builtinVariableGlobalSet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeUndefined();
    const char* name = args[0].string;
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) return RValue_makeUndefined();
    int32_t varID = ctx->globalVarNameMap[idx].value;
    if (ctx->globalVarCount > (uint32_t) varID) {
        RValue_free(&ctx->globalVars[varID]);
        RValue val = args[1];
        // Duplicate owned strings since args will be freed
        if (val.type == RVALUE_STRING && val.string != nullptr) {
            ctx->globalVars[varID] = RValue_makeOwnedString(safeStrdup(val.string));
        } else {
            ctx->globalVars[varID] = val;
        }
    }
    return RValue_makeUndefined();
}

// ===[ SCRIPT EXECUTE ]===

static RValue builtinScriptExecute(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t scriptIdx = RValue_toInt32(args[0]);

    // Look up the script to get its codeId
    if (scriptIdx < 0 || (uint32_t) scriptIdx >= ctx->dataWin->scpt.count) {
        fprintf(stderr, "VM: script_execute - invalid script index %d\n", scriptIdx);
        return RValue_makeUndefined();
    }

    int32_t codeId = ctx->dataWin->scpt.scripts[scriptIdx].codeId;
    if (0 > codeId || ctx->dataWin->code.count <= (uint32_t) codeId) {
        fprintf(stderr, "VM: script_execute - invalid codeId %d for script %d\n", codeId, scriptIdx);
        return RValue_makeUndefined();
    }

    // Pass remaining args (skip the script index)
    RValue* scriptArgs = (argCount > 1) ? &args[1] : nullptr;
    int32_t scriptArgCount = argCount - 1;

    return VM_callCodeIndex(ctx, codeId, scriptArgs, scriptArgCount);
}

// ===[ OS FUNCTIONS ]===

static RValue builtinOsGetLanguage(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeOwnedString(safeStrdup("en"));
}

static RValue builtinOsGetRegion(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeOwnedString(safeStrdup("US"));
}

// ===[ DS_MAP BUILTIN FUNCTIONS ]===

static RValue builtinDsMapCreate(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeReal((GMLReal) dsMapCreate());
}

static RValue builtinDsMapAdd(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);

    // Only add if key doesn't exist
    bool exists = shgeti(*mapPtr, key) != -1;

    if (exists) {
        free(key); // Key already exists, we didn't insert it
    } else {
        RValue val = args[2];
        if (val.type == RVALUE_STRING && val.string != nullptr) {
            val = RValue_makeOwnedString(safeStrdup(val.string));
        }
        shput(*mapPtr, key, val);
        // The RValue is now "owned" by the map, we do not need to free it!
    }

    return RValue_makeUndefined();
}

static RValue builtinDsMapSet(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);

    ptrdiff_t existingKeyIndex = shgeti(*mapPtr, key);

    if (existingKeyIndex != -1) {
        // If it already exists, we'll get the current value and free it
        RValue_free(&(*mapPtr)[existingKeyIndex].value);
    }

    RValue val = args[2];
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    }

    shput(*mapPtr, key, val);

    if (existingKeyIndex != -1) {
        // If it already existed, then shput still owns the old key
        // So we'll need to free the created key
        free(key);
    }

    return RValue_makeUndefined();
}

static RValue builtinDsMapReplace(VMContext* ctx, RValue* args, int32_t argCount) {
    // ds_map_replace is the same as ds_map_set in GMS 1.4
    return builtinDsMapSet(ctx, args, argCount);
}

static RValue builtinDsMapFindValue(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);
    ptrdiff_t idx = shgeti(*mapPtr, key);
    free(key);
    if (0 > idx) return RValue_makeUndefined();
    RValue val = (*mapPtr)[idx].value;
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(val.string));
    }
    return val;
}

static RValue builtinDsMapExists(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeReal(0.0);

    char* key = RValue_toString(args[1]);
    ptrdiff_t idx = shgeti(*mapPtr, key);
    free(key);
    return RValue_makeReal(idx >= 0 ? 1.0 : 0.0);
}

static RValue builtinDsMapFindFirst(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr || shlen(*mapPtr) == 0) return RValue_makeUndefined();
    return RValue_makeOwnedString(safeStrdup((*mapPtr)[0].key));
}

static RValue builtinDsMapFindNext(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* prevKey = RValue_toString(args[1]);
    ptrdiff_t idx = shgeti(*mapPtr, prevKey);
    free(prevKey);
    if (0 > idx || idx + 1 >= shlen(*mapPtr)) return RValue_makeUndefined();
    return RValue_makeOwnedString(safeStrdup((*mapPtr)[idx + 1].key));
}

static RValue builtinDsMapSize(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) shlen(*mapPtr));
}

static RValue builtinDsMapDestroy(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();
    // Free all keys and values
    for (ptrdiff_t i = 0; shlen(*mapPtr) > i; i++) {
        free((*mapPtr)[i].key);
        RValue_free(&(*mapPtr)[i].value);
    }
    shfree(*mapPtr);
    *mapPtr = nullptr;
    return RValue_makeUndefined();
}

// ===[ DS_LIST FUNCTIONS ]===

static RValue builtinDsListCreate(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeReal((GMLReal) dsListCreate());
}

static RValue builtinDsListAdd(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    int32_t id = RValue_toInt32(args[0]);
    DsList* list = dsListGet(id);
    if (list == nullptr) return RValue_makeUndefined();
    // ds_list_add can take multiple values after the list id
    repeat(argCount - 1, i) {
        RValue val = args[i + 1];
        if (val.type == RVALUE_STRING) {
            val = RValue_makeOwnedString(safeStrdup(val.string));
        }
        arrput(list->items, val);
    }
    return RValue_makeUndefined();
}

static RValue builtinDsListSize(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t id = RValue_toInt32(args[0]);
    DsList* list = dsListGet(id);
    if (list == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) arrlen(list->items));
}

static RValue builtinDsListFindIndex(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t id = RValue_toInt32(args[0]);
    DsList* list = dsListGet(id);
    if (list == nullptr) return RValue_makeReal(-1.0);
    RValue needle = args[1];
    for (int32_t i = 0; (int32_t) arrlen(list->items) > i; i++) {
        RValue item = list->items[i];
        if (item.type != needle.type) continue;
        switch (item.type) {
            case RVALUE_REAL:
                if (item.real == needle.real) return RValue_makeReal((GMLReal) i);
                break;
            case RVALUE_INT32:
            case RVALUE_BOOL:
                if (item.int32 == needle.int32) return RValue_makeReal((GMLReal) i);
                break;
#ifndef NO_RVALUE_INT64
            case RVALUE_INT64:
                if (item.int64 == needle.int64) return RValue_makeReal((GMLReal) i);
                break;
#endif
            case RVALUE_STRING:
                if (item.string != nullptr && needle.string != nullptr && strcmp(item.string, needle.string) == 0) return RValue_makeReal((GMLReal) i);
                break;
            default:
                break;
        }
    }
    return RValue_makeReal(-1.0);
}

// ===[ ARRAY FUNCTIONS ]===

static RValue builtinArrayLength1d(VMContext* ctx, RValue* args, int32_t argCount) {
    // array_length_1d(array) takes a single array argument
    if (args[0].type != RVALUE_ARRAY_REF)
        return RValue_makeReal(0.0);

    int32_t varID = args[0].int32;
    int32_t maxIndex = -1;

    // Search selfArrayMap on the current instance
    Instance* inst = ctx->currentInstance;
    if (inst != nullptr) {
        repeat(hmlen(inst->selfArrayMap), idx) {
            int64_t key = inst->selfArrayMap[idx].key;
            int32_t keyVarID = (int32_t)(key >> 32);
            if (keyVarID == varID) {
                int32_t keyArrayIndex = (int32_t)(key & 0xFFFFFFFF);
                if (keyArrayIndex > maxIndex) {
                    maxIndex = keyArrayIndex;
                }
            }
        }
    }

    // Also search globalArrayMap
    repeat(hmlen(ctx->globalArrayMap), idx) {
        int64_t key = ctx->globalArrayMap[idx].key;
        int32_t keyVarID = (int32_t)(key >> 32);
        if (keyVarID == varID) {
            int32_t keyArrayIndex = (int32_t)(key & 0xFFFFFFFF);
            if (keyArrayIndex > maxIndex) {
                maxIndex = keyArrayIndex;
            }
        }
    }

    // Also search localArrayMap
    repeat(hmlen(ctx->localArrayMap), idx) {
        int64_t key = ctx->localArrayMap[idx].key;
        int32_t keyVarID = (int32_t)(key >> 32);
        if (keyVarID == varID) {
            int32_t keyArrayIndex = (int32_t)(key & 0xFFFFFFFF);
            if (keyArrayIndex > maxIndex) {
                maxIndex = keyArrayIndex;
            }
        }
    }

    return RValue_makeReal((GMLReal)(maxIndex + 1));
}

// ===[ COLLISION FUNCTIONS]===

static RValue builtinPlaceFree(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeBool(true);

    Runner* runner = (Runner*) ctx->runner;
    Instance* caller = (Instance*) ctx->currentInstance;
    if (caller == nullptr) return RValue_makeBool(true);

    GMLReal testX = RValue_toReal(args[0]);
    GMLReal testY = RValue_toReal(args[1]);

    // Save current position and temporarily move to test position
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool free = true;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || !other->solid || other == caller) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                free = false;
                break;
            }
        }
    }

    // Restore original position
    caller->x = savedX;
    caller->y = savedY;

    return RValue_makeBool(free);
}

// ===[ STUBBED FUNCTIONS ]===

#define STUB_RETURN_ZERO(name) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeReal(0.0); \
    }

#define STUB_RETURN_TRUE(name) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeBool(true); \
    }

#define STUB_RETURN_VALUE(name, value) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeReal(value); \
    }

#define STUB_RETURN_UNDEFINED(name) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeUndefined(); \
    }

// Steam stubs
STUB_RETURN_ZERO(steam_initialised)
STUB_RETURN_ZERO(steam_stats_ready)
STUB_RETURN_ZERO(steam_file_exists)
STUB_RETURN_UNDEFINED(steam_file_write)
STUB_RETURN_UNDEFINED(steam_file_read)
STUB_RETURN_ZERO(steam_get_persona_name)

// ===[ Audio Built-in Functions ]===

// Helper to get the AudioSystem from VMContext (returns nullptr if no audio)
static AudioSystem* getAudioSystem(VMContext* ctx) {
    Runner* runner = (Runner*) ctx->runner;
    return runner->audioSystem;
}

// Track the last music instance for legacy audio_play_music / audio_stop_music
static int32_t lastMusicInstance = -1;

static RValue builtin_audioChannelNum(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t count = RValue_toInt32(args[0]);
    audio->vtable->setChannelCount(audio, count);
    return RValue_makeUndefined();
}

static RValue builtin_audioPlaySound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    int32_t soundIndex = RValue_toInt32(args[0]);
    int32_t priority = RValue_toInt32(args[1]);
    bool loop = RValue_toBool(args[2]);
    int32_t instanceId = audio->vtable->playSound(audio, soundIndex, priority, loop);
    return RValue_makeReal((GMLReal) instanceId);
}

static RValue builtin_audioStopSound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    audio->vtable->stopSound(audio, soundOrInstance);
    return RValue_makeUndefined();
}

static RValue builtin_audioStopAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    audio->vtable->stopAll(audio);
    lastMusicInstance = -1;
    return RValue_makeUndefined();
}

static RValue builtin_audioIsPlaying(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    bool playing = audio->vtable->isPlaying(audio, soundOrInstance);
    return RValue_makeBool(playing);
}

static RValue builtin_audioIsPaused(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    bool playing = audio->vtable->isPlaying(audio, soundOrInstance);
    return RValue_makeBool(!playing);
}


static RValue builtin_audioSoundGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float gain = (float) RValue_toReal(args[1]);
    uint32_t timeMs = (uint32_t) RValue_toInt32(args[2]);
    audio->vtable->setSoundGain(audio, soundOrInstance, gain, timeMs);
    return RValue_makeUndefined();
}

static RValue builtin_audioSoundPitch(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pitch = (float) RValue_toReal(args[1]);
    audio->vtable->setSoundPitch(audio, soundOrInstance, pitch);
    return RValue_makeUndefined();
}

static RValue builtin_audioSoundGetGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(0.0);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float gain = audio->vtable->getSoundGain(audio, soundOrInstance);
    return RValue_makeReal((GMLReal) gain);
}

static RValue builtin_audioSoundGetPitch(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(1.0);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pitch = audio->vtable->getSoundPitch(audio, soundOrInstance);
    return RValue_makeReal((GMLReal) pitch);
}

static RValue builtin_audioMasterGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    float gain = (float) RValue_toReal(args[0]);
    audio->vtable->setMasterGain(audio, gain);
    return RValue_makeUndefined();
}

static RValue builtin_audioGroupLoad(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t groupIndex = RValue_toInt32(args[0]);
    audio->vtable->groupLoad(audio, groupIndex);
    return RValue_makeUndefined();
}

static RValue builtin_audioGroupIsLoaded(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    int32_t groupIndex = RValue_toInt32(args[0]);
    bool loaded = audio->vtable->groupIsLoaded(audio, groupIndex);
    return RValue_makeBool(loaded);
}

static RValue builtin_audioPlayMusic(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    int32_t soundIndex = RValue_toInt32(args[0]);
    int32_t priority = RValue_toInt32(args[1]);
    bool loop = RValue_toBool(args[2]);
    int32_t instanceId = audio->vtable->playSound(audio, soundIndex, priority, loop);
    lastMusicInstance = instanceId;
    return RValue_makeReal((GMLReal) instanceId);
}

static RValue builtin_audioStopMusic(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    if (lastMusicInstance >= 0) {
        audio->vtable->stopSound(audio, lastMusicInstance);
        lastMusicInstance = -1;
    }
    return RValue_makeUndefined();
}

static RValue builtin_audioMusicGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    if (lastMusicInstance >= 0) {
        float gain = (float) RValue_toReal(args[0]);
        uint32_t timeMs = (uint32_t) RValue_toInt32(args[1]);
        audio->vtable->setSoundGain(audio, lastMusicInstance, gain, timeMs);
    }
    return RValue_makeUndefined();
}

static RValue builtin_audioMusicIsPlaying(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    if (lastMusicInstance >= 0) {
        return RValue_makeBool(audio->vtable->isPlaying(audio, lastMusicInstance));
    }
    return RValue_makeBool(false);
}

static RValue builtin_audioPauseSound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    audio->vtable->pauseSound(audio, soundOrInstance);
    return RValue_makeUndefined();
}

static RValue builtin_audioResumeSound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    audio->vtable->resumeSound(audio, soundOrInstance);
    return RValue_makeUndefined();
}

static RValue builtin_audioPauseAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    audio->vtable->pauseAll(audio);
    return RValue_makeUndefined();
}

static RValue builtin_audioResumeAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    audio->vtable->resumeAll(audio);
    return RValue_makeUndefined();
}

static RValue builtin_audioSoundGetTrackPosition(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(0.0);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pos = audio->vtable->getTrackPosition(audio, soundOrInstance);
    return RValue_makeReal((GMLReal) pos);
}

static RValue builtin_audioSoundSetTrackPosition(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pos = (float) RValue_toReal(args[1]);
    audio->vtable->setTrackPosition(audio, soundOrInstance, pos);
    return RValue_makeUndefined();
}

static RValue builtin_audioCreateStream(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    const char* filename = RValue_toString(args[0]);
    int32_t streamIndex = audio->vtable->createStream(audio, filename);
    return RValue_makeReal((GMLReal) streamIndex);
}

static RValue builtin_audioDestroyStream(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    int32_t streamIndex = RValue_toInt32(args[0]);
    bool success = audio->vtable->destroyStream(audio, streamIndex);
    return RValue_makeReal(success ? 1.0 : -1.0);
}

// Application surface stubs
STUB_RETURN_UNDEFINED(application_surface_enable)
STUB_RETURN_UNDEFINED(application_surface_draw_enable)

// Gamepad stubs
STUB_RETURN_ZERO(gamepad_get_device_count)
STUB_RETURN_ZERO(gamepad_is_connected)
STUB_RETURN_ZERO(gamepad_button_check)
STUB_RETURN_ZERO(gamepad_button_check_pressed)
STUB_RETURN_ZERO(gamepad_button_check_released)
STUB_RETURN_ZERO(gamepad_axis_value)
STUB_RETURN_ZERO(gamepad_get_description)
STUB_RETURN_ZERO(gamepad_button_value)

// ===[ INI Functions ]===

static IniFile* currentIni = nullptr;
static char* currentIniPath = nullptr;
static bool currentIniDirty = false;

// Some games (like Undertale) open and close the same INI file EVERY SINGLE FRAME!
// While on modern devices this isn't a huge deal, this WILL cause issues on devices that have less than stellar file systems (like the PlayStation 2)
// To avoid unnecessary disk reads, we cache the last-closed INI and reuse it on reopen
static IniFile* cachedIni = nullptr;
static char* cachedIniPath = nullptr;

static void discardIniCache(void) {
    if (cachedIni != nullptr) {
        Ini_free(cachedIni);
        cachedIni = nullptr;
    }
    free(cachedIniPath);
    cachedIniPath = nullptr;
}

static RValue builtinIniOpen(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();

    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");

    // If the same file is already open, do nothing
    if (currentIni != nullptr && currentIniPath != nullptr && strcmp(currentIniPath, path) == 0) {
        return RValue_makeUndefined();
    }

    // Close any previously open INI (implicit close, no disk write)
    if (currentIni != nullptr) {
        Ini_free(currentIni);
        currentIni = nullptr;
    }
    free(currentIniPath);
    currentIniPath = nullptr;

    // Check if we have a cached INI for this path
    if (cachedIni != nullptr && cachedIniPath != nullptr && strcmp(cachedIniPath, path) == 0) {
        currentIni = cachedIni;
        currentIniPath = cachedIniPath;
        cachedIni = nullptr;
        cachedIniPath = nullptr;
        currentIniDirty = false;
        return RValue_makeUndefined();
    }

    // Cache miss, discard the old cache and read from disk
    discardIniCache();

    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;

    currentIniPath = safeStrdup(path);

    char* content = fs->vtable->readFileText(fs, path);
    if (content != nullptr) {
        currentIni = Ini_parse(content);
        free(content);
    } else {
        currentIni = Ini_parse("");
    }

    currentIniDirty = false;

    return RValue_makeUndefined();
}

static RValue builtinIniClose(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (currentIni != nullptr) {
        Runner* runner = (Runner*) ctx->runner;
        FileSystem* fs = runner->fileSystem;

        if (currentIniDirty) {
            char* serialized = Ini_serialize(currentIni, INI_SERIALIZE_DEFAULT_INITIAL_CAPACITY);
            fs->vtable->writeFileText(fs, currentIniPath, serialized);
            free(serialized);
        }

        // Move to cache instead of freeing
        discardIniCache();
        cachedIni = currentIni;
        cachedIniPath = currentIniPath;
        currentIni = nullptr;
        currentIniPath = nullptr;
    } else {
        free(currentIniPath);
        currentIniPath = nullptr;
    }

    return RValue_makeUndefined();
}

static RValue builtinIniReadString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || currentIni == nullptr) return RValue_makeOwnedString(safeStrdup(""));

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");

    const char* value = Ini_getString(currentIni, section, key);
    if (value != nullptr) {
        return RValue_makeOwnedString(safeStrdup(value));
    }

    // Return the default value (3rd arg)
    if (args[2].type == RVALUE_STRING && args[2].string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(args[2].string));
    }
    char* str = RValue_toString(args[2]);
    return RValue_makeOwnedString(str);
}

static RValue builtinIniReadReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || currentIni == nullptr) return RValue_makeReal(0.0);

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");

    const char* value = Ini_getString(currentIni, section, key);
    if (value != nullptr) {
        return RValue_makeReal(atof(value));
    }

    return RValue_makeReal(RValue_toReal(args[2]));
}

static RValue builtinIniWriteString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || currentIni == nullptr) return RValue_makeUndefined();

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");
    const char* value = (args[2].type == RVALUE_STRING ? args[2].string : "");

    Ini_setString(currentIni, section, key, value);
    currentIniDirty = true;
    return RValue_makeUndefined();
}

static RValue builtinIniWriteReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || currentIni == nullptr) return RValue_makeUndefined();

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");
    char* valueStr = RValue_toString(args[2]);

    Ini_setString(currentIni, section, key, valueStr);
    currentIniDirty = true;
    free(valueStr);
    return RValue_makeUndefined();
}

static RValue builtinIniSectionExists(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || currentIni == nullptr) return RValue_makeBool(false);

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    return RValue_makeBool(Ini_hasSection(currentIni, section));
}

// ===[ Text File Functions ]===

typedef struct {
    char* content; // full file content (for read mode)
    char* writeBuffer; // accumulated text (for write mode)
    char* filePath; // relative path (for write mode, to flush on close)
    int32_t readPos;    // current byte position in content (read mode)
    int32_t contentLen; // length of content string
    bool isWriteMode;
    bool isOpen;
} OpenTextFile;

#define MAX_OPEN_TEXT_FILES 32
static OpenTextFile openTextFiles[MAX_OPEN_TEXT_FILES];

static int32_t findFreeTextFileSlot(void) {
    repeat(MAX_OPEN_TEXT_FILES, i) {
        if (!openTextFiles[i].isOpen) return (int32_t) i;
    }
    return -1;
}

static RValue builtinFileExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    return RValue_makeBool(fs->vtable->fileExists(fs, path));
}

static RValue builtinFileTextOpenRead(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1.0);
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;

    int32_t slot = findFreeTextFileSlot();
    if (0 > slot) {
        fprintf(stderr, "Warning: Too many open text files!\n");
        abort();
    }

    char* content = fs->vtable->readFileText(fs, path);
    if (content == nullptr) {
        // GML returns a valid handle even if the file doesn't exist; eof is immediately true
        content = safeStrdup("");
    }

    openTextFiles[slot] = (OpenTextFile) {
        .content = content,
        .writeBuffer = nullptr,
        .filePath = nullptr,
        .readPos = 0,
        .contentLen = (int32_t) strlen(content),
        .isWriteMode = false,
        .isOpen = true,
    };

    return RValue_makeReal((GMLReal) slot);
}

static RValue builtinFileTextOpenWrite(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1.0);
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    (void) ctx;

    int32_t slot = findFreeTextFileSlot();
    if (0 > slot) {
        fprintf(stderr, "Warning: Too many open text files!\n");
        abort();
    }

    openTextFiles[slot] = (OpenTextFile) {
        .content = nullptr,
        .writeBuffer = safeStrdup(""),
        .filePath = safeStrdup(path),
        .readPos = 0,
        .contentLen = 0,
        .isWriteMode = true,
        .isOpen = true,
    };

    return RValue_makeReal((GMLReal) slot);
}

static RValue builtinFileTextClose(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &openTextFiles[handle];
    if (file->isWriteMode && file->writeBuffer != nullptr && file->filePath != nullptr) {
        Runner* runner = (Runner*) ctx->runner;
        FileSystem* fs = runner->fileSystem;
        fs->vtable->writeFileText(fs, file->filePath, file->writeBuffer);
    }

    free(file->content);
    free(file->writeBuffer);
    free(file->filePath);
    *file = (OpenTextFile) {0};
    return RValue_makeUndefined();
}

static RValue builtinFileTextReadString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeOwnedString(safeStrdup(""));

    OpenTextFile* file = &openTextFiles[handle];
    if (file->readPos >= file->contentLen) return RValue_makeOwnedString(safeStrdup(""));

    // Read until newline, carriage return, or EOF (does NOT consume the newline)
    int32_t start = file->readPos;
    while (file->contentLen > file->readPos) {
        char c = file->content[file->readPos];
        if (TextUtils_isNewlineChar(c))
            break;
        file->readPos++;
    }

    int32_t len = file->readPos - start;
    char* result = safeMalloc((size_t) len + 1);
    memcpy(result, file->content + start, (size_t) len);
    result[len] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinFileTextReadln(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || MAX_OPEN_TEXT_FILES <= handle || !openTextFiles[handle].isOpen) return RValue_makeOwnedString(safeStrdup(""));

    OpenTextFile* file = &openTextFiles[handle];

    int size = 0;
    int readPos = file->readPos;

    // First we read everything to figure out what will be the size of the string
    // Skip past the current line (consume everything up to and including the newline)
    while (file->contentLen > readPos) {
        char c = file->content[readPos];
        readPos++;
        if (c == '\n')
            break;
        if (c == '\r') {
            // Handle \r\n
            if (file->contentLen > readPos && file->content[readPos] == '\n') {
                readPos++;
            }
            break;
        }
        size++;
    }

    // Now we copy it because we already know the size of the string!
    char* string = safeMalloc(size + 1); // +1 because the last one is null
    memcpy(string, file->content + file->readPos, size);
    string[size] = '\0';
    file->readPos = readPos;
    return RValue_makeOwnedString(string);
}

static RValue builtinFileTextReadReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeReal(0.0);

    OpenTextFile* file = &openTextFiles[handle];
    if (file->readPos >= file->contentLen) return RValue_makeReal(0.0);

    // strtod will parse the number and advance past it
    char* endPtr = nullptr;
    GMLReal value = GMLReal_strtod(file->content + file->readPos, &endPtr);
    if (endPtr != nullptr) {
        file->readPos = (int32_t) (endPtr - file->content);
    }

    return RValue_makeReal(value);
}

static RValue builtinFileTextWriteString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &openTextFiles[handle];
    if (!file->isWriteMode) return RValue_makeUndefined();

    char* str = RValue_toString(args[1]);
    size_t oldLen = strlen(file->writeBuffer);
    size_t addLen = strlen(str);
    file->writeBuffer = safeRealloc(file->writeBuffer, oldLen + addLen + 1);
    memcpy(file->writeBuffer + oldLen, str, addLen);
    file->writeBuffer[oldLen + addLen] = '\0';
    free(str);

    return RValue_makeUndefined();
}

static RValue builtinFileTextWriteln(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &openTextFiles[handle];
    if (!file->isWriteMode) return RValue_makeUndefined();

    size_t oldLen = strlen(file->writeBuffer);
    file->writeBuffer = safeRealloc(file->writeBuffer, oldLen + 2);
    file->writeBuffer[oldLen] = '\n';
    file->writeBuffer[oldLen + 1] = '\0';

    return RValue_makeUndefined();
}

static RValue builtinFileTextWriteReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &openTextFiles[handle];
    if (!file->isWriteMode) return RValue_makeUndefined();

    char* str = RValue_toString(args[1]);
    size_t oldLen = strlen(file->writeBuffer);
    size_t addLen = strlen(str);
    file->writeBuffer = safeRealloc(file->writeBuffer, oldLen + addLen + 1);
    memcpy(file->writeBuffer + oldLen, str, addLen);
    file->writeBuffer[oldLen + addLen] = '\0';
    free(str);

    return RValue_makeUndefined();
}

static RValue builtinFileTextEof(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(true);
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !openTextFiles[handle].isOpen) return RValue_makeBool(true);

    OpenTextFile* file = &openTextFiles[handle];
    return RValue_makeBool(file->readPos >= file->contentLen);
}

static RValue builtinFileDelete(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    fs->vtable->deleteFile(fs, path);
    return RValue_makeUndefined();
}

// Keyboard functions
static RValue builtinKeyboardCheck(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    return RValue_makeBool(RunnerKeyboard_check(runner->keyboard, key));
}

static RValue builtinKeyboardCheckPressed(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    return RValue_makeBool(RunnerKeyboard_checkPressed(runner->keyboard, key));
}

static RValue builtinKeyboardCheckReleased(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    return RValue_makeBool(RunnerKeyboard_checkReleased(runner->keyboard, key));
}

static RValue builtinKeyboardCheckDirect(VMContext* ctx, RValue* args, int32_t argCount) {
    // keyboard_check_direct is the same as keyboard_check for our purposes
    return builtinKeyboardCheck(ctx, args, argCount);
}

static RValue builtinKeyboardKeyPress(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    RunnerKeyboard_simulatePress(runner->keyboard, key);
    return RValue_makeUndefined();
}

static RValue builtinKeyboardKeyRelease(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    RunnerKeyboard_simulateRelease(runner->keyboard, key);
    return RValue_makeUndefined();
}

static RValue builtinKeyboardClear(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    RunnerKeyboard_clear(runner->keyboard, key);
    return RValue_makeUndefined();
}

// Joystick stubs
STUB_RETURN_ZERO(joystick_exists)
STUB_RETURN_ZERO(joystick_xpos)
STUB_RETURN_ZERO(joystick_ypos)
STUB_RETURN_ZERO(joystick_direction)
STUB_RETURN_ZERO(joystick_pov)
STUB_RETURN_ZERO(joystick_check_button)

// Window stubs
STUB_RETURN_ZERO(window_get_fullscreen)
STUB_RETURN_UNDEFINED(window_set_fullscreen)
STUB_RETURN_UNDEFINED(window_set_caption)
STUB_RETURN_UNDEFINED(window_set_size)
STUB_RETURN_UNDEFINED(window_center)
static RValue builtinWindowGetWidth(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowWidth);
}

static RValue builtinWindowGetHeight(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowHeight);
}

// Game stubs
STUB_RETURN_UNDEFINED(game_restart)
static RValue builtinGameEnd(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    runner->shouldExit = true;
    return RValue_makeUndefined();
}
STUB_RETURN_UNDEFINED(game_save)
STUB_RETURN_UNDEFINED(game_load)

static RValue builtinInstanceNumber(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    int32_t objectIndex = RValue_toInt32(args[0]);
    int32_t count = 0;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, objectIndex)) {
            count++;
        }
    }
    return RValue_makeReal((GMLReal) count);
}

static RValue builtinInstanceFind(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(-4.0); // noone
    Runner* runner = (Runner*) ctx->runner;
    int32_t objectIndex = RValue_toInt32(args[0]);
    int32_t n = RValue_toInt32(args[1]);
    int32_t count = 0;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, objectIndex)) {
            if (count == n) return RValue_makeReal((GMLReal) inst->instanceId);
            count++;
        }
    }
    return RValue_makeReal(-4.0); // noone
}

static RValue builtinInstanceExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    bool found = false;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    if (id >= 0 && runner->dataWin->objt.count > (uint32_t) id) {
        // Object type index: search for any active instance of this object (or descendants)
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
                found = true;
                break;
            }
        }
    } else {
        // Instance ID: search for a specific instance
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && inst->instanceId == (uint32_t) id) {
                found = true;
                break;
            }
        }
    }
    return RValue_makeBool(found);
}

static RValue builtinInstanceDestroy(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (1 > argCount) {
        // No args: destroy the current instance
        if (ctx->currentInstance != nullptr) {
            Runner_destroyInstance(runner, (Instance*) ctx->currentInstance);
        }
        return RValue_makeUndefined();
    }
    // 1 arg: find and destroy matching instances
    int32_t id = RValue_toInt32(args[0]);
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    if (id >= 0 && runner->dataWin->objt.count > (uint32_t) id) {
        // Object type index: destroy all active instances of this object (or descendants)
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
                Runner_destroyInstance(runner, inst);
            }
        }
    } else {
        // Instance ID: destroy that specific instance
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && inst->instanceId == (uint32_t) id) {
                Runner_destroyInstance(runner, inst);
                break;
            }
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    GMLReal x = RValue_toReal(args[0]);
    GMLReal y = RValue_toReal(args[1]);
    int32_t objectIndex = RValue_toInt32(args[2]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: instance_create: objectIndex %d out of range\n", objectIndex);
        return RValue_makeReal(0.0);
    }
    Instance* callerInst = (Instance*) ctx->currentInstance;
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    if (inst == nullptr) return RValue_makeReal(-4.0); // noone
    if (callerInst != nullptr && ctx->creatorVarID >= 0) {
        Instance_setSelfVar(inst, ctx->creatorVarID, RValue_makeReal((GMLReal) callerInst->instanceId));
    }
    return RValue_makeReal((GMLReal) inst->instanceId);
}

static RValue builtinInstanceCreateDepth(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    GMLReal x = RValue_toReal(args[0]);
    GMLReal y = RValue_toReal(args[1]);
    int32_t depth = RValue_toInt32(args[2]);
    int32_t objectIndex = RValue_toInt32(args[3]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: instance_create: objectIndex %d out of range\n", objectIndex);
        return RValue_makeReal(0.0);
    }
    Instance* callerInst = (Instance*) ctx->currentInstance;
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    if (inst == nullptr) return RValue_makeReal(-4.0); // noone
    if (callerInst != nullptr && ctx->creatorVarID >= 0) {
        Instance_setSelfVar(inst, ctx->creatorVarID, RValue_makeReal((GMLReal) callerInst->instanceId));
    }
    inst->depth = depth;
    return RValue_makeReal((GMLReal) inst->instanceId);
}

static RValue builtinInstanceChange(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeUndefined();

    int32_t objectIndex = RValue_toInt32(args[0]);
    bool performEvents = RValue_toBool(args[1]);

    if (0 > objectIndex || (uint32_t) objectIndex >= runner->dataWin->objt.count) {
        fprintf(stderr, "VM: instance_change: objectIndex %d out of range\n", objectIndex);
        return RValue_makeUndefined();
    }

    // Fire destroy event on old object if requested
    if (performEvents) {
        Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    }

    // Change object index and copy properties from new object definition
    GameObject* newObjDef = &runner->dataWin->objt.objects[objectIndex];
    inst->objectIndex = objectIndex;
    inst->spriteIndex = newObjDef->spriteId;
    inst->visible = newObjDef->visible;
    inst->solid = newObjDef->solid;
    inst->persistent = newObjDef->persistent;
    inst->depth = newObjDef->depth;
    inst->maskIndex = newObjDef->textureMaskId;
    inst->imageIndex = 0.0;

    // Fire create event on new object if requested
    if (performEvents) {
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    }

    return RValue_makeUndefined();
}

static RValue builtinInstanceDeactivateAll(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    bool notme = RValue_toBool(args[0]);

    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];

        if (!notme || instance != ctx->currentInstance) {
            instance->active = false;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceActivateAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];
        if (!instance->destroyed)
            ctx->runner->instances[i]->active = true;
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceActivateObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t objIndex = RValue_toInt32(args[0]);

    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];
        if (!instance->active && !instance->destroyed && (objIndex == INSTANCE_ALL || VM_isObjectOrDescendant(ctx->dataWin, instance->objectIndex, objIndex))) {
            instance->active = true;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceDeactivateObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t objIndex = RValue_toInt32(args[0]);

    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];
        if (instance->active && !instance->destroyed && (objIndex == INSTANCE_ALL || VM_isObjectOrDescendant(ctx->dataWin, instance->objectIndex, objIndex))) {
            instance->active = false;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinEventInherited(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr || 0 > ctx->currentEventObjectIndex || 0 > ctx->currentEventType) {
        fprintf(stderr, "VM: event_inherited called with no event context (inst=%p, eventObjIdx=%d, eventType=%d)\n", (void*) inst, ctx->currentEventObjectIndex, ctx->currentEventType);
        return RValue_makeReal(0.0);
    }

    DataWin* dataWin = ctx->dataWin;
    int32_t ownerObjectIndex = ctx->currentEventObjectIndex;
    if ((uint32_t) ownerObjectIndex >= dataWin->objt.count) {
        fprintf(stderr, "VM: event_inherited ownerObjectIndex %d out of range\n", ownerObjectIndex);
        return RValue_makeReal(0.0);
    }

    int32_t parentObjectIndex = dataWin->objt.objects[ownerObjectIndex].parentId;
    if (ctx->traceEventInherited) {
        fprintf(stderr, "VM: [%s] event_inherited owner=%s(%d) parent=%s(%d) event=%s (instanceId=%d)\n", dataWin->objt.objects[inst->objectIndex].name, dataWin->objt.objects[ownerObjectIndex].name, ownerObjectIndex, (0 > parentObjectIndex) ? "none" : dataWin->objt.objects[parentObjectIndex].name, parentObjectIndex, Runner_getEventName(ctx->currentEventType, ctx->currentEventSubtype), inst->instanceId);
    }
    if (0 > parentObjectIndex) return RValue_makeReal(0.0);

    Runner_executeEventFromObject(runner, inst, parentObjectIndex, ctx->currentEventType, ctx->currentEventSubtype);
    return RValue_makeReal(0.0);
}

static RValue builtinEventUser(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeReal(0.0);

    int32_t subevent = RValue_toInt32(args[0]);
    if (0 > subevent || 15 < subevent) return RValue_makeReal(0.0);

    Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + subevent);
    return RValue_makeReal(0.0);
}

static RValue builtinEventPerform(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeReal(0.0);

    int32_t eventType = RValue_toInt32(args[0]);
    int32_t eventSubtype = RValue_toInt32(args[1]);

    Runner_executeEvent(runner, inst, eventType, eventSubtype);
    return RValue_makeReal(0.0);
}

static RValue builtinActionKillObject(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (ctx->currentInstance != nullptr) {
        Runner_destroyInstance(runner, (Instance*) ctx->currentInstance);
    }
    return RValue_makeUndefined();
}

static RValue builtinActionCreateObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t objectIndex = RValue_toInt32(args[0]);
    GMLReal x = RValue_toReal(args[1]);
    GMLReal y = RValue_toReal(args[2]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: action_create_object: objectIndex %d out of range\n", objectIndex);
        return RValue_makeUndefined();
    }
    Instance* callerInst = (Instance*) ctx->currentInstance;
    if (ctx->actionRelativeFlag && callerInst != nullptr) {
        x += callerInst->x;
        y += callerInst->y;
    }
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    if (callerInst != nullptr && ctx->creatorVarID >= 0) {
        Instance_setSelfVar(inst, ctx->creatorVarID, RValue_makeReal((GMLReal) callerInst->instanceId));
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetRelative(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    ctx->actionRelativeFlag = RValue_toInt32(args[0]) != 0;
    return RValue_makeUndefined();
}

static RValue builtinActionMove(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    // action_move(direction_string, speed)
    // Direction string is 9 chars of '0'/'1' encoding a 3x3 direction grid:
    //   Pos: 0=UL(225) 1=U(270) 2=UR(315) 3=L(180) 4=STOP 5=R(0) 6=DL(135) 7=D(90) 8=DR(45)
    char* dirs = RValue_toString(args[0]);
    GMLReal spd = RValue_toReal(args[1]);

    static const GMLReal angles[] = {225, 270, 315, 180, -1, 0, 135, 90, 45};

    // Collect all enabled directions
    int candidates[9];
    int count = 0;
    for (int i = 0; 9 > i && dirs[i] != '\0'; i++) {
        if (dirs[i] == '1') {
            candidates[count++] = i;
        }
    }

    if (count == 0) {
        free(dirs);
        return RValue_makeUndefined();
    }

    // Pick one at random
    int pick = candidates[0 == count - 1 ? 0 : rand() % count];

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (4 == pick) {
            // STOP
            if (ctx->actionRelativeFlag) {
                inst->speed += (float) spd;
            } else {
                inst->speed = 0;
            }
        } else {
            GMLReal angle = angles[pick];
            if (ctx->actionRelativeFlag) {
                inst->direction += (float) angle;
                inst->speed += (float) spd;
            } else {
                inst->direction = (float) angle;
                inst->speed = (float) spd;
            }
        }
        Instance_computeComponentsFromSpeed(inst);
    }
    free(dirs);
    return RValue_makeUndefined();
}

static RValue builtinActionMoveTo(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal ax = RValue_toReal(args[0]);
    GMLReal ay = RValue_toReal(args[1]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->x += (float) ax;
            inst->y += (float) ay;
        } else {
            inst->x = (float) ax;
            inst->y = (float) ay;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetFriction(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal val = RValue_toReal(args[0]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->friction += (float) val;
        } else {
            inst->friction = (float) val;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetGravity(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal dir = RValue_toReal(args[0]);
    GMLReal grav = RValue_toReal(args[1]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->gravityDirection += (float) dir;
            inst->gravity += (float) grav;
        } else {
            inst->gravityDirection = (float) dir;
            inst->gravity = (float) grav;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetHspeed(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal val = RValue_toReal(args[0]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->hspeed += (float) val;
        } else {
            inst->hspeed = (float) val;
        }
        Instance_computeSpeedFromComponents(inst);
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetVspeed(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal val = RValue_toReal(args[0]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->vspeed += (float) val;
        } else {
            inst->vspeed = (float) val;
        }
        Instance_computeSpeedFromComponents(inst);
    }
    return RValue_makeUndefined();
}

// Buffer stubs
STUB_RETURN_ZERO(buffer_create)
STUB_RETURN_UNDEFINED(buffer_delete)
STUB_RETURN_UNDEFINED(buffer_write)
STUB_RETURN_ZERO(buffer_read)
STUB_RETURN_UNDEFINED(buffer_seek)
STUB_RETURN_ZERO(buffer_tell)
STUB_RETURN_ZERO(buffer_get_size)
STUB_RETURN_ZERO(buffer_base64_encode)

// PSN stubs
STUB_RETURN_UNDEFINED(psn_init)
STUB_RETURN_ZERO(psn_default_user)
STUB_RETURN_ZERO(psn_get_leaderboard_score)

// Draw functions
static RValue builtin_drawSprite(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);

    // If subimg < 0, use the current instance's imageIndex
    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSprite(runner->renderer, spriteIndex, subimg, x, y);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float xscale = (float) RValue_toReal(args[4]);
    float yscale = (float) RValue_toReal(args[5]);
    float rot = (float) RValue_toReal(args[6]);
    uint32_t color = (uint32_t) RValue_toInt32(args[7]);
    float alpha = (float) RValue_toReal(args[8]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpriteExt(runner->renderer, spriteIndex, subimg, x, y, xscale, yscale, rot, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteStretched(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float w = (float) RValue_toReal(args[4]);
    float h = (float) RValue_toReal(args[5]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpriteStretched(runner->renderer, spriteIndex, subimg, x, y, w, h, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteStretchedExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float w = (float) RValue_toReal(args[4]);
    float h = (float) RValue_toReal(args[5]);
    uint32_t color = (uint32_t) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpriteStretched(runner->renderer, spriteIndex, subimg, x, y, w, h, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpritePart(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    int32_t left = RValue_toInt32(args[2]);
    int32_t top = RValue_toInt32(args[3]);
    int32_t width = RValue_toInt32(args[4]);
    int32_t height = RValue_toInt32(args[5]);
    float x = (float) RValue_toReal(args[6]);
    float y = (float) RValue_toReal(args[7]);

    // If subimg < 0, use the current instance's imageIndex
    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpritePart(runner->renderer, spriteIndex, subimg, left, top, width, height, x, y);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpritePartExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    int32_t left = RValue_toInt32(args[2]);
    int32_t top = RValue_toInt32(args[3]);
    int32_t width = RValue_toInt32(args[4]);
    int32_t height = RValue_toInt32(args[5]);
    float x = (float) RValue_toReal(args[6]);
    float y = (float) RValue_toReal(args[7]);
    float xscale = (float) RValue_toReal(args[8]);
    float yscale = (float) RValue_toReal(args[9]);
    uint32_t color = (uint32_t) RValue_toInt32(args[10]);
    float alpha = (float) RValue_toReal(args[11]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpritePartExt(runner->renderer, spriteIndex, subimg, left, top, width, height, x, y, xscale, yscale, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawRectangle(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x1 = (float) RValue_toReal(args[0]);
    float y1 = (float) RValue_toReal(args[1]);
    float x2 = (float) RValue_toReal(args[2]);
    float y2 = (float) RValue_toReal(args[3]);
    bool outline = RValue_toBool(args[4]);

    runner->renderer->vtable->drawRectangle(runner->renderer, x1, y1, x2, y2, runner->renderer->drawColor, runner->renderer->drawAlpha, outline);
    return RValue_makeUndefined();
}

static RValue builtin_drawRectangleColor(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) { 
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x1 = (float) RValue_toReal(args[0]);
    float y1 = (float) RValue_toReal(args[1]);
    float x2 = (float) RValue_toReal(args[2]);
    float y2 = (float) RValue_toReal(args[3]);
    uint32_t color = (uint32_t) RValue_toInt32(args[4]);
    bool outline = RValue_toBool(args[8]);

    runner->renderer->vtable->drawRectangle(runner->renderer, x1, y1, x2, y2, color, runner->renderer->drawAlpha, outline);
    return RValue_makeUndefined();
}

static RValue builtin_drawHealthbar(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x1 = (float) RValue_toReal(args[0]);
    float y1 = (float) RValue_toReal(args[1]);
    float x2 = (float) RValue_toReal(args[2]);
    float y2 = (float) RValue_toReal(args[3]);
    float amount = (float) RValue_toReal(args[4]);

    amount = amount / (float)100; // 0 - 1;
    float healthbarX = (x1 * (1-amount) + x2 * amount);
    //float healthbarY = (y1 * (1-amount) + y2 * amount);

    uint32_t backCol = (uint32_t) RValue_toInt32(args[5]);
    uint32_t minCol = (uint32_t) RValue_toInt32(args[6]);
    uint32_t maxCol = (uint32_t) RValue_toInt32(args[7]);
    uint32_t intermediateColor = Renderer_mixColors(minCol,maxCol,amount);

    int32_t direction = RValue_toInt32(args[8]);
    
    bool showBack = RValue_toBool(args[9]);

    if (showBack) {
        runner->renderer->vtable->drawRectangle(runner->renderer, x1,y1,x2,y2,backCol, runner->renderer->drawAlpha, false);
    }

    runner->renderer->vtable->drawRectangle(runner->renderer,x1,y1,healthbarX,y2,intermediateColor, runner->renderer->drawAlpha, false);
}

static RValue builtin_drawSetColor(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawColor = (uint32_t) RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetAlpha(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawAlpha = (float) RValue_toReal(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetFont(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawFont = RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetHalign(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawHalign = RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetValign(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawValign = RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawText(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);

    char* processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawText(runner->renderer, processedText, x, y, 1.0f, 1.0f, 0.0f);
    free(processedText);
    free(str);
    return RValue_makeUndefined();
}

static RValue builtin_drawTextTransformed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);
    float xscale = (float) RValue_toReal(args[3]);
    float yscale = (float) RValue_toReal(args[4]);
    float angle = (float) RValue_toReal(args[5]);

    char* processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawText(runner->renderer, processedText, x, y, xscale, yscale, angle);
    free(processedText);
    free(str);
    return RValue_makeUndefined();
}
STUB_RETURN_UNDEFINED(draw_text_ext)
STUB_RETURN_UNDEFINED(draw_text_ext_transformed)

static RValue builtin_drawTextColor(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);
    int32_t c1 = (float) RValue_toInt32(args[3]);
    int32_t c2 = (float) RValue_toInt32(args[4]);
    int32_t c3 = (float) RValue_toInt32(args[5]);
    int32_t c4 = (float) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    char* processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawTextColor(runner->renderer, processedText, x, y, 1.0f, 1.0f, 0.0f, c1, c2, c3, c4, alpha);
    free(processedText);
    free(str);
    return RValue_makeUndefined();
}

static RValue builtin_drawTextColorTransformed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);
    float xscale = (float) RValue_toReal(args[3]);
    float yscale = (float) RValue_toReal(args[4]);
    float angle = (float) RValue_toReal(args[5]);
    int32_t c1 = (float) RValue_toInt32(args[6]);
    int32_t c2 = (float) RValue_toInt32(args[7]);
    int32_t c3 = (float) RValue_toInt32(args[8]);
    int32_t c4 = (float) RValue_toInt32(args[9]);
    float alpha = (float) RValue_toReal(args[10]);

    char* processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawTextColor(runner->renderer, processedText, x, y, xscale, yscale, angle, c1, c2, c3, c4, alpha);
    free(processedText);
    free(str);
    return RValue_makeUndefined();
}
STUB_RETURN_UNDEFINED(draw_text_color_ext)
STUB_RETURN_UNDEFINED(draw_text_color_ext_transformed)

STUB_RETURN_UNDEFINED(draw_surface)
STUB_RETURN_UNDEFINED(draw_surface_ext)
static RValue builtin_drawBackground(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 3 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);
    float y = (float) RValue_toReal(args[2]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, x, y, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawBackgroundExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 8 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);
    float y = (float) RValue_toReal(args[2]);
    float xscale = (float) RValue_toReal(args[3]);
    float yscale = (float) RValue_toReal(args[4]);
    float rot = (float) RValue_toReal(args[5]);
    uint32_t color = (uint32_t) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, x, y, 0.0f, 0.0f, xscale, yscale, rot, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawBackgroundStretched(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 5 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);
    float y = (float) RValue_toReal(args[2]);
    float w = (float) RValue_toReal(args[3]);
    float h = (float) RValue_toReal(args[4]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    TexturePageItem* tpag = &runner->dataWin->tpag.items[tpagIndex];
    float xscale = w / (float) tpag->boundingWidth;
    float yscale = h / (float) tpag->boundingHeight;

    runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, x, y, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawBackgroundPartExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 11 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    int32_t left = RValue_toInt32(args[1]);
    int32_t top = RValue_toInt32(args[2]);
    int32_t width = RValue_toInt32(args[3]);
    int32_t height = RValue_toInt32(args[4]);
    float x = (float) RValue_toReal(args[5]);
    float y = (float) RValue_toReal(args[6]);
    float xscale = (float) RValue_toReal(args[7]);
    float yscale = (float) RValue_toReal(args[8]);
    uint32_t color = (uint32_t) RValue_toInt32(args[9]);
    float alpha = (float) RValue_toReal(args[10]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    runner->renderer->vtable->drawSpritePart(runner->renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtinBackgroundGetWidth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t bgIndex = RValue_toInt32(args[0]);
    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->tpag.items[tpagIndex].boundingWidth);
}

static RValue builtinBackgroundGetHeight(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t bgIndex = RValue_toInt32(args[0]);
    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->tpag.items[tpagIndex].boundingHeight);
}

static RValue builtin_draw_self(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr && ctx->currentInstance != nullptr) {
        Renderer_drawSelf(runner->renderer, (Instance*) ctx->currentInstance);
    }
    return RValue_makeUndefined();
}

// draw_line(x1, y1, x2, y2)
static RValue builtin_draw_line(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        runner->renderer->vtable->drawLine(runner->renderer, x1, y1, x2, y2, 1.0f, runner->renderer->drawColor, runner->renderer->drawAlpha);
    }
    return RValue_makeUndefined();
}

// draw_line_width(x1, y1, x2, y2, w)
static RValue builtin_draw_line_width(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        float w = (float) RValue_toReal(args[4]);
        runner->renderer->vtable->drawLine(runner->renderer, x1, y1, x2, y2, w, runner->renderer->drawColor, runner->renderer->drawAlpha);
    }
    return RValue_makeUndefined();
}

// draw_line_width_colour(x1, y1, x2, y2, w, col1, col2)
static RValue builtin_draw_line_width_colour(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        float w = (float) RValue_toReal(args[4]);
        uint32_t col1 = (uint32_t) RValue_toInt32(args[5]);
        uint32_t col2 = (uint32_t) RValue_toInt32(args[6]);
        runner->renderer->vtable->drawLineColor(runner->renderer, x1, y1, x2, y2, w, col1, col2, runner->renderer->drawAlpha);
    }
    return RValue_makeUndefined();
}

// draw_triangle(x1, y1, x2, y2, x3, y3, outline)
static RValue builtin_draw_triangle(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        float x3 = (float) RValue_toReal(args[4]);
        float y3 = (float) RValue_toReal(args[5]);
        bool outline = (float) RValue_toBool(args[6]);
        runner->renderer->vtable->drawTriangle(runner->renderer, x1, y1, x2, y2, x3, y3, outline);
    }
    return RValue_makeUndefined();
}

static RValue builtin_draw_set_colour(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawColor = (uint32_t) RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_draw_get_colour(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        return RValue_makeReal((GMLReal) runner->renderer->drawColor);
    }
    return RValue_makeReal(0.0);
}

static RValue builtin_draw_get_color(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        return RValue_makeReal((GMLReal) runner->renderer->drawColor);
    }
    return RValue_makeReal(0.0);
}

static RValue builtin_draw_get_alpha(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        return RValue_makeReal((GMLReal) runner->renderer->drawAlpha);
    }
    return RValue_makeReal(0.0);
}

// merge_color(col1, col2, amount) - lerps between two colors
static RValue builtinMergeColor(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t col1 = RValue_toInt32(args[0]);
    int32_t col2 = RValue_toInt32(args[1]);
    GMLReal amount = RValue_toReal(args[2]);

    int32_t b1 = (col1 >> 16) & 0xFF;
    int32_t g1 = (col1 >> 8) & 0xFF;
    int32_t r1 = col1 & 0xFF;

    int32_t b2 = (col2 >> 16) & 0xFF;
    int32_t g2 = (col2 >> 8) & 0xFF;
    int32_t r2 = col2 & 0xFF;

    GMLReal inv = 1.0 - amount;
    int32_t r = (int32_t) (r1 * inv + r2 * amount);
    int32_t g = (int32_t) (g1 * inv + g2 * amount);
    int32_t b = (int32_t) (b1 * inv + b2 * amount);

    return RValue_makeReal((GMLReal) (((b << 16) & 0xFF0000) | ((g << 8) & 0xFF00) | (r & 0xFF)));
}

// Surface stubs
STUB_RETURN_ZERO(surface_create)
STUB_RETURN_UNDEFINED(surface_free)
STUB_RETURN_UNDEFINED(surface_set_target)
STUB_RETURN_UNDEFINED(surface_reset_target)
STUB_RETURN_ZERO(surface_exists)
// application_surface is surface ID -1 (sentinel); for it, return the window dimensions
static RValue builtinSurfaceGetWidth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t surfaceId = (int32_t) RValue_toReal(args[0]);
    if (surfaceId == -1) {
        return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowWidth);
    }
    logStubbedFunction(ctx, "surface_get_width");
    return RValue_makeReal(0.0);
}

static RValue builtinSurfaceGetHeight(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t surfaceId = (int32_t) RValue_toReal(args[0]);
    if (surfaceId == -1) {
        return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowHeight);
    }
    logStubbedFunction(ctx, "surface_get_height");
    return RValue_makeReal(0.0);
}

// Sprite functions
static RValue builtin_spriteGetWidth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].width);
}

static RValue builtin_spriteGetHeight(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].height);
}

static RValue builtin_spriteGetNumber(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].textureCount);
}

static RValue builtin_spriteGetXOffset(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].originX);
}

static RValue builtin_spriteGetYOffset(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].originY);
}

// sprite_create_from_surface(surface_id, x, y, w, h, removeback, smooth, xorig, yorig)
static RValue builtin_spriteCreateFromSurface(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || runner->renderer->vtable->createSpriteFromSurface == nullptr) return RValue_makeReal(-1);

    // surface_id (arg0) is ignored - we always capture from the application surface (FBO)
    int32_t x = RValue_toInt32(args[1]);
    int32_t y = RValue_toInt32(args[2]);
    int32_t w = RValue_toInt32(args[3]);
    int32_t h = RValue_toInt32(args[4]);
    bool removeback = RValue_toBool(args[5]);
    bool smooth = RValue_toBool(args[6]);
    int32_t xorig = RValue_toInt32(args[7]);
    int32_t yorig = RValue_toInt32(args[8]);

    int32_t result = runner->renderer->vtable->createSpriteFromSurface(runner->renderer, x, y, w, h, removeback, smooth, xorig, yorig);
    return RValue_makeReal((GMLReal) result);
}

// sprite_delete(sprite_index)
static RValue builtin_spriteDelete(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || runner->renderer->vtable->deleteSprite == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    runner->renderer->vtable->deleteSprite(runner->renderer, spriteIndex);
    return RValue_makeUndefined();
}

// Font/text measurement
static RValue builtin_stringWidth(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr || runner->renderer == nullptr) return RValue_makeReal(0.0);

    Renderer* renderer = runner->renderer;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || renderer->dataWin->font.count <= (uint32_t) fontIndex) return RValue_makeReal(0.0);

    Font* font = &renderer->dataWin->font.fonts[fontIndex];
    char* str = RValue_toString(args[0]);

    char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    free(str);
    int32_t textLen = (int32_t) strlen(processed);

    // Find the widest line
    float maxWidth = 0;
    int32_t lineStart = 0;
    while (textLen >= lineStart) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        if (lineWidth > maxWidth) maxWidth = lineWidth;

        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            break;
        }
    }

    free(processed);
    return RValue_makeReal((GMLReal) (maxWidth * font->scaleX));
}

static RValue builtin_stringHeight(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr || runner->renderer == nullptr) return RValue_makeReal(0.0);

    Renderer* renderer = runner->renderer;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || renderer->dataWin->font.count <= (uint32_t) fontIndex) return RValue_makeReal(0.0);

    Font* font = &renderer->dataWin->font.fonts[fontIndex];
    char* str = RValue_toString(args[0]);

    char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    free(str);
    int32_t textLen = (int32_t) strlen(processed);
    int32_t lineCount = TextUtils_countLines(processed, textLen);
    free(processed);

    return RValue_makeReal((GMLReal) ((float) lineCount * (float) font->emSize * font->scaleY));
}

STUB_RETURN_ZERO(string_width_ext)
STUB_RETURN_ZERO(string_height_ext)

// Color functions
static RValue builtinMakeColor(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    int32_t r = RValue_toInt32(args[0]);
    int32_t g = RValue_toInt32(args[1]);
    int32_t b = RValue_toInt32(args[2]);
    return RValue_makeReal((GMLReal) (r | (g << 8) | (b << 16)));
}

static RValue builtinMakeColour(VMContext* ctx, RValue* args, int32_t argCount) {
    return builtinMakeColor(ctx, args, argCount);
}

static RValue builtinMakeColorHsv(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    // GML uses 0-255 range for H, S, V
    GMLReal h = RValue_toReal(args[0]) / 255.0 * 360.0;
    GMLReal s = RValue_toReal(args[1]) / 255.0;
    GMLReal v = RValue_toReal(args[2]) / 255.0;

    GMLReal c = v * s;
    GMLReal x = c * (1.0 - GMLReal_fabs(GMLReal_fmod(h / 60.0, 2.0) - 1.0));
    GMLReal m = v - c;

    GMLReal r1, g1, b1;
    if (360.0 > h && h >= 300.0)      { r1 = c; g1 = 0; b1 = x; }
    else if (300.0 > h && h >= 240.0) { r1 = x; g1 = 0; b1 = c; }
    else if (240.0 > h && h >= 180.0) { r1 = 0; g1 = x; b1 = c; }
    else if (180.0 > h && h >= 120.0) { r1 = 0; g1 = c; b1 = x; }
    else if (120.0 > h && h >= 60.0)  { r1 = x; g1 = c; b1 = 0; }
    else                               { r1 = c; g1 = x; b1 = 0; }

    int32_t r = (int32_t) GMLReal_round((r1 + m) * 255.0);
    int32_t g = (int32_t) GMLReal_round((g1 + m) * 255.0);
    int32_t b = (int32_t) GMLReal_round((b1 + m) * 255.0);

    return RValue_makeReal((GMLReal) (r | (g << 8) | (b << 16)));
}

static RValue builtinMakeColourHsv(VMContext* ctx, RValue* args, int32_t argCount) {
    return builtinMakeColorHsv(ctx, args, argCount);
}

// Display stubs
STUB_RETURN_VALUE(display_get_width, 640.0)
STUB_RETURN_VALUE(display_get_height, 480.0)

// place_meeting(x, y, obj) - returns true if the calling instance would collide with obj at position (x, y)
static RValue builtinPlaceMeeting(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeBool(false);

    Runner* runner = (Runner*) ctx->runner;
    Instance* caller = (Instance*) ctx->currentInstance;
    if (caller == nullptr) return RValue_makeBool(false);

    GMLReal testX = RValue_toReal(args[0]);
    GMLReal testY = RValue_toReal(args[1]);
    int32_t target = RValue_toInt32(args[2]);

    // Save current position and temporarily move to test position
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool found = false;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || other == caller) continue;
            if (!Collision_matchesTarget(runner->dataWin, other, target)) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                found = true;
                break;
            }
        }
    }

    // Restore original position
    caller->x = savedX;
    caller->y = savedY;

    return RValue_makeBool(found);
}
// collision_line(x1, y1, x2, y2, obj, prec, notme)
static RValue builtinCollisionLine(VMContext* ctx, RValue* args, int32_t argCount) {
    if (7 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    GMLReal lx1 = RValue_toReal(args[0]);
    GMLReal ly1 = RValue_toReal(args[1]);
    GMLReal lx2 = RValue_toReal(args[2]);
    GMLReal ly2 = RValue_toReal(args[3]);
    int32_t targetObjIndex = RValue_toInt32(args[4]);
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);

    Instance* self = (Instance*) ctx->currentInstance;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (notme && inst == self) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // Fast reject: line's bounding rect vs instance bbox
        GMLReal lineLeft   = GMLReal_fmin(lx1, lx2);
        GMLReal lineRight  = GMLReal_fmax(lx1, lx2);
        GMLReal lineTop    = GMLReal_fmin(ly1, ly2);
        GMLReal lineBottom = GMLReal_fmax(ly1, ly2);
        // bbox.right/bbox.bottom are exclusive (marginRight + 1, marginBottom + 1), so use >= for those comparisons to correctly exclude boundary-touching cases
        // See GM-HTML5's yyInstance.js "Collision_Line"
        if (lineLeft >= bbox.right)
            continue;
        if (bbox.left > lineRight)
            continue;
        if (bbox.top > lineBottom)
            continue;
        if (lineTop >= bbox.bottom)
            continue;
        
        // Normalize line left-to-right for clipping
        GMLReal xl = lx1, yl = ly1, xr = lx2, yr = ly2;
        if (xl > xr) { GMLReal tmp = xl; xl = xr; xr = tmp; tmp = yl; yl = yr; yr = tmp; }

        GMLReal dx = xr - xl;
        GMLReal dy = yr - yl;

        // Clip line to bbox horizontally
        if (GMLReal_fabs(dx) > 0.0001) {
            if (bbox.left > xl) {
                GMLReal t = (bbox.left - xl) / dx;
                xl = bbox.left;
                yl = yl + t * dy;
            }
            if (xr > bbox.right) {
                GMLReal t = (bbox.right - xl) / (xr - xl);
                yr = yl + t * (yr - yl);
                xr = bbox.right;
            }
        }

        // Y-bounds check after horizontal clipping
        GMLReal clippedTop    = GMLReal_fmin(yl, yr);
        GMLReal clippedBottom = GMLReal_fmax(yl, yr);
        if (bbox.top > clippedBottom || clippedTop >= bbox.bottom) continue;

        // Bbox-only mode: collision confirmed
        if (prec == 0) {
            return RValue_makeReal((GMLReal) inst->instanceId);
        }

        // Precise mode: walk line pixel-by-pixel within bbox
        Sprite* spr = Collision_getSprite(ctx->dataWin, inst);
        if (spr == nullptr || spr->sepMasks != 1 || spr->masks == nullptr || spr->maskCount == 0) {
            // No precise mask available, treat as bbox hit
            return RValue_makeReal((GMLReal) inst->instanceId);
        }

        // Recompute dx/dy for the clipped segment
        GMLReal cdx = xr - xl;
        GMLReal cdy = yr - yl;
        bool found = false;

        if (GMLReal_fabs(cdy) >= GMLReal_fabs(cdx)) {
            // Vertical-major: normalize top-to-bottom
            GMLReal xt = xl, yt = yl, xb = xr, yb = yr;
            if (yt > yb) { GMLReal tmp = xt; xt = xb; xb = tmp; tmp = yt; yt = yb; yb = tmp; }
            GMLReal vdx = xb - xt;
            GMLReal vdy = yb - yt;

            int32_t startY = (int32_t) GMLReal_fmax(bbox.top, yt);
            int32_t endY   = (int32_t) GMLReal_fmin(bbox.bottom, yb);
            for (int32_t py = startY; endY >= py && !found; py++) {
                GMLReal px = (GMLReal_fabs(vdy) > 0.0001) ? xt + ((GMLReal) py - yt) * vdx / vdy : xt;
                if (Collision_pointInMask(spr, inst, px + 0.5, (GMLReal) py + 0.5)) {
                    found = true;
                }
            }
        } else {
            // Horizontal-major
            int32_t startX = (int32_t) GMLReal_fmax(bbox.left, xl);
            int32_t endX   = (int32_t) GMLReal_fmin(bbox.right, xr);
            for (int32_t px = startX; endX >= px && !found; px++) {
                GMLReal py = (GMLReal_fabs(cdx) > 0.0001) ? yl + ((GMLReal) px - xl) * cdy / cdx : yl;
                if (Collision_pointInMask(spr, inst, (GMLReal) px + 0.5, py + 0.5)) {
                    found = true;
                }
            }
        }

        if (!found) continue;
        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// collision_rectangle(x1, y1, x2, y2, obj, prec, notme)
static RValue builtinCollisionRectangle(VMContext* ctx, RValue* args, int32_t argCount) {
    if (7 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    GMLReal x1 = RValue_toReal(args[0]);
    GMLReal y1 = RValue_toReal(args[1]);
    GMLReal x2 = RValue_toReal(args[2]);
    GMLReal y2 = RValue_toReal(args[3]);
    int32_t targetObjIndex = RValue_toInt32(args[4]);
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);

    // Normalize rect
    if (x1 > x2) { GMLReal tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { GMLReal tmp = y1; y1 = y2; y2 = tmp; }

    Instance* self = (Instance*) ctx->currentInstance;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (notme && inst == self) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // AABB overlap test
        if (x1 >= bbox.right || bbox.left >= x2 || y1 >= bbox.bottom || bbox.top >= y2) continue;

        // Precise check if requested and sprite has precise masks
        if (prec != 0) {
            Sprite* spr = Collision_getSprite(ctx->dataWin, inst);
            if (spr != nullptr && spr->sepMasks == 1 && spr->masks != nullptr && spr->maskCount > 0) {
                // Check if any pixel in the overlap region hits the mask
                GMLReal iLeft   = GMLReal_fmax(x1, bbox.left);
                GMLReal iRight  = GMLReal_fmin(x2, bbox.right);
                GMLReal iTop    = GMLReal_fmax(y1, bbox.top);
                GMLReal iBottom = GMLReal_fmin(y2, bbox.bottom);

                bool found = false;
                int32_t startX = (int32_t) GMLReal_floor(iLeft);
                int32_t endX   = (int32_t) GMLReal_ceil(iRight);
                int32_t startY = (int32_t) GMLReal_floor(iTop);
                int32_t endY   = (int32_t) GMLReal_ceil(iBottom);

                for (int32_t py = startY; endY > py && !found; py++) {
                    for (int32_t px = startX; endX > px && !found; px++) {
                        if (Collision_pointInMask(spr, inst, (GMLReal) px + 0.5, (GMLReal) py + 0.5)) {
                            found = true;
                        }
                    }
                }
                if (!found) continue;
            }
        }

        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// collision_point(x, y, obj, prec, notme)
static RValue builtinCollisionPoint(VMContext* ctx, RValue* args, int32_t argCount) {
    if (5 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = RValue_toInt32(args[2]);
    int32_t prec = RValue_toInt32(args[3]);
    int32_t notme = RValue_toInt32(args[4]);

    Instance* self = (Instance*) ctx->currentInstance;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (notme && inst == self) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // Point-in-AABB test
        if (bbox.left > px || px >= bbox.right || bbox.top > py || py >= bbox.bottom) continue;

        // Precise check if requested
        if (prec != 0) {
            Sprite* spr = Collision_getSprite(ctx->dataWin, inst);
            if (spr != nullptr && spr->sepMasks == 1 && spr->masks != nullptr && spr->maskCount > 0) {
                if (!Collision_pointInMask(spr, inst, px, py)) continue;
            }
        }

        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// instance_position(x, y, obj)
static RValue builtinInstancePosition(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = RValue_toInt32(args[2]);

    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // Point-in-AABB test (no precise, no notme)
        if (bbox.left > px || px >= bbox.right || bbox.top > py || py >= bbox.bottom) continue;

        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// Misc stubs
STUB_RETURN_ZERO(get_timer)
static RValue builtinActionSetAlarm(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t steps = RValue_toInt32(args[0]);
    int32_t alarmIndex = RValue_toInt32(args[1]);

    if (0 > alarmIndex || alarmIndex >= GML_ALARM_COUNT) {
        return RValue_makeUndefined();
    }

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        Runner* runner = (Runner*) ctx->runner;

#ifndef DISABLE_VM_TRACING
        if (shgeti(ctx->alarmsToBeTraced, "*") != -1 || shgeti(ctx->alarmsToBeTraced, runner->dataWin->objt.objects[inst->objectIndex].name) != -1) {
            fprintf(stderr, "VM: [%s] Setting Alarm[%d] = %d (instanceId=%d)\n", runner->dataWin->objt.objects[inst->objectIndex].name, alarmIndex, steps, inst->instanceId);
        }
#endif

        inst->alarm[alarmIndex] = steps;
    }

    return RValue_makeUndefined();
}

static RValue builtinAlarmSet(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t alarmIndex = RValue_toInt32(args[0]);
    int32_t value = RValue_toInt32(args[1]);

    if (0 > alarmIndex || alarmIndex >= GML_ALARM_COUNT) {
        return RValue_makeUndefined();
    }

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;

#ifndef DISABLE_VM_TRACING
        Runner* runner = (Runner*) ctx->runner;
        if (shgeti(ctx->alarmsToBeTraced, "*") != -1 || shgeti(ctx->alarmsToBeTraced, runner->dataWin->objt.objects[inst->objectIndex].name) != -1) {
            fprintf(stderr, "VM: [%s] Setting Alarm[%d] = %d (instanceId=%d)\n", runner->dataWin->objt.objects[inst->objectIndex].name, alarmIndex, value, inst->instanceId);
        }
#endif

        inst->alarm[alarmIndex] = value;
    }

    return RValue_makeUndefined();
}

static RValue builtinAlarmGet(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t alarmIndex = RValue_toInt32(args[0]);

    if (0 > alarmIndex || alarmIndex >= GML_ALARM_COUNT) {
        return RValue_makeReal(-1);
    }

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        return RValue_makeReal((GMLReal) inst->alarm[alarmIndex]);
    }

    return RValue_makeReal(-1);
}

static RValue builtinActionIfVariable(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    bool check;
    switch (args[0].type) {
        case RVALUE_REAL: {
            check = args[0].real != 0.0;
            break;
        }
        case RVALUE_INT32: {
            check = args[0].int32 != 0;
            break;
        }
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: {
            check = args[0].int64 != 0;
            break;
        }
#endif
        case RVALUE_BOOL: {
            check = args[0].int32 != 0;
            break;
        }
        case RVALUE_STRING: {
            check = args[0].string != nullptr && args[0].string[0] != '\0';
            break;
        }
        default: {
            check = false;
            break;
        }
    }

    if (check) {
        return args[1];
    } else {
        return args[2];
    }
}

STUB_RETURN_UNDEFINED(action_sound)

// ===[ Tile Layer Functions ]===

static TileLayerState* getOrCreateTileLayer(Runner* runner, int32_t depth) {
    ptrdiff_t idx = hmgeti(runner->tileLayerMap, depth);
    if (0 > idx) {
        TileLayerState defaultVal = { .visible = true, .offsetX = 0.0f, .offsetY = 0.0f };
        hmput(runner->tileLayerMap, depth, defaultVal);
        idx = hmgeti(runner->tileLayerMap, depth);
    }
    return &runner->tileLayerMap[idx].value;
}

static RValue builtinTileLayerHide(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    TileLayerState* layer = getOrCreateTileLayer(runner, depth);
    layer->visible = false;
    return RValue_makeUndefined();
}

static RValue builtinTileLayerShow(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    TileLayerState* layer = getOrCreateTileLayer(runner, depth);
    layer->visible = true;
    return RValue_makeUndefined();
}

static RValue builtinTileLayerShift(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    float dx = (float) RValue_toReal(args[1]);
    float dy = (float) RValue_toReal(args[2]);
    TileLayerState* layer = getOrCreateTileLayer(runner, depth);
    layer->offsetX += dx;
    layer->offsetY += dy;
    return RValue_makeUndefined();
}

// ===[ PATH FUNCTIONS ]===

// path_start(path, speed, endaction, absolute) - HTML5: Assign_Path (yyInstance.js:2695-2743)
static RValue builtinPathStart(VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) return RValue_makeUndefined();

    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeUndefined();

    Runner* runner = (Runner*) ctx->runner;
    if (runner == nullptr) return RValue_makeUndefined();

    int32_t pathIdx = RValue_toInt32(args[0]);
    GMLReal speed = RValue_toReal(args[1]);
    int32_t endAction = RValue_toInt32(args[2]);
    bool absolute = RValue_toBool(args[3]);

    // Validate path index
    inst->pathIndex = -1;
    if (0 > pathIdx) return RValue_makeUndefined();
    if ((uint32_t) pathIdx >= runner->dataWin->path.count) return RValue_makeUndefined();

    GamePath* path = &runner->dataWin->path.paths[pathIdx];
    if (0.0 >= path->length) return RValue_makeUndefined();

    inst->pathIndex = pathIdx;
    inst->pathSpeed = (float) speed;

    if (inst->pathSpeed >= 0.0f) {
        inst->pathPosition = 0.0f;
    } else {
        inst->pathPosition = 1.0f;
    }

    inst->pathPositionPrevious = inst->pathPosition;
    inst->pathScale = 1.0f;
    inst->pathOrientation = 0.0f;
    inst->pathEndAction = endAction;

    if (absolute) {
        PathPositionResult startPos = GamePath_getPosition(path, inst->pathSpeed >= 0.0f ? 0.0 : 1.0);
        inst->x = (float) startPos.x;
        inst->y = (float) startPos.y;

        PathPositionResult origin = GamePath_getPosition(path, 0.0);
        inst->pathXStart = (float) origin.x;
        inst->pathYStart = (float) origin.y;
    } else {
        inst->pathXStart = inst->x;
        inst->pathYStart = inst->y;
    }

    return RValue_makeUndefined();
}

// path_end() - HTML5: Assign_Path(-1,...)
static RValue builtinPathEnd(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst != nullptr) {
        inst->pathIndex = -1;
    }
    return RValue_makeUndefined();
}

// string_hash_to_newline - converts # to \n in a string
static RValue builtinStringHashToNewline(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) { 
    if (1 > argCount) return RValue_makeString(""); 
    char* str = RValue_toString(args[0]); 
    char *result = TextUtils_preprocessGmlText(str);
    free(str); 
    return RValue_makeOwnedString(result);
}

// json_decode
static RValue builtinJsonDecode(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[json_decode] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    int32_t mapIndex = dsMapCreate();
    DsMapEntry **mapPtr = dsMapGet(mapIndex);
    const char* content = args[0].string;
    const JsonValue* json = JsonReader_parse(content);

    repeat(JsonReader_objectLength(json), i) {
        const char *key = safeStrdup(JsonReader_getObjectKey(json, i));
        RValue val = RValue_makeOwnedString(safeStrdup(JsonReader_getString(JsonReader_getObjectValue(json, i))));
        shput(*mapPtr, key, val);
    }

    JsonReader_free(json);

    return RValue_makeReal((double) mapIndex);
}

static RValue builtinObjectGetSprite(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[object_get_sprite] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    int32_t id = RValue_toInt32(args[0]);

    return RValue_makeReal((double) ctx->dataWin->objt.objects[id].spriteId);
}

// Shared implementation for font_add_sprite and font_add_sprite_ext
static RValue fontAddSpriteImpl(VMContext* ctx, int32_t spriteIndex, uint16_t* charCodes, uint32_t charCount, bool proportional, int32_t sep) {
    DataWin* dw = ctx->dataWin;

    if (0 > spriteIndex || (uint32_t) spriteIndex >= dw->sprt.count) {
        fprintf(stderr, "[font_add_sprite] Invalid sprite index %d\n", spriteIndex);
        return RValue_makeReal(-1.0);
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];

    if (charCount == 0 || sprite->textureCount == 0) {
        return RValue_makeReal(-1.0);
    }

    // Limit glyph count to sprite frame count
    uint32_t glyphCount = charCount;
    if (glyphCount > sprite->textureCount) glyphCount = sprite->textureCount;

    // Compute emSize (max bounding height across all frames) and biggestShift
    uint32_t maxHeight = 0;
    int32_t biggestShift = 0;
    repeat(glyphCount, i) {
        int32_t tpagIdx = DataWin_resolveTPAG(dw, sprite->textureOffsets[i]);
        if (0 > tpagIdx) continue;
        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        if (tpag->boundingHeight > maxHeight) maxHeight = tpag->boundingHeight;
        int32_t width = proportional ? (int32_t) tpag->sourceWidth : (int32_t) tpag->boundingWidth;
        if (width > biggestShift) biggestShift = width;
    }

    // Check if space (0x20) is in the string map
    bool hasSpace = false;
    repeat(glyphCount, i) {
        if (charCodes[i] == 0x20) { hasSpace = true; break; }
    }

    // Allocate glyphs (+ 1 for synthetic space if needed)
    uint32_t totalGlyphs = hasSpace ? glyphCount : glyphCount + 1;
    FontGlyph* glyphs = safeMalloc(totalGlyphs * sizeof(FontGlyph));

    repeat(glyphCount, i) {
        int32_t tpagIdx = DataWin_resolveTPAG(dw, sprite->textureOffsets[i]);
        FontGlyph* glyph = &glyphs[i];
        glyph->character = charCodes[i];
        glyph->kerningCount = 0;
        glyph->kerning = nullptr;

        if (0 > tpagIdx) {
            glyph->sourceX = 0;
            glyph->sourceY = 0;
            glyph->sourceWidth = 0;
            glyph->sourceHeight = 0;
            glyph->shift = (int16_t) sep;
            glyph->offset = 0;
            continue;
        }

        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        glyph->sourceX = 0; // not used for sprite fonts (TPAG resolved per glyph)
        glyph->sourceY = 0;
        glyph->sourceWidth = tpag->sourceWidth;
        glyph->sourceHeight = tpag->sourceHeight;

        int32_t advanceWidth = proportional ? (int32_t) tpag->sourceWidth : (int32_t) tpag->boundingWidth;
        glyph->shift = (int16_t) (advanceWidth + sep);

        // Horizontal offset: for proportional fonts, no offset; for non-proportional, use target offset minus origin
        glyph->offset = proportional ? 0 : (int16_t) ((int32_t) tpag->targetX - sprite->originX);
    }

    // Add synthetic space glyph if space is not in the string map
    if (!hasSpace) {
        FontGlyph* spaceGlyph = &glyphs[glyphCount];
        spaceGlyph->character = 0x20;
        spaceGlyph->sourceX = 0;
        spaceGlyph->sourceY = 0;
        spaceGlyph->sourceWidth = 0;
        spaceGlyph->sourceHeight = 0;
        spaceGlyph->shift = (int16_t) (biggestShift + sep);
        spaceGlyph->offset = 0;
        spaceGlyph->kerningCount = 0;
        spaceGlyph->kerning = nullptr;
    }

    // Grow the font array and create the new font
    uint32_t newFontIndex = dw->font.count;
    dw->font.count++;
    dw->font.fonts = safeRealloc(dw->font.fonts, dw->font.count * sizeof(Font));

    Font* font = &dw->font.fonts[newFontIndex];
    font->name = "sprite_font";
    font->displayName = "sprite_font";
    font->emSize = (maxHeight > 0) ? maxHeight : sprite->height;
    font->bold = false;
    font->italic = false;
    font->rangeStart = 0;
    font->charset = 0;
    font->antiAliasing = 0;
    font->rangeEnd = 0;
    font->textureOffset = 0; // not used for sprite fonts
    font->scaleX = 1.0f;
    font->scaleY = 1.0f;
    font->glyphCount = totalGlyphs;
    font->glyphs = glyphs;
    font->isSpriteFont = true;
    font->spriteIndex = spriteIndex;

    return RValue_makeReal((GMLReal) newFontIndex);
}

// font_add_sprite_ext(sprite, string_map, prop, sep)
static RValue builtinFontAddSpriteExt(VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) {
        fprintf(stderr, "[font_add_sprite_ext] Expected 4 arguments, got %d\n", argCount);
        return RValue_makeReal(-1.0);
    }

    int32_t spriteIndex = RValue_toInt32(args[0]);
    char* stringMap = RValue_toString(args[1]);
    bool proportional = RValue_toBool(args[2]);
    int32_t sep = RValue_toInt32(args[3]);

    // Decode the string map to get character codes (UTF-8 -> codepoints)
    int32_t mapLen = (int32_t) strlen(stringMap);
    int32_t mapPos = 0;
    uint32_t charCount = 0;
    uint16_t charCodes[1024];
    while (mapLen > mapPos && 1024 > charCount) {
        charCodes[charCount++] = TextUtils_decodeUtf8(stringMap, mapLen, &mapPos);
    }
    free(stringMap);

    return fontAddSpriteImpl(ctx, spriteIndex, charCodes, charCount, proportional, sep);
}

// font_add_sprite(sprite, first, prop, sep)
static RValue builtinFontAddSprite(VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) {
        fprintf(stderr, "[font_add_sprite] Expected 4 arguments, got %d\n", argCount);
        return RValue_makeReal(-1.0);
    }

    DataWin* dw = ctx->dataWin;
    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t first = RValue_toInt32(args[1]);
    bool proportional = RValue_toBool(args[2]);
    int32_t sep = RValue_toInt32(args[3]);

    // Build sequential character codes: first, first+1, first+2, ...
    uint32_t frameCount = 0;
    if (spriteIndex >= 0 && dw->sprt.count > (uint32_t) spriteIndex) {
        frameCount = dw->sprt.sprites[spriteIndex].textureCount;
    }
    if (frameCount > 1024) frameCount = 1024;

    uint16_t charCodes[1024];
    repeat(frameCount, i) {
        charCodes[i] = (uint16_t) (first + (int32_t) i);
    }

    return fontAddSpriteImpl(ctx, spriteIndex, charCodes, frameCount, proportional, sep);
}

static RValue builtinAssetGetIndex(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[asset_get_index] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    const char* name = RValue_toString(args[0]);

    repeat(ctx->dataWin->objt.count, i) {
        if (strcmp(ctx->dataWin->objt.objects[i].name, name) == 0)
            return RValue_makeReal((double) i);
    }

    return RValue_makeReal((double) -1);
}

// ===[ REGISTRATION ]===

void VMBuiltins_registerAll(bool isGMS2) {
    requireMessage(!initialized, "Attempting to register all VMBuiltins, but it was already registered!");
    initialized = true;

    // Core output
    registerBuiltin("show_debug_message", builtinShowDebugMessage);

    // String functions
    registerBuiltin("string_length", builtinStringLength);
    registerBuiltin("string", builtinString);
    registerBuiltin("string_upper", builtinStringUpper);
    registerBuiltin("string_lower", builtinStringLower);
    registerBuiltin("string_copy", builtinStringCopy);
    registerBuiltin("string_pos", builtinStringPos);
    registerBuiltin("string_char_at", builtinStringCharAt);
    registerBuiltin("string_delete", builtinStringDelete);
    registerBuiltin("string_insert", builtinStringInsert);
    registerBuiltin("string_replace_all", builtinStringReplaceAll);
    registerBuiltin("string_repeat", builtinStringRepeat);
    registerBuiltin("ord", builtinOrd);
    registerBuiltin("chr", builtinChr);

    // Type functions
    registerBuiltin("real", builtinReal);
    registerBuiltin("is_string", builtinIsString);
    registerBuiltin("is_real", builtinIsReal);
    registerBuiltin("is_undefined", builtinIsUndefined);

    // Math functions
    registerBuiltin("floor", builtinFloor);
    registerBuiltin("ceil", builtinCeil);
    registerBuiltin("round", builtinRound);
    registerBuiltin("abs", builtinAbs);
    registerBuiltin("sign", builtinSign);
    registerBuiltin("max", builtinMax);
    registerBuiltin("min", builtinMin);
    registerBuiltin("power", builtinPower);
    registerBuiltin("sqrt", builtinSqrt);
    registerBuiltin("sqr", builtinSqr);
    registerBuiltin("sin", builtinSin);
    registerBuiltin("cos", builtinCos);
    registerBuiltin("darctan2", builtinDarctan2);
    registerBuiltin("degtorad", builtinDegtorad);
    registerBuiltin("radtodeg", builtinRadtodeg);
    registerBuiltin("clamp", builtinClamp);
    registerBuiltin("lerp", builtinLerp);
    registerBuiltin("point_distance", builtinPointDistance);
    registerBuiltin("point_direction", builtinPointDirection);
    registerBuiltin("distance_to_point", builtinDistanceToPoint);
    registerBuiltin("distance_to_object", builtinDistanceToObject);
    registerBuiltin("move_towards_point", builtinMoveTowardsPoint);
    registerBuiltin("action_move_point", builtinMoveTowardsPoint);
    registerBuiltin("move_snap", builtinMoveSnap);
    registerBuiltin("lengthdir_x", builtinLengthdir_x);
    registerBuiltin("lengthdir_y", builtinLengthdir_y);

    // Random
    registerBuiltin("random", builtinRandom);
    registerBuiltin("random_range", builtinRandomRange);
    registerBuiltin("irandom", builtinIrandom);
    registerBuiltin("irandom_range", builtinIrandomRange);
    registerBuiltin("choose", builtinChoose);
    registerBuiltin("randomize", builtinRandomize);

    // Room
    registerBuiltin("room_get_name", builtinRoomGetName);
    registerBuiltin("room_goto_next", builtinRoomGotoNext);
    registerBuiltin("room_goto_previous", builtinRoomGotoPrevious);
    registerBuiltin("room_goto", builtinRoomGoto);
    registerBuiltin("room_restart", builtinRoomRestart);
    registerBuiltin("room_next", builtinRoomNext);
    registerBuiltin("room_previous", builtinRoomPrevious);
    registerBuiltin("room_set_persistent", builtinRoomSetPersistent);

    // GMS2 camera compatibility
    registerBuiltin("view_get_camera", builtinViewGetCamera);
    registerBuiltin("camera_get_view_x", builtinCameraGetViewX);
    registerBuiltin("camera_get_view_y", builtinCameraGetViewY);
    registerBuiltin("camera_get_view_width", builtinCameraGetViewWidth);
    registerBuiltin("camera_get_view_height", builtinCameraGetViewHeight);
    registerBuiltin("camera_set_view_pos", builtinCameraSetViewPos);

    // Variables
    registerBuiltin("variable_global_exists", builtinVariableGlobalExists);
    registerBuiltin("variable_global_get", builtinVariableGlobalGet);
    registerBuiltin("variable_global_set", builtinVariableGlobalSet);

    // Script
    registerBuiltin("script_execute", builtinScriptExecute);

    // OS
    registerBuiltin("os_get_language", builtinOsGetLanguage);
    registerBuiltin("os_get_region", builtinOsGetRegion);

    // ds_map
    registerBuiltin("ds_map_create", builtinDsMapCreate);
    registerBuiltin("ds_map_add", builtinDsMapAdd);
    registerBuiltin("ds_map_set", builtinDsMapSet);
    registerBuiltin("ds_map_replace", builtinDsMapReplace);
    registerBuiltin("ds_map_find_value", builtinDsMapFindValue);
    registerBuiltin("ds_map_exists", builtinDsMapExists);
    registerBuiltin("ds_map_find_first", builtinDsMapFindFirst);
    registerBuiltin("ds_map_find_next", builtinDsMapFindNext);
    registerBuiltin("ds_map_size", builtinDsMapSize);
    registerBuiltin("ds_map_destroy", builtinDsMapDestroy);

    // ds_list stubs
    registerBuiltin("ds_list_create", builtinDsListCreate);
    registerBuiltin("ds_list_add", builtinDsListAdd);
    registerBuiltin("ds_list_size", builtinDsListSize);
    registerBuiltin("ds_list_find_index", builtinDsListFindIndex);

    // Array
    registerBuiltin("array_length_1d", builtinArrayLength1d);

    // Steam stubs
    registerBuiltin("steam_initialised", builtin_steam_initialised);
    registerBuiltin("steam_stats_ready", builtin_steam_stats_ready);
    registerBuiltin("steam_file_exists", builtin_steam_file_exists);
    registerBuiltin("steam_file_write", builtin_steam_file_write);
    registerBuiltin("steam_file_read", builtin_steam_file_read);
    registerBuiltin("steam_get_persona_name", builtin_steam_get_persona_name);

    // Audio
    registerBuiltin("audio_channel_num", builtin_audioChannelNum);
    registerBuiltin("audio_play_sound", builtin_audioPlaySound);
    registerBuiltin("audio_stop_sound", builtin_audioStopSound);
    registerBuiltin("audio_stop_all", builtin_audioStopAll);
    registerBuiltin("audio_is_playing", builtin_audioIsPlaying);
    registerBuiltin("audio_is_paused", builtin_audioIsPaused);
    registerBuiltin("audio_sound_gain", builtin_audioSoundGain);
    registerBuiltin("audio_sound_pitch", builtin_audioSoundPitch);
    registerBuiltin("audio_sound_get_gain", builtin_audioSoundGetGain);
    registerBuiltin("audio_sound_get_pitch", builtin_audioSoundGetPitch);
    registerBuiltin("audio_master_gain", builtin_audioMasterGain);
    registerBuiltin("audio_group_load", builtin_audioGroupLoad);
    registerBuiltin("audio_group_is_loaded", builtin_audioGroupIsLoaded);
    registerBuiltin("audio_play_music", builtin_audioPlayMusic);
    registerBuiltin("audio_stop_music", builtin_audioStopMusic);
    registerBuiltin("audio_music_gain", builtin_audioMusicGain);
    registerBuiltin("audio_music_is_playing", builtin_audioMusicIsPlaying);
    registerBuiltin("audio_pause_sound", builtin_audioPauseSound);
    registerBuiltin("audio_resume_sound", builtin_audioResumeSound);
    registerBuiltin("audio_pause_all", builtin_audioPauseAll);
    registerBuiltin("audio_resume_all", builtin_audioResumeAll);
    registerBuiltin("audio_sound_get_track_position", builtin_audioSoundGetTrackPosition);
    registerBuiltin("audio_sound_set_track_position", builtin_audioSoundSetTrackPosition);
    registerBuiltin("audio_create_stream", builtin_audioCreateStream);
    registerBuiltin("audio_destroy_stream", builtin_audioDestroyStream);

    // Application surface
    registerBuiltin("application_surface_enable", builtin_application_surface_enable);
    registerBuiltin("application_surface_draw_enable", builtin_application_surface_draw_enable);

    // Gamepad
    registerBuiltin("gamepad_get_device_count", builtin_gamepad_get_device_count);
    registerBuiltin("gamepad_is_connected", builtin_gamepad_is_connected);
    registerBuiltin("gamepad_button_check", builtin_gamepad_button_check);
    registerBuiltin("gamepad_button_check_pressed", builtin_gamepad_button_check_pressed);
    registerBuiltin("gamepad_button_check_released", builtin_gamepad_button_check_released);
    registerBuiltin("gamepad_axis_value", builtin_gamepad_axis_value);
    registerBuiltin("gamepad_get_description", builtin_gamepad_get_description);
    registerBuiltin("gamepad_button_value", builtin_gamepad_button_value);

    // INI
    registerBuiltin("ini_open", builtinIniOpen);
    registerBuiltin("ini_close", builtinIniClose);
    registerBuiltin("ini_write_real", builtinIniWriteReal);
    registerBuiltin("ini_write_string", builtinIniWriteString);
    registerBuiltin("ini_read_string", builtinIniReadString);
    registerBuiltin("ini_read_real", builtinIniReadReal);
    registerBuiltin("ini_section_exists", builtinIniSectionExists);

    // File
    registerBuiltin("file_exists", builtinFileExists);
    registerBuiltin("file_text_open_write", builtinFileTextOpenWrite);
    registerBuiltin("file_text_open_read", builtinFileTextOpenRead);
    registerBuiltin("file_text_close", builtinFileTextClose);
    registerBuiltin("file_text_write_string", builtinFileTextWriteString);
    registerBuiltin("file_text_writeln", builtinFileTextWriteln);
    registerBuiltin("file_text_write_real", builtinFileTextWriteReal);
    registerBuiltin("file_text_eof", builtinFileTextEof);
    registerBuiltin("file_delete", builtinFileDelete);
    registerBuiltin("file_text_read_string", builtinFileTextReadString);
    registerBuiltin("file_text_read_real", builtinFileTextReadReal);
    registerBuiltin("file_text_readln", builtinFileTextReadln);

    // Keyboard
    registerBuiltin("keyboard_check", builtinKeyboardCheck);
    registerBuiltin("keyboard_check_pressed", builtinKeyboardCheckPressed);
    registerBuiltin("keyboard_check_released", builtinKeyboardCheckReleased);
    registerBuiltin("keyboard_check_direct", builtinKeyboardCheckDirect);
    registerBuiltin("keyboard_key_press", builtinKeyboardKeyPress);
    registerBuiltin("keyboard_key_release", builtinKeyboardKeyRelease);
    registerBuiltin("keyboard_clear", builtinKeyboardClear);

    // Joystick
    registerBuiltin("joystick_exists", builtin_joystick_exists);
    registerBuiltin("joystick_xpos", builtin_joystick_xpos);
    registerBuiltin("joystick_ypos", builtin_joystick_ypos);
    registerBuiltin("joystick_direction", builtin_joystick_direction);
    registerBuiltin("joystick_pov", builtin_joystick_pov);
    registerBuiltin("joystick_check_button", builtin_joystick_check_button);

    // Window
    registerBuiltin("window_get_fullscreen", builtin_window_get_fullscreen);
    registerBuiltin("window_set_fullscreen", builtin_window_set_fullscreen);
    registerBuiltin("window_set_caption", builtin_window_set_caption);
    registerBuiltin("window_set_size", builtin_window_set_size);
    registerBuiltin("window_center", builtin_window_center);
    registerBuiltin("window_get_width", builtinWindowGetWidth);
    registerBuiltin("window_get_height", builtinWindowGetHeight);

    // Game
    registerBuiltin("game_restart", builtin_game_restart);
    registerBuiltin("game_end", builtinGameEnd);
    registerBuiltin("game_save", builtin_game_save);
    registerBuiltin("game_load", builtin_game_load);

    // Instance
    registerBuiltin("instance_exists", builtinInstanceExists);
    registerBuiltin("instance_number", builtinInstanceNumber);
    registerBuiltin("instance_find", builtinInstanceFind);
    registerBuiltin("instance_destroy", builtinInstanceDestroy);
    if(!isGMS2) {
        registerBuiltin("instance_create", builtinInstanceCreate);
    }
    else {
        registerBuiltin("instance_create_depth", builtinInstanceCreateDepth);
    }
    registerBuiltin("instance_change", builtinInstanceChange);
    registerBuiltin("instance_deactivate_all", builtinInstanceDeactivateAll);
    registerBuiltin("instance_activate_all", builtinInstanceActivateAll);
    registerBuiltin("instance_activate_object", builtinInstanceActivateObject);
    registerBuiltin("instance_deactivate_object", builtinInstanceDeactivateObject);
    registerBuiltin("action_kill_object", builtinActionKillObject);
    registerBuiltin("action_create_object", builtinActionCreateObject);
    registerBuiltin("action_set_relative", builtinActionSetRelative);
    registerBuiltin("action_move", builtinActionMove);
    registerBuiltin("action_move_to", builtinActionMoveTo);
    registerBuiltin("action_set_friction", builtinActionSetFriction);
    registerBuiltin("action_set_gravity", builtinActionSetGravity);
    registerBuiltin("action_set_hspeed", builtinActionSetHspeed);
    registerBuiltin("action_set_vspeed", builtinActionSetVspeed);
    registerBuiltin("event_inherited", builtinEventInherited);
    registerBuiltin("event_user", builtinEventUser);
    registerBuiltin("event_perform", builtinEventPerform);

    // Buffer
    registerBuiltin("buffer_create", builtin_buffer_create);
    registerBuiltin("buffer_delete", builtin_buffer_delete);
    registerBuiltin("buffer_write", builtin_buffer_write);
    registerBuiltin("buffer_read", builtin_buffer_read);
    registerBuiltin("buffer_seek", builtin_buffer_seek);
    registerBuiltin("buffer_tell", builtin_buffer_tell);
    registerBuiltin("buffer_get_size", builtin_buffer_get_size);
    registerBuiltin("buffer_base64_encode", builtin_buffer_base64_encode);

    // PSN
    registerBuiltin("psn_init", builtin_psn_init);
    registerBuiltin("psn_default_user", builtin_psn_default_user);
    registerBuiltin("psn_get_leaderboard_score", builtin_psn_get_leaderboard_score);

    // Draw
    registerBuiltin("draw_sprite", builtin_drawSprite);
    registerBuiltin("draw_sprite_ext", builtin_drawSpriteExt);
    registerBuiltin("draw_sprite_stretched", builtin_drawSpriteStretched);
    registerBuiltin("draw_sprite_stretched_ext", builtin_drawSpriteStretchedExt);
    registerBuiltin("draw_sprite_part", builtin_drawSpritePart);
    registerBuiltin("draw_sprite_part_ext", builtin_drawSpritePartExt);
    registerBuiltin("draw_rectangle", builtin_drawRectangle);
    registerBuiltin("draw_rectangle_color", builtin_drawRectangleColor);
    registerBuiltin("draw_healthbar", builtin_drawHealthbar);
    registerBuiltin("draw_set_color", builtin_drawSetColor);
    registerBuiltin("draw_set_alpha", builtin_drawSetAlpha);
    registerBuiltin("draw_set_font", builtin_drawSetFont);
    registerBuiltin("draw_set_halign", builtin_drawSetHalign);
    registerBuiltin("draw_set_valign", builtin_drawSetValign);
    registerBuiltin("draw_text", builtin_drawText);
    registerBuiltin("draw_text_transformed", builtin_drawTextTransformed);
    registerBuiltin("draw_text_ext", builtin_draw_text_ext);
    registerBuiltin("draw_text_ext_transformed", builtin_draw_text_ext_transformed);
    registerBuiltin("draw_text_color", builtin_drawTextColor);
    registerBuiltin("draw_text_color_transformed", builtin_drawTextColorTransformed);
    registerBuiltin("draw_text_color_ext", builtin_draw_text_color_ext);
    registerBuiltin("draw_text_color_ext_transformed", builtin_draw_text_color_ext_transformed);
    registerBuiltin("draw_text_colour", builtin_drawTextColor);
    registerBuiltin("draw_text_colour_transformed", builtin_drawTextColorTransformed);
    registerBuiltin("draw_text_colour_ext", builtin_draw_text_color_ext);
    registerBuiltin("draw_text_colour_ext_transformed", builtin_draw_text_color_ext_transformed);
    registerBuiltin("draw_surface", builtin_draw_surface);
    registerBuiltin("draw_surface_ext", builtin_draw_surface_ext);
    if(!isGMS2) {
        registerBuiltin("draw_background", builtin_drawBackground);
        registerBuiltin("draw_background_ext", builtin_drawBackgroundExt);
        registerBuiltin("draw_background_stretched", builtin_drawBackgroundStretched);
        registerBuiltin("draw_background_part_ext", builtin_drawBackgroundPartExt);
        registerBuiltin("background_get_width", builtinBackgroundGetWidth);
        registerBuiltin("background_get_height", builtinBackgroundGetHeight);
    }
    registerBuiltin("draw_self", builtin_draw_self);
    registerBuiltin("draw_line", builtin_draw_line);
    registerBuiltin("draw_line_width", builtin_draw_line_width);
    registerBuiltin("draw_line_width_colour", builtin_draw_line_width_colour);
    registerBuiltin("draw_line_width_color", builtin_draw_line_width_colour);
    registerBuiltin("draw_triangle", builtin_draw_triangle);
    registerBuiltin("draw_set_colour", builtin_draw_set_colour);
    registerBuiltin("draw_get_colour", builtin_draw_get_colour);
    registerBuiltin("draw_get_color", builtin_draw_get_color);
    registerBuiltin("draw_get_alpha", builtin_draw_get_alpha);

    // Color
    registerBuiltin("merge_color", builtinMergeColor);
    registerBuiltin("merge_colour", builtinMergeColor);

    // Surface
    registerBuiltin("surface_create", builtin_surface_create);
    registerBuiltin("surface_free", builtin_surface_free);
    registerBuiltin("surface_set_target", builtin_surface_set_target);
    registerBuiltin("surface_reset_target", builtin_surface_reset_target);
    registerBuiltin("surface_exists", builtin_surface_exists);
    registerBuiltin("surface_get_width", builtinSurfaceGetWidth);
    registerBuiltin("surface_get_height", builtinSurfaceGetHeight);

    // Sprite info
    registerBuiltin("sprite_get_width", builtin_spriteGetWidth);
    registerBuiltin("sprite_get_height", builtin_spriteGetHeight);
    registerBuiltin("sprite_get_number", builtin_spriteGetNumber);
    registerBuiltin("sprite_get_xoffset", builtin_spriteGetXOffset);
    registerBuiltin("sprite_get_yoffset", builtin_spriteGetYOffset);
    registerBuiltin("sprite_create_from_surface", builtin_spriteCreateFromSurface);
    registerBuiltin("sprite_delete", builtin_spriteDelete);

    // Text measurement
    registerBuiltin("string_width", builtin_stringWidth);
    registerBuiltin("string_height", builtin_stringHeight);
    registerBuiltin("string_width_ext", builtin_string_width_ext);
    registerBuiltin("string_height_ext", builtin_string_height_ext);

    // Color
    registerBuiltin("make_color_rgb", builtinMakeColor);
    registerBuiltin("make_colour_rgb", builtinMakeColour);
    registerBuiltin("make_color_hsv", builtinMakeColorHsv);
    registerBuiltin("make_colour_hsv", builtinMakeColourHsv);

    // Display
    registerBuiltin("display_get_width", builtin_display_get_width);
    registerBuiltin("display_get_height", builtin_display_get_height);

    // Collision
    registerBuiltin("place_meeting", builtinPlaceMeeting);
    registerBuiltin("collision_rectangle", builtinCollisionRectangle);
    registerBuiltin("collision_line", builtinCollisionLine);
    registerBuiltin("collision_point", builtinCollisionPoint);
    registerBuiltin("instance_position", builtinInstancePosition);
    registerBuiltin("place_free", builtinPlaceFree);

    // Tile layers
    registerBuiltin("tile_layer_hide", builtinTileLayerHide);
    registerBuiltin("tile_layer_show", builtinTileLayerShow);
    registerBuiltin("tile_layer_shift", builtinTileLayerShift);

    // Path
    registerBuiltin("path_start", builtinPathStart);
    registerBuiltin("path_end", builtinPathEnd);

    // Misc
    registerBuiltin("get_timer", builtin_get_timer);
    registerBuiltin("action_if_variable", builtinActionIfVariable);
    registerBuiltin("action_set_alarm", builtinActionSetAlarm);
    registerBuiltin("alarm_set", builtinAlarmSet);
    registerBuiltin("alarm_get", builtinAlarmGet);
    registerBuiltin("action_sound",builtin_action_sound);
    registerBuiltin("string_hash_to_newline", builtinStringHashToNewline);
    registerBuiltin("json_decode", builtinJsonDecode);
    registerBuiltin("font_add_sprite", builtinFontAddSprite);
    registerBuiltin("font_add_sprite_ext", builtinFontAddSpriteExt);
    registerBuiltin("object_get_sprite", builtinObjectGetSprite);
    registerBuiltin("asset_get_index", builtinAssetGetIndex);
}
