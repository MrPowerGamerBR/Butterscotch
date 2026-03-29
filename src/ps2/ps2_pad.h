#pragma once

#include <stdint.h>

#include "json_reader.h"
#include "runner_keyboard.h"
#include "utils.h"


// Initialize the PS2 gamepad.
void PS2Pad_Init();

// Parse the controller mappings object from `config.jsn`.
// Upon calling `PS2Pad_Poll()`, the buttons will automatically
// be propagated to their mapped GML keyboard inputs.
void PS2Pad_ApplyMappings(JsonValue* mappings);

// Get a bitmap of the PS2 buttons pressed on the previous frame.
uint16_t PS2Pad_GetPrevButtons();

// Read the PS2 pad's inputs and apply them to the keyboard.
// Nonzero return indicates the gamepad could not be read for some reason.
// On success, the `keyboard`'s inputs will be triggered according to the
// polled buttons. Additionally, if a pointer for `dst_buttons` is provided,
// a bitmap consisting of the PS2 buttons pressed on this frame will be written to it.
uint8_t PS2Pad_Poll(RunnerKeyboardState* keyboard, uint16_t* dst_buttons);
