#include <jni.h>
#include "stdio_compat.h"
#include <stdint.h>
#include "string_compat.h"
#include <errno.h>
#include <sys/stat.h>
#include <android/log.h>
#include <GLES3/gl3.h>

#include "common.h"
#include "data_win.h"
#include "runner.h"
#include "overlay_file_system.h"
#include "ma_audio_system.h"
#include "noop_audio_system.h"
#include "gl/gl_renderer.h"
#include "stb_ds.h"

#include "log.h"

#define LOG_TAG "Butterscotch"
// ===[ Runner state ]===

static Runner* gRunner = nullptr;

// Global attributes that we need to store to persist between game_change calls.
static char* gCurrentDataWinPath = nullptr;
static char* gSavesPath = nullptr;
static YoYoOperatingSystem gReportedOs = OS_WINDOWS;
// Host-owned offscreen FBO that the composited frame is presented into for post-processing; 0 == straight to the window
static GLuint gHostFramebuffer = 0;
static float gWidescreenHackAspectRatio = 0.0f;
static float gFreeCamPanX = 0.0f;
static float gFreeCamPanY = 0.0f;
static float gFreeCamZoom = 1.0f;
static float gNormalizedCursorX = 0.0f;
static float gNormalizedCursorY = 0.0f;
// We don't need to worry about game changes because the profiler will be automatically disabled then
static int32_t gProfilerStartedAtFrame = 0;

void platformLog(const logType type, const char *format, va_list va) {
    int prio;
    switch (type) {
        case LOG_TYPE_NORMAL:
            prio = ANDROID_LOG_INFO;
            break;
        case LOG_TYPE_WARNING:
            prio = ANDROID_LOG_WARN;
            break;
        case LOG_TYPE_ERROR:
            prio = ANDROID_LOG_ERROR;
            break;
        case LOG_TYPE_DEBUG:
            prio = ANDROID_LOG_DEBUG;
            break;
    }

    __android_log_vprint(prio, LOG_TAG, format, va);
}

// Android has no platformGetWindowSize like the desktop, so we cache the EGL surface size the host
// passes into stepAndDraw and expose it through the getWindowSize hook below.
static int32_t gWindowW = 0;
static int32_t gWindowH = 0;

// Returns false until the first frame so callers fall back to the GEN8 default instead of reading 0x0.
static bool androidGetWindowSize(int32_t* outW, int32_t* outH) {
    if (gWindowW <= 0 || gWindowH <= 0) return false;
    if (outW != nullptr) *outW = gWindowW;
    if (outH != nullptr) *outH = gWindowH;
    return true;
}

// ===[ JNI -> Kotlin push callbacks ]===

static JavaVM* gJvm = nullptr;
static jclass gNativeClass = nullptr;
static jmethodID gOnTitleChangedMethod = nullptr;
static jmethodID gOnGameSizeChangedMethod = nullptr;

