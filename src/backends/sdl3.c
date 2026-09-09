#include <ctype.h>
#include "stdio_compat.h"

#include <SDL3/SDL.h>

#include "common.h"
#include "input_recording.h"
#include "platformdefs.h"
#include "gettime.h"
#include <ctype.h>
#include "runner_mouse.h"

static Runner *g_runner;
static int32_t fbWidth, fbHeight;
static SDL_Surface* scr;
static SDL_Window *window;
static SDL_Gamepad* openControllers[MAX_GAMEPADS];

static const int SDL_TO_GML_BUTTON[SDL_GAMEPAD_BUTTON_COUNT] = {
    [SDL_GAMEPAD_BUTTON_SOUTH]          = 0,
    [SDL_GAMEPAD_BUTTON_EAST]           = 1,
    [SDL_GAMEPAD_BUTTON_WEST]           = 2,
    [SDL_GAMEPAD_BUTTON_NORTH]          = 3,
    [SDL_GAMEPAD_BUTTON_BACK]           = 8,
    [SDL_GAMEPAD_BUTTON_GUIDE]          = 16,
    [SDL_GAMEPAD_BUTTON_START]          = 9,
    [SDL_GAMEPAD_BUTTON_LEFT_STICK]     = 10,
    [SDL_GAMEPAD_BUTTON_RIGHT_STICK]    = 11,
    [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]  = 4,
    [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = 5,
    [SDL_GAMEPAD_BUTTON_DPAD_UP]        = 12,
    [SDL_GAMEPAD_BUTTON_DPAD_DOWN]      = 13,
    [SDL_GAMEPAD_BUTTON_DPAD_LEFT]      = 14,
    [SDL_GAMEPAD_BUTTON_DPAD_RIGHT]     = 15,
};

static SDL_Window *tryOpenWindow(int reqW, int reqH, const char* title, Uint32 flags) {
    if (gfx == SOFTWARE) {
        return SDL_CreateWindow(
            title,
            reqW,
            reqH,
            flags
        );
    }
    if (gfx == LEGACY_GL) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);

        SDL_Window *newWindow = SDL_CreateWindow(
            title,
            reqW,
            reqH,
            flags
        );

        if (newWindow) {
            if (SDL_GL_CreateContext(newWindow)) {
                return newWindow;
            }
            SDL_DestroyWindow(newWindow);
        }
        return NULL;
    }
    for (size_t i = 0; i < sizeof(GLCommon_versions)/sizeof(GLCommon_versions[0]); i++) {
        SDL_Window *newWindow;
        int contextFlags = 0;

        SDL_GL_ResetAttributes();
#ifndef NDEBUG
        contextFlags |= SDL_GL_CONTEXT_DEBUG_FLAG;
#endif

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GLCommon_versions[i].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GLCommon_versions[i].minor);

        if (GLCommon_versions[i].gles) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        } else {
            if (GLCommon_versions[i].major >= 3) {
                if (GLCommon_versions[i].major == 3 && GLCommon_versions[i].minor == 2) {
                    contextFlags |= SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
                }
            } else {
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
            }
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, contextFlags);

        newWindow = SDL_CreateWindow(
            title,
            reqW,
            reqH,
            flags
        );

        if (newWindow) {
            if (SDL_GL_CreateContext(newWindow)) {
                return newWindow;
            }
            SDL_DestroyWindow(newWindow);
        }

    }
    return NULL;
}

void platformSetWindowTitle(const char* title) {
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    SDL_SetWindowTitle(window, windowTitle);
}

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    *outW = fbWidth;
    *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    fbWidth = width;
    fbHeight = height;
    SDL_SetWindowSize(window, width, height);
    if (gfx == SOFTWARE)
        scr = SDL_GetWindowSurface(window);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    *xPos = (double)mx;
    *yPos = (double)my;
}

static bool platformGetWindowFocus(void) {
    return SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
}

