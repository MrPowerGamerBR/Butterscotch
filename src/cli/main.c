#include <loop.h>
#include <getopt.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* For SDL_main */
#if defined(USE_SDL1)
#include <SDL/SDL_main.h>
#elif defined(USE_SDL2)
#include <SDL2/SDL_main.h>
#elif defined(USE_SDL3)
#include <SDL3/SDL_main.h>
#endif

#ifndef F_OK
#define F_OK 0 /* for MSVC */
#endif

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

static bool logColour;

void platformLog(const logType type, const char *format, va_list va) {
    FILE *out = stderr;
    const char* colourPrefix = ANSI_COLOUR_CODE_RESET;
    const char* textPrefix = "";
    switch (type) {
        case LOG_TYPE_NORMAL:
            out = stdout;
            break;
        case LOG_TYPE_WARNING:
            colourPrefix = ANSI_COLOUR_CODE_BOLD_YELLOW;
            textPrefix = "Warning: ";
            break;
        case LOG_TYPE_ERROR:
            colourPrefix = ANSI_COLOUR_CODE_BOLD_RED;
            textPrefix = "Error: ";
            break;
        case LOG_TYPE_DEBUG:
            colourPrefix = ANSI_COLOUR_CODE_BOLD_PURPLE;
            textPrefix = "Debug: ";
            break;
    }

    if (logColour) fputs(colourPrefix, out);
    fputs(textPrefix, out);
    if (logColour) fputs(ANSI_COLOUR_CODE_RESET, out);
    vfprintf(out, format, va);
}

static void printUsage(const char *argv0) {
    logInfo(
        "Usage: %s <path to data.win or game.unx>\n"
        "    --help                                 - Show this message\n"
#ifdef ENABLE_SCREENSHOTS
        "    --screenshot <filename>                - Specify the filename for screenshots\n"
        "    --screenshot-at-frame <frame>          - Take a screenshot at the specified frame\n"
        "    --screenshot-surfaces <filename>       - Take a screenshot of all surfaces at the specified frame\n"
        "    --screenshot-surfaces-at-frame <frame> - Specify the filename for surface screenshots\n"
#endif
#ifndef USE_GLFW2
        "    --headless                             - Launch without a window\n"
#endif
        "    --print-rooms                          - Print all rooms in the game and exit\n"
        "    --print-objects                        - Print all objects in the game and exit\n"
        "    --print-shaders                        - Print all shaders in the game and exit\n"
        "    --print-declared-functions             - Print all declared functions in the game and exit\n"
        "    --print-unknown-functions              - Print all unknown functions used by the game and exit\n"
#ifdef ENABLE_VM_TRACING
        "    --trace-variable-reads                 - Trace variable reads\n"
        "    --trace-variable-writes                - Trace variable writes\n"
        "    --trace-function-calls                 - Trace function calls\n"
        "    --trace-alarms                         - Trace alarms\n"
        "    --trace-instance-lifecycles            - Trace instance creations and deletions\n"
        "    --trace-events                         - Trace events\n"
        "    --trace-collisions                     - Trace collisions between instances\n"
        "    --trace-event-inherited                - Trace event inherited calls\n"
        "    --trace-tiles                          - Trace drawn tiles\n"
        "    --trace-opcodes                        - Trace opcodes\n"
        "    --trace-stack                          - Trace stack\n"
#endif
        "    --trace-frames                         - Log frametimes\n"
        "    --always-log-unknown-functions         - Always log unknown function calls instead of once per script\n"
#ifdef ENABLE_VM_STUB_LOGS
        "    --always-log-stubbed-functions         - Always log stubbed function calls instead of once per script\n"
#endif
        "    --exit-at-frame <frame>                - Exit at the specified frame\n"
#ifdef ENABLE_VM_TRACING
        "    --trace-bytecode-after-frame <frame>   - Delay stack and opcode tracing until the specified frame\n"
#endif
        "    --dump-frame <frame>                   - Dump the runner state at the specified frame\n"
        "    --dump-frame-json <frame>              - Dump the runner state in json at the specified frame\n"
        "    --dump-frame-json-file <file>          - Specify an output file for runner state dumps\n"
        "    --speed <speed>                        - Set a normal speed multiplier\n"
        "    --fast-forward-speed <speed>           - Set a fast-forward speed multiplier\n"
        "    --seed <seed>                          - Seed for the random number generator\n"
        "    --debug                                - Enable debug mode\n"
        "    --disassemble <script>                 - Disassemble the specified script and print to console (* disassembles all)\n"
        "    --record-inputs <file>                 - Record all keyboard inputs to a file\n"
        "    --playback-inputs <file>               - Playback input from file\n"
        "    --renderer <renderer>                  - Set the rendering API\n"
        "    --lazy-rooms                           - Lazily load rooms, increases load times but reduces memory usage\n"
        "    --eager-room <rooms>                   - When --lazy-rooms is set, keep these rooms always in memory\n"
        "    --os-type <os>                         - Set the reported OS type\n"
        "    --window-size <dimentions>             - Set a custom window size\n"
        "    --widescreen-hack <aspect ratio>       - Set a custom aspect ratio\n"
        "    --profile-gml-scripts                  - Log which GML scripts are the heaviest in terms of time and executed instructions\n"
        "    --save-folder <directory>              - Set the directory will save files will be stored\n"
        "    --game-args <args>                     - Arguments to pass to the game\n"
        "    --lazy-textures                        - Load textures into VRAM on first use, improving startup times\n"
        "    --lazy-audio                           - Load audio into RAM on first use, reducing memory usage\n"
        "    --load-type <type>                     - Specify how data.win is loaded, per-chunk or all at once\n"
        "    --disable-log-colours                  - Disable colours for warning, error, and debug logs\n"
        "    --disable-log-colors                   - Same as --disable-log-colours, but different spelling\n"
#ifdef EABLE_VM_OPCODE_PROFILER
        "    --profile-opcodes                      - Rank which GML opcodes were executed the most\n"
#endif
        , argv0
    );
}