static JNIEnv* getEnvNoAttach(void) {
    if (gJvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if ((*gJvm)->GetEnv(gJvm, (void**) &env, JNI_VERSION_1_6) != JNI_OK) return nullptr;
    return env;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, MAYBE_UNUSED void* reserved) {
    gJvm = vm;
    JNIEnv* env = nullptr;
    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass localCls = (*env)->FindClass(env, "net/perfectdreams/butterscotch/android/ButterscotchNative");
    if (localCls == nullptr) {
        logError("JNI_OnLoad: FindClass failed for ButterscotchNative");
        return JNI_ERR;
    }
    gNativeClass = (*env)->NewGlobalRef(env, localCls);
    (*env)->DeleteLocalRef(env, localCls);

    gOnTitleChangedMethod = (*env)->GetStaticMethodID(env, gNativeClass, "onTitleChanged", "(Ljava/lang/String;)V");
    gOnGameSizeChangedMethod = (*env)->GetStaticMethodID(env, gNativeClass, "onGameSizeChanged", "(II)V");
    if (gOnTitleChangedMethod == nullptr || gOnGameSizeChangedMethod == nullptr) {
        logError("JNI_OnLoad: GetStaticMethodID failed");
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

static void setWindowTitle(const char* title) {
    if (title == nullptr) title = "";
    logInfo("Window title: %s", title);
    JNIEnv* env = getEnvNoAttach();
    if (env == nullptr || gNativeClass == nullptr) return;
    jstring jTitle = (*env)->NewStringUTF(env, title);
    (*env)->CallStaticVoidMethod(env, gNativeClass, gOnTitleChangedMethod, jTitle);
    (*env)->DeleteLocalRef(env, jTitle);
}

// ===[ JNI exports ]===

#define JNI_FN(name) Java_net_perfectdreams_butterscotch_android_ButterscotchNative_##name

JNIEXPORT void JNICALL JNI_FN(init)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    // Set stdout and stderr to not be buffered
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    logInfo("Butterscotch native init");
}

JNIEXPORT jint JNICALL JNI_FN(getTargetFrameHz)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    Runner* runner = gRunner;
    if (runner == nullptr || runner->currentRoom == nullptr) return 0;
    return (jint) runner->currentRoom->speed;
}

// Fills out[4] with the letterboxed game viewport (x, y, w, h) in window pixels, as computed by the last stepAndDraw.
// This is the same box the runner blits the game into inside the host framebuffer, so the host can post-process the game region without touching the letterbox bars.
JNIEXPORT void JNICALL JNI_FN(getViewport)(JNIEnv* env, MAYBE_UNUSED jclass cls, jintArray out) {
    Runner* runner = gRunner;
    jint values[4];
    if (runner == nullptr) {
        values[0] = 0;
        values[1] = 0;
        values[2] = 0;
        values[3] = 0;
    } else {
        values[0] = (jint) runner->viewportX;
        values[1] = (jint) runner->viewportY;
        values[2] = (jint) runner->viewportW;
        values[3] = (jint) runner->viewportH;
    }
    (*env)->SetIntArrayRegion(env, out, 0, 4, values);
}

JNIEXPORT jint JNICALL JNI_FN(getRoomCount)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    Runner* runner = gRunner;
    if (runner == nullptr || runner->currentRoom == nullptr) return 0;
    return runner->dataWin->room.count;
}

JNIEXPORT jstring JNICALL JNI_FN(getRoomName)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint roomIndex) {
    Runner* runner = gRunner;
    if (runner == nullptr || runner->currentRoom == nullptr) return 0;
    return (*env)->NewStringUTF(env, runner->dataWin->room.rooms[roomIndex].name);
}

JNIEXPORT void JNICALL JNI_FN(gotoRoom)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint roomIndex) {
    Runner* runner = gRunner;
    if (runner == nullptr || runner->currentRoom == nullptr) return;
    runner->pendingRoom = roomIndex;
}

JNIEXPORT void JNICALL JNI_FN(setWidescreenHackAspectRatio)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jfloat aspectRatio) {
    gWidescreenHackAspectRatio = aspectRatio;
}

// panX/panY are fractions of the (zoomed) view, zoom is a magnification multiplier (1.0 = identity).
JNIEXPORT void JNICALL JNI_FN(setFreeCamera)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jfloat panX, jfloat panY, jfloat zoom) {
    gFreeCamPanX = panX;
    gFreeCamPanY = panY;
    gFreeCamZoom = zoom;
}

JNIEXPORT void JNICALL JNI_FN(setNormalizedCursorPosition)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jfloat x, jfloat y) {
    gNormalizedCursorX = x;
    gNormalizedCursorY = y;
}

JNIEXPORT void JNICALL JNI_FN(setMouseButtonState)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint button, jboolean down) {
    Runner* runner = gRunner;
    if (runner == nullptr) return;

    if (down) {
        RunnerMouse_onButtonDown(runner->mouse, button);
    } else {
        RunnerMouse_onButtonUp(runner->mouse, button);
    }
}

// ===[ DataWin handle API ]===

