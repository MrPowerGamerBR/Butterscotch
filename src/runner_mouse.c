#include "runner_mouse.h"
#include "utils.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool isValidButtonVirtual(int_fast8_t button) {
    return button >= -1 && GML_MOUSE_BUTTONS > button;
}

static bool isValidButton(int_fast8_t button) {
    return button >= 1 && GML_MOUSE_BUTTONS > button;
}

RunnerMouseState* RunnerMouse_create(void) {
    RunnerMouseState* m = safeCalloc(1, sizeof(RunnerMouseState));
    return m;
}

void RunnerMouse_free(RunnerMouseState* m) {
    free(m);
}

void RunnerMouse_beginFrame(RunnerMouseState* m) {
    memset(m->buttonPressed, 0, sizeof(m->buttonPressed));
}

void RunnerMouse_onButtonDown(RunnerMouseState* m, int32_t button) {
    if (!isValidButton(button)) return;
    m->buttonDown[button-1] = true;
    m->buttonPressed[button-1] = true;
}

void RunnerMouse_onButtonUp(RunnerMouseState* m, int32_t button) {
    if (!isValidButton(button)) return;
    m->buttonDown[button-1] = false;
}

bool RunnerMouse_checkButton(RunnerMouseState* m, int32_t button) {
    if (!isValidButtonVirtual(button)) return false;

    if (isValidButton(button)) {
        return m->buttonDown[button-1];
    }

    bool any = false;
    repeat(GML_MOUSE_BUTTON_COUNT, i) {
        any &= m->buttonDown[i];
    }

    if (button == MB_ANY) return any;
    else if (button == MB_NONE) return !any;
    return false;
}

bool RunnerMouse_checkButtonPressed(RunnerMouseState* m, int32_t button) {
    if (!isValidButtonVirtual(button)) return false;

    if (isValidButton(button)) {
        return m->buttonPressed[button-1];
    }

    bool any = false;
    repeat(GML_MOUSE_BUTTON_COUNT, i) {
        any &= m->buttonPressed[i];
    }

    if (button == MB_ANY) return any;
    else if (button == MB_NONE) return !any;
    return false;
}