static void parseCommandLineArgs(CommandLineArgs* args, int argc, char* argv[]) {
    ZERO_STRUCT(*args);

    static struct option longOptions[] = {
        {"help",          no_argument, nullptr, 'H'},
#ifdef ENABLE_SCREENSHOTS
        {"screenshot",          required_argument, nullptr, 's'},
        {"screenshot-at-frame", required_argument, nullptr, 'f'},
        {"screenshot-surfaces", required_argument, nullptr, 'U'},
        {"screenshot-surfaces-at-frame", required_argument, nullptr, 'V'},
#endif
        {"headless",            no_argument,       nullptr, 'h'},
        {"print-rooms", no_argument,               nullptr, 'r'},
        {"print-objects", no_argument,             nullptr, 'b'},
        {"print-shaders", no_argument,               nullptr, 998},
        {"print-declared-functions", no_argument,  nullptr, 'p'},
        {"print-unknown-functions", no_argument, nullptr, 'u'},
#ifdef ENABLE_VM_TRACING
        {"trace-variable-reads", required_argument,  nullptr, 'R'},
        {"trace-variable-writes", required_argument, nullptr, 'W'},
        {"trace-function-calls", required_argument,         nullptr, 'c'},
        {"trace-alarms", required_argument,         nullptr, 'a'},
        {"trace-instance-lifecycles", required_argument,         nullptr, 'l'},
        {"trace-events", required_argument,         nullptr, 'e'},
        {"trace-collisions", required_argument,     nullptr, 'C'},
        {"trace-event-inherited", no_argument, nullptr, 'E'},
        {"trace-tiles", required_argument, nullptr, 'T'},
        {"trace-opcodes", required_argument,       nullptr, 'o'},
        {"trace-stack", required_argument,         nullptr, 'S'},
#endif
        {"trace-frames", no_argument, nullptr, 'k'},
        {"always-log-unknown-functions", no_argument, nullptr, 'y'},
#ifdef ENABLE_VM_STUB_LOGS
        {"always-log-stubbed-functions", no_argument, nullptr, 'Y'},
#endif
        {"exit-at-frame", required_argument, nullptr, 'x'},
#ifdef ENABLE_VM_TRACING
        {"trace-bytecode-after-frame", required_argument, nullptr, 'F'},
#endif
        {"dump-frame", required_argument, nullptr, 'd'},
        {"dump-frame-json", required_argument, nullptr, 'j'},
        {"dump-frame-json-file", required_argument, nullptr, 'J'},
        {"speed", required_argument, nullptr, 'M'},
        {"fast-forward-speed", required_argument, nullptr, 'X'},
        {"seed", required_argument, nullptr, 'Z'},
        {"debug", no_argument, nullptr, 'D'},
        {"disassemble", required_argument, nullptr, 'A'},
        {"record-inputs", required_argument, nullptr, 'I'},
        {"playback-inputs", required_argument, nullptr, 'P'},
        {"renderer", required_argument, nullptr, 'g'},
        {"lazy-rooms", no_argument, nullptr, 'z'},
        {"eager-room", required_argument, nullptr, 'G'},
        {"os-type", required_argument, nullptr, 'O'},
        {"window-size", required_argument, nullptr, 'w'},
        {"widescreen-hack", optional_argument, nullptr, 1000},
        {"profile-gml-scripts", required_argument, nullptr, 'q'},
        {"save-folder", required_argument, nullptr, 'B'},
        {"game-args", required_argument, nullptr, 'N'},
        {"lazy-textures", no_argument, nullptr, 'L'},
        {"lazy-audio", no_argument, nullptr, 'K'},
        {"load-type", required_argument, nullptr, 999},
        {"disable-log-colours", no_argument, nullptr, 1003},
        {"disable-log-colors", no_argument, nullptr, 1003},
#ifdef ENABLE_VM_OPCODE_PROFILER
        {"profile-opcodes", no_argument, nullptr, 'Q'},
#endif
        {nullptr,               0,                 nullptr,  0 }
    };

#ifdef ENABLE_SCREENSHOTS
    args->screenshotFrames = nullptr;
#endif
    args->exitAtFrame = -1;
#ifdef ENABLE_VM_TRACING
    args->traceBytecodeAfterFrame = 0;
#endif
    args->speedMultiplier = 1.0;
    args->fastForwardSpeed = 0.0;
    args->osType = OS_WINDOWS;
    args->profilerFramesBetween = 0;
    args->loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
    args->disableLogColours = !isatty(1); // 1 == stdout
    // TODO: detect available driver features
    // at runtime to improve defaults.
#if defined(ENABLE_MODERN_GL)
    args->renderer = MODERN_GL;
#elif defined(ENABLE_LEGACY_GL)
    args->renderer = LEGACY_GL;
#elif defined(ENABLE_SW_RENDERER)
    args->renderer = SOFTWARE;
#else
    args->renderer = NOOP;
#endif

    int opt;
    while ((opt = getopt_long(argc, argv, "", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 'H':
                printUsage(argv[0]);
                exit(0);
#ifdef ENABLE_SCREENSHOTS
            case 's':
                args->screenshotPattern = optarg;
                break;
            case 'f': {
                char* endPtr;
                int frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    logError("Invalid frame number '%s'\n", optarg);
                    exit(1);
                }

                hmput(args->screenshotFrames, frame, true);
                break;
            }
            case 'U':
                args->screenshotSurfacesPattern = optarg;
                break;
            case 'V': {
                char* endPtr;
                int frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    logError("Invalid frame number '%s' for --screenshot-surfaces-at-frame\n", optarg);
                    exit(1);
                }
                hmput(args->screenshotSurfacesFrames, frame, true);
                break;
            }
#endif
            case 'h':
                args->headless = true;
                break;
            case 'r':
                args->printRooms = true;
                break;
            case 'b':
                args->printObjects = true;
                break;
            case 998: {
                args->printShaders = true;
                break;
            }
            case 'p':
                args->printDeclaredFunctions = true;
                break;
            case 'u':
                args->printUnknownFunctions = true;
                break;
            case 'L':
                args->lazyTextures = true;
                break;
            case 'K':
                args->lazyAudio = true;
                break;
#ifdef ENABLE_VM_TRACING
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
            case 'C':
                shput(args->collisionsToBeTraced, optarg, true);
                break;
            case 'o':
                shput(args->opcodesToBeTraced, optarg, true);
                break;
            case 'S':
                shput(args->stackToBeTraced, optarg, true);
                break;
#endif
            case 'k':
                args->traceFrames = true;
                break;
            case 'y':
                args->alwaysLogUnknownFunctions = true;
                break;
#ifdef ENABLE_VM_STUB_LOGS
            case 'Y':
                args->alwaysLogStubbedFunctions = true;
                break;
#endif
            case 'x': {
                char* endPtr;
                int frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    logError("Invalid frame number '%s' for --exit-at-frame\n", optarg);
                    exit(1);
                }
                args->exitAtFrame = frame;
                break;
            }
#ifdef ENABLE_VM_TRACING
            case 'F': {
                char* endPtr;
                int frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    logError("Invalid frame number '%s' for --trace-bytecode-after-frame\n", optarg);
                    exit(1);
                }
                args->traceBytecodeAfterFrame = frame;
                break;
            }