JNIEXPORT jlong JNICALL JNI_FN(dataWinParseLight)(JNIEnv* env, MAYBE_UNUSED jclass cls, jstring jWadPath) {
    const char* wadPath = (*env)->GetStringUTFChars(env, jWadPath, nullptr);
    DataWin* dataWin = DataWin_parse(
        wadPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseStrg = true,
        }
    );
    (*env)->ReleaseStringUTFChars(env, jWadPath, wadPath);
    return (jlong) (uintptr_t) dataWin;
}

static DataWin* requireDataWin(JNIEnv* env, jlong handle) {
    DataWin* dataWin = (DataWin*) (uintptr_t) handle;
    if (dataWin == nullptr) {
        jclass exClass = (*env)->FindClass(env, "java/lang/IllegalStateException");
        if (exClass != nullptr) {
            (*env)->ThrowNew(env, exClass, "DataWin handle is null (use-after-free or never parsed)");
        }
    }
    return dataWin;
}

JNIEXPORT void JNICALL JNI_FN(dataWinFree)(JNIEnv* env, MAYBE_UNUSED jclass cls, jlong handle) {
    DataWin* dataWin = requireDataWin(env, handle);
    if (dataWin == nullptr) return;
    DataWin_free(dataWin);
}

JNIEXPORT jstring JNICALL JNI_FN(dataWinName)(JNIEnv* env, MAYBE_UNUSED jclass cls, jlong handle) {
    DataWin* dataWin = requireDataWin(env, handle);
    if (dataWin == nullptr) return nullptr;
    const char* name = dataWin->gen8.name;
    return (name != nullptr && name[0] != '\0') ? (*env)->NewStringUTF(env, name) : nullptr;
}

JNIEXPORT jstring JNICALL JNI_FN(dataWinDisplayName)(JNIEnv* env, MAYBE_UNUSED jclass cls, jlong handle) {
    DataWin* dataWin = requireDataWin(env, handle);
    if (dataWin == nullptr) return nullptr;
    const char* name = dataWin->gen8.displayName;
    return (name != nullptr && name[0] != '\0') ? (*env)->NewStringUTF(env, name) : nullptr;
}

JNIEXPORT jint JNICALL JNI_FN(dataWinWadVersion)(JNIEnv* env, MAYBE_UNUSED jclass cls, jlong handle) {
    DataWin* dataWin = requireDataWin(env, handle);
    if (dataWin == nullptr) return 0;
    return (jint) dataWin->gen8.wadVersion;
}

JNIEXPORT jstring JNICALL JNI_FN(dataWinGmsVersion)(JNIEnv* env, MAYBE_UNUSED jclass cls, jlong handle) {
    DataWin* dataWin = requireDataWin(env, handle);
    if (dataWin == nullptr) return nullptr;
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", dataWin->gen8.major, dataWin->gen8.minor, dataWin->gen8.release, dataWin->gen8.build);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL JNI_FN(dataWinDetectedGmsVersion)(JNIEnv* env, MAYBE_UNUSED jclass cls, jlong handle) {
    DataWin* dataWin = requireDataWin(env, handle);
    if (dataWin == nullptr) return nullptr;
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build);
    return (*env)->NewStringUTF(env, buf);
}

// Extracts the Runner arguments from a string, returning the values on stb_ds array
// The "Runner arguments" is used for the "--game-args" and for the game_change GML function
// Returns the modified array
// (From desktop/main.c)
static char** extractRunnerArguments(char* rawArguments) {
    // The "saveptr" is used for strtok_r to store its state, so it is thread safe™
    char* saveptr;
    // We create a copy because strtok_r completely obliterates the original char buffer
    char* copy = safeStrdup(rawArguments);
    char* token = strtok_r(copy, " \t\r\n", &saveptr);
    char** array = nullptr;

    while (token != nullptr) {
        arrput(array, safeStrdup(token));
        token = strtok_r(nullptr, " \t\r\n", &saveptr);
    }

    free(copy);
    return array;
}

