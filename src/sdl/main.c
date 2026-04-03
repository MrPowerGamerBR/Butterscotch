#include "data_win.h"
#include "vm.h"

#include <SDL/SDL.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <malloc.h>
#endif

#include "runner_keyboard.h"
#include "runner.h"
#include "input_recording.h"
#include "sdl_renderer.h"
#include "sdl_file_system.h"

#if defined(NO_AUDIO) || defined(MIYOO_NO_AUDIO)
#include "noop_audio_system.h"
#else
#include "../glfw/ma_audio_system.h"
#endif

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "utils.h"
typedef struct {
    int key;
    bool value;
} FrameSetEntry;

typedef struct {
    const char* dataWinPath;
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
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printDeclaredFunctions;
    int exitAtFrame;
    double speedMultiplier;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char* recordInputsPath;
    const char* playbackInputsPath;
} CommandLineArgs;

static void parseCommandLineArgs(CommandLineArgs* args, int argc, char* argv[]) {
    memset(args, 0, sizeof(CommandLineArgs));

    static struct option longOptions[] = {
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
        {"exit-at-frame", required_argument, nullptr, 'x'},
        {"dump-frame", required_argument, nullptr, 'd'},
        {"dump-frame-json", required_argument, nullptr, 'j'},
        {"dump-frame-json-file", required_argument, nullptr, 'J'},
        {"speed", required_argument, nullptr, 'M'},
        {"seed", required_argument, nullptr, 'Z'},
        {"debug", no_argument, nullptr, 'D'},
        {"disassemble", required_argument, nullptr, 'A'},
        {"record-inputs", required_argument, nullptr, 'I'},
        {"playback-inputs", required_argument, nullptr, 'P'},
        {nullptr,               0,                 nullptr,  0 }
    };

    args->exitAtFrame = -1;
    args->speedMultiplier = 1.0;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longOptions, nullptr)) != -1) {
        switch (opt) {
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
            case 'd': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame\n", optarg);
                    exit(1);
                }
                int frameKey = (int) frame;
                hmput(args->dumpFrames, frameKey, true);
                break;
            }
            case 'j': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame-json\n", optarg);
                    exit(1);
                }
                int frameKey = (int) frame;
                hmput(args->dumpJsonFrames, frameKey, true);
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
            default:
                fprintf(stderr, "Usage: %s [--headless] <path to data.win or game.unx>\n", argv[0]);
                exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [--headless] <path to data.win or game.unx>\n", argv[0]);
        exit(1);
    }

    args->dataWinPath = argv[optind];

    if (args->headless && args->speedMultiplier != 1.0) {
        fprintf(stderr, "You can't set the speed multiplier while running in headless mode! Headless mode always runs in real time\n");
        exit(1);
    }
}