#endif
            case 'd': {
                char* endPtr;
                int frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    logError("Invalid frame number '%s' for --dump-frame\n", optarg);
                    exit(1);
                }
                hmput(args->dumpFrames, frame, true);
                break;
            }
            case 'j': {
                char* endPtr;
                int frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    logError("Invalid frame number '%s' for --dump-frame-json\n", optarg);
                    exit(1);
                }
                hmput(args->dumpJsonFrames, frame, true);
                break;
            }
            case 'J':
                args->dumpJsonFilePattern = optarg;
                break;
            case 'M': {
                char* endPtr;
                double speed = strtod(optarg, &endPtr);
                if (*endPtr != '\0' || speed <= 0.0) {
                    logError("Invalid speed multiplier '%s' for --speed (must be > 0)\n", optarg);
                    exit(1);
                }
                args->speedMultiplier = speed;
                break;
            }
            case 'X': {
                char* endPtr;
                double speed = strtod(optarg, &endPtr);
                if (*endPtr != '\0' || speed <= 0.0) {
                    logError("Invalid speed '%s' for --fast-forward-speed (must be > 0)\n", optarg);
                    exit(1);
                }
                args->fastForwardSpeed = speed;
                break;
            }
            case 'D':
                args->debug = true;
                break;
            case 'g':
                if (strcmp(optarg, "modern-gl") == 0)
                    args->renderer = MODERN_GL;
                else if (strcmp(optarg, "legacy-gl") == 0)
                    args->renderer = LEGACY_GL;
                else if (strcmp(optarg, "software") == 0)
                    args->renderer = SOFTWARE;
                else if (strcmp(optarg, "noop") == 0)
                    args->renderer = NOOP;
                else {
                    logError("Unknown renderer: %s!\n", optarg);
                    exit(1);
                }
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
#ifdef ENABLE_VM_TRACING
            case 'T':
                shput(args->tilesToBeTraced, optarg, true);
                break;