// Builds the VM/runner/renderer/audio from a data.win on disk and fires the first room, leaving the result in gRunner
static bool startRunnerFromPath(const char* dataWinPath, const char* savesPath, char** gameArgs, jint jOsType) {
    requireNotNull(dataWinPath);
    requireNotNull(savesPath);
    requireNotNull(gameArgs);

    if (gRunner != nullptr) {
        logWarn("startRunnerFromPath called while a runner is already alive; ignoring");
        return false;
    }

    if (mkdir(savesPath, 0777) != 0 && errno != EEXIST) {
        logWarn("Could not create saves dir %s: %s", savesPath, strerror(errno));
    }

    DataWin* dataWin = DataWin_parse(
        dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME,
            .lazyLoadRooms = false,
            .eagerlyLoadedRooms = nullptr
        }
    );

    if (dataWin == nullptr) {
        logError("Failed to parse data.win at %s", dataWinPath);
        return false;
    }

    char* bundleDir = safeStrdup(dataWinPath);
    bsGetDirname(bundleDir);

    VMContext* vm = VM_create(dataWin);
    Renderer* renderer = GLRenderer_create();
    ((GLRenderer*) renderer)->hostFramebuffer = gHostFramebuffer;
    OverlayFileSystem* overlayFs = OverlayFileSystem_create(bundleDir, savesPath);
    free(bundleDir);

    AudioSystem* audioSystem = (AudioSystem*) MaAudioSystem_create(dataWin);
    if (audioSystem == nullptr) {
        logWarn("MaAudioSystem_create returned NULL; falling back to silent audio");
        audioSystem = (AudioSystem*) NoopAudioSystem_create();
    }

    Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) overlayFs, audioSystem, 0);
    runner->osType = jOsType;
    runner->setWindowTitle = setWindowTitle;
    runner->windowHasFocus = nullptr;
    runner->getWindowSize = androidGetWindowSize;
    if (gameArgs != nullptr) {
        Runner_setGameArgs(runner, gameArgs, (int32_t) arrlen(gameArgs));
    }

    const char* initialTitle = dataWin->gen8.displayName;
    if (initialTitle == nullptr || initialTitle[0] == '\0') initialTitle = dataWin->gen8.name;
    setWindowTitle(initialTitle);

    JNIEnv* env = getEnvNoAttach();
    if (env != nullptr && gNativeClass != nullptr && gOnGameSizeChangedMethod != nullptr) {
        (*env)->CallStaticVoidMethod(env, gNativeClass, gOnGameSizeChangedMethod, (jint) dataWin->gen8.defaultWindowWidth, (jint) dataWin->gen8.defaultWindowHeight);
    }

    Runner_initFirstRoom(runner);

    // Store the current data.win and save path so we can reuse it when game_change is called
    char* newDataWinPath = safeStrdup(dataWinPath);
    char* newSavesPath = safeStrdup(savesPath);

    // Free the current stored paths
    free(gCurrentDataWinPath);
    free(gSavesPath);

    // And set it the new one!
    gCurrentDataWinPath = newDataWinPath;
    gSavesPath = newSavesPath;
    gReportedOs = jOsType;

    gRunner = runner;
    logInfo("Runner started OK");
    return true;
}

// Tears down the runenr/renderer/audio/VM/data.win and clears the gRunner
static void teardownRunner() {
    Runner* runner = gRunner;
    if (runner == nullptr) return;
    gRunner = nullptr;

    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    runner->renderer->vtable->destroy(runner->renderer);

    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    Runner_free(runner);
    VM_free(vm);
    DataWin_free(dataWin);
}

