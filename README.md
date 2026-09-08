<div align="center">
<img width="256" height="256" alt="Butterscotch Logo" src="https://github.com/user-attachments/assets/ef8bdd5c-d407-4b3c-a4d5-07b25e8bbc70" />
</div>

<h1 align="center">🥧 Butterscotch 🥧</h1>

<!-- Badges, about the GitHub repository itself -->
<p align="center">
<a href="https://github.com/ButterscotchRunner/CompatibilityList"><img src="https://img.shields.io/badge/butterscotch-compatibility_list-green"></a>
<a href="https://discord.gg/2gQR7t3WJR"><img src="https://img.shields.io/discord/1406856655920168971?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

> [!IMPORTANT]
> Butterscotch is still VERY early in development and it is NOT that good yet.

When you create a game in GameMaker: Studio and export it, GameMaker: Studio exports the game code as bytecode instead of native compiled code, and that bytecode is compatible with any other GameMaker: Studio runner (also known as YoYo runner), as long as they have matching GameMaker: Studio versions. This is similar to how Java applications work.

This is how projects such as [Droidtale](https://mrpowergamerbr.com/projects/droidtale) (which was also made by yours truly) can exist. We exploit that GameMaker: Studio games compile to bytecode, which means they can be ran on *any* platform that has an official runner for it!

Ever since I created Droidtale 10+ years ago, I had that lingering thought in my mind... If GameMaker games use bytecode, what prevents us from creating our *own* runner? And if we can write our *own* runner, what prevents us from porting GameMaker: Studio games to other platforms?

And that's where Butterscotch comes in! Butterscotch is an open source re-implementation of GameMaker: Studio's runner.

## Game Compatibility

Butterscotch's goal is to be able to have Undertale v1.08 (GameMaker: Studio 1.4.1804, WAD Version 16) fully playable. But we do want to support more GameMaker: Studio games in the future too!

While our target is Undertale v1.08, that doesn't mean that other games CAN'T run in Butterscotch! Because Butterscotch is a runner and not a Undertale port/remake, you CAN run other GameMaker: Studio games with it and, as long as the game is compiled with GameMaker: Studio 1.4.1804 and they only use GML variables and functions that Butterscotch supports, it should work fine.

Butterscotch supports the following WAD versions:

* WAD Version 8 (GameMaker: Studio 1.0.198+)
* WAD Version 9 (GameMaker: Studio 1.0.527+)
* WAD Version 10 (GameMaker: Studio 1.1.609+)
* WAD Version 11 (GameMaker: Studio 1.1.754+)
* WAD Version 12 (GameMaker: Studio 1.1.867+)
* WAD Version 13 (GameMaker: Studio 1.1.917+)
* WAD Version 14 (GameMaker: Studio 1.4.1464+)
* WAD Version 15 (GameMaker: Studio 1.4.1675+)
* WAD Version 16 (GameMaker: Studio 1.4.1767+)
* WAD Version 17 (GameMaker: Studio 2.2+)

Other modding tools, such as UndertaleModTool, calls it "bytecode version" instead of "WAD version". We decided to go with WAD version instead because there are GameMaker: Studio versions (WAD version 6 and 7) that DO NOT use bytecode altogether, so calling it "bytecode version" is not quite correct, and because that's what the YoYo Runner calls it under the hood.

Versions before GameMaker: Studio 1.0.198 (that is, pre-WAD version 8) uses raw GML code interpreted on load, so these versions would require a GML compiler to be supported in Butterscotch.

However, that doesn't mean that a game that uses a compatible version WILL run! The bytecode support is still a WIP, and Butterscotch may have quirks that the original GameMaker: Studio runner may not have.

Of course, there are exceptions that break game compatibility altogether:

* Games compiled with YYC, because they use native code instead of bytecode.
* Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.

## Supported Platforms

* Windows
* macOS
* Web
* PlayStation 2
* PlayStation 3
* PlayStation Vita
* Nintendo Switch
* LG webOS
* ...and maybe more in the future!

Additionally, any platform with reasonably complete C and POSIX conformance should work, the following have been tested.
* Linux with glibc as old as about ~1995
* FreeBSD as old as 2.2.8
* OpenBSD
* NetBSD
* Haiku

The following backends are available for desktop platforms (Windows and POSIX systems).
* GLFW 2
* GLFW 3
* SDL 1.2
* SDL 2
* SDL 3
* AppKit (macOS only)

The following compilers have been tested to successfully build butterscotch, older versions may work but are untested.
* GCC 2.7 and up in C++ mode, and 3.0 and up in C99 mode
* Clang 1.1 and up
* TinyCC 0.9.27 and up
* MSVC 4.0 and up

## Download Butterscotch

Below are direct links to download the latest auto-build of Butterscotch for your platform.

### Desktop

| OS | x86_64 | i386 | arm64 | armhf | PowerPC | PowerPC64 | PowerPC64 little-endian | RISC-V 64 | LoongArch64 | MIPS | MIPS little-endian | MIPS64 | MIPS64 little-endian |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Windows | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-windows-x86_64.zip) | [i486](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-windows-i486.zip) | [arm64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-sdl2-windows-arm64.zip) | [armv7](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-sdl2-windows-armv7.zip) | | | | | | | | | |
| macOS | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-appkit-macos-x86_64.zip) | | [arm64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-appkit-macos-arm64.zip) | | | | | | | | | | |
| Linux | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-amd64.zip) | [i486](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-i486.zip) | [aarch64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-aarch64.zip) | [arm](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-arm.zip) | [powerpc](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-powerpc.zip) | [powerpc64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-powerpc64.zip) | [powerpc64le](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-powerpc64le.zip) | [riscv64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-riscv.zip) | [loongarch64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-loongarch64.zip) | [mips](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-mips.zip) | [mipsel](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-mipsel.zip) | [mips64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-mips64.zip) | [mips64el](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-Linux-mips64el.zip) |
| FreeBSD 14.0 | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-amd64.zip) | [i486](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-i486.zip) | [aarch64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-aarch64.zip) | [arm](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-arm.zip) | | [powerpc64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-powerpc64.zip) | [powerpc64le](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-powerpc64le.zip) | [riscv64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-FreeBSD-riscv.zip) | | | | | |
| OpenBSD 7.8 | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-amd64.zip) | [i486](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-i486.zip) | [aarch64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-aarch64.zip) | [arm](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-arm.zip) | [powerpc](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-powerpc.zip) | [powerpc64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-powerpc64.zip) | | [riscv64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-riscv.zip) | | | | [mips64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-mips64.zip) | [mips64el](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-OpenBSD-mips64el.zip) |
| NetBSD 10.1 | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-NetBSD-amd64.zip) | [i486](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-NetBSD-i486.zip) | [aarch64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-NetBSD-aarch64.zip) | [arm](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-NetBSD-arm.zip) | | | | | | [mips](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-NetBSD-mips.zip) | [mipsel](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/Butterscotch-NetBSD-mipsel.zip) | | |
| Haiku | [x86_64](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-sdl2-haiku-x86-64.zip) | | | | | | | | | | | | |