static void freeCommandLineArgs(CommandLineArgs* args) {
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

// ===[ KEYBOARD INPUT ]===

// Track key state to filter out key repeat (for SDL 1.2 where EnableKeyRepeat may not work)
static bool s_keyState[512] = {false};

static int32_t sdlKeyToGml(SDLKey key) {
    // Letters: SDLK_a (97) -> 65 (GML key code)
    if (key >= SDLK_a && key <= SDLK_z) {
        return (key - SDLK_a) + 'A';  // Convert to uppercase GML codes
    }
    // Numbers: SDLK_0 (48) -> 48
    if (key >= SDLK_0 && key <= SDLK_9) {
        return key;
    }
    
#ifdef MIYOO_KEYBINDINGS
    // Miyoo CFW 2.0.0+ specific bindings
    // Map physical buttons to Undertale's expected key codes
    // Undertale uses: Z=confirm, X=cancel, C=menu, Shift=run, Enter=interact
    switch (key) {
        case SDLK_LALT:          return 'Z';  // A button -> Z (confirm)
        case SDLK_LCTRL:         return 'X';  // B button -> X (cancel)
        case SDLK_LSHIFT:        return 'C';  // X button -> C (menu)
        case SDLK_SPACE:         return 'Y';  // Y button -> Y (unused)
        case SDLK_TAB:           return VK_SHIFT; // L1 -> Shift (run)
        case SDLK_BACKSPACE:     return VK_SHIFT; // R1 -> Shift
        case SDLK_PAGEUP:        return VK_CONTROL; // L2 -> Control
        case SDLK_PAGEDOWN:      return VK_CONTROL; // R2 -> Control
        case SDLK_RALT:          return VK_ALT;   // L3 -> Alt
        case SDLK_RSHIFT:        return VK_ESCAPE; // R3 -> ESC
        case SDLK_RCTRL:         return VK_ESCAPE; // RESET -> ESC
        case SDLK_ESCAPE:        return VK_ESCAPE; // SELECT -> ESC (exit game)
        default: break;
    }
#endif
    
    // Special keys need mapping
    switch (key) {
        case SDLK_ESCAPE:        return VK_ESCAPE;
        case SDLK_RETURN:        return VK_ENTER;
        case SDLK_TAB:           return VK_TAB;
        case SDLK_BACKSPACE:     return VK_BACKSPACE;
        case SDLK_SPACE:         return VK_SPACE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:        return VK_SHIFT;
        case SDLK_LCTRL:
        case SDLK_RCTRL:         return VK_CONTROL;
        case SDLK_LALT:
        case SDLK_RALT:          return VK_ALT;
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
        case SDLK_PAGEUP:        return VK_PAGEUP;
        case SDLK_PAGEDOWN:      return VK_PAGEDOWN;
        default:                 return -1; // Unknown
    }
}

static InputRecording* globalInputRecording = nullptr;

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
            .skipLoadingPreciseMasksForNonPreciseSprites = true
        }
    );

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully! [Bytecode Version %u]\n", gen8->name, gen8->gameID, gen8->bytecodeVersion);

    #ifndef _WIN32
    #ifdef __GLIBC__
    #include <malloc.h>
    {
        struct mallinfo2 mi = mallinfo2();
        printf("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
    }
    #endif
    #endif

    // Build window title
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    // Initialize VM
    VMContext* vm = VM_create(dataWin);

    if (args.hasSeed) {
        srand((unsigned int) args.seed);
        vm->hasFixedSeed = true;
        printf("Using fixed RNG seed: %d\n", args.seed);
    }

    if (args.printRooms) {
        forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
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

    // Initialize the file system (with base path from data.win location)
    FileSystem* sdlFileSystem = SdlFileSystem_create(args.dataWinPath);

    // Initialize the runner
    Runner* runner = Runner_create(dataWin, vm, sdlFileSystem);
    runner->debugMode = args.debug;

    // Set up input recording/playback
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
    runner->vmContext->traceEventInherited = args.traceEventInherited;

    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }
    
    // Disable key repeat (GML doesn't use key repeat, matches glfw behavior)
    SDL_EnableKeyRepeat(0, 0);

    // Create SDL window at native resolution 320x240
    int windowWidth = 320;
    int windowHeight = 240;

    // In SDL 1.2, we can't control window visibility with flags, only create the surface
    // For headless mode, we just create an off-screen surface
    // 16-bit RGB565 — оптимизация для 32 МБ ОЗУ (2x меньше памяти для экрана и текстур)
    SDL_Surface* screen = SDL_SetVideoMode(windowWidth, windowHeight, 16, SDL_SWSURFACE);
    if (screen == nullptr) {
        fprintf(stderr, "Failed to create SDL surface: %s\n", SDL_GetError());
        SDL_Quit();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }
    // disable cursor
    SDL_ShowCursor(SDL_DISABLE);

    if (!args.headless) {
        SDL_WM_SetCaption(windowTitle, nullptr);
    }

    // Initialize the renderer - use optimized renderer with debug
    Renderer* renderer = SDLRendererOpt_create();
    renderer->vtable->init(renderer, dataWin);
    runner->renderer = renderer;

    // Initialize audio system
#if defined(NO_AUDIO) || defined(MIYOO_NO_AUDIO)
    // Audio disabled at compile time or Miyoo NOOP audio
    NoopAudioSystem* noopAudio = NoopAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*) noopAudio;
#else
    // SDL/Miyoo uses miniaudio for audio
    MaAudioSystem* maAudio = MaAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*) maAudio;
