#pragma once

#include "../runner_gamepad.h"

// Loads SDL gamecontroller mappings into SDL2 (call after glfwInit).
void Sdl2Gamepad_loadMappings(const char* mappings);
// Reads the physical joystick state from SDL2 and updates RunnerGamepadState.
void Sdl2Gamepad_poll(RunnerGamepadState* gp);
