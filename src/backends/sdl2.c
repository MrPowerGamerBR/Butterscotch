#include <ctype.h>
#include "stdio_compat.h"

#include <SDL2/SDL.h>

#include "common.h"
#include "input_recording.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"
#ifdef PLATFORM_SWITCH
#include <switch.h>
#include "switch_input.h"
#endif

static Runner *g_runner;
static SDL_Surface* scr;
static SDL_Window *window;
static SDL_GameController* openControllers[MAX_GAMEPADS];

static SDL_Window *tryOpenWindow(int reqW, int reqH, const char* title, Uint32 flags) {
    if (gfx == SOFTWARE) {
        return SDL_CreateWindow(
            title,
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            reqW, reqH,
            flags
        );
    }
    if (gfx == LEGACY_GL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);

        SDL_Window *newWindow = SDL_CreateWindow(
            title,
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            reqW, reqH,
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

#ifndef NDEBUG
        contextFlags |= SDL_GL_CONTEXT_DEBUG_FLAG;
#endif

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GLCommon_versions[i].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GLCommon_versions[i].minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);

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
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            reqW, reqH,
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
    if (gfx == SOFTWARE) {
        if (scr->w <= 0 || scr->h <= 0) return false;
        *outW = scr->w;
        *outH = scr->h;
    } else {
        int w = 0;
        int h = 0;
#if SDL_VERSION_ATLEAST(2, 0, 1)
        SDL_GL_GetDrawableSize(window, &w, &h);
#else
        SDL_GetWindowSize(window, &w, &h);
#endif
        if (w <= 0 || h <= 0) return false;
        *outW = w;
        *outH = h;
    }
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

static float platformGetWindowScale(void) {
    int32_t draw_w = 0, draw_h = 0;
    int logical_w, logical_h;
    platformGetWindowSize(&draw_w, &draw_h);
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    return (logical_h > 0) ? (float)draw_h / logical_h : 1.0f;
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;

#ifdef PLATFORM_SWITCH
    u32 operationMode = appletGetOperationMode();
    width = (operationMode == AppletOperationMode_Console) ? 1920 : 1280;
    height = (operationMode == AppletOperationMode_Console) ? 1080 : 720;
#endif

    float scale = platformGetWindowScale();
    SDL_SetWindowSize(window, (int)(width / scale), (int)(height / scale));

    if (gfx == SOFTWARE)
        scr = SDL_GetWindowSurface(window);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    float scale = platformGetWindowScale();
    *xPos = (double)mx * scale;
    *yPos = (double)my * scale;
}

static bool platformGetWindowFocus(void) {
    return SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
}

bool platformInit(int reqW, int reqH, const char *title, bool headless) {
    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_GAMECONTROLLER)) {
        logError("Failed to initialize SDL\n");
        return false;
    }

    for (int i = 0; i < MAX_GAMEPADS; i++) {
        openControllers[i] = NULL;
    }

    Uint32 flags = 0;
    if (gfx != SOFTWARE)
        flags |= SDL_WINDOW_OPENGL;
    if (headless)
        flags |= SDL_WINDOW_HIDDEN;
    else
        flags |= SDL_WINDOW_RESIZABLE;
#if SDL_VERSION_ATLEAST(2, 0, 1)
    flags |= SDL_WINDOW_ALLOW_HIGHDPI;
#endif

    window = tryOpenWindow(reqW, reqH, title, flags);

    if (!window && gfx != SOFTWARE) {
        logError("Fatal: Could not open window: %s\n", SDL_GetError());
        return false;
    }

    if (!window && gfx == SOFTWARE) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(0, 0, &mode) == 0) {
            logWarn("%dx%d unavailable, falling back to %dx%d: %s\n",
                    reqW, reqH, mode.w, mode.h, SDL_GetError());
            reqW = mode.w;
            reqH = mode.h;
            window = SDL_CreateWindow(
                    title,
                    SDL_WINDOWPOS_UNDEFINED,
                    SDL_WINDOWPOS_UNDEFINED,
                    mode.w, mode.h,
                    flags
            );
        }
    }
    if (!window) {
        logError("Fatal: Could not set any video mode: %s\n", SDL_GetError());
        return false;
    }
    if (gfx != SOFTWARE) {
#ifndef PLATFORM_VITA
        SDL_GL_SetSwapInterval(0); // disable vsync
#endif
    } else {
        scr = SDL_GetWindowSurface(window);
    }
    // If we don't do this, the window will be larger than it should be on HiDPI displays.
    platformSetWindowSize(reqW, reqH);

