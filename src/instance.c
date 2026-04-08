#include "instance.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"
#include "utils.h"

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) {
    Instance* inst = safeCalloc(1, sizeof(Instance));
    inst->instanceId = instanceId;
    inst->objectIndex = objectIndex;
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
    inst->spriteIndex = -1;
    inst->imageSpeed = 1.0f;
    inst->imageIndex = 0.0f;
    inst->imageXscale = 1.0f;
    inst->imageYscale = 1.0f;
    inst->imageAngle = 0.0f;
    inst->imageAlpha = 1.0f;
    inst->imageBlend = 0xFFFFFF;
    inst->depth = 0;
    inst->speed = 0.0f;
    inst->direction = 0.0f;
    inst->hspeed = 0.0f;
    inst->vspeed = 0.0f;
    inst->friction = 0.0f;
    inst->gravity = 0.0f;
    inst->gravityDirection = 270.0f;
    inst->pathIndex = -1;
    inst->pathScale = 1.0f;
    inst->selfVars = nullptr;
    inst->selfArrayMap = nullptr;
    inst->selfArrayVarTracker = nullptr;

    // Initialize alarms to -1 (inactive)
    repeat(GML_ALARM_COUNT, i) {
        inst->alarm[i] = -1;
    }

    return inst;
}

void Instance_free(Instance* instance) {
    if (instance == nullptr) return;

    // Free owned strings in selfVars hashmap
    repeat(hmlen(instance->selfVars), i) {
        RValue_free(&instance->selfVars[i].value);
    }
    hmfree(instance->selfVars);

    // Free selfArrayMap
    repeat(hmlen(instance->selfArrayMap), i) {
        RValue_free(&instance->selfArrayMap[i].value);
    }
    hmfree(instance->selfArrayMap);

    // Free selfArrayVarTracker
    hmfree(instance->selfArrayVarTracker);

    free(instance);
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
    inst->direction = (float) GMLReal_fmod(inst->direction, 360.0);

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