JNIEXPORT jboolean JNICALL JNI_FN(startRunner)(JNIEnv* env, MAYBE_UNUSED jclass cls, jstring jDataWinPath, jstring jSavesPath, jint jOsType, jint jHostFramebuffer) {
    if (gRunner != nullptr) {
        logWarn("startRunner called while a runner is already alive; ignoring");
        return JNI_FALSE;
    }
    gHostFramebuffer = (GLuint) jHostFramebuffer;
    const char* dataWinPath = (*env)->GetStringUTFChars(env, jDataWinPath, nullptr);
    const char* savesPath   = (*env)->GetStringUTFChars(env, jSavesPath,   nullptr);
    char** gameArgs = nullptr;
    arrput(gameArgs, safeStrdup("butterscotch")); // Synthetic argv[0]

    bool ok = startRunnerFromPath(dataWinPath, savesPath, gameArgs, jOsType);
    gWidescreenHackAspectRatio = 0.0f; // Reset the widescreen hack aspect ratio
    gFreeCamPanX = 0.0f; // Reset the free camera back to identity
    gFreeCamPanY = 0.0f;
    gFreeCamZoom = 1.0f;

    repeat(arrlen(gameArgs), i) free(gameArgs[i]);
    arrfree(gameArgs);
    (*env)->ReleaseStringUTFChars(env, jDataWinPath, dataWinPath);
    (*env)->ReleaseStringUTFChars(env, jSavesPath,   savesPath);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL JNI_FN(getRunningDataWinHandle)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr)
        return -1;

    return (jlong) gRunner->dataWin;
}

JNIEXPORT jlong JNICALL JNI_FN(getRunnerFrameCount)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr)
        return -1;

    return gRunner->frameCount;
}


JNIEXPORT jboolean JNICALL JNI_FN(isProfilerEnabled)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr)
        return false;

    return gRunner->vmContext->profiler != nullptr;
}

JNIEXPORT void JNICALL JNI_FN(setProfilerEnabled)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jboolean enabled) {
    if (gRunner == nullptr)
        return;

    if (enabled) {
        gProfilerStartedAtFrame = gRunner->frameCount;
    }

    Profiler_setEnabled(&gRunner->vmContext->profiler, enabled);
}

JNIEXPORT jlong JNICALL JNI_FN(getProfilerStartedAtFrame)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr)
        return -1;

    return gProfilerStartedAtFrame;
}

JNIEXPORT jlong JNICALL JNI_FN(getProfilerEntriesCount)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr)
        return 0;

    if (gRunner->vmContext->profiler == nullptr)
        return 0;

    return shlen(gRunner->vmContext->profiler->entries);
}

JNIEXPORT jstring JNICALL JNI_FN(getProfilerEntryKey)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jlong index) {
    if (gRunner == nullptr)
        return 0;

    if (gRunner->vmContext->profiler == nullptr)
        return 0;

    return (*env)->NewStringUTF(env, gRunner->vmContext->profiler->entries[index].key);
}

JNIEXPORT jlong JNICALL JNI_FN(getProfilerEntryNanos)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jlong index) {
    if (gRunner == nullptr)
        return 0;

    if (gRunner->vmContext->profiler == nullptr)
        return 0;

    return gRunner->vmContext->profiler->entries[index].value.nanos;
}

JNIEXPORT jlong JNICALL JNI_FN(getProfilerEntryOps)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jlong index) {
    if (gRunner == nullptr)
        return 0;

    if (gRunner->vmContext->profiler == nullptr)
        return 0;

    return gRunner->vmContext->profiler->entries[index].value.ops;
}

JNIEXPORT void JNICALL JNI_FN(beginFrame)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    Runner* runner = gRunner;
    if (runner == nullptr) return;
    RunnerKeyboard_beginFrame(runner->keyboard);

    RunnerGamepadState* gamepads = runner->gamepads;
    if (gamepads != nullptr) {
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            GamepadSlot* slot = &gamepads->slots[i];
            slot->connectedPrev = slot->connected;
            memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
            memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
        }
    }

    RunnerMouse_beginFrame(runner->mouse);
}

JNIEXPORT void JNICALL JNI_FN(onKeyDown)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint keyCode) {
    Runner* runner = gRunner;
    if (runner == nullptr) return;
    if (keyCode < 0 || keyCode >= GML_KEY_COUNT) return;
    RunnerKeyboard_onKeyDown(runner->keyboard, keyCode);
}

JNIEXPORT void JNICALL JNI_FN(onKeyUp)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint keyCode) {
    Runner* runner = gRunner;
    if (runner == nullptr) return;
    if (keyCode < 0 || keyCode >= GML_KEY_COUNT) return;
    RunnerKeyboard_onKeyUp(runner->keyboard, keyCode);
}

