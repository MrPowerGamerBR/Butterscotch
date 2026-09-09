#include "stdio_compat.h"
#include <time.h>

#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#include <dlfcn.h>

#include "common.h"
#include "input_recording.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

static Runner *g_runner;
static NSWindow *window = nil;
static NSOpenGLContext *glContext = nil;
static NSOpenGLView *glView = nil;

#define USE_PRIVATE_API 0

enum {
    IDX_LT = 6,
    IDX_RT = 7,
};

static void appkitMapControllerToSlot(GCController *controller, GamepadSlot *slot) {
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));

    if (!controller) {
        slot->connected = false;
        slot->guid[0] = '\0';
        slot->description[0] = '\0';
        return;
    }

    slot->connected = true;
    slot->jid = 0;

    NSString *name = controller.vendorName;
    if (name == nil) {
        name = controller.productCategory;
    }
    if (name == nil) {
        name = @"Gamepad";
    }
    strncpy(slot->description, [name UTF8String], sizeof(slot->description) - 1);
    slot->description[sizeof(slot->description) - 1] = '\0';

    snprintf(slot->guid, sizeof(slot->guid), "%p", (void *)controller);
    slot->guid[sizeof(slot->guid) - 1] = '\0';

    GCExtendedGamepad *extended = controller.extendedGamepad;
    GCGamepad *gamepad = controller.gamepad;
    GCMicroGamepad *micro = controller.microGamepad;

    if (extended) {
        slot->buttonDown[0] = extended.buttonA.pressed;
        slot->buttonValue[0] = extended.buttonA.value;
        slot->buttonDown[1] = extended.buttonB.pressed;
        slot->buttonValue[1] = extended.buttonB.value;
        slot->buttonDown[2] = extended.buttonX.pressed;
        slot->buttonValue[2] = extended.buttonX.value;
        slot->buttonDown[3] = extended.buttonY.pressed;
        slot->buttonValue[3] = extended.buttonY.value;

        slot->buttonDown[4] = extended.leftShoulder.pressed;
        slot->buttonValue[4] = extended.leftShoulder.value;
        slot->buttonDown[5] = extended.rightShoulder.pressed;
        slot->buttonValue[5] = extended.rightShoulder.value;

        float lt = extended.leftTrigger.value;
        float rt = extended.rightTrigger.value;
        if (lt < 0.0f) lt = 0.0f;
        if (rt < 0.0f) rt = 0.0f;
        slot->buttonValue[IDX_LT] = lt;
        slot->buttonValue[IDX_RT] = rt;
        slot->buttonDown[IDX_LT] = (lt >= slot->triggerThreshold);
        slot->buttonDown[IDX_RT] = (rt >= slot->triggerThreshold);

        slot->axisValue[0] = extended.leftThumbstick.xAxis.value;
        slot->axisValue[1] = -extended.leftThumbstick.yAxis.value;
        slot->axisValue[2] = extended.rightThumbstick.xAxis.value;
        slot->axisValue[3] = -extended.rightThumbstick.yAxis.value;

        if (extended.leftThumbstickButton) {
            slot->buttonDown[10] = extended.leftThumbstickButton.pressed;
            slot->buttonValue[10] = extended.leftThumbstickButton.value;
        }
        if (extended.rightThumbstickButton) {
            slot->buttonDown[11] = extended.rightThumbstickButton.pressed;
            slot->buttonValue[11] = extended.rightThumbstickButton.value;
        }

        slot->buttonDown[12] = extended.dpad.up.pressed;
        slot->buttonDown[13] = extended.dpad.down.pressed;
        slot->buttonDown[14] = extended.dpad.left.pressed;
        slot->buttonDown[15] = extended.dpad.right.pressed;
    } else if (gamepad) {
        slot->buttonDown[0] = gamepad.buttonA.pressed;
        slot->buttonValue[0] = gamepad.buttonA.value;
        slot->buttonDown[1] = gamepad.buttonB.pressed;
        slot->buttonValue[1] = gamepad.buttonB.value;
        slot->buttonDown[2] = gamepad.buttonX.pressed;
        slot->buttonValue[2] = gamepad.buttonX.value;
        slot->buttonDown[3] = gamepad.buttonY.pressed;
        slot->buttonValue[3] = gamepad.buttonY.value;

        slot->buttonDown[4] = gamepad.leftShoulder.pressed;
        slot->buttonValue[4] = gamepad.leftShoulder.value;
        slot->buttonDown[5] = gamepad.rightShoulder.pressed;
        slot->buttonValue[5] = gamepad.rightShoulder.value;

        slot->buttonDown[12] = gamepad.dpad.up.pressed;
        slot->buttonDown[13] = gamepad.dpad.down.pressed;
        slot->buttonDown[14] = gamepad.dpad.left.pressed;
        slot->buttonDown[15] = gamepad.dpad.right.pressed;
    } else if (micro) {
        slot->buttonDown[0] = micro.buttonA.pressed;
        slot->buttonValue[0] = micro.buttonA.value;
        slot->buttonDown[1] = micro.buttonX.pressed;
        slot->buttonValue[1] = micro.buttonX.value;

        slot->buttonDown[12] = micro.dpad.up.pressed;
        slot->buttonDown[13] = micro.dpad.down.pressed;
        slot->buttonDown[14] = micro.dpad.left.pressed;
        slot->buttonDown[15] = micro.dpad.right.pressed;

        slot->axisValue[0] = micro.dpad.xAxis.value;
        slot->axisValue[1] = -micro.dpad.yAxis.value;
    }

    for (int i = 0; GP_BUTTON_COUNT > i; i++) {
        if (i == IDX_LT || i == IDX_RT) continue;
        slot->buttonValue[i] = slot->buttonDown[i] ? 1.0f : slot->buttonValue[i];
    }
}

