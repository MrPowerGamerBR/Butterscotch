package com.mrpowergamerbr.butterscotch

import com.github.ajalt.clikt.core.CliktCommand
import com.github.ajalt.clikt.parameters.options.default
import com.github.ajalt.clikt.parameters.options.flag
import com.github.ajalt.clikt.parameters.options.multiple
import com.github.ajalt.clikt.parameters.options.option
import com.github.ajalt.clikt.parameters.types.boolean
import com.github.ajalt.clikt.parameters.types.double
import com.github.ajalt.clikt.parameters.types.int
import com.github.ajalt.clikt.parameters.types.long
import com.mrpowergamerbr.butterscotch.data.FormReader

class ButterscotchCLICommand : CliktCommand(name = "butterscotch") {
    private val screenshot by option("--screenshot", help = "Screenshot filename pattern (%s = frame number)")
    private val screenshotAtFrame by option("--screenshot-at-frame", help = "Frame number to capture screenshot").int().multiple()
    private val room by option("--room", help = "Start at a specific room (name or index)")
    private val listRooms by option("--list-rooms", help = "List all rooms and exit").flag()
    private val debugObj by option("--debug-obj", help = "Debug object name").multiple()
    private val traceCalls by option("--trace-calls", help = "Print all function calls of a specific object, can be set to \"*\" to log all objects").multiple()
    private val ignoreFunctionTracedCalls by option("--ignore-function-traced-calls", help = "Ignore specific function when tracing calls, useful to trim down logs").multiple()
    private val traceEvents by option("--trace-events", help = "Print all fired events for a specific object, can be set to \"*\" to log all objects").multiple()
    private val traceInstructions by option("--trace-instructions", help = "Print all bytecode instructions for a specific GML script, can be set to \"*\" to log all scripts, VERY NOISY").multiple()
    private val traceGlobalVars by option("--trace-global-vars", help = "Trace changes to a global variable (e.g. \"interact\"), can be set to \"*\" to log all").multiple()
    private val traceInstanceVars by option("--trace-instance-vars", help = "Trace instance variable writes. Supports: varname, obj.varname, obj.*, * (VERY NOISY)").multiple()
    private val tracePaths by option("--trace-paths", help = "Trace path following for a specific object, can be set to \"*\" to log all objects").multiple()
    private val drawPaths by option("--draw-paths", help = "Draw path overlays on screen for all instances following paths").flag()
    private val drawMasks by option("--draw-masks", help = "Draw collision boxes: green=non-precise, blue=precise (50% opacity)").flag()
    private val alwaysLogUnknownInstructions by option("--always-log-unknown-instructions", help = "Always log unknown instructions instead of only logging once").flag()

    private val listRoomInstances by option("--list-room-instances", help = "List all instances in a room (name or index)").multiple()
    private val debug by option("--debug", help = "Enable debug mode").flag()
    private val speed by option("--speed", help = "Game speed multiplier (e.g. 2.0 = twice as fast)").double().default(1.0)
    private val recordInputs by option("--record-inputs", help = "Record inputs to JSON file")
    private val playbackInputs by option("--playback-inputs", help = "Playback inputs from JSON file")
    private val vsync by option("--vsync", help = "Enable VSync, you must disable VSync if you are using a high speed rate for the game").boolean().default(true)
    private val rngSeed by option("--seed", help = "RNG seed for deterministic playback, randomize calls will always use this set seed").long()

    override fun run() {
        Butterscotch.debugObj = debugObj.toSet()
        Butterscotch.traceCalls = traceCalls.toSet()
        Butterscotch.traceFireEvents = traceEvents.toSet()
        Butterscotch.traceInstructions = traceInstructions.toSet()
        Butterscotch.ignoreFunctionTracedCalls = ignoreFunctionTracedCalls.toSet()
        Butterscotch.traceGlobalVars = traceGlobalVars.toSet()
        Butterscotch.traceInstanceVars = traceInstanceVars.toSet()
        Butterscotch.tracePaths = tracePaths.toSet()
        Butterscotch.drawPaths = drawPaths
        Butterscotch.drawMasks = drawMasks
        Butterscotch.alwaysLogUnknownInstructions = alwaysLogUnknownInstructions
        Butterscotch.debug = debug

        if (listRooms) {
            val gameData = FormReader("undertale/game.unx").read()
            for ((i, room) in gameData.rooms.withIndex()) {
                println("$i: ${room.name} (${room.width}x${room.height})")
            }
            return
        }

        if (debugObj.isNotEmpty()) {
            // Just parse the data file (which prints debug info) and exit
            FormReader("undertale/game.unx").read()
            return
        }

        if (listRoomInstances.isNotEmpty()) {
            val gameData = FormReader("undertale/game.unx").read()
            for (roomQuery in listRoomInstances) {
                val roomIndex = roomQuery.toIntOrNull()
                val room = if (roomIndex != null) {
                    gameData.rooms.getOrNull(roomIndex)
                } else {
                    gameData.rooms.find { it.name == roomQuery }
                }
                if (room == null) {
                    println("Room not found: $roomQuery")
                    continue
                }
                val actualIndex = gameData.rooms.indexOf(room)
                println("Room $actualIndex: ${room.name} (${room.width}x${room.height})")
                println("  Instances (${room.instances.size}):")
                for (inst in room.instances) {
                    val objName = gameData.objects.getOrNull(inst.objectDefId)?.name ?: "unknown"
                    println("    id=${inst.instanceId} obj=${inst.objectDefId} ($objName) pos=(${inst.x}, ${inst.y}) scale=(${inst.scaleX}, ${inst.scaleY}) rotation=${inst.rotation} creationCode=${inst.creationCodeId}")
                }
            }
            return
        }

        Butterscotch(
            screenshotPattern = screenshot,
            screenshotAtFrames = screenshotAtFrame.toSet(),
            startRoom = room,
            speedMultiplier = speed,
            recordInputsPath = recordInputs,
            playbackInputsPath = playbackInputs,
            vsync = vsync,
            rngSeed = rngSeed
        ).run()
    }
}

fun main(args: Array<String>) {
    ButterscotchCLICommand().main(args)
}