JNIEXPORT void JNICALL JNI_FN(onCharacter)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint codePoint) {
    Runner* runner = gRunner;
    if (runner == nullptr) return;
    if (codePoint <= 0) return;
    RunnerKeyboard_onCharacter(runner->keyboard, (unsigned int) codePoint);
}

// ===[ Gamepad input ]===

static GamepadSlot* androidGamepadSlot(int device) {
    Runner* runner = gRunner;
    if (runner == nullptr || runner->gamepads == nullptr) return nullptr;
    if (device < 0 || device >= MAX_GAMEPADS) return nullptr;
    return &runner->gamepads->slots[device];
}

static void androidGamepadEnsureConnected(GamepadSlot* slot, const char* name) {
    if (gRunner == nullptr || gRunner->gamepads == nullptr) return;
    if (!slot->connected) {
        slot->connected = true;
        gRunner->gamepads->connectedCount++;
    }
    if (name != nullptr && name[0] != '\0') {
        snprintf(slot->description, sizeof(slot->description), "%s", name);
    } else if (slot->description[0] == '\0') {
        snprintf(slot->description, sizeof(slot->description), "Gamepad");
    }
}

JNIEXPORT void JNICALL JNI_FN(gamepadConnected)(JNIEnv* env, MAYBE_UNUSED jclass cls, jint device, jstring jName) {
    GamepadSlot* slot = androidGamepadSlot(device);
    if (slot == nullptr) return;
    const char* name = (jName != nullptr) ? (*env)->GetStringUTFChars(env, jName, nullptr) : nullptr;
    androidGamepadEnsureConnected(slot, name);
    if (name != nullptr) (*env)->ReleaseStringUTFChars(env, jName, name);
}

JNIEXPORT void JNICALL JNI_FN(gamepadDisconnected)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint device) {
    GamepadSlot* slot = androidGamepadSlot(device);
    if (slot == nullptr) return;
    if (slot->connected && gRunner->gamepads->connectedCount > 0) {
        gRunner->gamepads->connectedCount--;
    }
    slot->connected = false;
    slot->description[0] = '\0';
    slot->guid[0] = '\0';
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonDownPrev, 0, sizeof(slot->buttonDownPrev));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));
}

JNIEXPORT void JNICALL JNI_FN(gamepadButton)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint device, jint button, jboolean down) {
    GamepadSlot* slot = androidGamepadSlot(device);
    if (slot == nullptr) return;
    if (button < 0 || button >= GP_BUTTON_COUNT) return;
    androidGamepadEnsureConnected(slot, nullptr);
    bool isDown = (down == JNI_TRUE);
    bool wasDown = slot->buttonDown[button];
    // Edge detection mirrors the keyboard path: pressed/released are one-frame flags cleared by beginFrame, so we only raise them on an actual transition.
    if (isDown && !wasDown) slot->buttonPressed[button] = true;
    if (!isDown && wasDown) slot->buttonReleased[button] = true;
    slot->buttonDown[button] = isDown;
    slot->buttonValue[button] = isDown ? 1.0f : 0.0f;
}

JNIEXPORT void JNICALL JNI_FN(gamepadAxis)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint device, jint axis, jfloat value) {
    GamepadSlot* slot = androidGamepadSlot(device);
    if (slot == nullptr) return;
    if (axis < 0 || axis >= GP_AXIS_COUNT) return;
    androidGamepadEnsureConnected(slot, nullptr);
    // Store the raw value (clamped to the normalized range); RunnerGamepad_axisValue applies the deadzone on read, matching every other backend.
    if (value < -1.0f) value = -1.0f;
    if (value > 1.0f) value = 1.0f;
    slot->axisValue[axis] = value;
}

#define BUTTERSCOTCH_DROID_CONTINUE 0
#define BUTTERSCOTCH_DROID_SHOULD_EXIT 1
#define BUTTERSCOTCH_DROID_CONTINUE_NO_SWAP 2

