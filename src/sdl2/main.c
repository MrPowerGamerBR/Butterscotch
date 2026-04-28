#include "data_win.h"
#include "sdl2/gl_legacy_renderer.h"
#include "vm.h"

#include <glad/glad.h>
#include <SDL2/SDL.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "runner_keyboard.h"
#include "sdl2_gamepad.h"
#include "runner.h"
#include "input_recording.h"
#include "debug_overlay.h"
#include "gl_renderer.h"
#include "sdl2_file_system.h"
#include "ma_audio_system.h"
#include "noop_audio_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "utils.h"
#include "profiler.h"

// ===[ COMMAND LINE ARGUMENTS ]===
typedef struct {
    int key;
    // We need this dummy value, think that the ds_map is like a Java HashMap NOT a HashSet
    // (Which is funny, because in Java HashSets are backed by HashMaps lol)
    bool value;
} FrameSetEntry;

typedef struct {
    const char* dataWinPath;
    const char* screenshotPattern;
    FrameSetEntry* screenshotFrames;
    FrameSetEntry* dumpFrames;
    FrameSetEntry* dumpJsonFrames;
    const char* dumpJsonFilePattern;
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* disassemble;
    StringBooleanEntry* tilesToBeTraced;
    bool alwaysLogUnknownFunctions;
    bool alwaysLogStubbedFunctions;
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printDeclaredFunctions;
    int exitAtFrame;
    int traceBytecodeAfterFrame;
    double speedMultiplier;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char* recordInputsPath;
    const char* playbackInputsPath;
    const char* renderer;
    YoYoOperatingSystem osType;
    bool lazyRooms;
    StringBooleanEntry* eagerRooms; // stb_ds string-keyed set of room names
    int profilerFramesBetween; // 0 = disabled
} CommandLineArgs;

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

static bool parseOsTypeArg(const char* s, YoYoOperatingSystem* out) {
    forEach(const OsTypeNameEntry, entry, OS_TYPE_NAMES, OS_TYPE_NAMES_COUNT) {
        if (strcmp(s, entry->name) == 0) {
            *out = entry->value;
            return true;
        }
    }
    return false;
}

static void printOsTypeNames(FILE* out) {
    forEachIndexed(const OsTypeNameEntry, entry, i, OS_TYPE_NAMES, OS_TYPE_NAMES_COUNT) {
        fprintf(out, "%s%s", i > 0 ? ", " : "", entry->name);
    }
}