#endif
    audioSystem->vtable->init(audioSystem, dataWin, sdlFileSystem);
    runner->audioSystem = audioSystem;

    // Initialize the first room
    Runner_initFirstRoom(runner);

    // Main loop
    bool debugPaused = false;
    bool shouldQuit = false;
    uint32_t lastFrameTime = SDL_GetTicks();

    while (!shouldQuit && !runner->shouldExit) {
        // Clear last frame's pressed/released state
        RunnerKeyboard_beginFrame(runner->keyboard);

        // Handle SDL events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                shouldQuit = true;
            } else if (event.type == SDL_KEYDOWN) {
                // Filter out key repeat by tracking key state
                SDLKey sym = event.key.keysym.sym;
                if (sym < 512 && s_keyState[sym]) continue; // Key already down, ignore repeat
                if (sym < 512) s_keyState[sym] = true;
                
                if (!InputRecording_isPlaybackActive(globalInputRecording)) {
                    int32_t gmlKey = sdlKeyToGml(sym);
                    if (gmlKey >= 0) {
                        RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                    }
                }
            } else if (event.type == SDL_KEYUP) {
                SDLKey sym = event.key.keysym.sym;
                if (sym < 512) s_keyState[sym] = false;
                
                if (!InputRecording_isPlaybackActive(globalInputRecording)) {
                    int32_t gmlKey = sdlKeyToGml(sym);
                    if (gmlKey >= 0) {
                        RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
                    }
                }
            }
        }

        // Process input recording/playback
        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);

        // Debug key bindings
        if (runner->debugMode) {
            // Pause/resume: P (keyboard) or SELECT+START (Miyoo)
            if (RunnerKeyboard_checkPressed(runner->keyboard, 'P')
#ifdef MIYOO_KEYBINDINGS
                || (RunnerKeyboard_checkPressed(runner->keyboard, VK_ESCAPE) && RunnerKeyboard_checkPressed(runner->keyboard, VK_ENTER))
#endif
            ) {
                debugPaused = !debugPaused;
                fprintf(stderr, "Debug: %s\n", debugPaused ? "Paused" : "Resumed");
            }

            // Next/prev room: PAGEUP/PAGEDOWN (keyboard) or L1/R1 (Miyoo: TAB/BACKSPACE → VK_SHIFT)
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)
#ifdef MIYOO_KEYBINDINGS
                || RunnerKeyboard_checkPressed(runner->keyboard, VK_SHIFT)
#endif
            ) {
                DataWin* dw = runner->dataWin;
                if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                    int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                    runner->pendingRoom = nextIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                }
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)
#ifdef MIYOO_KEYBINDINGS
                || RunnerKeyboard_checkPressed(runner->keyboard, VK_BACKSPACE)
#endif
            ) {
                DataWin* dw = runner->dataWin;
                if (runner->currentRoomOrderPosition > 0) {
                    int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                    runner->pendingRoom = prevIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                }
            }

            // Dump state: F12 (keyboard) or SELECT+START (Miyoo)
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)
#ifdef MIYOO_KEYBINDINGS
                || (RunnerKeyboard_checkPressed(runner->keyboard, VK_ESCAPE) && RunnerKeyboard_checkPressed(runner->keyboard, VK_ENTER))
#endif
            ) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                Runner_dumpState(runner);
            }

            // Dump JSON: F11 (keyboard) or L1+SELECT (Miyoo)
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)
#ifdef MIYOO_KEYBINDINGS
                || (RunnerKeyboard_checkPressed(runner->keyboard, VK_SHIFT) && RunnerKeyboard_checkPressed(runner->keyboard, VK_ESCAPE))
