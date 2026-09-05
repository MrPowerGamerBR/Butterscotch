#ifndef _BS_PLATFORMDEFS_H_
#define _BS_PLATFORMDEFS_H_

#include <stdbool.h>

#include "runner.h"
#include "input_recording.h"

// ===[ COMMAND LINE ARGUMENTS ]===

typedef struct { const char* name; YoYoOperatingSystem value; } OsTypeNameEntry;

static const OsTypeNameEntry OS_TYPE_NAMES[] = {
    {"unknown",       OS_UNKNOWN},
    {"windows",       OS_WINDOWS},
    {"win32",         OS_WINDOWS},
    {"macosx",        OS_MACOSX},
    {"macos",         OS_MACOSX},
    {"psp",           OS_PSP},
    {"ios",           OS_IOS},
    {"android",       OS_ANDROID},
    {"symbian",       OS_SYMBIAN},
    {"linux",         OS_LINUX},
    {"winphone",      OS_WINPHONE},
    {"tizen",         OS_TIZEN},
    {"win8native",    OS_WIN8NATIVE},
    {"wiiu",          OS_WIIU},
    {"3ds",           OS_3DS},
    {"psvita",        OS_PSVITA},
    {"bb10",          OS_BB10},
    {"ps4",           OS_PS4},
    {"xboxone",       OS_XBOXONE},
    {"ps3",           OS_PS3},
    {"xbox360",       OS_XBOX360},
    {"uwp",           OS_UWP},
    {"amazon",        OS_AMAZON},
    {"switch",        OS_SWITCH},
};
#define OS_TYPE_NAMES_COUNT (sizeof(OS_TYPE_NAMES)/sizeof(OS_TYPE_NAMES[0]))

enum GraphicsAPI {
    SOFTWARE,
    MODERN_GL,
    LEGACY_GL,
    NOOP
};

extern enum GraphicsAPI gfx;

typedef struct {
    int key;
    // We need this dummy value, think that the ds_map is like a Java HashMap NOT a HashSet
    // (Which is funny, because in Java HashSets are backed by HashMaps lol)
    bool value;
} FrameSetEntry;

#if defined(ENABLE_MODERN_GL) || defined(ENABLE_LEGACY_GL)
#define ENABLE_SCREENSHOTS
#endif

typedef struct {
    const char* dataWinPath;
    const char* saveFolder; // null = default to the directory containing dataWinPath
#ifdef ENABLE_SCREENSHOTS
    const char* screenshotPattern;
    FrameSetEntry* screenshotFrames;
    const char* screenshotSurfacesPattern;
    FrameSetEntry* screenshotSurfacesFrames;
#endif
    FrameSetEntry* dumpFrames;
    FrameSetEntry* dumpJsonFrames;
    const char* dumpJsonFilePattern;
    bool hostVariableJson;
    bool hostVariableJsonOnDemand;
    int hostVariableJsonInterval;
#ifdef ENABLE_VM_TRACING
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* collisionsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* tilesToBeTraced;
#endif
    StringBooleanEntry* disassemble;
    bool alwaysLogUnknownFunctions;
#ifdef ENABLE_VM_STUB_LOGS
    bool alwaysLogStubbedFunctions;
#endif
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printObjects;
    bool printShaders;
    bool printDeclaredFunctions;
    bool printUnknownFunctions;
    int exitAtFrame;
#ifdef ENABLE_VM_TRACING
    int traceBytecodeAfterFrame;
#endif
    double speedMultiplier;
    double fastForwardSpeed;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char* recordInputsPath;
    const char* playbackInputsPath;
    enum GraphicsAPI renderer;
    YoYoOperatingSystem osType;
    int32_t windowWidth, windowHeight; // 0 = auto (gen8 default, or the console-native size for console os-types)
    float widescreenAspect; // "widescreen hack" target aspect ratio (width/height), 0 = disabled
    char** gameArgs; // stb_ds array of owned strings, gameArgs[0] = runner executable path
    bool lazyRooms;
    StringBooleanEntry* eagerRooms; // stb_ds string-keyed set of room names
    bool lazyTextures;
    bool lazyAudio;
    DataWinLoadType loadType;
    int profilerFramesBetween; // 0 = disabled
#ifdef ENABLE_VM_OPCODE_PROFILER
    bool opcodeProfiler;
#endif
    bool disableLogColours;
} CommandLineArgs;

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless);
void platformInitFunctions(Runner *);
void platformExit(void);
void platformSwapBuffers(void);
void *platformGetProcAddress(const char *name);
bool platformHandleEvents(void);
void platformGetMousePos(double *xPos, double *yPos);
bool platformGetWindowSize(int32_t* outW, int32_t* outH);
bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH);
void platformSetWindowSize(int32_t width, int32_t height);
void platformSetWindowTitle(const char* title);
void platformSleepUntil(uint64_t time);

extern InputRecording *globalInputRecording;

// ===[ GL Versions ]===
static const struct {
    uint8_t major, minor;
    bool gles;
} GLCommon_versions[] = {
    /* Desktop GL */
    { 4, 6, false },
    { 4, 5, false },
    { 4, 4, false },
    { 4, 3, false },
    { 4, 2, false },
    { 4, 1, false },
    { 4, 0, false },
    { 3, 3, false },
    { 3, 2, false },
    { 3, 1, false },
    { 3, 0, false },
    { 2, 1, false },
    { 2, 0, false },
#ifndef USE_GLFW2
    /* GLES */
    { 3, 2, true  },
    { 3, 1, true  },
    { 3, 0, true  },
    { 2, 0, true  },
#endif
};

#endif /* _BS_PLATFORMDEFS_H_ */