static int nsKeyToGML(unsigned short keyCode)
{
    switch (keyCode)
    {
        // Letters
        case 0:  return 'A';
        case 11: return 'B';
        case 8:  return 'C';
        case 2:  return 'D';
        case 14: return 'E';
        case 3:  return 'F';
        case 5:  return 'G';
        case 4:  return 'H';
        case 34: return 'I';
        case 38: return 'J';
        case 40: return 'K';
        case 37: return 'L';
        case 46: return 'M';
        case 45: return 'N';
        case 31: return 'O';
        case 35: return 'P';
        case 12: return 'Q';
        case 15: return 'R';
        case 1:  return 'S';
        case 17: return 'T';
        case 32: return 'U';
        case 9:  return 'V';
        case 13: return 'W';
        case 7:  return 'X';
        case 16: return 'Y';
        case 6:  return 'Z';

        // Numbers
        case 29: return '0';
        case 18: return '1';
        case 19: return '2';
        case 20: return '3';
        case 21: return '4';
        case 23: return '5';
        case 22: return '6';
        case 26: return '7';
        case 28: return '8';
        case 25: return '9';

        // Function keys
        case 122: return 112; // F1
        case 120: return 113; // F2
        case 99:  return 114; // F3
        case 118: return 115; // F4
        case 96:  return 116; // F5
        case 97:  return 117; // F6
        case 98:  return 118; // F7
        case 100: return 119; // F8
        case 101: return 120; // F9
        case 109: return 121; // F10
        case 103: return 122; // F11
        case 111: return 123; // F12

        // Arrows
        case 123: return 37; // Left
        case 124: return 39; // Right
        case 125: return 40; // Down
        case 126: return 38; // Up

        // Editing
        case 51: return 8;   // Backspace
        case 117:return 46;  // Delete
        case 114:return 45;  // Insert
        case 115:return 36;  // Home
        case 119:return 35;  // End
        case 116:return 33;  // Page Up
        case 121:return 34;  // Page Down

        // Whitespace
        case 36: return 13;  // Enter
        case 48: return 9;   // Tab
        case 49: return 32;  // Space
        case 53: return 27;  // Escape

        // Modifiers
        case 56: return 16;  // Left Shift
        case 60: return 16;  // Right Shift
        case 59: return 17;  // Left Control
        case 62: return 17;  // Right Control
        case 58: return 18;  // Left Option (Alt)
        case 61: return 18;  // Right Option (Alt)
        case 55: return 91;  // Left Command
        case 54: return 92;  // Right Command
        case 57: return 20;  // Caps Lock

        default:
            return 0;
    }
}