#if !defined(PLATFORM_VITA) && !defined(PLATFORM_SWITCH)
    // init gamepad mappings
    const char* dbPath = "gamecontrollerdb.txt";
    if (SDL_GameControllerAddMappingsFromFile(dbPath) >= 0) {
        logInfo("Gamepad: Loaded SDL gamecontroller mappings successfully\n");
    } else {
        logWarn("Gamepad: SDL gamecontrollerdb.txt not found at %s or failed to load, using defaults\n", dbPath);
    }
#endif

    return true;
}

void platformExit(void) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (openControllers[i]) {
            SDL_GameControllerClose(openControllers[i]);
            openControllers[i] = NULL;
        }
    }
    SDL_Quit();
}

static void platformSetCursor(int32_t cursorType) {
    if (cursorType == GML_CR_NONE) {
        SDL_ShowCursor(SDL_DISABLE);
        return;
    }
    SDL_ShowCursor(SDL_ENABLE);

    SDL_SystemCursor sdlCursor;
    switch (cursorType) {
        case GML_CR_CROSS: sdlCursor = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
        case GML_CR_BEAM: sdlCursor = SDL_SYSTEM_CURSOR_IBEAM;     break;
        case GML_CR_SIZE_NESW: sdlCursor = SDL_SYSTEM_CURSOR_SIZENESW;  break;
        case GML_CR_SIZE_NS: sdlCursor = SDL_SYSTEM_CURSOR_SIZENS;    break;
        case GML_CR_SIZE_NWSE: sdlCursor = SDL_SYSTEM_CURSOR_SIZENWSE;  break;
        case GML_CR_SIZE_WE: sdlCursor = SDL_SYSTEM_CURSOR_SIZEWE;    break;
        case GML_CR_HOURGLASS: sdlCursor = SDL_SYSTEM_CURSOR_WAIT;      break;
        case GML_CR_DRAG: sdlCursor = SDL_SYSTEM_CURSOR_HAND;   break;
        case GML_CR_APPSTART: sdlCursor = SDL_SYSTEM_CURSOR_WAITARROW; break;
        case GML_CR_HANDPOINT: sdlCursor = SDL_SYSTEM_CURSOR_HAND;      break;
        case GML_CR_SIZE_ALL: sdlCursor = SDL_SYSTEM_CURSOR_SIZEALL;   break;
        default:  sdlCursor = SDL_SYSTEM_CURSOR_ARROW;     break;
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
        SDL_FreeSurface(nextFb);
        nextFb = NULL;
    }

    nextFb = SDL_CreateRGBSurfaceFrom(
        framebuffer,
        width,
        height,
        32,
        width * 4,
        0x00ff0000, // Rmask
        0x0000ff00, // Gmask
        0x000000ff, // Bmask
        0x00000000  // Amask
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

#ifndef PLATFORM_SWITCH
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

static void mapSdl2ToGml(SDL_GameController* gc, GamepadSlot* slot) {
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A)) slot->buttonDown[0] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)) slot->buttonDown[1] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X)) slot->buttonDown[2] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y)) slot->buttonDown[3] = true;

    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) slot->buttonDown[4] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) slot->buttonDown[5] = true;

    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) slot->buttonDown[8] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)) slot->buttonDown[9] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE)) slot->buttonDown[16] = true;

    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK)) slot->buttonDown[10] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) slot->buttonDown[11] = true;

    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) slot->buttonDown[12] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) slot->buttonDown[13] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) slot->buttonDown[14] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) slot->buttonDown[15] = true;

    float lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
    float rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    if (lt < 0.0f) lt = 0.0f;
    if (rt < 0.0f) rt = 0.0f;
    slot->buttonValue[IDX_LT] = lt;
    slot->buttonValue[IDX_RT] = rt;
    if (lt >= slot->triggerThreshold) slot->buttonDown[IDX_LT] = true;
    if (rt >= slot->triggerThreshold) slot->buttonDown[IDX_RT] = true;

    float lh = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    float lv = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    float rh = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
    float rv = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;

    slot->axisValue[0] = lh;
    slot->axisValue[1] = lv;
    slot->axisValue[2] = rh;
    slot->axisValue[3] = rv;

    for (int i = 0; GP_BUTTON_COUNT > i; i++) {
        if (i == IDX_LT || i == IDX_RT) continue;
        slot->buttonValue[i] = slot->buttonDown[i] ? 1.0f : 0.0f;
    }
}
#endif