static void parseCommandLineArgs(CommandLineArgs* args, int argc, char* argv[]) {
    memset(args, 0, sizeof(CommandLineArgs));

    static struct option longOptions[] = {
        {"screenshot",          required_argument, nullptr, 's'},
        {"screenshot-at-frame", required_argument, nullptr, 'f'},
        {"headless",            no_argument,       nullptr, 'h'},
        {"print-rooms", no_argument,               nullptr, 'r'},
        {"print-declared-functions", no_argument,  nullptr, 'p'},
        {"trace-variable-reads", required_argument,  nullptr, 'R'},
        {"trace-variable-writes", required_argument, nullptr, 'W'},
        {"trace-function-calls", required_argument,         nullptr, 'c'},
        {"trace-alarms", required_argument,         nullptr, 'a'},
        {"trace-instance-lifecycles", required_argument,         nullptr, 'l'},
        {"trace-events", required_argument,         nullptr, 'e'},
        {"trace-event-inherited", no_argument, nullptr, 'E'},
        {"trace-tiles", required_argument, nullptr, 'T'},
        {"trace-opcodes", required_argument,       nullptr, 'o'},
        {"trace-stack", required_argument,         nullptr, 'S'},
        {"trace-frames", no_argument, nullptr, 'k'},
        {"always-log-unknown-functions", no_argument, nullptr, 'y'},
        {"always-log-stubbed-functions", no_argument, nullptr, 'Y'},
        {"exit-at-frame", required_argument, nullptr, 'x'},
        {"trace-bytecode-after-frame", required_argument, nullptr, 'F'},
        {"dump-frame", required_argument, nullptr, 'd'},
        {"dump-frame-json", required_argument, nullptr, 'j'},
        {"dump-frame-json-file", required_argument, nullptr, 'J'},
        {"speed", required_argument, nullptr, 'M'},
        {"seed", required_argument, nullptr, 'Z'},
        {"debug", no_argument, nullptr, 'D'},
        {"disassemble", required_argument, nullptr, 'A'},
        {"record-inputs", required_argument, nullptr, 'I'},
        {"playback-inputs", required_argument, nullptr, 'P'},
        {"renderer", required_argument, nullptr, 'g'},
        {"lazy-rooms", no_argument, nullptr, 'z'},
        {"eager-room", required_argument, nullptr, 'G'},
        {"os-type", required_argument, nullptr, 'O'},
        {"profiler", required_argument, nullptr, 'q'},
        {nullptr,               0,                 nullptr,  0 }
    };

    args->screenshotFrames = nullptr;
    args->exitAtFrame = -1;
    args->traceBytecodeAfterFrame = 0;
    args->speedMultiplier = 1.0;
    args->renderer = "gl";
    args->osType = OS_WINDOWS;
    args->profilerFramesBetween = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 's':
                args->screenshotPattern = optarg;
                break;
            case 'f': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s'\n", optarg);
                    exit(1);
                }

                hmput(args->screenshotFrames, (int) frame, true);
                break;
            }
            case 'h':
                args->headless = true;
                break;
            case 'r':
                args->printRooms = true;
                break;
            case 'p':
                args->printDeclaredFunctions = true;
                break;
            case 'R':
                shput(args->varReadsToBeTraced, optarg, true);
                break;
            case 'W':
                shput(args->varWritesToBeTraced, optarg, true);
                break;
            case 'c':
                shput(args->functionCallsToBeTraced, optarg, true);
                break;
            case 'a':
                shput(args->alarmsToBeTraced, optarg, true);
                break;
            case 'l':
                shput(args->instanceLifecyclesToBeTraced, optarg, true);
                break;
            case 'e':
                shput(args->eventsToBeTraced, optarg, true);
                break;
            case 'o':
                shput(args->opcodesToBeTraced, optarg, true);
                break;
            case 'S':
                shput(args->stackToBeTraced, optarg, true);
                break;
            case 'k':
                args->traceFrames = true;
                break;
            case 'y':
                args->alwaysLogUnknownFunctions = true;
                break;
            case 'Y':
                args->alwaysLogStubbedFunctions = true;
                break;
            case 'x': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --exit-at-frame\n", optarg);
                    exit(1);
                }
                args->exitAtFrame = (int) frame;
                break;
            }
            case 'F': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --trace-bytecode-after-frame\n", optarg);
                    exit(1);
                }
                args->traceBytecodeAfterFrame = (int) frame;
                break;
            }
            case 'd': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame\n", optarg);
                    exit(1);
                }
                hmput(args->dumpFrames, (int) frame, true);
                break;
            }
            case 'j': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame-json\n", optarg);
                    exit(1);
                }
                hmput(args->dumpJsonFrames, (int) frame, true);
                break;
            }
            case 'J':
                args->dumpJsonFilePattern = optarg;
                break;
            case 'M': {
                char* endPtr;
                double speed = strtod(optarg, &endPtr);
                if (*endPtr != '\0' || speed <= 0.0) {
                    fprintf(stderr, "Error: Invalid speed multiplier '%s' for --speed (must be > 0)\n", optarg);
                    exit(1);
                }
                args->speedMultiplier = speed;
                break;
            }
            case 'D':
                args->debug = true;
                break;
            case 'g':
                args->renderer = optarg;
                break;
            case 'z':
                args->lazyRooms = true;
                break;
            case 'G':
                shput(args->eagerRooms, optarg, true);
                break;
            case 'A':
                shput(args->disassemble, optarg, true);
                break;
            case 'T':
                shput(args->tilesToBeTraced, optarg, true);
                break;
            case 'E':
                args->traceEventInherited = true;
                break;
            case 'Z': {
                char* endPtr;
                long seedVal = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0') {
                    fprintf(stderr, "Error: Invalid seed value '%s' for --seed\n", optarg);
                    exit(1);
                }
                args->seed = (int) seedVal;
                args->hasSeed = true;
                break;
            }
            case 'I':
                args->recordInputsPath = optarg;
                break;
            case 'P':
                args->playbackInputsPath = optarg;
                break;
            case 'q': {
                char* endPtr;
                long framesBetween = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || framesBetween <= 0) {
                    fprintf(stderr, "Error: Invalid frame count '%s' for --profiler (must be > 0)\n", optarg);
                    exit(1);
                }
                args->profilerFramesBetween = (int) framesBetween;
                break;
            }
            case 'O':
                if (!parseOsTypeArg(optarg, &args->osType)) {
                    fprintf(stderr, "Error: Invalid --os-type value '%s' (expected: ", optarg);
                    printOsTypeNames(stderr);
                    fprintf(stderr, ")\n");
                    exit(1);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s [--headless] [--screenshot=PATTERN] [--screenshot-at-frame=N ...] <path to data.win or game.unx>\n", argv[0]);
                exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [--headless] [--screenshot=PATTERN] [--screenshot-at-frame=N ...] <path to data.win or game.unx>\n", argv[0]);
        exit(1);
    }

    args->dataWinPath = argv[optind];

    if (hmlen(args->screenshotFrames) > 0 && args->screenshotPattern == nullptr) {
        fprintf(stderr, "Error: --screenshot-at-frame requires --screenshot to be set\n");
        exit(1);
    }

    if (args->headless && args->speedMultiplier != 1.0) {
        fprintf(stderr, "You can't set the speed multiplier while running in headless mode! Headless mode always run in real time\n");
        exit(1);
    }

}

