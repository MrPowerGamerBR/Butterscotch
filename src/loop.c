#include <ctype.h>

#include "data_win.h"
#include "vm.h"
#include "loop.h"

#include "platformdefs.h"
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <time.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <io.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 33)
#define HAVE_MALLINFO2
#endif
#endif
#endif

#include "runner_keyboard.h"
#include "runner.h"
#include "input_recording.h"
#include "debug_overlay.h"
#if (defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL) || ((defined(USE_GLFW3) || defined(USE_GLFW2)) && defined(ENABLE_SW_RENDERER))) && \
    !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(PLATFORM_PS3) && !defined(PLATFORM_VITA) && !defined(__SWITCH__)
#define USE_GLAD
#include <glad/glad.h>
#endif
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
#include "gl_renderer.h"
#ifdef ENABLE_LEGACY_GL
#include "gl_legacy_renderer.h"
#endif
#include "gl_common.h"
#endif
#ifdef ENABLE_SW_RENDERER
#include "sw_renderer.h"
#endif
#ifdef ENABLE_NOOP_RENDERER
#include "noop_renderer.h"
#endif
#include "overlay_file_system.h"
#if defined(USE_OPENAL)
#include "al_audio_system.h"
#elif defined(USE_MINIAUDIO)
#include "ma_audio_system.h"
#endif
#include "noop_audio_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "utils.h"
#include "profiler.h"
#include "gettime.h"

#ifdef PLATFORM_VITA
#include "vita_textures.h"
#endif

enum GraphicsAPI gfx;

#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
const GLuint *hostFramebuffer;
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
/* we define this ourselves because psapi.h isn't available in msvc 4.0 */
typedef struct {
    DWORD cb;
    DWORD PageFaultCount;
    size_t PeakWorkingSetSize;
    size_t WorkingSetSize;
    size_t QuotaPeakPagedPoolUsage;
    size_t QuotaPagedPoolUsage;
    size_t QuotaPeakNonPagedPoolUsage;
    size_t QuotaNonPagedPoolUsage;
    size_t PagefileUsage;
    size_t PeakPagefileUsage;
} BS_PROCESS_MEMORY_COUNTERS;
#endif