@interface GameView : NSOpenGLView
@end

@implementation GameView

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];

    NSTrackingArea *area = [[NSTrackingArea alloc]
        initWithRect:self.bounds
        options:NSTrackingMouseMoved |
                NSTrackingActiveInKeyWindow |
                NSTrackingInVisibleRect
        owner:self
        userInfo:nil];

    [self addTrackingArea:area];
}

- (void)keyDown:(NSEvent *)event {
    RunnerKeyboard_onKeyDown(g_runner->keyboard, nsKeyToGML(event.keyCode));
}
- (void)keyUp:(NSEvent *)event {
    RunnerKeyboard_onKeyUp(g_runner->keyboard, nsKeyToGML(event.keyCode));
}

- (void)mouseDown:(NSEvent *)event {
    RunnerMouse_onButtonDown(g_runner->mouse, GML_MB_LEFT);
}

- (void)mouseUp:(NSEvent *)event {
    RunnerMouse_onButtonUp(g_runner->mouse, GML_MB_LEFT);
}

- (void)rightMouseDown:(NSEvent *)event {
    RunnerMouse_onButtonDown(g_runner->mouse, GML_MB_RIGHT);
}

- (void)rightMouseUp:(NSEvent *)event {
    RunnerMouse_onButtonUp(g_runner->mouse, GML_MB_RIGHT);
}

- (void)otherMouseDown:(NSEvent *)event {
    // AppKit: 2 = middle, 3+ = extra buttons
    RunnerMouse_onButtonDown(g_runner->mouse, (int32_t)event.buttonNumber + 1);
}

- (void)otherMouseUp:(NSEvent *)event {
    RunnerMouse_onButtonUp(g_runner->mouse, (int32_t)event.buttonNumber + 1);
}

- (void)scrollWheel:(NSEvent *)event {
    double dy = event.scrollingDeltaY;
    RunnerMouse_onWheel(g_runner->mouse, dy);
}

@end

void platformSetWindowTitle(const char* title) {
    [window setTitle:[NSString stringWithFormat:@"Butterscotch - %s", title]];
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    NSRect bounds = [glView bounds];
    CGFloat scale = [window backingScaleFactor];
    *outW = (int32_t)bounds.size.width * scale;
    *outH = (int32_t)bounds.size.height * scale;

    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    NSRect backing = [glView convertRectToBacking:[glView bounds]];
    *outW = (int32_t)backing.size.width;
    *outH = (int32_t)backing.size.height;
    return true;
}

void platformSetWindowSize(int32_t width, int32_t height) {
    NSRect frame = [window frame];
    NSRect content = NSMakeRect(0, 0, width, height);

    NSRect newFrame = [window frameRectForContentRect:content];

    // Keep the top-left corner fixed.
    newFrame.origin.x = frame.origin.x;
    newFrame.origin.y = NSMaxY(frame) - newFrame.size.height;

    [window setFrame:newFrame display:YES animate:NO];
}

void platformGetMousePos(double *xPos, double *yPos) {
    NSPoint mouseLocation = [window mouseLocationOutsideOfEventStream];
    *xPos = mouseLocation.x;
    *yPos = mouseLocation.y;
}