bool platformHandleEvents(void) {
#ifdef PLATFORM_SWITCH
    return SwitchInput_handleEvents(g_runner);
#else
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            default:
                if (InputRecording_isPlaybackActive(globalInputRecording)) continue;
            case SDL_WINDOWEVENT:
            case SDL_QUIT:
                break;
        }
        switch(e.type) {
            case SDL_KEYDOWN:
                // During playback, suppress real keyboard input
                if (e.key.repeat != 0)
                    break;
                RunnerKeyboard_onKeyDown(g_runner->keyboard, SDLKeyToGml(e.key.keysym.sym));
                break;
            case SDL_KEYUP:
                // During playback, suppress real keyboard input
                RunnerKeyboard_onKeyUp(g_runner->keyboard, SDLKeyToGml(e.key.keysym.sym));
                break;
            case SDL_TEXTINPUT:
                // During playback, suppress real keyboard input
                RunnerKeyboard_onCharacter(g_runner->keyboard, utf8_to_codepoint(e.text.text));
                break;
            case SDL_MOUSEBUTTONDOWN: {
                int32_t gmlBtn = SDLMouseButtonToGml(e.button.button);
                if (gmlBtn >= 0) RunnerMouse_onButtonDown(g_runner->mouse, gmlBtn);
            } break;
            case SDL_MOUSEBUTTONUP: {
                int32_t gmlBtn = SDLMouseButtonToGml(e.button.button);
                if (gmlBtn >= 0) RunnerMouse_onButtonUp(g_runner->mouse, gmlBtn);
            } break;
            case SDL_MOUSEWHEEL:
                if (e.wheel.y != 0)
                    RunnerMouse_onWheel(g_runner->mouse, (float)e.wheel.y);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && gfx == SOFTWARE)
                    scr = SDL_GetWindowSurface(window);
                break;
            case SDL_CONTROLLERDEVICEADDED: {
                int device_index = e.cdevice.which;
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (openControllers[i] == NULL) {
                        openControllers[i] = SDL_GameControllerOpen(device_index);
                        break;
                    }
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                int instance_id = e.cdevice.which;
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (openControllers[i]) {
                        SDL_Joystick* joy = SDL_GameControllerGetJoystick(openControllers[i]);
                        if (joy && SDL_JoystickInstanceID(joy) == instance_id) {
                            SDL_GameControllerClose(openControllers[i]);
                            openControllers[i] = NULL;
                            break;
                        }
                    }
                }
                break;
            }
            case SDL_QUIT:
                return true;
                break;
            default:
                break;
        }
    }

    g_runner->gamepads->connectedCount = 0;
    for (int slotIdx = 0; slotIdx < MAX_GAMEPADS; slotIdx++) {
        GamepadSlot* slot = g_runner->gamepads->slots + slotIdx;
        SDL_GameController* gc = openControllers[slotIdx];

        memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
        memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
        memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
        memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
        memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
        memset(slot->axisValue, 0, sizeof(slot->axisValue));

        if (gc && SDL_GameControllerGetAttached(gc)) {
            slot->connected = true;
            slot->jid = slotIdx;

            const char* name = SDL_GameControllerName(gc);
            if (name != NULL) {
                strncpy(slot->description, name, sizeof(slot->description) - 1);
                slot->description[sizeof(slot->description) - 1] = '\0';
            } else {
                slot->description[0] = '\0';
            }

            char guidStr[64] = {0};
            SDL_Joystick* joy = SDL_GameControllerGetJoystick(gc);
            if (joy) {
                SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guidStr, sizeof(guidStr));
            }
            strncpy(slot->guid, guidStr, sizeof(slot->guid) - 1);
            slot->guid[sizeof(slot->guid) - 1] = '\0';

            mapSdl2ToGml(gc, slot);

            for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
                bool wasDown = slot->buttonDownPrev[btn];
                if (slot->buttonDown[btn] && !wasDown) slot->buttonPressed[btn] = true;
                if (!slot->buttonDown[btn] && wasDown) slot->buttonReleased[btn] = true;
            }
            g_runner->gamepads->connectedCount++;
        } else {
            if (gc) {
                SDL_GameControllerClose(gc);
                openControllers[slotIdx] = NULL;
            }
            slot->connected = false;
            slot->guid[0] = '\0';
        }
    }

    return false;
#endif
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000)
        SDL_Delay((remaining - 1000000) / 1000000);

    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}
