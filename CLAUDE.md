# Butterscotch

A GameMaker: Studio 1.x runtime (Bytecode 16) in Kotlin, targeting Undertale v1.08.

## Project Goal

Run Undertale v1.08 as far as possible using a custom bytecode interpreter with LWJGL/OpenGL rendering.

## Technology

- Kotlin/JVM 21, Gradle build
- LWJGL 3.4.1 (GLFW, OpenGL, STB)
- JOML for math
- Clikt (CLI argument parsing)
- kotlinx-serialization-json (input recording/playback)
- No audio support (all audio functions are stubbed)

## Key Facts

- **Data file**: `undertale/game.unx` (60 MB IFF/FORM container)
- **Bytecode version**: 16 (confirmed in GEN8 chunk)
- **Compiled with**: GameMaker Studio 1.4.1539
- **Window size**: 640x480 (320x240 game scaled 2x via views)
- **Frame rate**: 30 FPS (room_speed = 30)
- **First room**: room_start (index 0) containing obj_time + obj_screen

## Research Documentation

- [Data File Format](docs/data-format.md) - IFF chunk format for game.unx (GEN8, STRG, SPRT, TPAG, TXTR, OBJT, ROOM, CODE, VARI, FUNC, etc.)
- [Bytecode 16](docs/bytecode.md) - Instruction set, encoding, opcodes, stack-based VM
- [Runtime Architecture](docs/runtime.md) - Game loop, instance system, events, drawing, variables
- [Undertale Analysis](docs/undertale-analysis.md) - Rooms, objects, sprites specific to the intro/menu sequence
- [Architecture](docs/architecture.md) - Module structure, implementation status, VM design

## Key References

- [UndertaleModTool](https://github.com/UnderminersTeam/UndertaleModTool) - Definitive data format reference (C#)
- [OpenGMK](https://github.com/OpenGMK/OpenGMK) - GM8 runner in Rust (architecture inspiration)
- [OpenGML](https://github.com/maiple/opengml) - GML 1.4 interpreter in C++
- [Altar.NET](https://github.com/PoroCYon/Altar.NET) - GM:S data.win unpacker
- [GM:S 1.4 Manual](https://docs2.yoyogames.com/) - Official GML documentation

## Undertale Intro Room Flow

```
room_start (640x480) -> room_introstory (320x240, 2x view) -> room_introimage (320x240) -> room_intromenu (320x240)
```

Key objects: `obj_time` (persistent controller), `obj_screen` (persistent), `obj_introimage`, `obj_titleimage`, `obj_intromenu`, `obj_unfader`

## Decompiled GML Code

- **Folder**: `undertale_gml_code/` - All 6272 code entries decompiled to GML via UndertaleModTool
- **Naming**: `gml_Object_<name>_<event>.gml`, `gml_Script_<name>.gml`, `gml_GlobalScript_<name>.gml`, `gml_RoomCC_<name>.gml`
- **CLI Tool**: `UTMT_CLI_v0.8.4.1-Ubuntu/UndertaleModCli` - UndertaleModTool CLI (`./UndertaleModCli load ../undertale/game.unx -s Scripts/Resource\ Exporters/ExportAllCode.csx`)

## CLI Parameters

Butterscotch uses [Clikt](https://ajalt.github.io/clikt/) for CLI argument parsing.

| Parameter | Description |
|-----------|-------------|
| `--debug` | Enable debug mode (room navigation, pause/step) |
| `--screenshot <pattern>` | Screenshot filename pattern (`%s` = frame number) |
| `--screenshot-at-frame <N>` | Capture screenshot at frame N (repeatable, enables headless mode) |
| `--room <name-or-index>` | Start at a specific room (e.g. `--room room_ruins1` or `--room 5`) |
| `--list-rooms` | Print all rooms and exit |
| `--debug-obj <name>` | Print info about an object (repeatable) |
| `--trace-calls <name>` | Trace function calls for an object, `*` for all (repeatable) |
| `--ignore-function-traced-calls <name>` | Ignore specific functions when tracing (repeatable) |
| `--trace-events <name>` | Trace fired events for an object, `*` for all (repeatable) |
| `--trace-instructions <name>` | Trace bytecode instructions for a GML script, `*` for all, VERY NOISY (repeatable) |
| `--speed <multiplier>` | Game speed multiplier (e.g. `2.0` = twice as fast, default `1.0`) |
| `--record-inputs <path>` | Record keyboard inputs to a JSON file |
| `--playback-inputs <path>` | Playback keyboard inputs from a JSON file |

### Debug Mode Keys

When `--debug` is enabled:
- `Page Up` / `Page Down`: Navigate to next/previous room
- `P`: Pause/unpause the game
- `O`: Step forward one frame (while paused)

### Headless Mode

When `--screenshot-at-frame` is specified, the game runs in headless mode: no vsync, no visible window, processes frames as fast as possible and captures screenshots at the specified frames. Useful for automated testing.

### Input Recording/Playback

`--record-inputs` and `--playback-inputs` use a JSON format mapping frame numbers to lists of GM key codes. Useful for reproducible testing.

## Development Commands

```bash
./gradlew run                                    # Run the application
./gradlew build                                  # Build
./gradlew test                                   # Run tests
./gradlew run --args="--room room_ruins1"        # Start at a specific room
./gradlew run --args="--debug"                   # Run with debug mode
```
