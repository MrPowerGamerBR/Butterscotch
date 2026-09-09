#include "string_compat.h"
#include "stdio_compat.h"
#include <time.h>
#include "math_compat.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_SW_RENDERER
#include <glad/glad.h>
#endif
#include <GLFW/glfw3.h>

#include "common.h"
#include "input_recording.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

static GLFWwindow *window;
static Runner *g_runner;

// Butterscotch expects framebuffer pixels, but GLFW3 expects logical pixels.
// We round the logical size UP (ceil) so the resulting framebuffer is never SMALLER than requested.
static void framebufferToLogical(float xs, float ys, int fbW, int fbH, int* outW, int* outH) {
    *outW = (xs > 0.0f) ? (int) ceilf((float) fbW / xs) : fbW;
    *outH = (ys > 0.0f) ? (int) ceilf((float) fbH / ys) : fbH;
}

static GLFWwindow *tryOpenWindow(int reqW, int reqH, const char* title) {
    if (gfx == SOFTWARE || gfx == LEGACY_GL) {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, (gfx == SOFTWARE) ? 0 : 1);

#ifndef NDEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

        return glfwCreateWindow(reqW, reqH, title, NULL, NULL);
    }

    for (size_t i = 0; i < sizeof(GLCommon_versions)/sizeof(GLCommon_versions[0]); i++) {
        GLFWwindow *window;

        glfwDefaultWindowHints();

#ifndef NDEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GLCommon_versions[i].major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GLCommon_versions[i].minor);

        if (GLCommon_versions[i].gles) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
        } else {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
            if (GLCommon_versions[i].major >= 3) {
                if (GLCommon_versions[i].major == 3 && GLCommon_versions[i].minor == 2) {
                    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
                } else {
                    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_FALSE);
                }
            } else {
                glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
            }
        }

        window = glfwCreateWindow(reqW, reqH, title, NULL, NULL);
        if (window) {
            return window;
        }

    }

    return NULL;
}

void platformSetWindowTitle(const char* title) {
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    glfwSetWindowTitle(window, windowTitle);
}

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH || !window) return false;
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH || !window) return false;
    int w = 0;
    int h = 0;
    glfwGetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    if (!window) return;
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    int logicalW, logicalH;
    framebufferToLogical(xs, ys, width, height, &logicalW, &logicalH);
    glfwSetWindowSize(window, logicalW, logicalH);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    glfwGetCursorPos(window, xPos, yPos);
}

static bool platformGetWindowFocus(void) {
    return glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
}

static void glfwErrorCallback(int code, const char* description) {
    logWarn("GLFW error 0x%x: %s\n", code, description);
}

static int32_t glfwKeyToGml(int glfwKey) {
    // Letters and numbers are the same as GML
    if (glfwKey >= 'A' && glfwKey <= 'Z') return glfwKey;
    if (glfwKey >= '0' && glfwKey <= '9') return glfwKey;
    // Special keys need mapping
    switch (glfwKey) {
        case GLFW_KEY_ESCAPE:        return VK_ESCAPE;
        case GLFW_KEY_ENTER:         return VK_ENTER;
        case GLFW_KEY_TAB:           return VK_TAB;
        case GLFW_KEY_BACKSPACE:     return VK_BACKSPACE;
        case GLFW_KEY_SPACE:         return VK_SPACE;
        case GLFW_KEY_LEFT_SHIFT:
        case GLFW_KEY_RIGHT_SHIFT:   return VK_SHIFT;
        case GLFW_KEY_LEFT_CONTROL:
        case GLFW_KEY_RIGHT_CONTROL: return VK_CONTROL;
        case GLFW_KEY_LEFT_ALT:
        case GLFW_KEY_RIGHT_ALT:     return VK_ALT;
        case GLFW_KEY_UP:            return VK_UP;
        case GLFW_KEY_DOWN:          return VK_DOWN;
        case GLFW_KEY_LEFT:          return VK_LEFT;
        case GLFW_KEY_RIGHT:         return VK_RIGHT;
        case GLFW_KEY_F1:            return VK_F1;
        case GLFW_KEY_F2:            return VK_F2;
        case GLFW_KEY_F3:            return VK_F3;
        case GLFW_KEY_F4:            return VK_F4;
        case GLFW_KEY_F5:            return VK_F5;
        case GLFW_KEY_F6:            return VK_F6;
        case GLFW_KEY_F7:            return VK_F7;
        case GLFW_KEY_F8:            return VK_F8;
        case GLFW_KEY_F9:            return VK_F9;
        case GLFW_KEY_F10:           return VK_F10;
        case GLFW_KEY_F11:           return VK_F11;
        case GLFW_KEY_F12:           return VK_F12;
        case GLFW_KEY_INSERT:        return VK_INSERT;
        case GLFW_KEY_DELETE:        return VK_DELETE;
        case GLFW_KEY_HOME:          return VK_HOME;
        case GLFW_KEY_END:           return VK_END;
        case GLFW_KEY_PAGE_UP:       return VK_PAGEUP;
        case GLFW_KEY_PAGE_DOWN:     return VK_PAGEDOWN;
        default:                     return -1; // Unknown
    }
}