All desktop builds use SDL2, with the exception of Mac OS using AppKit and i486 Windows using SDL 1.2.

All RISC architecture (ARM, MIPS, PowerPC, RISC-V) builds require hardware floating-point support.

### Consoles

| Platform | Download | Notes |
|---|---|---|
| PlayStation 2 | [butterscotch-ps2.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-ps2.zip) | All WAD versions. ELF executable to be used with the [ISO generator](https://butterscotch.mrpowergamerbr.com/). |
| PlayStation 2 | [butterscotch-ps2-wad14.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-ps2-wad14.zip) | WAD 14 only. ELF executable to be used with the [ISO generator](https://butterscotch.mrpowergamerbr.com/). |
| PlayStation 2 | [butterscotch-ps2-wad16.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-ps2-wad16.zip) | WAD 16 only. ELF executable to be used with the [ISO generator](https://butterscotch.mrpowergamerbr.com/). |
| PlayStation 2 | [butterscotch-ps2-wad17.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-ps2-wad17.zip) | WAD 17 only. ELF executable to be used with the [ISO generator](https://butterscotch.mrpowergamerbr.com/). |
| PlayStation 3 | [butterscotch-ps3.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-ps3.zip) | To be used with the [preprocessor](https://github.com/ButterscotchRunner/ButterscotchPreprocessor). |
| PlayStation Vita | [butterscotch-vita.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-vita.zip) | Place data.win in `ux0:/data/butterscotch/` directory. |
| Nintendo Switch | [butterscotch-switch.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-switch.zip) | Place data.win in `sdmc:/switch/butterscotch/` directory. |

### Other

| Platform | Download | Notes |
| -------- | -------- | ----- |
| WebAssembly | [butterscotch-web.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-web.zip) | [Play online](https://butterscotch.mrpowergamerbr.com/web/) |
| Android | [butterscotch-android.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-android.zip) | 
| webOS | [butterscotch-webos.zip](https://nightly.link/ButterscotchRunner/Butterscotch/workflows/build/main/butterscotch-webos.zip) | After installing the IPK, place the game's `data.win` and other required files in `/media/developer/apps/usr/palm/applications/com.mrpowergamerbr.butterscotch/` |

## Community Ports

* [Xbox 360 (Butterscotch-360)](https://github.com/ceilingtilefan/Butterscotch-360) by @ceilingtilefan
* [3DS and Wii U (Cinnamon)](https://github.com/Project-Sunshine-Native/cinnamon) by @casrielasriel, @grayforz24682, @d16.dorian, @ralcactus

## Building Butterscotch

```bash
mkdir build && cd build
cmake -DBACKEND=glfw3 -DCMAKE_BUILD_TYPE=Debug ..
make
```

If you are using CLion, set the platform in `Settings` > `Build, Execution, Deployment` > `CMake` and add `-DBACKEND=glfw3`

Then run Butterscotch with `./butterscotch` - it will automatically search for `data.win`, `assets/game.unx`, `assets/game.droid` or `../Resources/game.ios` relative to the binary, otherwise you can specify a path with `./butterscotch /path/to/data.win`.

## CLI parameters

The desktop target has a lot of nifty CLI parameters that you can use to trace and debug games running on it.

```
--help                                 - Show this message
--screenshot <filename>                - Specify the filename for screenshots
--screenshot-at-frame <frame>          - Take a screenshot at the specified frame
--screenshot-surfaces <filename>       - Take a screenshot of all surfaces at the specified frame
--screenshot-surfaces-at-frame <frame> - Specify the filename for surface screenshots
--headless                             - Launch without a window
--print-rooms                          - Print all rooms in the game and exit
--print-objects                        - Print all objects in the game and exit
--print-shaders                        - Print all shaders in the game and exit
--print-declared-functions             - Print all declared functions in the game and exit
--print-unknown-functions              - Print all unknown functions used by the game and exit
--trace-variable-reads                 - Trace variable reads
--trace-variable-writes                - Trace variable writes
--trace-function-calls                 - Trace function calls
--trace-alarms                         - Trace alarms
--trace-instance-lifecycles            - Trace instance creations and deletions
--trace-events                         - Trace events
--trace-collisions                     - Trace collisions between instances
--trace-event-inherited                - Trace event inherited calls
--trace-tiles                          - Trace drawn tiles
--trace-opcodes                        - Trace opcodes
--trace-stack                          - Trace stack
--trace-frames                         - Log frametimes
--always-log-unknown-functions         - Always log unknown function calls instead of once per script
--always-log-stubbed-functions         - Always log stubbed function calls instead of once per script
--exit-at-frame <frame>                - Exit at the specified frame
--trace-bytecode-after-frame <frame>   - Delay stack and opcode tracing until the specified frame
--dump-frame <frame>                   - Dump the runner state at the specified frame
--dump-frame-json <frame>              - Dump the runner state in json at the specified frame
--dump-frame-json-file <file>          - Specify an output file for runner state dumps
--speed <speed>                        - Set a normal speed multiplier
--fast-forward-speed <speed>           - Set a fast-forward speed multiplier
--seed <seed>                          - Seed for the random number generator
--debug                                - Enable debug mode
--disassemble <script>                 - Disassemble the specified script and print to console (\* disassembles all)
--record-inputs <file>                 - Record all keyboard inputs to a file
--playback-inputs <file>               - Playback input from file
--renderer <renderer>                  - Set the rendering API
--lazy-rooms                           - Lazily load rooms, increases load times but reduces memory usage
--eager-room <rooms>                   - When --lazy-rooms is set, keep these rooms always in memory
--os-type <os>                         - Set the reported OS type
--window-size <dimentions>             - Set a custom window size
--widescreen-hack <aspect ratio>       - Set a custom aspect ratio
--profile-gml-scripts                  - Log which GML scripts are the heaviest in terms of time and executed instructions
--save-folder <directory>              - Set the directory will save files will be stored
--game-args <args>                     - Arguments to pass to the game
--profile-opcodes                      - Rank which GML opcodes were executed the most
--lazy-textures                        - Load textures into VRAM on first use, improving startup times
--load-type <type>                     - Specify how data.win is loaded, per-chunk or all at once
--disable-log-colours                  - Disable colours for warning, error, and debug logs
--disable-log-colors                   - Same as --disable-log-colours, but different spelling
```

## Debug Features

When running Butterscotch with `--debug`, the following hotkeys are enabled:

* `Page Up`: Moves forward one room
* `Page Down`: Moves backwards one room
* `P`: Pauses the game
* `O`: While paused, advances the game loop by one frame
* `F12`: Dumps the current runner state to the console
* `F11`: Dumps the current runner state to the console (JSON format), or dumps it to a file if `--dump-frame-json-file` is set.
* `F10`: Sets the `global.interact` flag to `0`. Useful in Undertale when you are moving through rooms and one of them starts a cutscene that doesn't let you move.

## Performance

Performance is pretty good on any modern computer, but when running on low end targets (like the PS2) it is *very* slow when there's a lot of instances on screen, or when a instance does a for loop.

## Then why not have a transpiler?

The issue with a transpiler is that, if you try transpiling the game in the "naive" way, that is, emitting VM calls like it was the original bytecode, you won't get any
*improvement* from it, you would need to create a *good* transpiler that actually transpiles it into *good* code, and that's way harder.

Having a transpiler also have other disadvantages:

1. You lose the ability of debugging the runner at a "high level" by tracing opcodes.
2. Compilation is SLOW, transpiling Undertale in a naive way to C and building it takes 90 seconds on a modern computer, and building it to other targets is so slow that I wasn't even able to test it.

## Screenshots

### Undertale (GLFW) [WAD Version 16]

<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/6651cc2e-0d6d-4354-b98d-081e84a981df" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/1d6edc51-2829-4f8f-b900-393f21a6655b" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/0d41f16c-7ee5-47de-a2e8-5831cdcd2745" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/45dc47fb-6d8a-44d4-8cbb-2e5791100144" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/7db1c869-e625-4558-9119-0f23da0f020c" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/71fc7616-d580-48fe-aa6d-1e6ceea41bdb" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/4098936e-a1b9-4971-901d-702ec390afa7" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/dd3dcce3-3d78-452f-9af0-27133497650c" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/2e356d04-5aaf-47d4-9bc3-4abba78cd18d" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/a9cbc57f-e9c1-4985-a6af-a98e5fce5ff3" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/e5c67781-0ffc-43c8-9c7d-333254eed704" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/93900e3c-79b5-4a05-bd6c-d68814e9e101" />

### Undertale (PlayStation 2) [WAD Version 16]

Here's a video :3 https://youtu.be/PuzBxe0VGtY

Here's also another video, this time showing off the Asriel Dreemurr final battle https://youtu.be/vkQMqXr0MQE

### DELTARUNE (SURVEY_PROGRAM) (PlayStation 2) [WAD Version 16]

Here's a video :3 https://youtu.be/TLJtV2WnrmQ

### DELTARUNE Chapter 2 (GLFW) [WAD Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/d0df9858-ad2b-4642-9f32-a542d1d942e0" />

### DELTARUNE Chapter 2 (PlayStation 2) [WAD Version 17]

Here's a video :3 https://youtu.be/uuN72Hv50d4

### DELTARUNE Chapter 3 (GLFW) [WAD Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/7b49d434-e66f-4ee3-bfe8-c0b4f45ceeb7" />
<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/afbe62ad-4706-4882-a9c9-6c239ed57c69" />
<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/d83c9f8c-e9b9-410e-8d3d-3663ede23fab" />

### DELTARUNE Chapter 3 (PlayStation 2) [WAD Version 17]

Here's a video :3 https://youtu.be/c9r79sQABYg

### DELTARUNE Chapter Selector (GLFW) [WAD Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/b8a848df-fd1c-49b7-9602-e8020ac86d5d" />

### Undertale 10th Anniversary (GLFW) [WAD Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/4ec0c64e-23f1-4bb1-8291-6aaf626a690f" />
<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/4ea7d078-784d-4861-aeb1-4ee2d1d70508" />
<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/45eb5be9-5e7b-4930-bb7e-2f2c49c76a49" />

### NXTALE (Undertale for Xbox One) (GLFW) [WAD Version 17]

<img width="160" alt="image" src="https://github.com/user-attachments/assets/7c4e2224-76e4-495e-8382-fad2dbdef207" />
<img width="160" alt="image" src="https://github.com/user-attachments/assets/6af34191-66c6-44dd-8712-907641520073" />
<img width="160" alt="image" src="https://github.com/user-attachments/assets/150aec4c-8cfb-4cef-9db0-f339158b0d14" />
<img width="160" alt="image" src="https://github.com/user-attachments/assets/4e3489e8-11de-4c8c-953c-f7b776bb4eb8" />

### AM2R (GLFW) [WAD Version 14]

<img width="160" alt="image" src="https://github.com/user-attachments/assets/3e46dfed-487c-4d91-9cd5-c71adc7a6cb5" />
<img width="160" alt="image" src="https://github.com/user-attachments/assets/4a4b6da1-dae4-4d0f-8611-12e7a1fc8d8c" />
<img width="160" alt="image" src="https://github.com/user-attachments/assets/d522be68-1003-4208-bf6b-d59a004606ba" />

### GameMaker: Studio Platformer Demo (GLFW) [WAD Version 10]

<img width="160" alt="image" src="https://github.com/user-attachments/assets/e8cd174c-5113-416b-9e3a-c4029e1e3176" />
<img width="160" alt="image" src="https://github.com/user-attachments/assets/3702a261-01fe-4b04-9e6c-b69336c2ce46" />
