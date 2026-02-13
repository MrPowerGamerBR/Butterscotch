# Butterscotch

> [!CAUTION]
> This is "vibe coded" to see how further Claude Code (Opus 4.6) can get trying to implement a GameMaker: Studio runner, and because it is fun!
>
> If you want a proper well made GameMaker runtime alternative, check out [OpenGM](https://github.com/misternebula/OpenGM)

## CLI parameters & debug features

Butterscotch has some CLI parameters and debug features that are useful when debugging the runner

* `--debug`: Enables debug features
* `--screenshot file-%s.png`: Saves a screenshot of the game to the specified file, the `%s` is the current frame index
* `--screenshot-at-frame FrameIndex`: Saves a screenshot of the game at the specified frame, can be used multiple times
* `--room RoomNameOrIndex`: Starts the game directly on the desired room, example: `--room room_ruins1`
* `--list-rooms`: Prints all the rooms to the console
* `--debug-obj`: Prints information about the desired object
* `--trace-calls`: Traces functions calls made by a specific object, example: `--trace-calls obj_friendypellet`. Can also trace all calls with `--trace-calls *`
* `--ignore-function-traced-calls`: Ignores specific function calls when tracing
* `--trace-events`: Traces all fired events for a specific object, example: `--trace-events obj_floweybattle1`. Can also trace all events with `--trace-events *`
* `--trace-instructions`: Traces all bytecode instructions executed by the VM for a specific GML script, example: `--trace-instructions gml_Object_obj_floweybattle1_Step_0`. Can be set to `--trace-instructions *` to trace all scripts. VERY NOISY!
* `--trace-global-vars`: Traces all global variables being manipulated, example: `--trace-global-vars interact`. Can also trace all globals with `--trace-global-vars *`
* `--trace-instance-vars`: Traces instance variable writes. Supports: `varname`, `obj.varname`, `obj.*`, `*` (VERY NOISY). Example: `--trace-instance-vars obj_shaker.hshake`
* `--trace-paths`: Traces path following for a specific object, example: `--trace-paths obj_toroverworld3`. Can also trace all objects with `--trace-paths *`
* `--draw-paths`: Draw path overlays on screen for all instances following paths
* `--always-log-unknown-instructions`: Always log unknown instructions instead of only logging once
* `--seed`: Forces the game to always use a specific RNG seed, even if the game sets a custom seed with `random_set_seed` or if `randomize` is called, example `--seed 40028922`, useful for reproducing bugs

### Debug Features

When `--debug` is enabled, the following features are enabled:

* `Page Up` and `Page Down`: Moves to the next room and to the previous room, respectively.
* `P` pauses the game, and `O` steps the game forward by one frame.
* `'` opens the debug console

#### Debug Console

Sometimes you need to debug something, like, for example, you want to jump to a specific room, but you don't want to jump it directly from the CLI because that skips the initial setup that the first room does, which breaks things.

You can see all of the debug console commands with `help`.

<img width="896" height="719" alt="20260213_165254_Butterscotch - UNDERTALE" src="https://github.com/user-attachments/assets/d51e18f6-7527-4570-a6b2-e3295f6da699" />

## Undertale running via Butterscotch

It is VERY buggy... but I will be honest, it is kinda impressive that it *does* run.

https://github.com/user-attachments/assets/cf30fc29-f39d-4982-9b3c-8d269485db5e