#endif
            ) {
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

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");
                runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
                printf("Changed global.interact [%d] value!\n", interactVarId);
            }
        }

        // Run the game step
        bool shouldStep = true;
        if (runner->debugMode && debugPaused) {
            // Frame advance: O (keyboard) or A (Miyoo: LALT → 'Z')
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O')
#ifdef MIYOO_KEYBINDINGS
                || RunnerKeyboard_checkPressed(runner->keyboard, 'Z')
#endif
            ;
            if (shouldStep) fprintf(stderr, "Debug: Frame advance (frame %d)\n", runner->frameCount);
        }

        uint32_t frameStartTime = SDL_GetTicks();

        if (shouldStep) {
            if (args.traceFrames) {
                fprintf(stderr, "Frame %d (Start)\n", runner->frameCount);
            }

            // Run one game step
            Runner_step(runner);

            // Dump full runner state if requested
            if (hmget(args.dumpFrames, runner->frameCount)) {
                Runner_dumpState(runner);
            }

            // Dump runner state as JSON if requested
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

        // Compute game resolution from enabled viewports
        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;
        
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        if (viewsEnabled) {
            int32_t maxRight = 0;
            int32_t maxBottom = 0;
            repeat(8, vi) {
                if (!activeRoom->views[vi].enabled) continue;
                int32_t right = activeRoom->views[vi].portX + activeRoom->views[vi].portWidth;
                int32_t bottom = activeRoom->views[vi].portY + activeRoom->views[vi].portHeight;
                if (right > maxRight) maxRight = right;
                if (bottom > maxBottom) maxBottom = bottom;
            }
            // Only override default if we found valid viewports
            if (maxRight > 0 && maxBottom > 0) {
                gameW = maxRight;
                gameH = maxBottom;
            }
        } else {
            // Views disabled: use room size as game resolution
            gameW = activeRoom->width;
            gameH = activeRoom->height;
        }

        // Render
        if (!args.headless) {
            renderer->vtable->beginFrame(renderer, gameW, gameH, windowWidth, windowHeight);

            // Render each enabled view
            bool anyViewRendered = false;

            if (viewsEnabled) {
                repeat(8, vi) {
                    if (!activeRoom->views[vi].enabled) continue;

                    int32_t viewX = activeRoom->views[vi].viewX;
                    int32_t viewY = activeRoom->views[vi].viewY;
                    int32_t viewW = activeRoom->views[vi].viewWidth;
                    int32_t viewH = activeRoom->views[vi].viewHeight;
                    int32_t portX = activeRoom->views[vi].portX;
                    int32_t portY = activeRoom->views[vi].portY;
                    int32_t portW = activeRoom->views[vi].portWidth;
                    int32_t portH = activeRoom->views[vi].portHeight;
                    float viewAngle = runner->viewAngles[vi];

                    runner->viewCurrent = vi;
                    renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);
                    Runner_draw(runner);
                    renderer->vtable->endView(renderer);
                    anyViewRendered = true;
                }
            }

            if (!anyViewRendered) {
                runner->viewCurrent = 0;
                renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
                Runner_draw(runner);
                renderer->vtable->endView(renderer);
            }

            runner->viewCurrent = 0;
            renderer->vtable->endFrame(renderer);
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
            printf("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
            shouldQuit = true;
        }

        if (shouldStep && args.traceFrames) {
            uint32_t frameElapsedMs = SDL_GetTicks() - frameStartTime;
            fprintf(stderr, "Frame %d (End, %u ms)\n", runner->frameCount, frameElapsedMs);
        }

        // Update debug overlay info (if debug mode is enabled)
        if (runner->debugMode) {
            uint32_t frameElapsedMs = SDL_GetTicks() - frameStartTime;

            // Get free memory via mallinfo2 if available
            int freeMemBytes = 0;
#ifdef __GLIBC__
            struct mallinfo2 mi = mallinfo2();
            freeMemBytes = mi.fordblks; // free bytes in heap
#endif

            // Count instances
            int instanceCount = 0;
            if (runner->instances) {
                instanceCount = (int)arrlen(runner->instances);
            }

            // Current room info
            const char* roomName = runner->currentRoom ? runner->currentRoom->name : "unknown";
            uint32_t roomSpeed = runner->currentRoom ? runner->currentRoom->speed : 0;

            SDLDebugInfo debugInfo;
            memset(&debugInfo, 0, sizeof(debugInfo));
            debugInfo.frameTimeMs = (float)frameElapsedMs;
            debugInfo.instanceCount = instanceCount;
            debugInfo.freeMemoryBytes = freeMemBytes;
            debugInfo.roomName = roomName;
            debugInfo.roomSpeed = roomSpeed;
            debugInfo.frameCount = runner->frameCount;

            SDLRendererOpt_updateDebugInfo(runner->renderer, &debugInfo);
        }

        // Limit frame rate
        if (!args.headless && runner->currentRoom->speed > 0) {
            uint32_t targetFrameTime = (uint32_t)(1000.0 / (runner->currentRoom->speed * args.speedMultiplier));
            uint32_t elapsed = SDL_GetTicks() - lastFrameTime;
            if (elapsed < targetFrameTime) {
                SDL_Delay(targetFrameTime - elapsed);
            }
            lastFrameTime = SDL_GetTicks();
        } else {
            lastFrameTime = SDL_GetTicks();
        }
    }

    // Save input recording if active
    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) {
            InputRecording_save(globalInputRecording);
        }
        InputRecording_free(globalInputRecording);
        globalInputRecording = nullptr;
    }

    // Cleanup - destroy subsystems first, then free containers
    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    renderer->vtable->destroy(renderer);
    runner->renderer = nullptr;

    SDL_Quit();

    // Free runner and its internal data (doesn't free renderer/audioSystem)
    Runner_free(runner);
    SdlFileSystem_destroy(sdlFileSystem);
    VM_free(vm);
    DataWin_free(dataWin);

    freeCommandLineArgs(&args);

    printf("Bye! :3\n");
    return 0;
}
