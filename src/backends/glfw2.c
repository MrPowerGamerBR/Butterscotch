#include "stdio_compat.h"
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_SW_RENDERER
#include <glad/glad.h>
#endif
#include <GL/glfw.h>

#include "common.h"
#include "input_recording.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

static Runner *g_runner;

static bool tryOpenWindow(int reqW, int reqH) {
#ifdef GLFW_OPENGL_VERSION_MAJOR
    if (gfx == SOFTWARE || gfx == LEGACY_GL) {
        glfwOpenWindowHint(GLFW_OPENGL_VERSION_MAJOR, 1);
        glfwOpenWindowHint(GLFW_OPENGL_VERSION_MINOR, (gfx == SOFTWARE) ? 0 : 1);
        return glfwOpenWindow(reqW, reqH, 8, 8, 8, 8, 24, 8, GLFW_WINDOW) != 0;
    }

    for (size_t i = 0; i < sizeof(GLCommon_versions)/sizeof(GLCommon_versions[0]); i++) {
        glfwOpenWindowHint(GLFW_OPENGL_VERSION_MAJOR, GLCommon_versions[i].major);
        glfwOpenWindowHint(GLFW_OPENGL_VERSION_MINOR, GLCommon_versions[i].minor);

        if (GLCommon_versions[i].major >= 3) {
            glfwOpenWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
            if (GLCommon_versions[i].major == 3 && GLCommon_versions[i].minor == 2) {
                glfwOpenWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
            } else {
                glfwOpenWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_FALSE);
            }
        } else {
            glfwOpenWindowHint(GLFW_OPENGL_PROFILE, 0);
        }

#ifndef NDEBUG
        glfwOpenWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif
        if (glfwOpenWindow(reqW, reqH, 8, 8, 8, 8, 24, 8, GLFW_WINDOW) != 0) {
            return true;
        }
        glfwCloseWindow();
    }

    return false;
#else
    return glfwOpenWindow(reqW, reqH, 8, 8, 8, 8, 24, 8, GLFW_WINDOW) != 0;
#endif
}

void platformSetWindowTitle(const char* title) {
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    glfwSetWindowTitle(windowTitle);
}

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    int w = 0;
    int h = 0;
    glfwGetWindowSize(&w, &h);
    if (w <= 0 || h <= 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    glfwSetWindowSize(width, height);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    int mx = 0, my = 0;
    glfwGetMousePos(&mx, &my);

    *xPos = (double)mx;
    *yPos = (double)my;
}

static bool platformGetWindowFocus(void) {
    return glfwGetWindowParam(GLFW_ACTIVE);
}

static int32_t glfwKeyToGml(int glfwKey) {
    // Letters and numbers are the same as GML
    if (glfwKey >= 'A' && glfwKey <= 'Z') return glfwKey;
    if (glfwKey >= '0' && glfwKey <= '9') return glfwKey;
    // Special keys need mapping
    switch (glfwKey) {
        case GLFW_KEY_ESC:       return VK_ESCAPE;
        case GLFW_KEY_ENTER:     return VK_ENTER;
        case GLFW_KEY_TAB:       return VK_TAB;
        case GLFW_KEY_BACKSPACE: return VK_BACKSPACE;
        case GLFW_KEY_SPACE:     return VK_SPACE;
        case GLFW_KEY_LSHIFT:
        case GLFW_KEY_RSHIFT:    return VK_SHIFT;
        case GLFW_KEY_LCTRL:
        case GLFW_KEY_RCTRL:     return VK_CONTROL;
        case GLFW_KEY_LALT:
        case GLFW_KEY_RALT:      return VK_ALT;
        case GLFW_KEY_UP:        return VK_UP;
        case GLFW_KEY_DOWN:      return VK_DOWN;
        case GLFW_KEY_LEFT:      return VK_LEFT;
        case GLFW_KEY_RIGHT:     return VK_RIGHT;
        case GLFW_KEY_F1:        return VK_F1;
        case GLFW_KEY_F2:        return VK_F2;
        case GLFW_KEY_F3:        return VK_F3;
        case GLFW_KEY_F4:        return VK_F4;
        case GLFW_KEY_F5:        return VK_F5;
        case GLFW_KEY_F6:        return VK_F6;
        case GLFW_KEY_F7:        return VK_F7;
        case GLFW_KEY_F8:        return VK_F8;
        case GLFW_KEY_F9:        return VK_F9;
        case GLFW_KEY_F10:       return VK_F10;
        case GLFW_KEY_F11:       return VK_F11;
        case GLFW_KEY_F12:       return VK_F12;
        case GLFW_KEY_INSERT:    return VK_INSERT;
        case GLFW_KEY_DEL:       return VK_DELETE;
        case GLFW_KEY_HOME:      return VK_HOME;
        case GLFW_KEY_END:       return VK_END;
        case GLFW_KEY_PAGEUP:    return VK_PAGEUP;
        case GLFW_KEY_PAGEDOWN:  return VK_PAGEDOWN;
        default:                 return -1; // Unknown
    }
}

