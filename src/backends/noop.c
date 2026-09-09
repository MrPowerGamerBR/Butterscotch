#include "common.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

#include <string.h>
#include <time.h>

// No-op windowing backend: does not open a window or require SDL/GLFW.

static Runner *g_runner = NULL;
static int32_t g_width = 0;
static int32_t g_height = 0;
static bool g_initialized = false;

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title;
    (void)headless;
    g_width = reqW > 0 ? reqW : 640;
    g_height = reqH > 0 ? reqH : 480;
    g_initialized = true;
    logInfo("No-op platform backend: %dx%d (no window)\n", g_width, g_height);
    return true;
}

void platformExit(void) {
    g_initialized = false;
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->setCursor = NULL;
    runner->currentCursor = GML_CR_DEFAULT;
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH) return false;
    if (!g_initialized) return false;
    *outW = g_width;
    *outH = g_height;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width > 0) g_width = width;
    if (height > 0) g_height = height;
}

void platformSetWindowTitle(const char *title) {
    (void)title;
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (xPos) *xPos = 0.0;
    if (yPos) *yPos = 0.0;
}

void platformSwapBuffers(void) {
    static uint32_t frames = 0;
    static time_t t = 0;
    time_t now = time(NULL);
    ++frames;
    if (t != now) {
        logInfo("fps: %u\n", frames);
        t = now;
        frames = 0;
    }
}

void *platformGetProcAddress(const char *name) {
    (void)name;
    return NULL;
}

bool platformHandleEvents(void) {
    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = (int64_t)time - (int64_t)nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
#ifdef _WIN32
        Sleep(remaining / 1000000);
#else
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
#endif
    }
    while (nowNanos() < time) {
        YIELD();
    }
}

void platformSetNextFramebuffer(uint32_t* framebuffer, int width, int height) {
    (void)framebuffer, (void)width, (void)height;
}