bool platformInit(int reqW, int reqH, const char *title, bool headless) {
    // Init SDL
    if (!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)) {
        logError("Failed to initialize SDL\n");
        return false;
    }

    for (int i = 0; i < MAX_GAMEPADS; i++) {
        openControllers[i] = NULL;
    }

    Uint32 flags = (gfx == SOFTWARE ? 0 : SDL_WINDOW_OPENGL) | (headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
    fbWidth = reqW;
    fbHeight = reqH;

    window = tryOpenWindow(fbWidth, fbHeight, title, flags);

    if (!window && gfx != SOFTWARE) {
        logError("Fatal: Could not open window: %s\n", SDL_GetError());
        return false;
    }

    if (!window && gfx == SOFTWARE) {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (mode != NULL) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%dx%d unavailable, falling back to %dx%d: %s",
                        fbWidth, fbHeight, mode->w, mode->h, SDL_GetError());

            fbWidth = mode->w;
            fbHeight = mode->h;

            window = SDL_CreateWindow(title, fbWidth, fbHeight, flags);
        }
    }
    if (!window) {
        logError("Fatal: Could not set any video mode: %s\n", SDL_GetError());
        return false;
    }
    if (gfx != SOFTWARE) {
        SDL_GL_SetSwapInterval(0); // disable vsync
    } else
        scr = SDL_GetWindowSurface(window);

    return true;
}

void platformExit(void) {
    SDL_Quit();
}

static void platformSetCursor(int32_t cursorType) {
    if (cursorType == GML_CR_NONE) {
        SDL_HideCursor();
        return;
    }
    SDL_ShowCursor();

    SDL_SystemCursor sdlCursor;
    switch (cursorType) {
        case GML_CR_CROSS: sdlCursor = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
        case GML_CR_BEAM: sdlCursor = SDL_SYSTEM_CURSOR_TEXT; break;
        case GML_CR_SIZE_NESW: sdlCursor = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
        case GML_CR_SIZE_NS: sdlCursor = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
        case GML_CR_SIZE_NWSE: sdlCursor = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
        case GML_CR_SIZE_WE: sdlCursor = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
        case GML_CR_HOURGLASS: sdlCursor = SDL_SYSTEM_CURSOR_WAIT; break;
        case GML_CR_DRAG: sdlCursor = SDL_SYSTEM_CURSOR_POINTER; break;
        case GML_CR_APPSTART: sdlCursor = SDL_SYSTEM_CURSOR_PROGRESS; break;
        case GML_CR_HANDPOINT: sdlCursor = SDL_SYSTEM_CURSOR_POINTER; break;
        case GML_CR_SIZE_ALL: sdlCursor = SDL_SYSTEM_CURSOR_MOVE; break;
        default: sdlCursor = SDL_SYSTEM_CURSOR_DEFAULT; break;
    }

    SDL_Cursor* cursor = SDL_CreateSystemCursor(sdlCursor);
    if (cursor) {
        SDL_SetCursor(cursor);
    }
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = platformGetWindowFocus;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_DEFAULT;
}

#ifdef ENABLE_SW_RENDERER

static SDL_Surface* nextFb = NULL;

void platformSetNextFramebuffer(uint32_t* framebuffer, int width, int height) {
    if (nextFb) {
        SDL_DestroySurface(nextFb);
        nextFb = NULL;
    }

    nextFb = SDL_CreateSurfaceFrom(
        width,
        height,
        SDL_PIXELFORMAT_XRGB8888,
        framebuffer,
        width * 4
    );
}

#endif

void platformSwapBuffers(void) {
#ifdef ENABLE_SW_RENDERER
    if(gfx == SOFTWARE) {
        SDL_BlitSurface(nextFb, NULL, scr, NULL);
        SDL_UpdateWindowSurface(window);
    }
#endif
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
    if (gfx == LEGACY_GL || gfx == MODERN_GL)
        SDL_GL_SwapWindow(window);
#endif
}

#if defined(ENABLE_MODERN_GL) || defined(ENABLE_LEGACY_GL)

void *platformGetProcAddress(const char *name) {
    return SDL_GL_GetProcAddress(name);
}

#endif