NSMenu* createAppMenu() {
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"App"];

    // About
    NSMenuItem *aboutItem = [[NSMenuItem alloc]
        initWithTitle:@"About"
        action:@selector(orderFrontStandardAboutPanel:)
        keyEquivalent:@""];

    [appMenu addItem:aboutItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    // Preferences
    NSMenuItem *prefsItem = [[NSMenuItem alloc]
        initWithTitle:@"Settings..."
        action:@selector(openPreferences:)
        keyEquivalent:@","];

    [appMenu addItem:prefsItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    // Services submenu
    NSMenuItem *servicesItem = [[NSMenuItem alloc]
        initWithTitle:@"Services"
        action:nil
        keyEquivalent:@""];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    [appMenu setSubmenu:servicesMenu forItem:servicesItem];
    [appMenu addItem:servicesItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    // Hide
    [appMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Hide"
        action:@selector(hide:)
        keyEquivalent:@"h"]];

    // Hide Others
    [appMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Hide Others"
        action:@selector(hideOtherApplications:)
        keyEquivalent:@"h"]];

    // Show All
    [appMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Show All"
        action:@selector(unhideAllApplications:)
        keyEquivalent:@""]];

    [appMenu addItem:[NSMenuItem separatorItem]];

    // Quit
    NSString *appName = [[NSProcessInfo processInfo] processName];

    [appMenu addItem:[[NSMenuItem alloc]
        initWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
        action:@selector(terminate:)
        keyEquivalent:@"q"]];

    return appMenu;
}

NSMenu* createWindowMenu() {
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];

    // Minimize
    [windowMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Minimize"
        action:@selector(performMiniaturize:)
        keyEquivalent:@"m"]];

    // Zoom
    [windowMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Zoom"
        action:@selector(performZoom:)
        keyEquivalent:@""]];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    // Move window to left/right side (macOS 10.11+)
    [windowMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Move Window to Left Side of Screen"
        action:@selector(moveWindowLeft:)
        keyEquivalent:@""]];

    [windowMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Move Window to Right Side of Screen"
        action:@selector(moveWindowRight:)
        keyEquivalent:@""]];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    // Fullscreen
    [windowMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Enter Full Screen"
        action:@selector(toggleFullScreen:)
        keyEquivalent:@"f"]];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    // Arrange
    [windowMenu addItem:[[NSMenuItem alloc]
        initWithTitle:@"Bring All to Front"
        action:@selector(arrangeInFront:)
        keyEquivalent:@""]];

    // Tell AppKit this is the window menu
    [NSApp setWindowsMenu:windowMenu];

    return windowMenu;
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *preferencesWindow;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [self setupMenu];
}

- (void)setupMenu {
    NSMenu *appMenu = createAppMenu();
    NSMenu *windowMenu = createWindowMenu();

    // Create main menu
    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@"Main Menu"];

    // App menu
    NSMenuItem *appMenuItem = [[NSMenuItem alloc]
        initWithTitle:@"App"
        action:nil
        keyEquivalent:@""];
    [mainMenu addItem:appMenuItem];
    [mainMenu setSubmenu:appMenu forItem:appMenuItem];

    // Window menu
    NSMenuItem *windowMenuItem = [[NSMenuItem alloc]
        initWithTitle:@"Window"
        action:nil
        keyEquivalent:@""];
    [mainMenu addItem:windowMenuItem];
    [mainMenu setSubmenu:windowMenu forItem:windowMenuItem];

    // Set the main menu for the application
    [NSApp setMainMenu:mainMenu];
}

@end

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    // Create application
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    [delegate setupMenu];

    if (@available(macOS 10.15, *)) {
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    }

    // Create OpenGL pixel format
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile,
        NSOpenGLProfileVersionLegacy,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFADepthSize, 24,
        0
    };

    NSOpenGLPixelFormat *pf =
        [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];

    if (!pf)
        return false;

    // Create window
    NSRect rect = NSMakeRect(100, 100, reqW, reqH);

    window =
        [[NSWindow alloc]
            initWithContentRect:rect
                      styleMask:
                          NSWindowStyleMaskTitled |
                          NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable |
                          NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];

    if (!window)
        return false;

    [window setTitle:[NSString stringWithFormat:@"%s", title]];
    [window setAcceptsMouseMovedEvents:YES];

    // Create OpenGL view
    glView =
        [[GameView alloc]
            initWithFrame:rect
              pixelFormat:pf];

    [window setContentView:glView];
    [window makeFirstResponder:glView];
    [window setContentView:glView];

    glContext = [glView openGLContext];
        
    [glContext makeCurrentContext];

    GLint swap = 0;
    [glContext setValues:&swap
            forParameter:NSOpenGLContextParameterSwapInterval];

    if (!headless)
        [window makeKeyAndOrderFront:nil];

    [NSApp activateIgnoringOtherApps:YES];

    return true;
}