static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode; (void)mods; (void)window;
    // During playback, suppress real keyboard input (window events like close still work)
    if (InputRecording_isPlaybackActive(globalInputRecording)) return;
    int32_t gmlKey = glfwKeyToGml(key);
    if (action == GLFW_PRESS) RunnerKeyboard_onKeyDown(g_runner->keyboard, gmlKey);
    else if (action == GLFW_RELEASE) RunnerKeyboard_onKeyUp(g_runner->keyboard, gmlKey);
    // GLFW_REPEAT is ignored (GML doesn't use key repeat)
}

static void characterCallback(GLFWwindow* _window, unsigned int codepoint) {
    (void)_window;
    if (InputRecording_isPlaybackActive(globalInputRecording)) return;
    RunnerKeyboard_onCharacter(g_runner->keyboard, codepoint);
}

#ifdef ENABLE_SW_RENDERER

static void resizeCallback(GLFWwindow* window, int width, int height) {
    (void)window;
    glViewport(0, 0, width, height);
}

#endif

static int32_t glfwMouseButtonToGml(int glfwButton) {
    switch (glfwButton) {
        case GLFW_MOUSE_BUTTON_LEFT: return GML_MB_LEFT;
        case GLFW_MOUSE_BUTTON_RIGHT: return GML_MB_RIGHT;
        case GLFW_MOUSE_BUTTON_MIDDLE: return GML_MB_MIDDLE;
        default: return INT32_MIN; // Unknown
    }
}

static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods; (void)window;
    int32_t gmlButton = glfwMouseButtonToGml(button);
    if (0 > gmlButton) return;
    if (action == GLFW_PRESS) RunnerMouse_onButtonDown(g_runner->mouse, gmlButton);
    else if (action == GLFW_RELEASE) RunnerMouse_onButtonUp(g_runner->mouse, gmlButton);
}

static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset; (void)window;
    RunnerMouse_onWheel(g_runner->mouse, yoffset);
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    // Init GLFW
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        logError("Failed to initialize GLFW\n");
        return false;
    }

    // init gamepad mappings
    const char* dbPath = "gamecontrollerdb.txt";
    FILE* f = fopen(dbPath, "rb");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buffer = (char*) malloc(len + 1);
        if (buffer != NULL) {
            safeFread(buffer, len, f, dbPath);
            buffer[len] = '\0';
            if (buffer[0] != '\0') {
                if (glfwUpdateGamepadMappings(buffer)) {
                    logInfo("Gamepad: Loaded SDL gamecontroller mappings successfully\n");
                } else {
                    logWarn("Gamepad: Failed to load SDL gamecontroller mappings\n");
                }
            }
            free(buffer);
        }
        fclose(f);
    } else
        logWarn("Gamepad: SDL gamecontrollerdb.txt not found at %s, using defaults\n", dbPath);

    if (headless)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    window = tryOpenWindow(reqW, reqH, title);
    if (!window) {
        logError("Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // Disable v-sync, we control timing ourselves

    // If we don't do this, the window will be larger than it should be if you are using Wayland fractional scaling
    // We set the window size AFTER the window creation so we can use glfwGetWindowContentScale
    platformSetWindowSize(reqW, reqH);

    // Set up keyboard input
    glfwSetKeyCallback(window, keyCallback);
    glfwSetCharCallback(window, characterCallback);
    // Set up mouse input
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);

    return true;
}

void platformExit(void) {
    glfwDestroyWindow(window);
    glfwTerminate();
}

static void platformSetCursor(int32_t cursorType) {
    if (cursorType == GML_CR_NONE) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        return;
    }
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    int glfwShape;
    switch (cursorType) {
        case GML_CR_CROSS:  glfwShape = GLFW_CROSSHAIR_CURSOR; break;
        case GML_CR_BEAM:  glfwShape = GLFW_IBEAM_CURSOR; break;
        case GML_CR_SIZE_NS:  glfwShape = GLFW_VRESIZE_CURSOR; break;
        case GML_CR_SIZE_WE:  glfwShape = GLFW_HRESIZE_CURSOR; break;
        case GML_CR_DRAG: glfwShape = GLFW_HAND_CURSOR; break;
        case GML_CR_HANDPOINT: glfwShape = GLFW_HAND_CURSOR; break;
        #if (GLFW_VERSION_MINOR >= 4)
        case GML_CR_SIZE_ALL: glfwShape = GLFW_RESIZE_ALL_CURSOR; break;
        case GML_CR_SIZE_NWSE:  glfwShape = GLFW_RESIZE_NWSE_CURSOR; break;
        case GML_CR_SIZE_NESW:  glfwShape = GLFW_RESIZE_NESW_CURSOR; break;
        #endif
        default:  glfwShape = GLFW_ARROW_CURSOR; break;
    }

    GLFWcursor* cursor = glfwCreateStandardCursor(glfwShape);
    if (cursor) {
        glfwSetCursor(window, cursor);
    }
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = platformGetWindowFocus;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_DEFAULT;
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE)
        glfwSetWindowSizeCallback(window, resizeCallback);
