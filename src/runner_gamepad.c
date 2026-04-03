#include "runner_gamepad.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

//DELTARUNE HACK
int RawToGPDelta(int32_t gmlButton) {
    switch (gmlButton) {
        case 0: return GP_PADU;
        case 1: return GP_PADD;
        case 2: return GP_PADL;
        case 3: return GP_PADR;
        case 13: return GP_FACE1;
        case 12: return GP_FACE2;
        case 15: return GP_FACE4;
        case 14: return GP_FACE3;
        default: return gmlButton;
    }
}

//UNDERTALE HACK
int RawToGPUndertale(int32_t gmlButton) {
    switch (gmlButton) {
        case 1: return GP_FACE2;
        case 2: return GP_FACE1;
        case 3: return GP_FACE3;
        case 4: return GP_FACE4;
        default: return gmlButton;
    }
}

static int gmlButtonToIndex(int32_t gmlButton) {
    gmlButton = RawToGPDelta(gmlButton);
    switch (gmlButton) {
        case GP_FACE1: return 0;
        case GP_FACE2: return 1;
        case GP_FACE3: return 2;
        case GP_FACE4: return 3;
        case GP_SHOULDERL: return 4;
        case GP_SHOULDERR: return 5;
        case GP_SHOULDERLB: return 6;
        case GP_SHOULDERRB: return 7;
        case GP_SELECT: return 8;
        case GP_START: return 9;
        case GP_STICKL: return 10;
        case GP_STICKR: return 11;
        case GP_PADU: return 12;
        case GP_PADD: return 13;
        case GP_PADL: return 14;
        case GP_PADR: return 15;
        case GP_HOME: return 16;
        default: return gmlButton;
    }
}

static int gmlAxisToIndex(int32_t gmlAxis) {
    switch (gmlAxis) {
        case GP_AXIS_LH: return 0;
        case GP_AXIS_LV: return 1;
        case GP_AXIS_RH: return 2;
        case GP_AXIS_RV: return 3;
        default: return gmlAxis;
    }
}

RunnerGamepadState* RunnerGamepad_create(void) {
    RunnerGamepadState* gp = safeCalloc(1, sizeof(RunnerGamepadState));
    for (int i = 0; MAX_GAMEPADS > i; i++) {
        gp->slots[i].deadzone = 0.15f;
        gp->slots[i].triggerThreshold = 0.5f;
    }
    return gp;
}

void RunnerGamepad_free(RunnerGamepadState* gp) {
    free(gp);
}

void RunnerGamepad_beginFrame(RunnerGamepadState* gp) {
    for (int i = 0; MAX_GAMEPADS > i; i++) {
        gp->slots[i].connectedPrev = gp->slots[i].connected;
        memset(gp->slots[i].buttonPressed,  0, sizeof(gp->slots[i].buttonPressed));
        memset(gp->slots[i].buttonReleased, 0, sizeof(gp->slots[i].buttonReleased));
    }
    gp->connectedCount = 0;
}

int RunnerGamepad_getDeviceCount(RunnerGamepadState* gp) {
    return gp->connectedCount;
}

bool RunnerGamepad_isConnected(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return false;
    return gp->slots[device].connected;
}

bool RunnerGamepad_buttonCheck(RunnerGamepadState* gp, int device, int button) {
    if (device < 0 || device >= MAX_GAMEPADS) return false;
    if (!gp->slots[device].connected) return false;
    int idx = gmlButtonToIndex(button);
    if (idx < 0 || idx >= GP_BUTTON_COUNT) return false;
    return gp->slots[device].buttonDown[idx];
}

bool RunnerGamepad_buttonCheckPressed(RunnerGamepadState* gp, int device, int button) {
    if (device < 0 || device >= MAX_GAMEPADS) return false;
    if (!gp->slots[device].connected) return false;
    int idx = gmlButtonToIndex(button);
    if (idx < 0 || idx >= GP_BUTTON_COUNT) return false;
    return gp->slots[device].buttonPressed[idx];
}

bool RunnerGamepad_buttonCheckReleased(RunnerGamepadState* gp, int device, int button) {
    if (device < 0 || device >= MAX_GAMEPADS) return false;
    if (!gp->slots[device].connected) return false;
    int idx = gmlButtonToIndex(button);
    if (idx < 0 || idx >= GP_BUTTON_COUNT) return false;
    return gp->slots[device].buttonReleased[idx];
}

float RunnerGamepad_buttonValue(RunnerGamepadState* gp, int device, int button) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0.0f;
    if (!gp->slots[device].connected) return 0.0f;
    int idx = gmlButtonToIndex(button);
    if (idx < 0 || idx >= GP_BUTTON_COUNT) return 0.0f;
    return gp->slots[device].buttonValue[idx];
}

float RunnerGamepad_axisValue(RunnerGamepadState* gp, int device, int axis) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0.0f;
    if (!gp->slots[device].connected) return 0.0f;
    int idx = gmlAxisToIndex(axis);
    if (idx < 0 || idx >= GP_AXIS_COUNT) return 0.0f;
    return gp->slots[device].axisValue[idx];
}

const char* RunnerGamepad_getDescription(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return "";
    if (!gp->slots[device].connected) return "";
    return gp->slots[device].description;
}

const char* RunnerGamepad_getGuid(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return "device index out of range";
    if (!gp->slots[device].connected) return "none";
    const char* g = gp->slots[device].guid;
    return (g[0] != '\0') ? g : "none";
}

float RunnerGamepad_getButtonThreshold(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0.0f;
    return gp->slots[device].triggerThreshold;
}

void RunnerGamepad_setButtonThreshold(RunnerGamepadState* gp, int device, float threshold) {
    if (device < 0 || device >= MAX_GAMEPADS) return;
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 1.0f) threshold = 1.0f;
    gp->slots[device].triggerThreshold = threshold;
}

float RunnerGamepad_getAxisDeadzone(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0.0f;
    return gp->slots[device].deadzone;
}

void RunnerGamepad_setAxisDeadzone(RunnerGamepadState* gp, int device, float deadzone) {
    if (device < 0 || device >= MAX_GAMEPADS) return;
    if (deadzone < 0.0f) deadzone = 0.0f;
    if (deadzone > 1.0f) deadzone = 1.0f;
    gp->slots[device].deadzone = deadzone;
}

int RunnerGamepad_getAxisCount(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0;
    if (!gp->slots[device].connected) return 0;
    return GP_AXIS_COUNT;
}

int RunnerGamepad_getButtonCount(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0;
    if (!gp->slots[device].connected) return 0;
    return GP_BUTTON_COUNT;
}

int RunnerGamepad_getHatCount(RunnerGamepadState* gp, int device) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0;
    if (!gp->slots[device].connected) return 0;
    return 1;
}

int RunnerGamepad_getHatValue(RunnerGamepadState* gp, int device, int hat) {
    if (device < 0 || device >= MAX_GAMEPADS) return 0;
    if (!gp->slots[device].connected) return 0;
    if (hat != 0) return 0;
    const GamepadSlot* slot = &gp->slots[device];
    int mask = 0;
    if (slot->buttonDown[12]) mask |= 1;
    if (slot->buttonDown[15]) mask |= 2;
    if (slot->buttonDown[13]) mask |= 4;
    if (slot->buttonDown[14]) mask |= 8;
    return mask;
}