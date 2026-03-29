
#include "ps2/ps2_pad.h"

#include <libpad.h>

// Controller button to GML key mapping
typedef struct {
    uint16_t padButton;
    int32_t gmlKey;
} PadMapping;

// 256-byte aligned buffer for libpad
static char padBuf[256] __attribute__((aligned(64)));

static PadMapping* padMappings = nullptr;
static int padMappingCount = 0;
// Previous frame's button state for detecting press/release edges
static uint16_t prevButtons = 0xFFFF; // All buttons released (buttons are active-low)

void PS2Pad_Init() {
    padInit(0);
    padPortOpen(0, 0, padBuf);

    int padState;
    do {
        padState = padGetState(0, 0);
    } while (PAD_STATE_STABLE != padState && PAD_STATE_FINDCTP1 != padState);
}

void PS2Pad_ApplyMappings(JsonValue* mappings) {
    if (mappings == nullptr || !JsonReader_isObject(mappings)) {
        return;
    }

    if (padMappings) {
        free(padMappings);
        padMappings = nullptr;
    }

    padMappingCount = JsonReader_objectLength(mappings);
    padMappings = safeMalloc(sizeof(PadMapping) * padMappingCount);
    repeat(padMappingCount, i) {
        const char* padButtonStr = JsonReader_getObjectKey(mappings, i);
        JsonValue* gmlKeyVal = JsonReader_getObjectValue(mappings, i);
        padMappings[i].padButton = (uint16_t) atoi(padButtonStr);
        padMappings[i].gmlKey = (int32_t) JsonReader_getInt(gmlKeyVal);
        printf("controllerMapping pad=%d -> gmlKey=%d\n", padMappings[i].padButton, padMappings[i].gmlKey);
    }
}

uint16_t PS2Pad_GetPrevButtons() {
    return prevButtons;
}

uint8_t PS2Pad_Poll(RunnerKeyboardState* keyboard, uint16_t* dst_buttons) {
    struct padButtonStatus padStatus;

    unsigned char padResult = padRead(0, 0, &padStatus);
    uint16_t buttons = 0xFFFF; // all released by default

    if (padResult != 0) {
        buttons = padStatus.btns;

        repeat(padMappingCount, i) {
            uint16_t mask = padMappings[i].padButton;
            int32_t gmlKey = padMappings[i].gmlKey;

            // PS2 buttons are active-low: 0 = pressed, 1 = released
            bool wasPressed = (prevButtons & mask) == 0;
            bool isPressed = (buttons & mask) == 0;

            if (isPressed && !wasPressed) {
                RunnerKeyboard_onKeyDown(keyboard, gmlKey);
            } else if (!isPressed && wasPressed) {
                RunnerKeyboard_onKeyUp(keyboard, gmlKey);
            }
        }

        prevButtons = buttons;
        if (dst_buttons) {
            *dst_buttons = buttons;
        }
    }

    return padResult;
}