static void freeCommandLineArgs(CommandLineArgs* args) {
    hmfree(args->screenshotFrames);
    hmfree(args->dumpFrames);
    hmfree(args->dumpJsonFrames);
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
    shfree(args->disassemble);
    shfree(args->tilesToBeTraced);
}

// ===[ SCREENSHOT ]===
static void captureScreenshot(const char* filenamePattern, int frameNumber, int width, int height) {
    char filename[512];
    snprintf(filename, sizeof(filename), filenamePattern, frameNumber);

    int stride = width * 4;
    unsigned char* pixels = safeMalloc(stride * height);
    if (pixels == nullptr) {
        fprintf(stderr, "Error: Failed to allocate memory for screenshot (%dx%d)\n", width, height);
        return;
    }

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // OpenGL reads bottom-to-top, but PNG is top-to-bottom.
    // Use stb's negative stride trick: point to the last row and use a negative stride to flip vertically.
    unsigned char* lastRow = pixels + (height - 1) * stride;
    stbi_write_png(filename, width, height, 4, lastRow, -stride);

    free(pixels);
    printf("Screenshot saved: %s\n", filename);
}

// ===[ KEYBOARD INPUT ]===

static int32_t sdl2KeyToGml(int sdl2Key) {
    // Letters: SDLK_A (65) -> 65
    if (sdl2Key >= SDLK_a && sdl2Key <= SDLK_z) return sdl2Key - 32;
    // Numbers: SDLK_0 (48) -> 48 (same as GML)
    if (sdl2Key >= SDLK_0 && sdl2Key <= SDLK_9) return sdl2Key;
    // Special keys need mapping
    switch (sdl2Key) {
        case SDLK_ESCAPE:        return VK_ESCAPE;
        case SDLK_RETURN:         return VK_ENTER;
        case SDLK_TAB:           return VK_TAB;
        case SDLK_BACKSPACE:     return VK_BACKSPACE;
        case SDLK_SPACE:         return VK_SPACE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:   return VK_SHIFT;
        case SDLK_LCTRL:
        case SDLK_RCTRL: return VK_CONTROL;
        case SDLK_LALT:
        case SDLK_RALT:     return VK_ALT;
        case SDLK_UP:            return VK_UP;
        case SDLK_DOWN:          return VK_DOWN;
        case SDLK_LEFT:          return VK_LEFT;
        case SDLK_RIGHT:         return VK_RIGHT;
        case SDLK_F1:            return VK_F1;
        case SDLK_F2:            return VK_F2;
        case SDLK_F3:            return VK_F3;
        case SDLK_F4:            return VK_F4;
        case SDLK_F5:            return VK_F5;
        case SDLK_F6:            return VK_F6;
        case SDLK_F7:            return VK_F7;
        case SDLK_F8:            return VK_F8;
        case SDLK_F9:            return VK_F9;
        case SDLK_F10:           return VK_F10;
        case SDLK_F11:           return VK_F11;
        case SDLK_F12:           return VK_F12;
        case SDLK_INSERT:        return VK_INSERT;
        case SDLK_DELETE:        return VK_DELETE;
        case SDLK_HOME:          return VK_HOME;
        case SDLK_END:           return VK_END;
        case SDLK_PAGEUP:       return VK_PAGEUP;
        case SDLK_PAGEDOWN:     return VK_PAGEDOWN;
        default:                     return -1; // Unknown
    }
}

