# KGMSRunner

> [!CAUTION]
> This is "vibe coded" to see how further Claude Code (Opus 4.6) can get trying to implement a GameMaker: Studio runner, and because it is fun!
>
> If you want a proper well made GameMaker runtime alternative, check out [OpenGM](https://github.com/misternebula/OpenGM)

## CLI parameters & debug features

KGMSRunner has some CLI parameters and debug features that are useful when debugging the runner

* `--debug`: Enables debug features
* `--screenshot file-%s.png`: Saves a screenshot of the game to the specified file, the `%s` is the current frame index
* `--screenshot-at-frame FrameIndex`: Saves a screenshot of the game at the specified frame, can be used multiple times
* `--room RoomNameOrIndex`: Starts the game directly on the desired room, example: `--room room_ruins1`
* `--list-rooms`: Prints all the rooms to the console
* `--debug-obj`: Prints information about the desired object
* `--trace-calls`: Traces functions calls made by a specific object, example: `--trace-calls obj_friendypellet`. Can also trace all calls with `--trace-calls *`
* `--ignore-function-traced-calls`: Ignores specific function calls when tracing

### Debug Features

When `--debug` is enabled, the following features are enabled:

* `Page Up` and `Page Down`: Moves to the next room and to the previous room, respectively.

## Undertale running via KGMSRunner

It is VERY buggy... but I will be honest, it is kinda impressive that it *does* run.

https://github.com/user-attachments/assets/eb11ff6e-88c7-4008-9262-d33db9bee8fa

