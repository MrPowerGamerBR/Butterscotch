<h1 align="center">🥧 Butterscotch (MiyooCFW) 🥧</h1>

<p align="center">
<b>An open-source re-implementation of GameMaker: Studio's runner</b>
</p>

<p align="center">
<a href="https://github.com/MrPowerGamerBR/Butterscotch"><img src="https://img.shields.io/badge/original-MrPowerGamerBR%2FButterscotch-blue" alt="Original Project"></a>
</p>

> [!IMPORTANT]
> This is a **fork** of the original [Butterscotch](https://github.com/MrPowerGamerBR/Butterscotch) project by [@MrPowerGamerBR](https://github.com/MrPowerGamerBR).
> 
> Butterscotch is still VERY early in development.

## About

Butterscotch is an open-source re-implementation of GameMaker: Studio's runner. When you create a game in GameMaker: Studio and export it, the game code is compiled to bytecode that can run on any platform with a compatible runner - similar to how Java applications work.

This project aims to reimplement that runner, allowing GameMaker: Studio games to run on platforms beyond the official ones.

**Original Project:** https://github.com/MrPowerGamerBR/Butterscotch

**Butterscotch PlayStation 2 ISO Generator:** https://butterscotch.mrpowergamerbr.com/

## Game Compatibility

The target is **Undertale v1.08** (GameMaker: Studio 1.4.1804, Bytecode Version 16). Other games compiled with GameMaker: Studio 1.4.1804 should work as long as they only use supported GML variables and functions.

**Not supported:**
- Games compiled with YYC (native code instead of bytecode)
- Games compiled with GMRT (native code instead of bytecode)

## Supported Platforms

This fork supports the following platforms:

| Platform | Backend | Status |
|----------|---------|--------|
| Linux | GLFW, OpenGL | ✅ Working |
| Windows | GLFW, OpenGL, MinGW | ✅ Working |
| PlayStation 2 | ps2sdk, gsKit | 🚧 Experimental |
| Linux/macOS/Windows | SDL 1.2 (Software) | ✅ Working |
| Miyoo CFW | SDL 1.2 (Software) | ✅ (No audio) |

## Building Butterscotch

### Prerequisites

**Linux (GLFW):**
```bash
sudo apt-get install cmake libsdl1.2-dev libsdl-mixer1.2-dev libsdl-ttf2.0-dev libsdl-image1.2-dev
```

**macOS (GLFW):**
```bash
brew install cmake sdl sdl_mixer sdl_ttf sdl_image
```

### Build Commands

```bash
mkdir build && cd build

# For GLFW backend (Linux/Windows)
cmake -DPLATFORM=glfw -DCMAKE_BUILD_TYPE=Debug ..
make

# For SDL 1.2 backend (software rendering)
cmake -DPLATFORM=sdl -DCMAKE_BUILD_TYPE=Debug ..
make

# For Miyoo CFW (cross-compile)
cmake -DPLATFORM=miyoo -DCMAKE_TOOLCHAIN_FILE=../cmake/miyoo.cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

**CLion users:** Set the platform in `Settings` > `Build, Execution, Deployment` > `CMake` and add `-DPLATFORM=glfw` (or `sdl`, `miyoo`, `ps2`)

### Running

```bash
./butterscotch /path/to/data.win
```

## CLI Parameters

The GLFW and SDL backends support various CLI parameters for debugging:

| Parameter | Description |
|-----------|-------------|
| `--debug` | Enables debugging hotkeys |
| `--headless` | Runs without displaying a window |
| `--seed=N` | Sets a fixed RNG seed |
| `--speed=X` | Speed multiplier |
| `--exit-at-frame=N` | Exit after N frames |
| `--screenshot=file_%d.png` | Screenshot at specific frames |
| `--print-rooms` | Print all rooms and exit |
| `--trace-variable-reads` | Trace variable reads |
| `--trace-variable-writes` | Trace variable writes |
| `--trace-function-calls` | Trace function calls |
| `--trace-opcodes` | Trace bytecode opcodes |
| `--trace-frames` | Log frame timing |
| `--disassemble` | Disassemble a specific script |
| `--record-inputs` | Record user inputs |
| `--playback-inputs` | Playback recorded inputs |

## Debug Features

When running with `--debug`:

| Key | Action |
|-----|--------|
| `Page Up` | Move forward one room |
| `Page Down` | Move backward one room |
| `P` | Pause the game |
| `O` | Advance one frame (while paused) |
| `F12` | Dump runner state to console |
| `F11` | Dump runner state as JSON |
| `F10` | Reset `global.interact` flag (Undertale) |

## Screenshots

### Undertale (SDL 1.2 MiyooCFW backend, running on Powkiddy V90)

<table>
  <tr>
    <td><img src="screenshots/system-3.png" width="160" alt="Screenshot 1"></td>
    <td><img src="screenshots/system-7.png" width="160" alt="Screenshot 2"></td>
    <td><img src="screenshots/system-10.png" width="160" alt="Screenshot 3"></td>
  </tr>
  <tr>
    <td><img src="screenshots/system-11.png" width="160" alt="Screenshot 4"></td>
    <td><img src="screenshots/system-12.png" width="160" alt="Screenshot 5"></td>
    <td><img src="screenshots/system-13.png" width="160" alt="Screenshot 6"></td>
  </tr>
  <tr>
    <td><img src="screenshots/system-14.png" width="160" alt="Screenshot 7"></td>
  </tr>
</table>

### Undertale (PlayStation 2)

Watch the video: [Undertale on PlayStation 2](https://youtu.be/3MoAPO8H85U)

## Architecture

### Backend Implementations

- **GLFW Backend** (`src/glfw/`) - OpenGL hardware-accelerated rendering
- **SDL Backend** (`src/sdl/`) - SDL 1.2 software rendering
- **PS2 Backend** (`src/ps2/`) - PlayStation 2 specific implementation using ps2sdk/gsKit
- **Miyoo Backend** - SDL 1.2 for Miyoo CFW devices

### Core Components

- `src/vm/` - Virtual machine and bytecode interpreter
- `src/gml/` - GameMaker Language runtime
- `vendor/` - Third-party libraries (stb, miniaudio, glad)

## Differences from Original

This fork includes:

- Additional platform support (SDL 1.2 backend, Miyoo CFW)
- Improved software rendering for low-end devices
- Cross-platform build system improvements
- Various bug fixes and optimizations

## Performance

Performance is good on modern computers. On low-end targets (like PS2), performance may be slow when there are many instances on screen or when instances execute large loops.

## Credits

- **Original Author:** [@MrPowerGamerBR](https://github.com/MrPowerGamerBR)
- **Original Repository:** [MrPowerGamerBR/Butterscotch](https://github.com/MrPowerGamerBR/Butterscotch)
- **Discord:** [Join the community](https://discord.gg/2gQR7t3WJR)

## License

Same license as the original Butterscotch project. See [LICENSE](LICENSE) for details.

## Resources

- [UndertaleModTool](https://github.com/UnderminersTeam/UndertaleModTool) - Bytecode documentation
- [GameMaker-HTML5](https://github.com/YoYoGames/GameMaker-HTML5) - GML builtin variables and functions
- [OpenGM](https://github.com/misternebula/OpenGM) - Another GameMaker runner re-implementation