#endif
            case 'E':
                args->traceEventInherited = true;
                break;
            case 'Z': {
                char* endPtr;
                int seedVal = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0') {
                    logError("Invalid seed value '%s' for --seed\n", optarg);
                    exit(1);
                }
                args->seed = seedVal;
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
                int framesBetween = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || framesBetween <= 0) {
                    logError("Invalid frame count '%s' for --profile-gml-scripts (must be > 0)\n", optarg);
                    exit(1);
                }
                args->profilerFramesBetween = framesBetween;
                break;
            }
            case 'B':
                args->saveFolder = optarg;
                break;
            case 'N': {
                repeat(arrlen(args->gameArgs), i) {
                    free(args->gameArgs[i]);
                }
                arrfree(args->gameArgs);
                args->gameArgs = extractRunnerArguments(optarg);
                break;
            }
#ifdef ENABLE_VM_OPCODE_PROFILER
            case 'Q':
                args->opcodeProfiler = true;
                break;
#endif
            case 'O':
                if (!parseOsTypeArg(optarg, &args->osType)) {
                    logError("Invalid --os-type value '%s' (expected: ", optarg);
                    printOsTypeNames(stderr);
                    logError(")\n");
                    exit(1);
                }
                break;
            case 'w': {
                int32_t w = 0, h = 0;
                if (sscanf(optarg, "%dx%d", &w, &h) != 2 || 0 >= w || 0 >= h) {
                    logError("Invalid --window-size value '%s' (expected WxH, e.g. 960x544)\n", optarg);
                    exit(1);
                }
                args->windowWidth = w;
                args->windowHeight = h;
                break;
            }
            case 999: {
                if (strcmp(optarg, "load-in-memory-ahead-of-time") == 0) {
                    args->loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
                } else if (strcmp(optarg, "map-file") == 0) {
                    args->loadType = DATAWINLOADTYPE_MAP_FILE;
                } else if (strcmp(optarg, "load-per-chunk") == 0) {
                    args->loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
                } else {
                    logError("Unknown load type '%s'\n", optarg);
                    exit(1);
                }
                break;
            }
            case 1000: {
                if (optarg == nullptr) {
                    args->widescreenAspect = 16.0f / 9.0f;
                    break;
                }
                int aw = 0, ah = 0;
                double ratio = 0.0;
                char* endPtr;
                if (sscanf(optarg, "%d:%d", &aw, &ah) == 2 && aw > 0 && ah > 0) {
                    args->widescreenAspect = (float) aw / (float) ah;
                } else if ((ratio = strtod(optarg, &endPtr)), *endPtr == '\0' && ratio > 0.0) {
                    args->widescreenAspect = (float) ratio;
                } else {
                    logError("Invalid --widescreen-hack value '%s' (expected W:H like 16:9, or a decimal like 1.7778)\n", optarg);
                    exit(1);
                }
                break;
            }
            case 1003:
                args->disableLogColours = true;
                break;
            default:
                printUsage(argv[0]);
                exit(1);
        }
    }

    if (optind >= argc) {
        const char* defaultDataWinPaths[4] = {"data.win", "assets/game.unx", "assets/game.droid", "../Resources/game.ios"}; // default WAD paths for Windows/Linux/Android/macOS
        static char resolvedPath[2048];

        char baseDir[2048] = {0};
        strncpy(baseDir, argv[0], sizeof(baseDir) - 1);
        
        char* lastSlash = strrchr(baseDir, '/');
        if (!lastSlash) lastSlash = strrchr(baseDir, '\\');
        
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
        } else {
            baseDir[0] = '\0';
        }
        repeat(4, i) {
            snprintf(resolvedPath, sizeof(resolvedPath), "%s%s", baseDir, defaultDataWinPaths[i]);
            if (access(resolvedPath, F_OK) == 0) {
                args->dataWinPath = resolvedPath;
                break;
            }
        }
        if (args->dataWinPath == nullptr) {
            printUsage(argv[0]);
            exit(1);
        }
    } else {
        args->dataWinPath = argv[optind];
    }

#ifdef ENABLE_SCREENSHOTS
    if (hmlen(args->screenshotFrames) > 0 && args->screenshotPattern == nullptr) {
        logError("--screenshot-at-frame requires --screenshot to be set\n");
        exit(1);
    }

    if (hmlen(args->screenshotSurfacesFrames) > 0 && args->screenshotSurfacesPattern == nullptr) {
        logError("--screenshot-surfaces-at-frame requires --screenshot-surfaces to be set\n");
        exit(1);
    }
#endif

    if (args->headless && args->speedMultiplier != 1.0) {
        logError("You can't set the speed multiplier while running in headless mode! Headless mode always run in real time\n");
        exit(1);
    }
}

int main(int argc, char* argv[]) {
    setbuf(stderr, NULL);

    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv);
    logColour = !args.disableLogColours;
    int ret = loop(args, argv[0]);
    freeCommandLineArgs(&args);
    return ret;
}
