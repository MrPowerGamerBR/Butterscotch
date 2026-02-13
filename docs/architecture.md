# Butterscotch Architecture

## Technology Stack

- **Language**: Kotlin (JVM 21)
- **Windowing**: LWJGL 3 + GLFW
- **Rendering**: OpenGL 3.3 Core Profile (via LWJGL)
- **Image decoding**: STB (via LWJGL)
- **Math**: JOML
- **CLI**: Clikt
- **Serialization**: kotlinx-serialization-json (input recording/playback)

## Module Structure

```
com.mrpowergamerbr.butterscotch/
  Butterscotch.kt            - Main class: GLFW window, game loop, headless mode, input recording
  ButterscotchLauncher.kt    - CLI entry point (Clikt command parsing, launches Butterscotch)

  data/                      - Data file parsing
    GameData.kt              - All data classes (Gen8Info, SpriteData, RoomData, CodeEntryData, etc.)
    FormReader.kt            - IFF FORM chunk reader (parses all chunks from game.unx)

  vm/                        - Virtual machine
    VM.kt                    - Bytecode interpreter (stack-based, executes BC16 instructions)
    Instruction.kt           - Instruction decoder (opcode + operand extraction)
    GMLValue.kt              - Runtime value type (Real, Str, ArrayVal, Undefined)

  runtime/                   - Game runtime
    GameRunner.kt            - Game loop orchestration, event dispatch, room transitions,
                               instance management, collision, path following, input state
    Instance.kt              - Live game instance (variables, built-in properties, alarms)

  graphics/                  - Rendering
    Renderer.kt              - OpenGL renderer (sprites, text, rectangles, backgrounds,
                               tiles, views, draw state, texture management)

  builtin/                   - Built-in GML functions
    BuiltinRegistry.kt       - Single file registering all built-in functions:
                               math, string, drawing, instance, room, keyboard,
                               data structures, file/INI stubs, audio stubs, collision,
                               paths, sprites, OS/system, surfaces, arrays, events, etc.
```

## Data Loading Pipeline

```
game.unx -> FormReader (parse FORM header)
  -> iterate chunks
  -> for each chunk, delegate to ChunkReader
  -> build GameData with all parsed assets
  -> resolve cross-references (sprites -> TPAG -> TXTR, objects -> events -> CODE, etc.)
```

## VM Design

### GMLValue (sealed class)
```kotlin
sealed class GMLValue {
    data class Real(val value: Double) : GMLValue()
    data class Str(val value: String) : GMLValue()
    data class ArrayVal(val data: MutableMap<Int, MutableMap<Int, GMLValue>>) : GMLValue()
    data object Undefined : GMLValue()
}
```

Includes `toReal()`, `toInt()`, `toStr()`, `toBool()` conversions and companion `of()` factory methods. Boolean is represented as Real (>= 0.5 = true).

### Variable Storage
- **Instance variables**: HashMap per instance, keyed by variable ID
- **Global variables**: Single shared HashMap
- **Local variables**: Stack-allocated per code entry execution
- **Built-in variables**: Intercepted reads/writes mapped to instance fields (x, y, sprite_index, etc.)

### Execution Flow
```
1. Decode instruction (4 bytes)
2. Match opcode
3. Execute (push/pop/arithmetic/branch/call)
4. Advance instruction pointer
5. Repeat until Return/Exit
```

## Rendering Pipeline

```
1. Clear with room bg color
2. For each enabled view:
   a. Set up orthographic projection for view rect
   b. Set viewport to port rect
   c. Draw backgrounds (non-foreground, tiled if needed)
   d. Gather all visible instances
   e. Sort by depth (descending = highest depth drawn first)
   f. For each instance, call Draw event (or default draw)
   g. Draw foreground backgrounds
3. Swap buffers
```

### Default Draw
If an instance has no Draw event:
```kotlin
if (instance.visible && instance.spriteIndex >= 0) {
    drawSprite(instance.spriteIndex, instance.imageIndex, instance.x, instance.y)
}
```

### Sprite Drawing
```
1. Look up sprite definition -> get TPAG for current frame
2. Look up TXTR page -> get/create OpenGL texture
3. Compute UV coordinates from TPAG source rect
4. Apply target offset, scale, rotation, blend color, alpha
5. Draw textured quad
```

## Implementation Status

All phases below are implemented and working:

### Data Loading
- Parses all chunks from game.unx (GEN8, STRG, SPRT, BGND, TPAG, TXTR, OBJT, ROOM, CODE, VARI, FUNC, SCPT, FONT, PATH)
- Loads textures into OpenGL on demand
- Resolves cross-references (sprites -> TPAG -> TXTR, objects -> events -> CODE, etc.)

### Bytecode VM
- Full instruction decoder for BC16 opcodes (push, pop, arithmetic, comparison, branch, call, with/env)
- Stack-based execution with type conversions
- Variable read/write for all scopes: instance, global, local, builtin, stacktop/dot-access
- Function calls (user scripts + built-in functions)
- `with` blocks (PushEnv/PopEnv)
- Array support (1D and 2D)

### Instance & Room System
- Instance creation/destruction with proper lifecycle events
- Room loading with instances, tiles, backgrounds, views
- Event dispatch (Create, Destroy, Step, Begin Step, End Step, Alarm, Keyboard, KeyPress, KeyRelease, Draw, Other, Collision)
- Room transitions with persistent instance carry-over
- Object parent/child event inheritance (`event_inherited()`)
- Path following system

### Rendering
- Sprite rendering with TPAG/TXTR lookup, scaling, rotation, blending
- Partial sprite drawing (`draw_sprite_part`)
- Font/text rendering with alignment and transformed text
- Draw state (color, alpha, font, halign, valign)
- View/port transformation (320x240 -> 640x480 scaling)
- Rectangle drawing, background drawing, tile rendering
- HiDPI/Wayland framebuffer scaling support

### Built-in Functions (~120+)
- Math: random, trig, rounding, clamping, interpolation, distance/direction
- String: manipulation, measurement, hashing
- Drawing: sprites, text, rectangles, backgrounds, color operations
- Instance: create, destroy, exists, find, number
- Room: goto, next, previous, exists
- Keyboard: check, check_pressed, check_released, clear
- Data structures: ds_map, ds_list (full CRUD)
- Collision: point, rectangle, circle, line (bbox-based)
- File/INI: stubs (file_exists returns false, ini_read returns defaults)
- Audio: all stubs (caster_*, audio_*, sound_*)
- Events: event_inherited, event_user, event_perform, script_execute
- DnD actions: action_kill_object, action_move_to, action_move, action_set_friction, action_set_alarm
- Paths: path_start, path_end
- System: OS info, timers, display, window, surface stubs
- Type checking: is_undefined, is_string, is_real, is_array, typeof

### Debug & Testing Infrastructure
- Headless mode for automated screenshot capture
- Input recording/playback for reproducible testing
- Per-object call tracing, event tracing, instruction tracing
- Debug mode with room navigation and frame stepping
- Configurable game speed multiplier