static void GLFWCALL keyCallback(int key, int action) {
    // During playback, suppress real keyboard input (window events like close still work)
    if (InputRecording_isPlaybackActive(globalInputRecording)) return;
    int32_t gmlKey = glfwKeyToGml(key);
    if (action == GLFW_PRESS) RunnerKeyboard_onKeyDown(g_runner->keyboard, gmlKey);
    else if (action == GLFW_RELEASE) RunnerKeyboard_onKeyUp(g_runner->keyboard, gmlKey);
    // GLFW_REPEAT is ignored (GML doesn't use key repeat)
}

static void GLFWCALL characterCallback(int codepoint, int action) {
    if (action != GLFW_PRESS) return;
    if (InputRecording_isPlaybackActive(globalInputRecording)) return;
    RunnerKeyboard_onCharacter(g_runner->keyboard, codepoint);
}

#ifdef ENABLE_SW_RENDERER

static void GLFWCALL resizeCallback(int width, int height) {
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

static void GLFWCALL mouseButtonCallback(int button, int action) {
    int32_t gmlButton = glfwMouseButtonToGml(button);
    if (0 > gmlButton) return;
    if (action == GLFW_PRESS) RunnerMouse_onButtonDown(g_runner->mouse, gmlButton);
    else if (action == GLFW_RELEASE) RunnerMouse_onButtonUp(g_runner->mouse, gmlButton);
}

static int g_last_wheel_pos = 0;
static void GLFWCALL scrollCallback(int pos) {
    double yoffset = (double)(pos - g_last_wheel_pos);
    g_last_wheel_pos = pos;
    if (g_runner) RunnerMouse_onWheel(g_runner->mouse, yoffset);
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    if (headless) {
        logError("Headless mode is not supported with GLFW 2\n");
        return false;
    }

    // Init GLFW
    if (!glfwInit()) {
        logError("Failed to initialize GLFW\n");
        return false;
    }

    if (!tryOpenWindow(reqW, reqH)) {
        logError("Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    glfwSetWindowTitle(title);

    glfwSwapInterval(0); // Disable v-sync, we control timing ourselves

    // Set up keyboard input
    glfwSetKeyCallback(keyCallback);
    glfwSetCharCallback(characterCallback);
    // Set up mouse input
    glfwSetMouseButtonCallback(mouseButtonCallback);
    glfwSetMouseWheelCallback(scrollCallback);

    return true;
}

void platformExit(void) {
    glfwCloseWindow();
    glfwTerminate();
}

static void platformSetCursor(int32_t cursorType) {
    // GLFW2 only supports showing/hiding
    if (cursorType == GML_CR_NONE) {
        // GLFW2's mouse cursor locks the mouse position when it's invisible on Windows.
        // This just makes it visible/invisible as intended.
#ifdef _WIN32
        while (ShowCursor(FALSE) >= 0);
#else
        glfwDisable(GLFW_MOUSE_CURSOR);
#endif
    } else {
#ifdef _WIN32
        while (ShowCursor(TRUE) < 0);
#else
        glfwEnable(GLFW_MOUSE_CURSOR);
#endif
    }
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = platformGetWindowFocus;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_DEFAULT;
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE)
        glfwSetWindowSizeCallback(resizeCallback);
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
    glfwSwapBuffers();
}

void *platformGetProcAddress(const char *name) {
#ifdef _WIN32
    // glfw2's glfwGetProcAddress is broken on Windows.
    // This just implements it in a way that's fixed so it can be passed to GLAD.
    void *ret = (void *)wglGetProcAddress(name);
    // Fallback for driver-specific error codes and legacy OpenGL core functions.
    if (ret == 0 || ret == (void *)1 || ret == (void *)2 || ret == (void *)3 || ret == (void *)-1) {
        HMODULE handle = GetModuleHandle("opengl32.dll");
        if (handle)
            ret = (void *)GetProcAddress(handle, name);
    }
    return ret;
#else
    return glfwGetProcAddress(name);
#endif
}

bool platformHandleEvents(void) {
    if (!glfwGetWindowParam(GLFW_OPENED))
        return true;
    glfwPollEvents();
    return false;
}

void platformSleepUntil(uint64_t time) {
    double remaining = ((int64_t)time - (int64_t)nowNanos()) / 1000000000.0;
    if (remaining > 0.002) // glfwSleep takes seconds as a double
        glfwSleep(remaining - 0.001);

    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}
