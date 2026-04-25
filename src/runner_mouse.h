#pragma once

#include "common.h"
#include <stdint.h>

// Mouse buttons
// TODO verify side buttons are correct
#define MB_ANY -1
#define MB_NONE 0
#define MB_LEFT 1
#define MB_RIGHT 2
#define MB_MIDDLE 3
#define MB_SIDE1 4
#define MB_SIDE2 5
#define GML_MOUSE_BUTTONS 6

#define GML_MOUSE_BUTTON_COUNT 5

typedef struct RunnerMouseState {
    double mouseX, mouseY;
    bool buttonDown[GML_MOUSE_BUTTON_COUNT];
    bool buttonPressed[GML_MOUSE_BUTTON_COUNT];
} RunnerMouseState;

RunnerMouseState* RunnerMouse_create(void);
void RunnerMouse_free(RunnerMouseState* m);
void RunnerMouse_beginFrame(RunnerMouseState* m);
void RunnerMouse_onButtonDown(RunnerMouseState* m, int32_t button);
void RunnerMouse_onButtonUp(RunnerMouseState* m, int32_t button);
bool RunnerMouse_checkButton(RunnerMouseState* m, int32_t button);
bool RunnerMouse_checkButtonPressed(RunnerMouseState* m, int32_t button);