#endif
}

#ifdef ENABLE_SW_RENDERER

static uint32_t* nextFb = NULL;
static int fbWidth = 0, fbHeight = 0;

void platformSetNextFramebuffer(uint32_t* framebuffer, int width, int height) {
    nextFb = framebuffer;
    fbWidth = width;
    fbHeight = height;
}

#endif

void platformSwapBuffers(void) {
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE && nextFb) {
        glRasterPos2f(-1, 1);
        glPixelZoom(1, -1);
        glDrawPixels(fbWidth, fbHeight, GL_BGRA, GL_UNSIGNED_BYTE, nextFb);
        nextFb = NULL;
    }
#endif
    glfwSwapBuffers(window);
}

void *platformGetProcAddress(const char *name) {
    return (void *)glfwGetProcAddress(name);
}

enum {
    IDX_LT = 6,
    IDX_RT = 7,
};

static void mapGlfwToGml(const GLFWgamepadstate* glfwState, GamepadSlot* slot) {
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));

    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_A]) slot->buttonDown[0] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_B]) slot->buttonDown[1] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_X]) slot->buttonDown[2] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_Y]) slot->buttonDown[3] = true;

    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER]) slot->buttonDown[4] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER]) slot->buttonDown[5] = true;

    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_BACK]) slot->buttonDown[8] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_START]) slot->buttonDown[9] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_GUIDE]) slot->buttonDown[16] = true;

    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB]) slot->buttonDown[10] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB]) slot->buttonDown[11] = true;

    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]) slot->buttonDown[12] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]) slot->buttonDown[13] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT]) slot->buttonDown[14] = true;
    if (glfwState->buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]) slot->buttonDown[15] = true;

    float lt = glfwState->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER];
    float rt = glfwState->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER];
    if (lt < 0.0f) lt = 0.0f;
    if (rt < 0.0f) rt = 0.0f;
    slot->buttonValue[IDX_LT] = lt;
    slot->buttonValue[IDX_RT] = rt;
    if (lt >= slot->triggerThreshold) slot->buttonDown[IDX_LT] = true;
    if (rt >= slot->triggerThreshold) slot->buttonDown[IDX_RT] = true;

    float lh = glfwState->axes[GLFW_GAMEPAD_AXIS_LEFT_X];
    float lv = glfwState->axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
    float rh = glfwState->axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
    float rv = glfwState->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];

    slot->axisValue[0] = lh;
    slot->axisValue[1] = lv;
    slot->axisValue[2] = rh;
    slot->axisValue[3] = rv;

    for (int i = 0; GP_BUTTON_COUNT > i; i++) {
        if (i == IDX_LT || i == IDX_RT) continue;
        slot->buttonValue[i] = slot->buttonDown[i] ? 1.0f : 0.0f;
    }
}

bool platformHandleEvents(void) {
    if (glfwWindowShouldClose(window))
        return true;

    glfwPollEvents();

    for (int slotIdx = 0; slotIdx < 1 && slotIdx < MAX_GAMEPADS; slotIdx++) {
        GamepadSlot* slot = g_runner->gamepads->slots + slotIdx;

        bool currentlyConnected = false;
        int  foundJid = -1;

        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_16; jid++) {
            if (glfwJoystickPresent(jid) && glfwJoystickIsGamepad(jid)) {
                foundJid = jid;
                currentlyConnected = true;
                break;
            }
        }

        if (currentlyConnected) {
            GLFWgamepadstate state;
            if (glfwGetGamepadState(foundJid, &state)) {
                mapGlfwToGml(&state, slot);
                slot->jid = foundJid;
                slot->connected = true;

                const char* name = glfwGetJoystickName(foundJid);
                if (name != NULL) {
                    strncpy(slot->description, name, sizeof(slot->description) - 1);
                    slot->description[sizeof(slot->description) - 1] = '\0';
                }

                const char* guid = glfwGetJoystickGUID(foundJid);
                if (guid != NULL) {
                    strncpy(slot->guid, guid, sizeof(slot->guid) - 1);
                    slot->guid[sizeof(slot->guid) - 1] = '\0';
                } else {
                    slot->guid[0] = '\0';
                }
            } else {
                slot->connected = false;
                slot->guid[0] = '\0';
            }
        } else {
            slot->connected = false;
            slot->guid[0] = '\0';
        }

        if (slot->connected) {
            for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
                bool wasDown = slot->buttonDownPrev[btn];
                if (slot->buttonDown[btn] && !wasDown) slot->buttonPressed[btn] = true;
                if (!slot->buttonDown[btn] && wasDown) slot->buttonReleased[btn] = true;
            }
            g_runner->gamepads->connectedCount++;
        }
    }

    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
#ifdef _WIN32
        Sleep(remaining / 1000000);
#else
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
#endif
    }
    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}