void platformExit(void) {
    if (@available(macOS 10.15, *)) {
        [GCController stopWirelessControllerDiscovery];
    }
    [glContext clearDrawable];
    [window close];
    glContext = nil;
    glView = nil;
    window = nil;
}

static void platformSetCursor(int32_t cursorType) {
    [NSCursor unhide];
    switch (cursorType) {
        case GML_CR_DEFAULT:
        case GML_CR_ARROW:
            [[NSCursor arrowCursor] set];
            break;
        case GML_CR_NONE:
            [NSCursor hide];
            break;
        case GML_CR_CROSS:
            [[NSCursor crosshairCursor] set];
            break;
        case GML_CR_BEAM:
            [[NSCursor IBeamCursor] set];
            break;
#if USE_PRIVATE_API
        case GML_CR_SIZE_NESW:
            [[NSCursor _windowResizeNorthEastSouthWestCursor] set];
            break;
        case GML_CR_SIZE_NWSE:
            [[NSCursor _windowResizeNorthWestSouthEastCursor] set];
            break;
#endif
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060 // 10.6
        case GML_CR_SIZE_NS:
            [[NSCursor resizeUpDownCursor] set];
            break;
        case GML_CR_SIZE_WE:
            [[NSCursor resizeLeftRightCursor] set];
            break;
#endif
        case GML_CR_DRAG:
            [[NSCursor closedHandCursor] set];
            break;
        case GML_CR_HANDPOINT:
            [[NSCursor pointingHandCursor] set];
            break;
        case GML_CR_SIZE_ALL:
            [[NSCursor openHandCursor] set];
            break;

        // Uparrow, hourglass, and appstart cursors are not available on macOS, so we fall back to the arrow cursor for these cases.
        case GML_CR_UPARROW:
        case GML_CR_HOURGLASS:
        case GML_CR_APPSTART:
        default:
            [[NSCursor arrowCursor] set];
            break;
    }
}

static bool windowIsFocused(void) {
    return [window isKeyWindow];
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = windowIsFocused;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_ARROW;
}

void platformSwapBuffers(void)
{
    [glContext flushBuffer];
}

void *platformGetProcAddress(const char *name) {
    return dlsym(RTLD_DEFAULT, name);
}

bool platformHandleEvents(void)
{
    if (![window isVisible])
        return true;

    NSEvent *event;
    while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
        [NSApp sendEvent:event];
    }

    [NSApp updateWindows];

    if (g_runner && g_runner->gamepads) {
        NSArray<GCController *> *controllers = [GCController controllers];
        NSUInteger count = MIN((NSUInteger)MAX_GAMEPADS, controllers.count);
        g_runner->gamepads->connectedCount = 0;

        for (int slotIdx = 0; slotIdx < MAX_GAMEPADS; slotIdx++) {
            GamepadSlot *slot = g_runner->gamepads->slots + slotIdx;
            GCController *controller = (slotIdx < (int)count) ? controllers[slotIdx] : nil;

            appkitMapControllerToSlot(controller, slot);

            if (slot->connected) {
                for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
                    bool wasDown = slot->buttonDownPrev[btn];
                    slot->buttonPressed[btn] = (slot->buttonDown[btn] && !wasDown);
                    slot->buttonReleased[btn] = (!slot->buttonDown[btn] && wasDown);
                }
                g_runner->gamepads->connectedCount++;
            }
        }
    }

    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
    }
    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}

#ifdef ENABLE_SW_RENDERER

void platformSetNextFramebuffer(uint32_t* framebuffer, int width, int height)
{
    // TODO: this is a placeholder
    (void) framebuffer;
    (void) width;
    (void) height;
}

#endif