static bool performGameChange(const char* workingDirectory, char* launchParameters) {
    char** newArguments = extractRunnerArguments(launchParameters);

    // Find the data.win named by the "-game <file>" entry inside the launch parameters
    char* dataWinFilename = nullptr;
    size_t argCount = arrlen(newArguments);
    repeat(argCount, i) {
        if (strcmp(newArguments[i], "-game") == 0 && argCount - 1 > i) {
            dataWinFilename = newArguments[i + 1];
            break;
        }
    }

    if (dataWinFilename == nullptr) {
        logError("Runner: Launch parameters '%s' did not contain a '-game <file>' entry! Shutting down...\n", launchParameters);
        repeat(arrlen(newArguments), i) {
            free(newArguments[i]);
        }
        arrfree(newArguments);
        return false;
    }

    // Get the parent directory of the main data.win file
    char* parentDir = safeStrdup(gCurrentDataWinPath);
    bsGetDirname(parentDir);

    // The pendingWorkingDirectory contains a slash at the beginning of it (example: /chapter3)
    // The parentDir does NOT have a trailing slash, so we don't need to bother with it
    size_t newPathLen = strlen(parentDir) + strlen(workingDirectory) + 1 + strlen(dataWinFilename) + 1;
    char* newPath = safeMalloc(newPathLen);
    snprintf(newPath, newPathLen, "%s%s/%s", parentDir, workingDirectory, dataWinFilename);

    free(parentDir);

    // Rebuild the gameArgs
    char** gameArgs = nullptr;
    arrput(gameArgs, safeStrdup("butterscotch")); // Synthetic argv[0]
    repeat(arrlen(newArguments), i) {
        arrput(gameArgs, safeStrdup(newArguments[i]));
    }

    teardownRunner();
    bool ok = startRunnerFromPath(newPath, gSavesPath, gameArgs, gReportedOs);

    free(newPath);
    repeat(arrlen(gameArgs), i) free(gameArgs[i]);
    arrfree(gameArgs);
    repeat(arrlen(newArguments), i) {
        free(newArguments[i]);
    }
    arrfree(newArguments);
    return ok;
}