static size_t get_used_memory(void) {
#if defined(__linux__)
    int fd = open("/proc/self/smaps_rollup", O_RDONLY);
    if (fd < 0)
        return 0;

    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    char *p = buf;
    while (*p) {
        if (strncmp(p, "Anonymous:", 10) == 0) {
            p += 10;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t kb = 0;
            while (*p >= '0' && *p <= '9')
                kb = kb * 10 + (size_t)(*p++ - '0');
            return kb * 1024;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
#elif defined(__APPLE__)
    task_basic_info_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
#elif defined(_WIN32)
    typedef BOOL (WINAPI *GetProcessMemoryInfo_t)(HANDLE, BS_PROCESS_MEMORY_COUNTERS*, DWORD);
    static GetProcessMemoryInfo_t func = NULL;
    static bool initialized = false;

    if (!initialized) {
        initialized = true;
        HMODULE dll = LoadLibrary("psapi.dll");
        if (dll) {
            FARPROC p = GetProcAddress(dll, "GetProcessMemoryInfo");
            memcpy(&func, &p, sizeof(func));
        }
    }

    if (func) {
        BS_PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof(pmc);
        if (func(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return pmc.WorkingSetSize;
    }
#endif
    return 0;
}

#ifdef USE_GLAD
static bool platformInitGlad(void) {
    glGetString = (PFNGLGETSTRINGPROC)platformGetProcAddress("glGetString");
    if (!glGetString)
        return 0;

    logInfo("OpenGL Version: %s\n", (const char*)glGetString(GL_VERSION));
    GLVer ver = GLCommon_getGLVersion();

    if (ver.isGLES) {
        if (!gladLoadGLES2Loader(platformGetProcAddress))
            return false;
    } else {
        if (!gladLoadGLLoader(platformGetProcAddress))
            return false;
    }
    return true;
}
#endif

#if (defined(ENABLE_MODERN_GL) || defined(ENABLE_LEGACY_GL)) && !defined(NDEBUG) && !defined(PLATFORM_VITA)
#define USE_OPENGL_DEBUG
static void APIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, MAYBE_UNUSED GLsizei length, const GLchar* message, MAYBE_UNUSED const void* userParam) {
    const char* sourceStr;
    switch (source) {
        case GL_DEBUG_SOURCE_API: sourceStr = "API"; break;
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM: sourceStr = "Window System"; break;
        case GL_DEBUG_SOURCE_SHADER_COMPILER: sourceStr = "Shader Compiler"; break;
        case GL_DEBUG_SOURCE_THIRD_PARTY: sourceStr = "Third Party"; break;
        case GL_DEBUG_SOURCE_APPLICATION: sourceStr = "Application"; break;
        case GL_DEBUG_SOURCE_OTHER: sourceStr = "Other"; break;
        default: sourceStr = "Unknown"; break;
    }

    const char* typeStr;
    switch (type) {
        case GL_DEBUG_TYPE_ERROR: typeStr = "Error"; break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeStr = "Deprecated Behaviour"; break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: typeStr = "Undefined Behaviour"; break;
        case GL_DEBUG_TYPE_PORTABILITY: typeStr = "Portability"; break;
        case GL_DEBUG_TYPE_PERFORMANCE: typeStr = "Performance"; break;
        case GL_DEBUG_TYPE_MARKER: typeStr = "Marker"; break;
        case GL_DEBUG_TYPE_PUSH_GROUP: typeStr = "Push Group"; break;
        case GL_DEBUG_TYPE_POP_GROUP: typeStr = "Pop Group"; break;
        case GL_DEBUG_TYPE_OTHER: typeStr = "Other"; break;
        default: typeStr = "Unknown"; break;
    }

    const char* severityStr;
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH: severityStr = "High"; break;
        case GL_DEBUG_SEVERITY_MEDIUM: severityStr = "Medium"; break;
        case GL_DEBUG_SEVERITY_LOW: severityStr = "Low"; break;
        case GL_DEBUG_SEVERITY_NOTIFICATION: severityStr = "Notification"; break;
        default: severityStr = "Unknown"; break;
    }

    logInfo("[OpenGL %s] id=%u Type: %s; Severity: %s; Message: %.*s\n", sourceStr, id, typeStr, severityStr, (int) length, message);
}

static void installGLDebugCallback(void) {
    if (glDebugMessageCallback && glDebugMessageControl) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugCallback, NULL);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        return;
    }

    if (glDebugMessageCallbackKHR && glDebugMessageControlKHR) {
        glEnable(GL_DEBUG_OUTPUT_KHR);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
        glDebugMessageCallbackKHR(glDebugCallback, NULL);
        glDebugMessageControlKHR(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        return;
    }

    if (glDebugMessageCallbackARB && glDebugMessageControlARB) {
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
        glDebugMessageCallbackARB(glDebugCallback, NULL);
        glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        return;
    }
}
#endif

// Resolves the window size for the specified operating system.
// The "--window-size" argument takes precedence over the default resolution for each platform.
static void resolveWindowSize(const CommandLineArgs* args, uint32_t gen8Width, uint32_t gen8Height, int32_t* outW, int32_t* outH) {
    if (args->windowWidth > 0 && args->windowHeight > 0) {
        *outW = args->windowWidth;
        *outH = args->windowHeight;
        return;
    }

    switch (args->osType) {
        case OS_PS4:
        case OS_XBOXONE:
        case OS_PS3:
        case OS_XBOX360:
            *outW = 1920;
            *outH = 1080;
            break;
        case OS_SWITCH:
            *outW = 1280;
            *outH = 720;
            break;
        case OS_PSVITA:
            *outW = 960;
            *outH = 544;
            break;
        default:
            *outW = (int32_t) gen8Width;
            *outH = (int32_t) gen8Height;
            break;
    }

    // Widescreen hack handling to grow the window size to match
    if (args->widescreenAspect > 0.0f && *outW > 0 && *outH > 0) {
        float nativeAspect = (float) *outW / (float) *outH;
        if (args->widescreenAspect > nativeAspect) {
            int widened = (int) ((float) *outH * args->widescreenAspect + 0.5f);
            if (widened > *outW) *outW = widened;
        } else if (args->widescreenAspect < nativeAspect) {
            int heightened = (int) ((float) *outW / args->widescreenAspect + 0.5f);
            if (heightened > *outH) *outH = heightened;
        }
    }
}

// Extracts the Runner arguments from a string, returning the values on stb_ds array
// The "Runner arguments" is used for the "--game-args" and for the game_change GML function
// Returns the modified array
char** extractRunnerArguments(char* rawArguments) {
    // The "saveptr" is used for strtok_r to store its state
    // So it is thread safe™
    char *saveptr;
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

// ===[ SCREENSHOT ]===
// Reads the contents of an FBO (use 0 for the default framebuffer) into a PNG file.
// If forceOpaque is true, the alpha channel is overwritten with 255, fixing any clobbering done by blending modes.
#ifdef ENABLE_SCREENSHOTS
// When flipY is true, the image will be flipped vertically.
static void writeFramebufferAsPng(GLuint fbo, int width, int height, const char* filename, const char* logPrefix, bool forceOpaque, bool flipY) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    int stride = width * 4;
    unsigned char* pixels = (unsigned char *)safeMalloc(stride * height);
    if (pixels == nullptr) {
        logWarn("Failed to allocate memory for %s (%dx%d)\n", logPrefix, width, height);
        return;
    }

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (forceOpaque) {
        int totalPixels = width * height;
        repeat(totalPixels, i) pixels[i * 4 + 3] = 255;
    }

    if (flipY) {
        // Use stb's negative stride trick: point to the last row and use a negative stride to flip vertically.
        unsigned char* lastRow = pixels + (height - 1) * stride;
        stbi_write_png(filename, width, height, 4, lastRow, -stride);
    } else {
        stbi_write_png(filename, width, height, 4, pixels, stride);
    }

    free(pixels);
    logInfo("%s: %s (%dx%d)\n", logPrefix, filename, width, height);
}

static void captureScreenshot(GLuint fbo, const char* filenamePattern, int frameNumber, int width, int height, bool flipY) {
    char filename[512];
    snprintf(filename, sizeof(filename), filenamePattern, frameNumber);
    writeFramebufferAsPng(fbo, width, height, filename, "Screenshot saved", true, flipY);
}

// Dumps every live surface in the GL renderer as a PNG.
// Filename pattern takes two %d slots: frame number, then surface ID.
static void dumpAllSurfaces(GLRenderer* gl, const char* filenamePattern, int frameNumber) {
    repeat(gl->surfaceCount, surfaceId) {
        if (gl->surfaces[surfaceId] == 0)
            continue;

        int width = gl->surfaceWidth[surfaceId];
        int height = gl->surfaceHeight[surfaceId];
        if (0 >= width || 0 >= height) continue;

        char filename[512];
        snprintf(filename, sizeof(filename), filenamePattern, frameNumber, (int) surfaceId);
        writeFramebufferAsPng(gl->surfaces[surfaceId], width, height, filename, "Surface dump", false, false);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
}
#endif

// ===[ KEYBOARD INPUT ]===

InputRecording* globalInputRecording = nullptr;

#if defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define BUTTERSCOTCH_HAS_ASAN 1
    #endif
#endif
#if defined(__SANITIZE_ADDRESS__)
    #define BUTTERSCOTCH_HAS_ASAN 1
#endif

#if BUTTERSCOTCH_HAS_ASAN
void __asan_set_death_callback(void (*callback)(void));
#endif

static volatile sig_atomic_t crashSaveInProgress = 0;

static void saveRecordingOnCrash(void) {
    if (crashSaveInProgress) return;
    crashSaveInProgress = 1;
    if (globalInputRecording != nullptr && globalInputRecording->isRecording) {
        InputRecording_save(globalInputRecording);
    }
}

static void crashSignalHandler(int sig) {
    saveRecordingOnCrash();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandlers(void) {
#if BUTTERSCOTCH_HAS_ASAN
    __asan_set_death_callback(saveRecordingOnCrash);
#endif
    signal(SIGSEGV, crashSignalHandler);
    signal(SIGABRT, crashSignalHandler);
#ifdef SIGBUS
    signal(SIGBUS,  crashSignalHandler);
#endif
    signal(SIGFPE,  crashSignalHandler);
    signal(SIGILL,  crashSignalHandler);
}

void saveInputRecording() {
    // Save input recording if active, then free
    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) {
            InputRecording_save(globalInputRecording);
        }
        InputRecording_free(globalInputRecording);
        globalInputRecording = nullptr;
    }
}

#if !defined(_WIN32) && !defined(PLATFORM_VITA) && !defined(__SWITCH__)
#define USE_CRASH_SIGNAL_HANDLER
typedef struct { int key; struct sigaction value; } PreviousSignalActionEntry;
static PreviousSignalActionEntry* previousSignalActions = nullptr;

static void onCrashSignal(int sig) {
    saveInputRecording();
    // Restore the previous handler (ASAN) and re-raise so it can report the fault
    ssize_t idx = hmgeti(previousSignalActions, sig);
    sigaction(sig, &previousSignalActions[idx].value, nullptr);
    raise(sig);
}
#endif

char* collapseNewlines(const char *input) {
    if (input == nullptr) {
        return nullptr;
    }

    size_t len = strlen(input);
    char *result = (char *)malloc(len + 1);
    if (result == nullptr) {
        return nullptr;
    }

    size_t j = 0;
    bool isNewline = false;
    repeat(len, i) {
        if (input[i] == '\n' || input[i] == '\r') {
            if (isNewline)
                continue;

            isNewline = true;
            result[j++] = '\n';
            continue;
        } else {
            isNewline = false;
        }
        result[j++] = input[i];
    }
    result[j] = '\0';

    return result;
}

static void PreProcessedStuff_free(void) {
#ifdef PLATFORM_VITA
    VitaTextures_Free();
#endif
}

// ===[ MAIN ]===
int loop(CommandLineArgs args, const char *argv0) {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    char* currentDataWinPath = safeStrdup(args.dataWinPath);
    char** currentGameArgs = args.gameArgs;
    repeat(arrlen(args.gameArgs), i) {
        arrput(currentGameArgs, args.gameArgs[i]);
    }
    // The first argument will ALWAYS be the argv[0]
    arrins(currentGameArgs, 0, safeStrdup(argv0 != nullptr ? argv0 : ""));

    bool platformInitialized = false;
    int32_t inputFrameCount = 0;

    bool fastForwardActive = false;
    bool fastForwardTabPrev = false;
    while (true) {
        logInfo("Loading %s...\n", args.dataWinPath);

        DataWinParserOptions options = {0};
        options.parseGen8 = true;
        options.parseOptn = true;
        options.parseLang = true;
        options.parseExtn = true;
        options.parseSond = true;
        options.parseAgrp = true;
        options.parseSprt = true;
        options.parseBgnd = true;
        options.parsePath = true;
        options.parseScpt = true;
        options.parseGlob = true;
        options.parseShdr = true;
        options.parseFont = true;
        options.parseTmln = true;
        options.parseObjt = true;
        options.parseRoom = true;
        options.parseTpag = true;
        options.parseCode = true;
        options.parseVari = true;
        options.parseFunc = true;
        options.parseStrg = true;
        options.parseTxtr = true;
#ifdef PLATFORM_VITA
        do {
            char *lastSlash = strrchr(args.dataWinPath, '/');
            if (!lastSlash) {
                lastSlash = strrchr(args.dataWinPath, ':');
                if (!lastSlash) /* should be impossible if dataWinPath is valid */
                    break;
            }
            size_t texBinPathSize = lastSlash - args.dataWinPath + 1;
            const char *texBinName = "textures.bin";
            size_t texBinNameSize = strlen(texBinName) + 1;
            char *texBinPath = (char *)safeMalloc(texBinPathSize + texBinNameSize);
            memcpy(texBinPath, args.dataWinPath, texBinPathSize);
            memcpy(texBinPath + texBinPathSize, texBinName, texBinNameSize);
            FILE *texBinFile = fopen(texBinPath, "rb");
            free(texBinPath);
            if (!texBinFile)
                break;
            if (!VitaTextures_Init(texBinFile)) {
                logWarn("textures.bin found but failed to load!\n");
                break;
            }
            options.parseTxtr = false;
        } while(0);
#endif
#if defined(USE_MINIAUDIO) || defined(USE_OPENAL)
        if (!args.headless)
            options.parseAudo = true;
#endif
        options.skipLoadingPreciseMasksForNonPreciseSprites = true;
        options.loadType = args.loadType;
        options.lazyLoadRooms = args.lazyRooms;
        options.lazyLoadTextures = args.lazyTextures;
        options.lazyLoadAudio = args.lazyAudio;
        options.eagerlyLoadedRooms = args.eagerRooms;
        DataWin* dataWin = DataWin_parse(currentDataWinPath, options);

        Gen8* gen8 = &dataWin->gen8;
        logInfo("Loaded \"%s\" (%d) successfully! [WAD Version %u / GameMaker version %u.%u.%u.%u]\n", gen8->name, gen8->gameID, gen8->wadVersion, dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build);

#ifdef HAVE_MALLINFO2
        {
            struct mallinfo2 mi = mallinfo2();
            logInfo("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
        }
#endif

        // Build window title
        char windowTitle[256];
        snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

        // Initialize VM
        VMContext* vm = VM_create(dataWin);

        Profiler_setEnabled(&vm->profiler, args.profilerFramesBetween > 0);
#ifdef ENABLE_VM_OPCODE_PROFILER
        vm->opcodeProfilerEnabled = args.opcodeProfiler;
        if (vm->opcodeProfilerEnabled) {
            vm->opcodeVariantCounts = (uint64_t *)safeCalloc(256 * 256, sizeof(uint64_t));
            vm->opcodeRValueTypeCounts = (uint64_t *)safeCalloc(256 * 256, sizeof(uint64_t));
        }
#endif

        if (args.hasSeed)
            vm->hasFixedSeed = true;

        if (args.printRooms) {
            // Under --lazy-rooms we load each room for display and then free it again so the dump
            // reflects what each room contains without keeping all of them resident simultaneously.
            forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
                if (!room->present) {
                    logInfo("[%d] <absent>\n", (int)idx);
                    continue;
                }
                bool loadedHere = false;
                if (!room->payloadLoaded) {
                    DataWin_loadRoomPayload(dataWin, (int32_t) idx);
                    loadedHere = true;
                }

                logInfo("[%d] %s ()\n", (int)idx, room->name);

                forEachIndexed(RoomGameObject, roomGameObject, idx2, room->gameObjects, room->gameObjectCount) {
                    if (roomGameObject->objectDefinition < 0 || (uint32_t) roomGameObject->objectDefinition >= dataWin->objt.count) {
                       logInfo("  [%d] <no object> (x=%d,y=%d)\n", (int)idx2, roomGameObject->x, roomGameObject->y);
                        continue;
                    }
                    GameObject* gameObject = &dataWin->objt.objects[roomGameObject->objectDefinition];
                    logInfo(
                        "  [%d] %s (x=%d,y=%d,persistent=%d,solid=%d,spriteId=%d,preCreateCode=%d,creationCode=%d)\n",
                        (int)idx2,
                        gameObject->name,
                        roomGameObject->x,
                        roomGameObject->y,
                        gameObject->persistent,
                        gameObject->solid,
                        gameObject->spriteId,
                        roomGameObject->preCreateCode,
                        roomGameObject->creationCode
                    );
                }

                if (loadedHere && !room->eagerlyLoaded) {
                    DataWin_freeRoomPayload(room);
                }
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printObjects) {
            forEachIndexed(GameObject, obj, idx, dataWin->objt.objects, dataWin->objt.count) {
                uint32_t totalEvents = 0;
                repeat(OBJT_EVENT_TYPE_COUNT, e) {
                    totalEvents += obj->eventLists[e].eventCount;
                }
                logInfo("[%u] %s:\n", (unsigned int)idx, obj->name);
                if (obj->parentId >= 0 && (uint32_t) obj->parentId < dataWin->objt.count) {
                    logInfo("  Parent: %s (%d)\n", dataWin->objt.objects[obj->parentId].name, obj->parentId);
                } else {
                    logInfo("  Parent: none\n");
                }
                if (obj->spriteId >= 0 && (uint32_t) obj->spriteId < dataWin->sprt.count) {
                    logInfo("  Sprite: %s (%d)\n", dataWin->sprt.sprites[obj->spriteId].name, obj->spriteId);
                } else {
                    logInfo("  Sprite: none\n");
                }
                logInfo("  Solid: %d\n", obj->solid);
                logInfo("  Persistent: %d\n", obj->persistent);
                logInfo("  Visible: %d\n", obj->visible);
                logInfo("  Depth: %d\n", obj->depth);
                logInfo("  Events (%u):\n", totalEvents);
                {
                repeat(OBJT_EVENT_TYPE_COUNT, e) {
                    ObjectEventList* list = &obj->eventLists[e];
                    repeat(list->eventCount, eIdx) {
                        ObjectEvent* event = &list->events[eIdx];
                        const char* eventName = Runner_getEventName((int32_t) e, (int32_t) event->eventSubtype);
                        int32_t codeId = -1;
                        if (event->actionCount > 0) codeId = event->actions[0].codeId;
                        logInfo("    %s:\n", eventName);
                        logInfo("      Sub Type: %u\n", event->eventSubtype);
                        logInfo("      Code ID: %d\n", codeId);
                        logInfo("      Actions: %u\n", event->actionCount);
                    }
                }
                }
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printShaders) {
            forEachIndexed(Shader, shader, idx, dataWin->shdr.shaders, dataWin->shdr.count) {
                logInfo("[%u] %s:\n", (unsigned int)idx, shader->name);
                logInfo("GLSL Vertex Shader:\n");
                char* glslVertex = collapseNewlines(shader->glsl_Vertex);
                logInfo("%s\n", glslVertex);
                free(glslVertex);

                logInfo("GLSL Fragment Shader:\n");
                char* glslFragment = collapseNewlines(shader->glsl_Fragment);
                logInfo("%s\n", glslFragment);
                free(glslFragment);

                logInfo("GLSL ES Vertex Shader:\n");
                char* glslESVertex = collapseNewlines(shader->glslES_Vertex);
                logInfo("%s\n", glslESVertex);
                free(glslESVertex);

                logInfo("GLSL ES Fragment Shader:\n");
                char* glslESFragment = collapseNewlines(shader->glslES_Fragment);
                logInfo("%s\n", glslESFragment);
                free(glslESFragment);
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printDeclaredFunctions) {
            repeat(hmlen(vm->codeIndexByName), i) {
                logInfo("[%d] %s\n", vm->codeIndexByName[i].value, vm->codeIndexByName[i].key);
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printUnknownFunctions) {
            uint32_t unimplementedCount = 0;
            logInfo("Unknown Functions:\n");
            repeat(dataWin->func.functionCount, i) {
                const char* name = dataWin->func.functions[i].name;
                if (name == nullptr)
                    continue;

                // Implemented as a user script/code entry?
                if (shgeti(vm->codeIndexByName, (char*) name) >= 0)
                    continue;

                // Implemented as a registered builtin?
                if (VM_findBuiltin(vm, name) != nullptr)
                    continue;

                logInfo("- %s\n", name);
                unimplementedCount++;
            }

            if (unimplementedCount == 0) {
                logInfo("All %u referenced functions are implemented! :3\n", dataWin->func.functionCount);
            } else {
                logInfo("%u unknown function(s) out of %u referenced\n", unimplementedCount, dataWin->func.functionCount);
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (shlen(args.disassemble) > 0) {
            VM_buildCrossReferences(vm);
            if (shgeti(args.disassemble, "*") >= 0) {
                repeat(dataWin->code.count, i) {
                    VM_disassemble(vm, (int32_t) i);
                }
            } else {
                for (ptrdiff_t i = 0; shlen(args.disassemble) > i; i++) {
                    const char* name = args.disassemble[i].key;
                    ptrdiff_t idx = shgeti(vm->codeIndexByName, (char*) name);
                    if (idx >= 0) {
                        VM_disassemble(vm, vm->codeIndexByName[idx].value);
                    } else {
                        logWarn("Script '%s' not found in funcMap\n", name);
                    }
                }
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        // Initialize the file system
        char* dataWinDir = nullptr;
        {
            const char* lastSlash = strrchr(args.dataWinPath, '/');
            const char* lastBackslash = strrchr(args.dataWinPath, '\\');
            if (lastBackslash != nullptr && (lastSlash == nullptr || lastBackslash > lastSlash))
                lastSlash = lastBackslash;
            if (lastSlash != nullptr) {
                size_t len = (size_t) (lastSlash - args.dataWinPath + 1);
                dataWinDir = (char *)safeMalloc(len + 1);
                memcpy(dataWinDir, args.dataWinPath, len);
                dataWinDir[len] = '\0';
            } else {
                dataWinDir = safeStrdup("./");
            }
        }
        const char* savePath = args.saveFolder != nullptr ? args.saveFolder : dataWinDir;
        OverlayFileSystem* overlayFs = OverlayFileSystem_create(dataWinDir, savePath);
        free(dataWinDir);

        gfx = args.renderer;

#ifndef ENABLE_LEGACY_GL
        if (gfx == LEGACY_GL) {
            logError("The legacy gl renderer is not available in this build!\n");
            return 0;
        }
#endif
#ifndef ENABLE_MODERN_GL
        if (gfx == MODERN_GL) {
            logError("The modern gl renderer is not available in this build!\n");
            return 0;
        }
#endif
#ifndef ENABLE_SW_RENDERER
        if (gfx == SOFTWARE) {
            logError("The software renderer is not available in this build!\n");
            return 0;
        }
#endif
#ifndef ENABLE_NOOP_RENDERER
        if (gfx == NOOP) {
            logError("The noop renderer is not available in this build!\n");
            return 0;
        }
#endif

#ifdef ENABLE_SCREENSHOTS
        if (gfx != MODERN_GL && hmlen(args.screenshotSurfacesFrames)) {
            logError("You can only use --screenshot-surfaces with the modern gl renderer!\n");
            return 0;
        }
#endif


        int32_t windowW, windowH;
        resolveWindowSize(&args, gen8->defaultWindowWidth, gen8->defaultWindowHeight, &windowW, &windowH);

        if (!platformInitialized) {
            if (!platformInit(windowW, windowH, windowTitle, args.headless)) {
                DataWin_free(dataWin);
                PreProcessedStuff_free();
                return 1;
            }

#ifdef USE_GLAD
#if defined(USE_GLFW3) || defined(USE_GLFW2)
            if (gfx == LEGACY_GL || gfx == MODERN_GL || gfx == SOFTWARE) {
#else
            if (gfx == LEGACY_GL || gfx == MODERN_GL) {
#endif
                if (!platformInitGlad()) {
                    logError("Failed to initialize GLAD\n");
                    platformExit();
                    DataWin_free(dataWin);
                    PreProcessedStuff_free();
                    return 1;
                }
            }
#endif

            // Install the OpenGL debug message callback
#ifdef USE_OPENGL_DEBUG
            if (gfx == MODERN_GL)
                installGLDebugCallback();
#endif

            platformInitialized = true;
        } else {
            // game_change path: reuse the existing window/GL context, just retitle and resize for the new game.
            platformSetWindowTitle(gen8->displayName);
            platformSetWindowSize(windowW, windowH);
        }

        // Initialize the renderer
        // NOTE: headless mode keeps rendering active (hidden window + normal renderer).
        // NOOP is a separate renderer that stubs all draw calls.
        Renderer* renderer = nullptr;
#ifdef ENABLE_SW_RENDERER
        if (gfx == SOFTWARE)
            renderer = SWRenderer_create();
#endif
#ifdef ENABLE_NOOP_RENDERER
        if (gfx == NOOP) {
            renderer = NoopRenderer_create();
        }
#endif
#ifdef ENABLE_LEGACY_GL
        if (gfx == LEGACY_GL) {
            renderer = GLLegacyRenderer_create();
            static GLuint hostfb = 0;
            hostFramebuffer = &hostfb;
        }
#endif
#ifdef ENABLE_MODERN_GL
        if (gfx == MODERN_GL) {
            renderer = GLRenderer_create();
            hostFramebuffer = &((GLRenderer *)renderer)->hostFramebuffer;
        }
#endif
        if (!renderer) {
            logError("Failed to initialize a renderer\n");
            platformExit();
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 1;
        }

        // Initialize the audio system
        AudioSystem* audioSystem = nullptr;
        if (args.headless) {
            audioSystem = (AudioSystem*) NoopAudioSystem_create();
        } else {
#if defined(USE_OPENAL)
            audioSystem = (AudioSystem*) AlAudioSystem_create();
#elif defined(USE_MINIAUDIO)
            audioSystem = (AudioSystem*) MaAudioSystem_create(dataWin);
#else
            audioSystem = (AudioSystem*) NoopAudioSystem_create();
#endif
        }

        // Initialize the runner
        Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) overlayFs, audioSystem, args.seed);

        if (!args.lazyTextures) {
            repeat(runner->dataWin->txtr.count, i) {
#ifdef ENABLE_MODERN_GL
                if (gfx == MODERN_GL)
                    GLRenderer_ensureTextureLoaded((GLRenderer*) renderer, (int32_t) i);
#endif

#ifdef ENABLE_LEGACY_GL
                if (gfx == LEGACY_GL)
                    GLLegacyRenderer_ensureTextureLoaded((GLLegacyRenderer*) renderer, (int32_t) i);
#endif
            }
        }
        runner->debugMode = args.debug;
        runner->osType = args.osType;
        runner->setWindowSize = platformSetWindowSize;
        runner->getWindowSize = platformGetWindowSize;
        runner->setWindowTitle = platformSetWindowTitle;
        Runner_setGameArgs(runner, currentGameArgs, (int32_t) arrlen(currentGameArgs));
        platformInitFunctions(runner);

        // Set up input recording/playback (both can be active: playback then continue recording)
        if (args.playbackInputsPath != nullptr) {
            globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
        } else if (args.recordInputsPath != nullptr) {
            globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
        }
        if (globalInputRecording != nullptr) {
            globalInputRecording->filterDebugKeys = args.debug;
            installCrashHandlers();
        }
#ifdef ENABLE_VM_TRACING
        shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
        shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
        shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
        shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
        shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
        shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
        shcopyFromTo(args.collisionsToBeTraced, runner->vmContext->collisionsToBeTraced);
        shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
        shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);
        shcopyFromTo(args.tilesToBeTraced, runner->vmContext->tilesToBeTraced);
        runner->vmContext->traceBytecodeAfterFrame = args.traceBytecodeAfterFrame;
#endif
#ifdef ENABLE_VM_STUB_LOGS
        runner->vmContext->alwaysLogStubbedFunctions = args.alwaysLogStubbedFunctions;
#endif
        runner->vmContext->alwaysLogUnknownFunctions = args.alwaysLogUnknownFunctions;
        runner->vmContext->traceEventInherited = args.traceEventInherited;

#ifdef USE_CRASH_SIGNAL_HANDLER
        struct sigaction sa = {0};
        sa.sa_handler = onCrashSignal;
        sigemptyset(&sa.sa_mask);
        struct sigaction prev;
        sigaction(SIGABRT, &sa, &prev);
        PreviousSignalActionEntry p;
        p.key = SIGABRT;
        p.value = prev;
        hmputs(previousSignalActions, p);
        sigaction(SIGSEGV, &sa, &prev);
        PreviousSignalActionEntry p2;
        p.key = SIGSEGV;
        p.value = prev;
        hmputs(previousSignalActions, p2);
#endif

        // Initialize the first room and fire Game Start / Room Start events
        Runner_initFirstRoom(runner);

        // Main loop
        bool debugShowCollisionMasks = false;
        bool freeCamActive = false;
        bool actuallyShuttingDown = false;
        uint64_t lastFrameTime = nowNanos();
        uint64_t lastFrameStartTime = lastFrameTime; // for delta_time
        bool shouldWindowClose = false;
        while (true) {
            if (runner->shouldExit || shouldWindowClose) {
                actuallyShuttingDown = true;
                break;
            }

            if (runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr) {
                // Break from the game loop, we'll handle this later
                break;
            }

            uint64_t frameStartNow = nowNanos();
            runner->deltaTime = (int64_t)(frameStartNow - lastFrameStartTime) / 1000.0;
            lastFrameStartTime = frameStartNow;

            // Clear last frame's pressed/released state, then poll new input events
            RunnerKeyboard_beginFrame(runner->keyboard);
            RunnerGamepad_beginFrame(runner->gamepads);
            RunnerMouse_beginFrame(runner->mouse);
            if (platformHandleEvents()) {
                shouldWindowClose = true;
                continue;
            }
            
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F8)) {
                bool isPaused = Runner_isPaused(runner);
                Runner_setPaused(runner, !isPaused);
            }
            
            // Run the game step if the game is paused
            bool shouldStep = !runner->paused;
            if (runner->debugMode && runner->paused) {
                shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
                if (shouldStep) logDebug("Frame advance (frame %d)\n", runner->frameCount);
            }

            uint64_t frameStartTime = 0;

            if (shouldStep) {
                if (args.traceFrames) {
                    frameStartTime = nowNanos();
                    logInfo("Frame %d (Start)\n", runner->frameCount);
                }

                // Process input recording/playback (must happen after platformHandleEvents, before Runner_step)
                InputRecording_processFrame(globalInputRecording, runner->keyboard, inputFrameCount++);

                // Go to next room
                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
                    DataWin* dw = runner->dataWin;
                    if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                        int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                        runner->pendingRoom = nextIdx;
                        runner->audioSystem->vtable->stopAll(runner->audioSystem);
                        logDebug("Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                    }
                }

                // Go to previous room
                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
                    DataWin* dw = runner->dataWin;
                    if (runner->currentRoomOrderPosition > 0) {
                        int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                        runner->pendingRoom = prevIdx;
                        runner->audioSystem->vtable->stopAll(runner->audioSystem);
                        logDebug("Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                    }
                }

                // Dump runner state to console
                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) {
                    logDebug("Dumping runner state at frame %d\n", runner->frameCount);
                    Runner_dumpState(runner);
                }

                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) {
                    logDebug("Dumping runner state at frame %d\n", runner->frameCount);
                    char* json = Runner_dumpStateJson(runner);

                    if (args.dumpJsonFilePattern != nullptr) {
                        char filename[512];
                        snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                        FILE* f = fopen(filename, "wb");
                        if (f != nullptr) {
                            fwrite(json, 1, strlen(json), f);
                            fputc('\n', f);
                            fclose(f);
                            logInfo("JSON dump saved: %s\n", filename);
                        } else {
                            logWarn("Could not write JSON dump to '%s'\n", filename);
                        }
                    } else {
                        logInfo("%s\n", json);
                    }

                    free(json);
                }

                // Toggle the collision mask debug overlay
                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F2)) {
                    debugShowCollisionMasks = !debugShowCollisionMasks;
                    logDebug("Collision mask overlay %s!\n", debugShowCollisionMasks ? "enabled" : "disabled");
                }

                // Enable free cam
                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F3)) {
                    runner->freeCamPanX = 0.0f;
                    runner->freeCamPanY = 0.0f;
                    runner->freeCamZoom = 1.0f;

                    freeCamActive = !freeCamActive;
                    logDebug("Free cam %s!\n", freeCamActive ? "enabled" : "disabled");
                }

                if (freeCamActive) {
                    if (RunnerKeyboard_check(runner->keyboard, VK_UP)) {
                        runner->freeCamPanY -= (float) (0.000005f * runner->deltaTime);
                    }

                    if (RunnerKeyboard_check(runner->keyboard, VK_DOWN)) {
                        runner->freeCamPanY += (float) (0.000005f * runner->deltaTime);
                    }

                    if (RunnerKeyboard_check(runner->keyboard, VK_LEFT)) {
                        runner->freeCamPanX -= (float) (0.000005f * runner->deltaTime);
                    }

                    if (RunnerKeyboard_check(runner->keyboard, VK_RIGHT)) {
                        runner->freeCamPanX += (float) (0.000005f * runner->deltaTime);
                    }
                }

                // Reset global interact state because I HATE when I get stuck while moving through rooms
                if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                    int32_t interactVarId = shget(runner->vmContext->varNameMap, "interact");

                    Instance_setSelfVar(runner->vmContext->globalScopeInstance, interactVarId, RValue_makeInt32(0));
                    logInfo("Changed global.interact [%d] value!\n", interactVarId);
                }

                bool currentKeyDown[GML_KEY_COUNT];
                bool currentKeyPressed[GML_KEY_COUNT];
                bool currentKeyReleased[GML_KEY_COUNT];

                if (freeCamActive) {
                    // THIS IS A HACK!! We don't want to pass keys to the runner, but we DO want to keep it so we can hold the arrow keys to move the camera
                    memcpy(currentKeyDown, runner->keyboard->keyDown, sizeof(runner->keyboard->keyDown));
                    memcpy(currentKeyPressed, runner->keyboard->keyPressed, sizeof(runner->keyboard->keyPressed));
                    memcpy(currentKeyReleased, runner->keyboard->keyReleased, sizeof(runner->keyboard->keyReleased));

                    memset(runner->keyboard->keyDown, 0, sizeof(runner->keyboard->keyDown));
                    memset(runner->keyboard->keyPressed, 0, sizeof(runner->keyboard->keyPressed));
                    memset(runner->keyboard->keyReleased, 0, sizeof(runner->keyboard->keyReleased));
                }

                // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
                Runner_step(runner);

                if (freeCamActive) {
                    memcpy(runner->keyboard->keyDown, currentKeyDown, sizeof(runner->keyboard->keyDown));
                    memcpy(runner->keyboard->keyPressed, currentKeyPressed, sizeof(runner->keyboard->keyPressed));
                    memcpy(runner->keyboard->keyReleased, currentKeyReleased, sizeof(runner->keyboard->keyReleased));
                }

                if (args.profilerFramesBetween > 0 && runner->frameCount > 0 && runner->frameCount % args.profilerFramesBetween == 0) {
                    char* profilerReport = Profiler_createReport(vm->profiler, 20, args.profilerFramesBetween);
                    if (profilerReport != nullptr) {
                        logInfo("%s\n", profilerReport);
                        free(profilerReport);
                    }
                    Profiler_reset(vm->profiler);
                }

                // Update audio system (gain fading, cleanup ended sounds)
                float dt = (float) (runner->deltaTime / 1000000.0);
                if (0.0f > dt) dt = 0.0f;
                if (dt > 0.1f) dt = 0.1f; // cap delta to avoid huge fades on lag spikes
                runner->audioSystem->vtable->update(runner->audioSystem, dt);

                // Dump full runner state if this frame was requested
                if (hmget(args.dumpFrames, runner->frameCount)) {
                    Runner_dumpState(runner);
                }

                // Dump runner state as JSON if this frame was requested
                if (hmget(args.dumpJsonFrames, runner->frameCount)) {
                    char* json = Runner_dumpStateJson(runner);
                    if (args.dumpJsonFilePattern != nullptr) {
                        char filename[512];
                        snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                        FILE* f = fopen(filename, "wb");
                        if (f != nullptr) {
                            fwrite(json, 1, strlen(json), f);
                            fputc('\n', f);
                            fclose(f);
                            logInfo("JSON dump saved: %s\n", filename);
                        } else {
                            logWarn("Could not write JSON dump to '%s'\n", filename);
                        }
                    } else {
                        logInfo("%s\n", json);
                    }
                    free(json);
                }

                // Clear the default framebuffer (window background) to black
#ifdef ENABLE_SW_RENDERER
                if (gfx == SOFTWARE)
                    SWRenderer_clearFrameBuffer(renderer, 0);
#endif
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
                if (gfx == LEGACY_GL || gfx == MODERN_GL) {
                    glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
#endif

                // Query actual framebuffer size
                int32_t fbWidth, fbHeight;
                platformGetWindowSize(&fbWidth, &fbHeight);

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

                // Widescreen hack: render into a surface grown toward the requested aspect to fake a different aspect
                // ratio. The game's logical applicationWidth/Height is left untouched (so the reads above stay the real
                // size and this never compounds frame-to-frame); only the local gameW/gameH used for the projection/FBO
                // grow. A wider-than-native target grows width (reveal left/right); a taller one grows height (reveal
                // top/bottom). Runner_drawViews reads widescreenExtraWidth/Height to expand each view to match.
                runner->widescreenExtraWidth = 0;
                runner->widescreenExtraHeight = 0;
                if (args.widescreenAspect > 0.0f && runner->usingAppSurface && gameW > 0 && gameH > 0) {
                    float nativeAspect = (float) gameW / (float) gameH;
                    if (args.widescreenAspect > nativeAspect) {
                        int32_t targetW = (int32_t) ((float) gameH * args.widescreenAspect + 0.5f);
                        if (targetW > gameW) {
                            runner->widescreenExtraWidth = targetW - gameW;
                            gameW = targetW;
                        }
                    } else if (args.widescreenAspect < nativeAspect) {
                        int32_t targetH = (int32_t) ((float) gameW / args.widescreenAspect + 0.5f);
                        if (targetH > gameH) {
                            runner->widescreenExtraHeight = targetH - gameH;
                            gameH = targetH;
                        }
                    }
                }

                if (shouldStep) {
                    Runner_drawPre(runner, fbWidth, fbHeight);

                    // Calculate viewport (letterboxing) in screen coordinates for mouse mapping
                    int32_t winW, winH;
                    platformGetScaledWindowSize(&winW, &winH);

                    Runner_beginFrame(runner, gameW, gameH, winW, winH, fbWidth, fbHeight);

                    double mx, my;
                    platformGetMousePos(&mx, &my);
                    Runner_updateMousePosition(runner, winW, winH, mx, my);

                    Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
                    renderer->vtable->endFrameInit(renderer);
                    Runner_drawPost(runner, fbWidth, fbHeight);
                    renderer->vtable->endFrameEnd(renderer);
                    Runner_drawGUI(runner, fbWidth, fbHeight, gameW, gameH);
                }

#ifdef ENABLE_SCREENSHOTS
                // Capture screenshot if this frame matches a requested frame
                bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);

                if (shouldScreenshot || RunnerKeyboard_checkPressed(runner->keyboard, VK_F5)) {
                    captureScreenshot(0, args.screenshotPattern, runner->frameCount, fbWidth, fbHeight, true);
                    glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
                }

                // Dump all surfaces if this frame matches a requested frame
                bool shouldDumpSurfaces = hmget(args.screenshotSurfacesFrames, runner->frameCount);

                if (shouldDumpSurfaces || RunnerKeyboard_checkPressed(runner->keyboard, VK_F6)) {
                    GLRenderer* gl = (GLRenderer*) renderer;
                    dumpAllSurfaces(gl, args.screenshotSurfacesPattern, runner->frameCount);
                    glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
                }
#endif

                if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
                    logInfo("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
                    shouldWindowClose = true;
                }

                if (shouldStep && args.traceFrames) {
                    double frameElapsedMs = (int64_t)(nowNanos() - frameStartTime) / 1000000.0;
                    logInfo("Frame %d (End, %.2f ms)\n", runner->frameCount, frameElapsedMs);
                }

                // Only swap when there isn't a room change to match the original runner.
                if (runner->pendingRoom == -1)
                    platformSwapBuffers();
                Runner_handlePendingRoomChange(runner);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_BACKSPACE)) {
                size_t bytes_used = get_used_memory();
                if (bytes_used == 0)
                    logWarn("Unable to get memory usage\n");
                else
                    logInfo("Memory use right now: %zu bytes (%.1f MB)\n", bytes_used, bytes_used / 1024.0f / 1024.0f);
            }

            // Limit frame rate to room speed (skip in headless mode for max speed!!)
            if (!args.headless && runner->currentRoom->speed > 0) {
                bool fastForwardTabNow = RunnerKeyboard_checkPressed(runner->keyboard, VK_TAB);
                if (args.fastForwardSpeed > 0.0 && fastForwardTabNow && !fastForwardTabPrev) {
                    fastForwardActive = !fastForwardActive;
                    lastFrameTime = nowNanos();
                }
                fastForwardTabPrev = fastForwardTabNow;
                double effectiveSpeed = (args.fastForwardSpeed > 0.0 && fastForwardActive) ? args.fastForwardSpeed : args.speedMultiplier;
                uint64_t targetFrameTime = 1000000000 / (runner->currentRoom->speed * effectiveSpeed);
                uint64_t nextFrameTime = lastFrameTime + targetFrameTime;
                platformSleepUntil(nextFrameTime);
            }
            lastFrameTime = nowNanos();
        }

        saveInputRecording();

        // Snapshot any pending game_change request before we tear the runner down
        char* nextWorkingDirectory = runner->pendingWorkingDirectory;
        char* nextLaunchParameters = runner->pendingLaunchParameters;
        runner->pendingWorkingDirectory = nullptr;
        runner->pendingLaunchParameters = nullptr;

        // Cleanup
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = nullptr;
        renderer->vtable->destroy(renderer);

        // Keep the window + GL context alive across game_change so we don't spawn a new window
        if (actuallyShuttingDown) {
            platformExit();
            platformInitialized = false;
        }

        Runner_free(runner);
        OverlayFileSystem_destroy(overlayFs);