static InputRecording* globalInputRecording = nullptr;

static void keyCallback(Runner* runner, int key, int action) {
    // During playback, suppress real keyboard input (window events like close still work)
    if (InputRecording_isPlaybackActive(globalInputRecording)) return;
    int32_t gmlKey = sdl2KeyToGml(key);
    if (0 > gmlKey) return;
    if (action == 1) RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
    else if (action == 0) RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
}

static void setSDL2WindowTitle(void* window, const char* title) {
    SDL_SetWindowTitle((SDL_Window*)window, title);
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

#ifndef _WIN32
typedef struct { int key; struct sigaction value; } PreviousSignalActionEntry;
static PreviousSignalActionEntry* previousSignalActions = nullptr;

static void onCrashSignal(int sig) {
    saveInputRecording();
    // Restore the previous handler (ASAN) and re-raise so it can report the fault
    sigaction(sig, &previousSignalActions[hmgeti(previousSignalActions, sig)].value, nullptr);
    raise(sig);
}
#endif

// ===[ MAIN ]===
int main(int argc, char* argv[]) {
    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv);

    printf("Loading %s...\n", args.dataWinPath);

    DataWin* dataWin = DataWin_parse(
        args.dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = false,
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
            .lazyLoadRooms = args.lazyRooms,
            .eagerlyLoadedRooms = args.eagerRooms
        }
    );

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully! [Bytecode Version %u / GameMaker version %u.%u.%u.%u]\n", gen8->name, gen8->gameID, gen8->bytecodeVersion, dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build);

    #ifdef __GLIBC__
    {
        struct mallinfo2 mi = mallinfo2();
        printf("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
    }
    #endif

    // Build window title
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    // Initialize VM
    VMContext* vm = VM_create(dataWin);

    Profiler_setEnabled(&vm->profiler, args.profilerFramesBetween > 0);

    if (args.hasSeed) {
        srand((unsigned int) args.seed);
        vm->hasFixedSeed = true;
        printf("Using fixed RNG seed: %d\n", args.seed);
    }

    if (args.printRooms) {
        // Under --lazy-rooms we load each room for display and then free it again so the dump
        // reflects what each room contains without keeping all of them resident simultaneously.
        forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
            bool loadedHere = false;
            if (!room->payloadLoaded) {
                DataWin_loadRoomPayload(dataWin, (int32_t) idx);
                loadedHere = true;
            }

            printf("[%d] %s ()\n", idx, room->name);

            forEachIndexed(RoomGameObject, roomGameObject, idx2, room->gameObjects, room->gameObjectCount) {
                GameObject* gameObject = &dataWin->objt.objects[roomGameObject->objectDefinition];
                printf(
                    "  [%d] %s (x=%d,y=%d,persistent=%d,solid=%d,spriteId=%d,preCreateCode=%d,creationCode=%d)\n",
                    idx2,
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
        return 0;
    }

    if (args.printDeclaredFunctions) {
        repeat(hmlen(vm->funcMap), i) {
            printf("[%d] %s\n", vm->funcMap[i].value, vm->funcMap[i].key);
        }
        VM_free(vm);
        DataWin_free(dataWin);
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
                ptrdiff_t idx = shgeti(vm->funcMap, (char*) name);
                if (idx >= 0) {
                    VM_disassemble(vm, vm->funcMap[idx].value);
                } else {
                    fprintf(stderr, "Error: Script '%s' not found in funcMap\n", name);
                }
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 0;
    }

    // Initialize the file system
    Sdl2FileSystem* sdl2FileSystem = Sdl2FileSystem_create(args.dataWinPath);

    // Init SDL2
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "Failed to initialize SDL2\n");
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Load SDL gamecontroller mappings
    {
        const char* dbPath = "gamecontrollerdb.txt";
        FILE* f = fopen(dbPath, "r");
        if (f != NULL) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* buffer = (char*) malloc(len + 1);
            if (buffer != NULL) {
                fread(buffer, 1, len, f);
                buffer[len] = '\0';
                Sdl2Gamepad_loadMappings(buffer);
                free(buffer);
            }
            fclose(f);
        } else {
            fprintf(stderr, "Gamepad: SDL gamecontrollerdb.txt not found at %s, using defaults\n", dbPath);
        }
    }

    SDL_Window* window = SDL_CreateWindow(windowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)gen8->defaultWindowWidth, (int)gen8->defaultWindowHeight, SDL_WINDOW_OPENGL);
    if (window == nullptr) {
        fprintf(stderr, "Failed to create SDL2 window\n");
        SDL_Quit();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    if (strcmp(args.renderer, "legacy-gl") == 0) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    }

    if (args.headless) {
        SDL_HideWindow(window);
    }

    SDL_GLContext context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(0); // Disable v-sync, we control timing ourselves

    // Load OpenGL function pointers via GLAD
    if (!gladLoadGLLoader((GLADloadproc) SDL_GL_GetProcAddress)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Initialize the renderer
    Renderer* renderer = nullptr;
    if(strcmp(args.renderer, "legacy-gl") == 0)
        renderer = GLLegacyRenderer_create();
    else
        renderer = GLRenderer_create();

    // Initialize the audio system
    AudioSystem* audioSystem = nullptr;
    if (!args.headless) {
        audioSystem = (AudioSystem*) MaAudioSystem_create();
    } else {
        audioSystem = (AudioSystem*) NoopAudioSystem_create();
    }

    // Initialize the runner
    Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) sdl2FileSystem, audioSystem);
    runner->debugMode = args.debug;
    runner->osType = args.osType;
    runner->nativeWindow = window;
    runner->setWindowTitle = setSDL2WindowTitle;

    // Set up input recording/playback (both can be active: playback then continue recording)
    if (args.playbackInputsPath != nullptr) {
        globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
    } else if (args.recordInputsPath != nullptr) {
        globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
    }
    shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
    shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
    shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
    shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
    shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
    shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
    shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
    shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);
    shcopyFromTo(args.tilesToBeTraced, runner->vmContext->tilesToBeTraced);
    runner->vmContext->traceBytecodeAfterFrame = args.traceBytecodeAfterFrame;
    runner->vmContext->alwaysLogUnknownFunctions = args.alwaysLogUnknownFunctions;
    runner->vmContext->alwaysLogStubbedFunctions = args.alwaysLogStubbedFunctions;
    runner->vmContext->traceEventInherited = args.traceEventInherited;

