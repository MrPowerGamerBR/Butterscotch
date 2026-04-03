#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_GAMEPADS 16

#define GP_BUTTON_COUNT 17
#define GP_AXIS_COUNT 4

#define GP_FACE1      32769
#define GP_FACE2      32770
#define GP_FACE3      32771
#define GP_FACE4      32772
#define GP_SHOULDERL  32773
#define GP_SHOULDERR  32774
#define GP_SHOULDERLB 32775
#define GP_SHOULDERRB 32776
#define GP_SELECT     32777
#define GP_START      32778
#define GP_STICKL     32779
#define GP_STICKR     32780
#define GP_PADU       32781
#define GP_PADD       32782
#define GP_PADL       32783
#define GP_PADR       32784
#define GP_HOME       32799

#define GP_AXIS_LH    32785
#define GP_AXIS_LV    32786
#define GP_AXIS_RH    32787
#define GP_AXIS_RV    32788

typedef struct {
    bool connectedPrev;
    bool connected;
    int jid;
    char description[256];
    char guid[64];
    bool buttonDownPrev[GP_BUTTON_COUNT];
    bool buttonDown[GP_BUTTON_COUNT];
    bool buttonPressed[GP_BUTTON_COUNT];
    bool buttonReleased[GP_BUTTON_COUNT];
    float buttonValue[GP_BUTTON_COUNT];
    float axisValue[GP_AXIS_COUNT];
    float deadzone;
    float triggerThreshold;
} GamepadSlot;

typedef struct {
    GamepadSlot slots[MAX_GAMEPADS];
    int connectedCount;
} RunnerGamepadState;

RunnerGamepadState* RunnerGamepad_create(void);
void RunnerGamepad_free(RunnerGamepadState* gp);

void RunnerGamepad_beginFrame(RunnerGamepadState* gp);

int RawToGPUndertale(int32_t gmlButton);
int RunnerGamepad_getDeviceCount(RunnerGamepadState* gp);
bool RunnerGamepad_isConnected(RunnerGamepadState* gp, int device);
bool RunnerGamepad_buttonCheck(RunnerGamepadState* gp, int device, int button);
bool RunnerGamepad_buttonCheckPressed(RunnerGamepadState* gp, int device, int button);
bool RunnerGamepad_buttonCheckReleased(RunnerGamepadState* gp, int device, int button);
float RunnerGamepad_buttonValue(RunnerGamepadState* gp, int device, int button);
float RunnerGamepad_axisValue(RunnerGamepadState* gp, int device, int axis);
const char* RunnerGamepad_getDescription(RunnerGamepadState* gp, int device);
const char* RunnerGamepad_getGuid(RunnerGamepadState* gp, int device);
float RunnerGamepad_getButtonThreshold(RunnerGamepadState* gp, int device);
void RunnerGamepad_setButtonThreshold(RunnerGamepadState* gp, int device, float threshold);
float RunnerGamepad_getAxisDeadzone(RunnerGamepadState* gp, int device);
void RunnerGamepad_setAxisDeadzone(RunnerGamepadState* gp, int device, float deadzone);
int RunnerGamepad_getAxisCount(RunnerGamepadState* gp, int device);
int RunnerGamepad_getButtonCount(RunnerGamepadState* gp, int device);
int RunnerGamepad_getHatCount(RunnerGamepadState* gp, int device);
int RunnerGamepad_getHatValue(RunnerGamepadState* gp, int device, int hat);