JNIEXPORT jint JNICALL JNI_FN(stepAndDraw)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls, jint winW, jint winH, jfloat deltaTime) {
    Runner* runner = gRunner;
    runner->deltaTime = deltaTime * 1000000;

    if (runner == nullptr)
        return BUTTERSCOTCH_DROID_SHOULD_EXIT;

    if (runner->shouldExit)
        return BUTTERSCOTCH_DROID_SHOULD_EXIT;

    if (runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr) {
        // Snapshot the pending game_change request, then rebuild the runner from the new game
        char* nextWorkingDirectory = runner->pendingWorkingDirectory;
        char* nextLaunchParameters = runner->pendingLaunchParameters;
        runner->pendingWorkingDirectory = nullptr;
        runner->pendingLaunchParameters = nullptr;

        bool ok = performGameChange(nextWorkingDirectory, nextLaunchParameters);
        free(nextWorkingDirectory);
        free(nextLaunchParameters);
        return ok ? BUTTERSCOTCH_DROID_CONTINUE : BUTTERSCOTCH_DROID_SHOULD_EXIT;
    }

    Runner_step(runner);

    // Apply the visual-only free camera. Identity (0,0,1) when the UI hasn't enabled photo mode, so this is a no-op in the common case.
    runner->freeCamPanX = gFreeCamPanX;
    runner->freeCamPanY = gFreeCamPanY;
    runner->freeCamZoom = (gFreeCamZoom > 0.0f) ? gFreeCamZoom : 1.0f;

    // Update audio system (gain fading, cleanup ended sounds)
    if (0.0f > deltaTime) deltaTime = 0.0f;
    if (deltaTime > 0.1f) deltaTime = 0.1f; // cap delta to avoid huge fades on lag spikes
    runner->audioSystem->vtable->update(runner->audioSystem, deltaTime);

    // winW/winH is the EGL surface size the host gives us; the desktop instead queries it via
    // platformGetWindowSize. Cache it so the getWindowSize hook can read it.
    if (winW < 1) winW = 1;
    if (winH < 1) winH = 1;
    int32_t fbWidth = winW;
    int32_t fbHeight = winH;
    gWindowW = fbWidth;
    gWindowH = fbHeight;

    Gen8* gen8 = &runner->dataWin->gen8;

    if (!runner->appSurfaceEnabled) {
        runner->applicationWidth = fbWidth;
        runner->applicationHeight = fbHeight;
        runner->usingAppSurface = false;
    } else {
        if (runner->applicationWidth <= 0 || runner->applicationHeight <= 0) {
            runner->applicationWidth = (int32_t) gen8->defaultWindowWidth;
            runner->applicationHeight = (int32_t) gen8->defaultWindowHeight;
        }
        runner->usingAppSurface = true;
    }

    int32_t gameW = runner->applicationWidth;
    int32_t gameH = runner->applicationHeight;

    runner->widescreenExtraWidth = 0;
    runner->widescreenExtraHeight = 0;
    if (gWidescreenHackAspectRatio && runner->usingAppSurface && gameW > 0 && gameH > 0) {
        float nativeAspect = (float) gameW / (float) gameH;
        if (gWidescreenHackAspectRatio > nativeAspect) {
            int32_t targetW = (int32_t) ((float) gameH * gWidescreenHackAspectRatio + 0.5f);
            if (targetW > gameW) {
                runner->widescreenExtraWidth = targetW - gameW;
                gameW = targetW;
            }
        } else if (gWidescreenHackAspectRatio < nativeAspect) {
            int32_t targetH = (int32_t) ((float) gameW / gWidescreenHackAspectRatio + 0.5f);
            if (targetH > gameH) {
                runner->widescreenExtraHeight = targetH - gameH;
                gameH = targetH;
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, gHostFramebuffer);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    Runner_drawPre(runner, winW, winH);

    // gameW/gameH include the widescreen expansion at this point, so the mouse mapping maps across the expanded view instead of the original room size
    Runner_beginFrame(runner, gameW, gameH, winW, winH, winW, winH);

    Runner_updateMousePosition(runner, winW, winH, gNormalizedCursorX * winW, gNormalizedCursorY * winH);

    Runner_drawViews(runner, gameW, gameH, false);
    runner->renderer->vtable->endFrameInit(runner->renderer);
    Runner_drawPost(runner, winW, winH);
    runner->renderer->vtable->endFrameEnd(runner->renderer);
    Runner_drawGUI(runner, winW, winH, gameW, gameH);

    // Only present this frame when there isn't a room change queued, to match the original runner
    bool shouldSwap = (runner->pendingRoom == -1);
    Runner_handlePendingRoomChange(runner);

    return shouldSwap ? BUTTERSCOTCH_DROID_CONTINUE : BUTTERSCOTCH_DROID_CONTINUE_NO_SWAP;
}

// Suspend the audio backend without tearing the runner down (the app was backgrounded).
// The miniaudio device mixes on its own thread, so just parking the render loop wouldn't silence it; we have to stop the device
JNIEXPORT void JNICALL JNI_FN(suspendAudio)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr || gRunner->audioSystem == nullptr)
        return;

    gRunner->audioSystem->vtable->suspend(gRunner->audioSystem);
}

// Resume the audio backend after a suspendAudio (the app came back to the foreground)
JNIEXPORT void JNICALL JNI_FN(resumeAudio)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr || gRunner->audioSystem == nullptr)
        return;

    gRunner->audioSystem->vtable->resume(gRunner->audioSystem);
}

JNIEXPORT void JNICALL JNI_FN(stopRunner)(MAYBE_UNUSED JNIEnv* env, MAYBE_UNUSED jclass cls) {
    if (gRunner == nullptr)
        return;

    logInfo("Stopping runner");

    teardownRunner();

    free(gCurrentDataWinPath);
    gCurrentDataWinPath = nullptr;
    free(gSavesPath);
    gSavesPath = nullptr;
}