#ifdef ENABLE_VM_OPCODE_PROFILER
        VM_printOpcodeProfilerReport(vm);
#endif
        VM_free(vm);
        DataWin_free(dataWin);
        PreProcessedStuff_free();

        if (actuallyShuttingDown) {
            free(currentDataWinPath);
            repeat(arrlen(currentGameArgs), i) {
                free(currentGameArgs[i]);
            }
            arrfree(currentGameArgs);
            logInfo("Bye! :3\n");
#ifdef _WIN32
            timeEndPeriod(1);
#endif
            return 0;
        }

        // game_change was called, so we need to restart the runner with the new data.win and launch parameters
        if (nextWorkingDirectory != nullptr && nextLaunchParameters != nullptr) {
            char** newArguments = nullptr;
            newArguments = extractRunnerArguments(nextLaunchParameters);

            // Extract the data.win filename from "-game <file>" inside the new launch parameters
            char* dataWinFilename = nullptr;
            {
                // After extraction, we now need to figure out where is the "-game" argument
                size_t length = arrlen(newArguments);
                repeat(length, i) {
                    if (strcmp(newArguments[i], "-game") == 0) {
                        // So we already know that the data.win file will be the NEXT one
                        if (length - 1 == i)
                            break; // Where's the value?? Bailing...

                        dataWinFilename = safeStrdup(newArguments[i + 1]);
                        break;
                    }
                }
            }

            if (dataWinFilename == nullptr) {
                logError("Runner: Launch parameters '%s' did not contain a '-game <file>' entry! Shutting down...\n", nextLaunchParameters);
                free(nextWorkingDirectory);
                free(nextLaunchParameters);
                free(currentDataWinPath);
                repeat(arrlen(newArguments), i) {
                    free(newArguments[i]);
                }
                arrfree(newArguments);
                {
                repeat(arrlen(currentGameArgs), i) {
                    free(currentGameArgs[i]);
                }
                }
                arrfree(currentGameArgs);
                return 1;
            }

            // Get the parent directory of the main data.win file
            char* parentDir = safeStrdup(currentDataWinPath);
            {
                char* lastSlash = strrchr(parentDir, '/');
                char* lastBackslash = strrchr(parentDir, '\\');
                char* sep = (lastSlash > lastBackslash) ? lastSlash : lastBackslash;
                if (sep != nullptr) {
                    *sep = '\0';
                } else {
                    parentDir[0] = '.';
                    parentDir[1] = '\0';
                }
            }

            // The pendingWorkingDirectory contains a slash at the beginning of it (example: /chapter3)
            // The parentDir does NOT have a trailing slash, so we don't need to bother with it
            size_t newPathLen = strlen(parentDir) + strlen(nextWorkingDirectory) + 1 + strlen(dataWinFilename) + 1;
            char* newPath = (char *)safeMalloc(newPathLen);
            snprintf(newPath, newPathLen, "%s%s/%s", parentDir, nextWorkingDirectory, dataWinFilename);

            free(parentDir);
            free(currentDataWinPath);
            currentDataWinPath = newPath;
            args.dataWinPath = currentDataWinPath;

            // Rebuild the gameArgs
            // First, we'll remove ALL args except the first one (which is the argv[0])
            while (arrlen(currentGameArgs) > 1) {
                free(currentGameArgs[1]);
                arrdel(currentGameArgs, 1);
            }

            repeat(arrlen(newArguments), i) {
                arrput(currentGameArgs, newArguments[i]);
            }

            free(dataWinFilename);
            free(nextWorkingDirectory);
            free(nextLaunchParameters);
            arrfree(newArguments);
        }
    }
}

void freeCommandLineArgs(CommandLineArgs* args) {
#ifdef ENABLE_SCREENSHOTS
    hmfree(args->screenshotFrames);
    hmfree(args->screenshotSurfacesFrames);
#endif
    hmfree(args->dumpFrames);
    hmfree(args->dumpJsonFrames);
#ifdef ENABLE_VM_TRACING
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->collisionsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
    shfree(args->tilesToBeTraced);
#endif
    shfree(args->disassemble);
    repeat(arrlen(args->gameArgs), i) free(args->gameArgs[i]);
    arrfree(args->gameArgs);
}