#ifndef _WIN32
    struct sigaction sa = { .sa_handler = onCrashSignal };
    sigemptyset(&sa.sa_mask);
    struct sigaction prev;
    sigaction(SIGABRT, &sa, &prev);
    hmput(previousSignalActions, SIGABRT, prev);
    sigaction(SIGSEGV, &sa, &prev);
    hmput(previousSignalActions, SIGSEGV, prev);
#endif

    // Initialize the first room and fire Game Start / Room Start events
    Runner_initFirstRoom(runner);

    // Main loop
    bool debugPaused = false;
    bool debugShowCollisionMasks = false;
    bool shouldClose = false;
    SDL_Event sdlEvent;
    double lastFrameTime = ((double)SDL_GetTicks64() / 1000.0);
    while (!shouldClose && !runner->shouldExit) {
        // Clear last frame's pressed/released state, then poll new input events
        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerGamepad_beginFrame(runner->gamepads);
        while(SDL_PollEvent(&sdlEvent)) {
            switch(sdlEvent.type) {
                case SDL_QUIT:
                    shouldClose = true;
                    break;
                case SDL_KEYDOWN:
                    keyCallback(runner, sdlEvent.key.keysym.sym, 1);
                    break;
                case SDL_KEYUP:
                    keyCallback(runner, sdlEvent.key.keysym.sym, 0);
                    break;
            }
        }
        Sdl2Gamepad_poll(runner->gamepads);

        // Process input recording/playback (must happen after SDL_PollEvent, before Runner_step)
        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);

        // Debug key bindings
        if (runner->debugMode) {
            // Pause
            if (RunnerKeyboard_checkPressed(runner->keyboard, 'P')) {
                debugPaused = !debugPaused;
                fprintf(stderr, "Debug: %s\n", debugPaused ? "Paused" : "Resumed");
            }

            // Go to next room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
                DataWin* dw = runner->dataWin;
                if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                    int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                    runner->pendingRoom = nextIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                }
            }

            // Go to previous room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
                DataWin* dw = runner->dataWin;
                if (runner->currentRoomOrderPosition > 0) {
                    int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                    runner->pendingRoom = prevIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                }
            }

            // Dump runner state to console
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                Runner_dumpState(runner);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                char* json = Runner_dumpStateJson(runner);

                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }

                free(json);
            }

            // Toggle the collision mask debug overlay
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F2)) {
                debugShowCollisionMasks = !debugShowCollisionMasks;
                fprintf(stderr, "Debug: Collision mask overlay %s!\n", debugShowCollisionMasks ? "enabled" : "disabled");
            }

            // Reset global interact state because I HATE when I get stuck while moving through rooms
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");

                runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
                printf("Changed global.interact [%d] value!\n", interactVarId);
            }
        }

        // Run the game step if the game is paused
        bool shouldStep = true;
        if (runner->debugMode && debugPaused) {
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
            if (shouldStep) fprintf(stderr, "Debug: Frame advance (frame %d)\n", runner->frameCount);
        }

        double frameStartTime = 0;

        if (shouldStep) {
            if (args.traceFrames) {
                frameStartTime = ((double)SDL_GetTicks64() / 1000.0);
                fprintf(stderr, "Frame %d (Start)\n", runner->frameCount);
            }

            // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
            Runner_step(runner);

            if (args.profilerFramesBetween > 0 && runner->frameCount > 0 && runner->frameCount % args.profilerFramesBetween == 0) {
                char* profilerReport = Profiler_createReport(vm->profiler, 20, args.profilerFramesBetween);
                if (profilerReport != nullptr) {
                    fprintf(stderr, "%s\n", profilerReport);
                    free(profilerReport);
                }
                Profiler_reset(vm->profiler);
            }

            // Update audio system (gain fading, cleanup ended sounds)
            float dt = (float) (((double)SDL_GetTicks64() / 1000.0) - lastFrameTime);
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
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }
                free(json);
            }
        }

        Room* activeRoom = runner->currentRoom;

        // Query actual framebuffer size (differs from window size on Wayland with fractional scaling)
        int fbWidth, fbHeight;
        SDL_GL_GetDrawableSize(window, &fbWidth, &fbHeight);

        // Clear the default framebuffer (window background) to black
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;

        // The application surface (FBO) is sized to defaultWindowWidth x defaultWindowHeight.
        // It is a bit hard to understand, but here's how it works:
        // The Port X/Port Y controls the position of the game viewport within the application surface.
        // The Port W/Port H controls the size of the game viewport within the application surface.
        // Think of it like if you had an image (or... well, a framebuffer) and you are "pasting" it over the application surface.
        // And the Port W/Port H are scaled by the window size too (set by the GEN8 chunk)
        float displayScaleX = 1.0f;
        float displayScaleY = 1.0f;
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        if (viewsEnabled) {
            int32_t minLeft = INT32_MAX, minTop = INT32_MAX;
            int32_t maxRight = INT32_MIN, maxBottom = INT32_MIN;
            repeat(MAX_VIEWS, vi) {
                RuntimeView* view = &runner->views[vi];
                if (!view->enabled) continue;
                if (minLeft > view->portX) minLeft = view->portX;
                if (minTop > view->portY) minTop = view->portY;
                int32_t right = view->portX + view->portWidth;
                int32_t bottom = view->portY + view->portHeight;
                if (right > maxRight) maxRight = right;
                if (bottom > maxBottom) maxBottom = bottom;
            }
            if (maxRight > minLeft && maxBottom > minTop) {
                displayScaleX = (float) gameW / (float) (maxRight - minLeft);
                displayScaleY = (float) gameH / (float) (maxBottom - minTop);
            }
        }

        renderer->vtable->beginFrame(renderer, gameW, gameH, fbWidth, fbHeight);

        // Clear FBO with room background color
        if (runner->drawBackgroundColor) {
            int rInt = BGR_R(runner->backgroundColor);
            int gInt = BGR_G(runner->backgroundColor);
            int bInt = BGR_B(runner->backgroundColor);
            glClearColor(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f);
        } else {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);

        // Render each enabled view (or a default full-screen view if views are disabled)
        bool anyViewRendered = false;

        if (viewsEnabled) {
            repeat(MAX_VIEWS, vi) {
                RuntimeView* view = &runner->views[vi];
                if (!view->enabled) continue;

                int32_t viewX = view->viewX;
                int32_t viewY = view->viewY;
                int32_t viewW = view->viewWidth;
                int32_t viewH = view->viewHeight;
                int32_t portX = (int32_t) ((float) view->portX * displayScaleX + 0.5f);
                int32_t portY = (int32_t) ((float) view->portY * displayScaleY + 0.5f);
                int32_t portW = (int32_t) ((float) view->portWidth * displayScaleX + 0.5f);
                int32_t portH = (int32_t) ((float) view->portHeight * displayScaleY + 0.5f);
                float viewAngle = view->viewAngle;

                runner->viewCurrent = vi;
                renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);

                Runner_draw(runner);

                if (debugShowCollisionMasks) DebugOverlay_drawCollisionMasks(runner);

                renderer->vtable->endView(renderer);

                int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : portW;
                int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : portH;
                renderer->vtable->beginGUI(renderer, guiW, guiH, portX, portY, portW, portH);
                Runner_drawGUI(runner);
                renderer->vtable->endGUI(renderer);

                anyViewRendered = true;
            }
        }

        if (!anyViewRendered) {
            // No views enabled or views disabled: render with default full-screen view
            runner->viewCurrent = 0;
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            Runner_draw(runner);

            if (debugShowCollisionMasks) DebugOverlay_drawCollisionMasks(runner);

            renderer->vtable->endView(renderer);

            int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : gameW;
            int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : gameH;
            renderer->vtable->beginGUI(renderer, guiW, guiH, 0, 0, gameW, gameH);
            Runner_drawGUI(runner);
            renderer->vtable->endGUI(renderer);
        }

        // Reset view_current to 0 so non-Draw events (Step, Alarm, Create) see view_current = 0
        runner->viewCurrent = 0;

        renderer->vtable->endFrame(renderer);

        // Capture screenshot if this frame matches a requested frame
        bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);

        if (shouldScreenshot) {
            // Bind FBO so glReadPixels reads from the game's native-resolution texture
            GLRenderer* gl = (GLRenderer*) renderer;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->fbo);
            captureScreenshot(args.screenshotPattern, runner->frameCount, gameW, gameH);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
            printf("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
            shouldClose = true;
        }

        if (shouldStep && args.traceFrames) {
            double frameElapsedMs = (((double)SDL_GetTicks64() / 1000.0) - frameStartTime) * 1000.0;
            fprintf(stderr, "Frame %d (End, %.2f ms)\n", runner->frameCount, frameElapsedMs);
        }

        SDL_GL_SwapWindow(window);

        // Limit frame rate to room speed (skip in headless mode for max speed!!)
        if (!args.headless && runner->currentRoom->speed > 0) {
            double targetFrameTime = 1.0 / (runner->currentRoom->speed * args.speedMultiplier);
            double nextFrameTime = lastFrameTime + targetFrameTime;
            // Sleep for most of the remaining time, then spin-wait for precision
            double remaining = nextFrameTime - ((double)SDL_GetTicks64() / 1000.0);
            if (remaining > 0.002) {
                #ifdef _WIN32
                Sleep((DWORD) ((remaining - 0.001) * 1000));
                #else
                struct timespec ts = {
                    .tv_sec = 0,
                    .tv_nsec = (long) ((remaining - 0.001) * 1e9)
                };
                nanosleep(&ts, nullptr);
                #endif
            }
            while (((double)SDL_GetTicks64() / 1000.0) < nextFrameTime) {
                // Spin-wait for the remaining sub-millisecond
            }
            lastFrameTime = nextFrameTime;
        } else {
            lastFrameTime = ((double)SDL_GetTicks64() / 1000.0);
        }
    }

    saveInputRecording();

    // Cleanup
    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    renderer->vtable->destroy(renderer);

    SDL_DestroyWindow(window);
    SDL_Quit();

    Runner_free(runner);
    Sdl2FileSystem_destroy(sdl2FileSystem);
    VM_free(vm);
    DataWin_free(dataWin);

    freeCommandLineArgs(&args);

    printf("Bye! :3\n");
    return 0;
}