static int32_t SDLKeyToGml(int sdlkey) {
    // Letters and numbers are the same as GML
    if (sdlkey >= 'a' && sdlkey <= 'z') return toupper(sdlkey);
    if (sdlkey >= '0' && sdlkey <= '9') return sdlkey;
    // Special keys need mapping
    switch (sdlkey) {
        case SDLK_ESCAPE:    return VK_ESCAPE;
        case SDLK_RETURN:    return VK_ENTER;
        case SDLK_TAB:       return VK_TAB;
        case SDLK_BACKSPACE: return VK_BACKSPACE;
        case SDLK_SPACE:     return VK_SPACE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:    return VK_SHIFT;
        case SDLK_LCTRL:
        case SDLK_RCTRL:     return VK_CONTROL;
        case SDLK_LALT:
        case SDLK_RALT:      return VK_ALT;
        case SDLK_UP:        return VK_UP;
        case SDLK_DOWN:      return VK_DOWN;
        case SDLK_LEFT:      return VK_LEFT;
        case SDLK_RIGHT:     return VK_RIGHT;
        case SDLK_F1:        return VK_F1;
        case SDLK_F2:        return VK_F2;
        case SDLK_F3:        return VK_F3;
        case SDLK_F4:        return VK_F4;
        case SDLK_F5:        return VK_F5;
        case SDLK_F6:        return VK_F6;
        case SDLK_F7:        return VK_F7;
        case SDLK_F8:        return VK_F8;
        case SDLK_F9:        return VK_F9;
        case SDLK_F10:       return VK_F10;
        case SDLK_F11:       return VK_F11;
        case SDLK_F12:       return VK_F12;
        case SDLK_INSERT:    return VK_INSERT;
        case SDLK_DELETE:    return VK_DELETE;
        case SDLK_HOME:      return VK_HOME;
        case SDLK_END:       return VK_END;
        case SDLK_PAGEUP:    return VK_PAGEUP;
        case SDLK_PAGEDOWN:  return VK_PAGEDOWN;
        default:             return -1; // Unknown
    }
}

static uint32_t utf8_to_codepoint(const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] < 0x80)
        return p[0];

    if ((p[0] & 0xE0) == 0xC0)
        return ((p[0] & 0x1F) << 6) |
               (p[1] & 0x3F);

    if ((p[0] & 0xF0) == 0xE0)
        return ((p[0] & 0x0F) << 12) |
               ((p[1] & 0x3F) << 6) |
               (p[2] & 0x3F);

    if ((p[0] & 0xF8) == 0xF0)
        return ((p[0] & 0x07) << 18) |
               ((p[1] & 0x3F) << 12) |
               ((p[2] & 0x3F) << 6) |
               (p[3] & 0x3F);

    return 0xFFFD; // replacement character
}

static int32_t SDLMouseButtonToGml(int sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_LEFT: return GML_MB_LEFT;
        case SDL_BUTTON_RIGHT: return GML_MB_RIGHT;
        case SDL_BUTTON_MIDDLE: return GML_MB_MIDDLE;
        default: return -1;
    }
}

enum {
    IDX_LT = 6,
    IDX_RT = 7,
};

static void mapSdl3ToGml(SDL_Gamepad* gp, GamepadSlot* slot) {
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        int gmlIdx = SDL_TO_GML_BUTTON[i];

        if (gmlIdx == 0 && i != SDL_GAMEPAD_BUTTON_SOUTH) continue;

        slot->buttonDown[gmlIdx] = SDL_GetGamepadButton(gp, (SDL_GamepadButton)i);
        slot->buttonValue[gmlIdx] = slot->buttonDown[gmlIdx] ? 1.0f : 0.0f;
    }

    const float invMaxAxis = 1.0f / 32767.0f;

    float lt = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * invMaxAxis;
    float rt = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * invMaxAxis;

    lt = SDL_clamp(lt, 0.0f, 1.0f);
    rt = SDL_clamp(rt, 0.0f, 1.0f);

    slot->buttonValue[IDX_LT] = lt;
    slot->buttonValue[IDX_RT] = rt;

    slot->buttonDown[IDX_LT] = (lt >= slot->triggerThreshold);
    slot->buttonDown[IDX_RT] = (rt >= slot->triggerThreshold);

    slot->axisValue[0] = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) * invMaxAxis;
    slot->axisValue[1] = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) * invMaxAxis;
    slot->axisValue[2] = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) * invMaxAxis;
    slot->axisValue[3] = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) * invMaxAxis;
}

