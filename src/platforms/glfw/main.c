#include "../../data_win.h"
#include "../../vm.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runner_keyboard.h"
#include "runner.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "../../utils.h"

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
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printDeclaredFunctions;
    int exitAtFrame;
} CommandLineArgs;

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
        {"trace-opcodes", required_argument,       nullptr, 'o'},
        {"trace-stack", required_argument,         nullptr, 'S'},
        {"trace-frames", no_argument, nullptr, 'k'},
        {"exit-at-frame", required_argument, nullptr, 'x'},
        {"dump-frame", required_argument, nullptr, 'd'},
        {nullptr,               0,                 nullptr,  0 }
    };

    args->screenshotFrames = nullptr;
    args->exitAtFrame = -1;

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
                hmput(args->dumpFrames, (int) frame, true);
                break;
            }
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
}

static void freeCommandLineArgs(CommandLineArgs* args) {
    hmfree(args->screenshotFrames);
    hmfree(args->dumpFrames);
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
}

// ===[ SCREENSHOT ]===
static void captureScreenshot(const char* filenamePattern, int frameNumber, int width, int height) {
    char filename[512];
    snprintf(filename, sizeof(filename), filenamePattern, frameNumber);

    int stride = width * 4;
    unsigned char* pixels = malloc(stride * height);
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

static int32_t glfwKeyToGml(int glfwKey) {
    // Letters: GLFW_KEY_A (65) -> 65 (same as GML)
    if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z) return glfwKey;
    // Numbers: GLFW_KEY_0 (48) -> 48
    if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9) return glfwKey;
    // Special keys need mapping
    switch (glfwKey) {
        case GLFW_KEY_ESCAPE:        return VK_ESCAPE;
        case GLFW_KEY_ENTER:         return VK_ENTER;
        case GLFW_KEY_TAB:           return VK_TAB;
        case GLFW_KEY_BACKSPACE:     return VK_BACKSPACE;
        case GLFW_KEY_SPACE:         return VK_SPACE;
        case GLFW_KEY_LEFT_SHIFT:
        case GLFW_KEY_RIGHT_SHIFT:   return VK_SHIFT;
        case GLFW_KEY_LEFT_CONTROL:
        case GLFW_KEY_RIGHT_CONTROL: return VK_CONTROL;
        case GLFW_KEY_LEFT_ALT:
        case GLFW_KEY_RIGHT_ALT:     return VK_ALT;
        case GLFW_KEY_UP:            return VK_UP;
        case GLFW_KEY_DOWN:          return VK_DOWN;
        case GLFW_KEY_LEFT:          return VK_LEFT;
        case GLFW_KEY_RIGHT:         return VK_RIGHT;
        case GLFW_KEY_F1:            return VK_F1;
        case GLFW_KEY_F2:            return VK_F2;
        case GLFW_KEY_F3:            return VK_F3;
        case GLFW_KEY_F4:            return VK_F4;
        case GLFW_KEY_F5:            return VK_F5;
        case GLFW_KEY_F6:            return VK_F6;
        case GLFW_KEY_F7:            return VK_F7;
        case GLFW_KEY_F8:            return VK_F8;
        case GLFW_KEY_F9:            return VK_F9;
        case GLFW_KEY_F10:           return VK_F10;
        case GLFW_KEY_F11:           return VK_F11;
        case GLFW_KEY_F12:           return VK_F12;
        case GLFW_KEY_INSERT:        return VK_INSERT;
        case GLFW_KEY_DELETE:        return VK_DELETE;
        case GLFW_KEY_HOME:          return VK_HOME;
        case GLFW_KEY_END:           return VK_END;
        case GLFW_KEY_PAGE_UP:       return VK_PAGEUP;
        case GLFW_KEY_PAGE_DOWN:     return VK_PAGEDOWN;
        default:                     return -1; // Unknown
    }
}

static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void) scancode; (void) mods;
    Runner* runner = (Runner*) glfwGetWindowUserPointer(window);
    int32_t gmlKey = glfwKeyToGml(key);
    if (0 > gmlKey) return;
    if (action == GLFW_PRESS) RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
    else if (action == GLFW_RELEASE) RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
    // GLFW_REPEAT is ignored (GML doesn't use key repeat)
}

// ===[ MAIN ]===
int main(int argc, char* argv[]) {
    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv);

    printf("Loading %s...\n", args.dataWinPath);

    DataWin* dataWin = DataWin_parse(args.dataWinPath);

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully!\n", gen8->name, gen8->gameID);

    // Build window title
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    // Initialize VM
    VMContext* vm = VM_create(dataWin);

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

    // Initialize the runner
    Runner* runner = Runner_create(dataWin, vm);
    shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
    shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
    shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
    shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
    shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
    shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
    shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
    shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);

    // Init GLFW
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    if (args.headless) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    GLFWwindow* window = glfwCreateWindow((int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight, windowTitle, nullptr, nullptr);
    if (window == nullptr) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // Disable v-sync, we control timing ourselves

    // Load OpenGL function pointers via GLAD
    if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Set up keyboard input
    glfwSetWindowUserPointer(window, runner);
    glfwSetKeyCallback(window, keyCallback);

    // Initialize the first room and fire Game Start / Room Start events
    Runner_initFirstRoom(runner);

    // Main loop
    double lastFrameTime = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        // Clear last frame's pressed/released state, then poll new input events
        RunnerKeyboard_beginFrame(runner->keyboard);
        glfwPollEvents();

        if (args.traceFrames)
            printf("Frame %d (Start)\n", runner->frameCount);

        // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
        Runner_step(runner);

        // Dump full runner state if this frame was requested
        if (hmget(args.dumpFrames, runner->frameCount)) {
            Runner_dumpState(runner);
        }

        Room* activeRoom = runner->currentRoom;

        int rInt = BGR_R(activeRoom->backgroundColor);
        int gInt = BGR_G(activeRoom->backgroundColor);
        int bInt = BGR_B(activeRoom->backgroundColor);

        glClearColor(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Fire Draw events for all visible instances, sorted by depth
        Runner_draw(runner);

        // Capture screenshot if this frame matches a requested frame
        bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);

        if (shouldScreenshot) {
            captureScreenshot(args.screenshotPattern, runner->frameCount, (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight);
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
            printf("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        if (args.traceFrames)
            printf("Frame %d (End)\n", runner->frameCount);

        glfwSwapBuffers(window);

        // Limit frame rate to room speed (skip in headless mode for max speed!!)
        if (!args.headless && runner->currentRoom->speed > 0) {
            double targetFrameTime = 1.0 / runner->currentRoom->speed;
            double nextFrameTime = lastFrameTime + targetFrameTime;
            // Sleep for most of the remaining time, then spin-wait for precision
            double remaining = nextFrameTime - glfwGetTime();
            if (remaining > 0.002) {
                struct timespec ts = {
                    .tv_sec = 0,
                    .tv_nsec = (long) ((remaining - 0.001) * 1e9)
                };
                nanosleep(&ts, nullptr);
            }
            while (glfwGetTime() < nextFrameTime) {
                // Spin-wait for the remaining sub-millisecond
            }
            lastFrameTime = nextFrameTime;
        } else {
            lastFrameTime = glfwGetTime();
        }
    }

    // Cleanup
    glfwDestroyWindow(window);
    glfwTerminate();

    Runner_free(runner);
    VM_free(vm);
    DataWin_free(dataWin);

    freeCommandLineArgs(&args);
    return 0;
}
