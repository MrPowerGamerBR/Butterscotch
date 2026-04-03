#pragma once

#include "../runner_gamepad.h"

// Loads SDL gamecontroller mappings into GLFW (call after glfwInit).
void GlfwGamepad_loadMappings(const char* mappings);
// Reads the physical joystick state from GLFW and updates RunnerGamepadState.
void GlfwGamepad_poll(RunnerGamepadState* gp);