bool platformHandleEvents(void) {
    bool should_exit = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch(e.type) {
            case SDL_EVENT_KEY_DOWN:
                // During playback, suppress real keyboard input
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                if (e.key.repeat != 0)
                    break;
                RunnerKeyboard_onKeyDown(g_runner->keyboard, SDLKeyToGml(e.key.key));
                break;
            case SDL_EVENT_KEY_UP:
                // During playback, suppress real keyboard input
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                RunnerKeyboard_onKeyUp(g_runner->keyboard, SDLKeyToGml(e.key.key));
                break;
            case SDL_EVENT_TEXT_INPUT:
                // During playback, suppress real keyboard input
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                RunnerKeyboard_onCharacter(g_runner->keyboard, utf8_to_codepoint(e.text.text));
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                int32_t gmlBtn = SDLMouseButtonToGml(e.button.button);
                if (gmlBtn >= 0) RunnerMouse_onButtonDown(g_runner->mouse, gmlBtn);
            } break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                int32_t gmlBtn = SDLMouseButtonToGml(e.button.button);
                if (gmlBtn >= 0) RunnerMouse_onButtonUp(g_runner->mouse, gmlBtn);
            } break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                if (e.wheel.y != 0)
                    RunnerMouse_onWheel(g_runner->mouse, (float)e.wheel.y);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                fbWidth = e.window.data1;
                fbHeight = e.window.data2;
                if (gfx == SOFTWARE)
                    scr = SDL_GetWindowSurface(window);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                int device_index = e.cdevice.which;
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (openControllers[i] == NULL) {
                        openControllers[i] = SDL_OpenGamepad(device_index);
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                int instanceId = e.cdevice.which;
                repeat(MAX_GAMEPADS, i) {
                    if (openControllers[i]) {
                        SDL_Joystick* joy = SDL_GetGamepadJoystick(openControllers[i]);
                        if (joy && SDL_GetJoystickID(joy) == (SDL_JoystickID) instanceId) {
                            SDL_CloseGamepad(openControllers[i]);
                            openControllers[i] = NULL;
                            break;
                        }
                    }
                }
                break;
            }
            case SDL_EVENT_QUIT:
                should_exit = true;
                break;
            default:
                break;
        }
    }

    g_runner->gamepads->connectedCount = 0;
    for (int slotIdx = 0; slotIdx < MAX_GAMEPADS; slotIdx++) {
        GamepadSlot* slot = g_runner->gamepads->slots + slotIdx;
        SDL_Gamepad* gp = openControllers[slotIdx];

        memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
        memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
        memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
        memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
        memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
        memset(slot->axisValue, 0, sizeof(slot->axisValue));

        if (gp && SDL_GamepadConnected(gp)) {
            slot->connected = true;
            slot->jid = slotIdx;

            const char* name = SDL_GetGamepadName(gp);
            if (name != NULL) {
                strncpy(slot->description, name, sizeof(slot->description) - 1);
                slot->description[sizeof(slot->description) - 1] = '\0';
            } else {
                slot->description[0] = '\0';
            }

            char guidStr[64] = {0};
            SDL_Joystick* joy = SDL_GetGamepadJoystick(gp);
            if (joy) {
                SDL_GUIDToString(SDL_GetJoystickGUID(joy), guidStr, sizeof(guidStr));
            }
            strncpy(slot->guid, guidStr, sizeof(slot->guid) - 1);
            slot->guid[sizeof(slot->guid) - 1] = '\0';

            mapSdl3ToGml(gp, slot);

            for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
                bool wasDown = slot->buttonDownPrev[btn];
                slot->buttonPressed[btn] = (slot->buttonDown[btn] && !wasDown);
                slot->buttonReleased[btn] = (!slot->buttonDown[btn] && wasDown);
            }
            g_runner->gamepads->connectedCount++;
        } else {
            if (gp) {
                SDL_CloseGamepad(gp);
                openControllers[slotIdx] = NULL;
            }
            slot->connected = false;
            slot->guid[0] = '\0';
        }
    }

    return should_exit;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 0) {
        SDL_DelayPrecise((Uint64)remaining);
    }
}
